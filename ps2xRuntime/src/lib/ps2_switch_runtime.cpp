// GOW-Port: Nintendo Switch homebrew start-up (libnx calls userAppInit/userAppExit around main()).
// - Logs: stdout/stderr go to `nxlink -s` on the PC when a host listens, else to sdmc:/switch/gow/log.txt (also what
//   an emulator such as Citron shows in its virtual SD card).
// - The settings play.cmd sets on Windows are defaults here (no shell to set them): GS work on its worker thread, the
//   asynchronous GS FINISH, and the memory card file next to the game data.
#if defined(PLATFORM_SWITCH)

#include <switch.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <fcntl.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C"
{
    static void logDrain(bool locked);

    // Crashes (data aborts, bad jumps): libnx calls this on its own stack instead of ending the process silently. The
    // log gets the exception and the registers as offsets in the ELF (addr2line -e ps2EntryRunner.elf <offset>).
    extern char __start__;
    alignas(16) u8 __nx_exception_stack[0x8000];
    u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

    // Load address of this module: __start__ is an absolute symbol (0) in the -pie link, so &__start__ is not relocated;
    // the start of the code mapping that holds this function is the module base.
    static u64 moduleBase()
    {
        MemoryInfo info{};
        u32 page = 0;
        if (R_SUCCEEDED(svcQueryMemory(&info, &page, reinterpret_cast<u64>(&moduleBase))))
            return info.addr;
        return reinterpret_cast<u64>(&__start__);
    }

    void __libnx_exception_handler(ThreadExceptionDump *ctx)
    {
        const u64 base = moduleBase();
        std::fprintf(stderr, "[switch] EXCEPTION desc=%u pc=%#lx (elf+%#lx) lr=%#lx (elf+%#lx) far=%#lx sp=%#lx esr=%#x\n",
                     ctx->error_desc, ctx->pc.x, ctx->pc.x - base, ctx->lr.x, ctx->lr.x - base, ctx->far.x, ctx->sp.x, ctx->esr);
        for (int i = 0; i < 29; i += 4)
            std::fprintf(stderr, "[switch]   x%-2d=%#lx x%-2d=%#lx x%-2d=%#lx x%-2d=%#lx\n", i, ctx->cpu_gprs[i].x, i + 1,
                         i + 1 < 29 ? ctx->cpu_gprs[i + 1].x : 0ul, i + 2, i + 2 < 29 ? ctx->cpu_gprs[i + 2].x : 0ul, i + 3,
                         i + 3 < 29 ? ctx->cpu_gprs[i + 3].x : 0ul);
        std::fflush(stderr);
        logDrain(false); // the log ring (below) still holds the last lines
        svcExitProcess();
    }

    // GOW_EE_SAMPLE sampler: std::this_thread::sleep_for throws ENOSYS on this libc
    void ps2xSwitchSleepNs(long long ns) { svcSleepThread(ns); }

    // ThreadNaming.h: pin the calling thread to one application core (0-2)
    void ps2xSwitchPinCurrentThread(int core, int priorityBoost)
    {
        const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
        s32 priority = 0;
        Result prc = 0;
        if (priorityBoost != 0 && R_SUCCEEDED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)))
            prc = svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(priority - priorityBoost));
        std::fprintf(stderr, "[switch] thread pinned to core %d%s, priority boost %d%s\n", core,
                     R_SUCCEEDED(rc) ? "" : " (failed)", priorityBoost, R_SUCCEEDED(prc) ? "" : " (failed)");
    }

    // Thread stacks: libnx gives every pthread (std::thread included) 128 KiB unless a size is set; the runtime's
    // threads were sized by the 1 MiB Windows default (the VIF1/VU1 worker overflowed 128 KiB on its first VU1 job:
    // a silent jump to 0). Linked with --wrap=pthread_create (CMakeLists.txt), threads without an explicit stack size
    // get 1 MiB; explicit sizes (SDL's) are kept.
    int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);

    int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
    {
        constexpr size_t hostThreadStack = 1u << 20;
        size_t size = 0;
        if (attr)
            pthread_attr_getstacksize(attr, &size);
        if (size != 0)
            return __real_pthread_create(thread, attr, start, arg);
        pthread_attr_t sized;
        if (attr)
            sized = *attr;
        else
            pthread_attr_init(&sized);
        pthread_attr_setstacksize(&sized, hostThreadStack);
        const int result = __real_pthread_create(thread, &sized, start, arg);
        if (!attr)
            pthread_attr_destroy(&sized);
        return result;
    }

    static int s_nxlinkSocket = -1;
    // The EE thread is the bottleneck on the A57s. Applications run the CPU at 1020 MHz; 1785 MHz is Nintendo's own
    // boost clock (games use it while loading; sys-clk sets it the same way). GOW_CPU_MHZ in env.txt picks another
    // rate, 0 leaves the clock alone. The system resets the rate on dock changes and when the game returns from HOME,
    // so a small thread re-applies it every 2 s; the rate found at start is restored at exit.
    // Memory (EMC) and GPU are left at the system's rates unless env.txt asks: GOW_EMC_MHZ (1600 = the docked rate,
    // "max" = 1600) and GOW_GPU_MHZ (e.g. 768, 460.8; "max" = 768 docked / 460.8 handheld, Nintendo's top rate for the
    // mode). The start-up line [switch] mode/clocks says docked or handheld and the three rates either way.
    enum ClockIndex { ClockCpu, ClockEmc, ClockGpu, ClockCount };
    struct ClockSlot
    {
        const char *name;
        PcvModuleId id;   // clkrst (HOS 8+)
        PcvModule module; // pcv (older)
        u32 hzWanted;     // 0 = leave alone
        u32 hzBefore;
    };
    static ClockSlot s_clocks[ClockCount] = {
        {"cpu", PcvModuleId_CpuBus, PcvModule_CpuBus, 0, 0},
        {"emc", PcvModuleId_EMC, PcvModule_EMC, 0, 0},
        {"gpu", PcvModuleId_GPU, PcvModule_GPU, 0, 0},
    };
    static bool s_clockServiceOpen = false;
    static Thread s_clockThread;
    static UEvent s_clockStop;

    static bool clockGet(const ClockSlot &clock, u32 *hz)
    {
        if (hosversionAtLeast(8, 0, 0))
        {
            ClkrstSession session;
            if (R_FAILED(clkrstOpenSession(&session, clock.id, 3)))
                return false;
            const bool ok = R_SUCCEEDED(clkrstGetClockRate(&session, hz));
            clkrstCloseSession(&session);
            return ok;
        }
        return R_SUCCEEDED(pcvGetClockRate(clock.module, hz));
    }

    static Result clockSet(const ClockSlot &clock, u32 hz)
    {
        if (hosversionAtLeast(8, 0, 0))
        {
            ClkrstSession session;
            Result rc = clkrstOpenSession(&session, clock.id, 3);
            if (R_SUCCEEDED(rc))
            {
                rc = clkrstSetClockRate(&session, hz);
                clkrstCloseSession(&session);
            }
            return rc;
        }
        return pcvSetClockRate(clock.module, hz);
    }

    static bool anyClockWanted()
    {
        for (const ClockSlot &clock : s_clocks)
            if (clock.hzWanted != 0u)
                return true;
        return false;
    }

    static void clockKeeper(void *)
    {
        // waitSingle returns 0 when the stop event fires, a timeout result every 2 s otherwise
        while (R_FAILED(waitSingle(waiterForUEvent(&s_clockStop), 2000000000ull)))
        {
            for (const ClockSlot &clock : s_clocks)
            {
                u32 now = 0;
                if (clock.hzWanted != 0u && clockGet(clock, &now) && now != clock.hzWanted)
                {
                    clockSet(clock, clock.hzWanted);
                    std::fprintf(stderr, "[switch] %s clock was %u Hz, re-applied %u\n", clock.name, now, clock.hzWanted);
                }
            }
        }
    }

    // env value in MHz (decimals allowed: 460.8); "max" -> maxHz; unset -> fallbackHz
    static u32 clockWantedHz(const char *variable, u32 fallbackHz, u32 maxHz)
    {
        const char *value = std::getenv(variable);
        if (!value)
            return fallbackHz;
        if (std::strcmp(value, "max") == 0)
            return maxHz;
        return static_cast<u32>(std::strtod(value, nullptr) * 1000000.0 + 0.5);
    }

    static void startClocks()
    {
        const AppletOperationMode mode = appletGetOperationMode();
        const bool docked = mode == AppletOperationMode_Console;
        s_clocks[ClockCpu].hzWanted = clockWantedHz("GOW_CPU_MHZ", 1785000000u, 1785000000u);
        s_clocks[ClockEmc].hzWanted = clockWantedHz("GOW_EMC_MHZ", 0u, 1600000000u);
        s_clocks[ClockGpu].hzWanted = clockWantedHz("GOW_GPU_MHZ", 0u, docked ? 768000000u : 460800000u);
        const bool clkrst = hosversionAtLeast(8, 0, 0);
        if (R_FAILED(clkrst ? clkrstInitialize() : pcvInitialize()))
        {
            std::fprintf(stderr, "[switch] mode %s, clock service unavailable\n", docked ? "docked" : "handheld");
            for (ClockSlot &clock : s_clocks)
                clock.hzWanted = 0;
            return;
        }
        s_clockServiceOpen = true;
        u32 now[ClockCount] = {};
        Result rc[ClockCount] = {};
        for (int i = 0; i < ClockCount; ++i)
        {
            ClockSlot &clock = s_clocks[i];
            clockGet(clock, &clock.hzBefore);
            if (clock.hzWanted != 0u)
                rc[i] = clockSet(clock, clock.hzWanted);
            clockGet(clock, &now[i]);
        }
        std::fprintf(stderr,
                     "[switch] mode %s; clocks (Hz, start -> now): cpu %u -> %u (rc=%x), emc %u -> %u (rc=%x), gpu %u -> %u (rc=%x)\n",
                     docked ? "docked" : "handheld", s_clocks[ClockCpu].hzBefore, now[ClockCpu], rc[ClockCpu],
                     s_clocks[ClockEmc].hzBefore, now[ClockEmc], rc[ClockEmc], s_clocks[ClockGpu].hzBefore, now[ClockGpu],
                     rc[ClockGpu]);
        if (!anyClockWanted())
            return;
        ueventCreate(&s_clockStop, false);
        if (R_FAILED(threadCreate(&s_clockThread, clockKeeper, nullptr, nullptr, 0x4000, 0x3B, -2)) ||
            R_FAILED(threadStart(&s_clockThread)))
            s_clockThread = {};
    }

    static void stopClocks()
    {
        if (!s_clockServiceOpen)
            return;
        if (s_clockThread.handle)
        {
            ueventSignal(&s_clockStop);
            threadWaitForExit(&s_clockThread);
            threadClose(&s_clockThread);
        }
        for (ClockSlot &clock : s_clocks)
        {
            if (clock.hzWanted != 0u && clock.hzBefore != 0u)
                clockSet(clock, clock.hzBefore);
            clock.hzWanted = 0;
        }
        if (hosversionAtLeast(8, 0, 0))
            clkrstExit();
        else
            pcvExit();
        s_clockServiceOpen = false;
    }

    // Log ring: stdout/stderr go into a memory buffer that a low-priority thread on core 0 writes to the SD card every
    // 100 ms. Unbuffered writes straight to the SD file blocked the printing thread (console 2026-10-23, update_ee3 with
    // PS2X_GS_WORKER_TRACE: consecutive trace lines from three threads ~56 ms apart), so every speed line on the EE, GS
    // stage or VU1 thread could stall a frame. [switch-log] every 30 s says what the SD writes cost.
    static constexpr size_t kLogRing = 1u << 20;
    static char s_logBuf[kLogRing];
    static char s_logOut[kLogRing];
    static size_t s_logLen = 0;
    static u64 s_logDropped = 0;
    static Mutex s_logLock;
    static int s_logFd = -1;
    static Thread s_logThread;
    static volatile bool s_logStop = false;
    static bool s_logThreadUp = false;
    static devoptab_t s_logDevoptab;
    static u64 s_logWrites = 0, s_logBytes = 0, s_logWriteNs = 0, s_logMaxNs = 0;

    static int logOpen(struct _reent *, void *, const char *, int, int) { return 0; }
    static int logClose(struct _reent *, void *) { return 0; }
    static ssize_t logWrite(struct _reent *, void *, const char *ptr, size_t len)
    {
        if (!s_logThreadUp)
            return write(s_logFd, ptr, len);
        mutexLock(&s_logLock);
        if (s_logLen + len <= kLogRing)
        {
            std::memcpy(s_logBuf + s_logLen, ptr, len);
            s_logLen += len;
        }
        else
            ++s_logDropped;
        mutexUnlock(&s_logLock);
        return static_cast<ssize_t>(len);
    }

    // locked: false only from the exception handler (the crashed thread may hold the lock)
    static void logDrain(bool locked)
    {
        size_t n = 0;
        if (locked)
            mutexLock(&s_logLock);
        n = s_logLen;
        if (n != 0u)
            std::memcpy(s_logOut, s_logBuf, n);
        s_logLen = 0;
        if (locked)
            mutexUnlock(&s_logLock);
        if (n == 0u || s_logFd < 0)
            return;
        const u64 t0 = armGetSystemTick();
        for (size_t off = 0; off < n;)
        {
            const ssize_t w = write(s_logFd, s_logOut + off, n - off);
            if (w <= 0)
                break;
            off += static_cast<size_t>(w);
        }
        const u64 ns = armTicksToNs(armGetSystemTick() - t0);
        ++s_logWrites;
        s_logBytes += n;
        s_logWriteNs += ns;
        if (ns > s_logMaxNs)
            s_logMaxNs = ns;
    }

    static void logThreadMain(void *)
    {
        u64 lastReport = armGetSystemTick();
        while (!s_logStop)
        {
            svcSleepThread(100000000ll);
            logDrain(true);
            if (armTicksToNs(armGetSystemTick() - lastReport) >= 30000000000ull)
            {
                lastReport = armGetSystemTick();
                std::fprintf(stderr, "[switch-log] sd writes=%llu bytes=%llu avg_ms=%.2f max_ms=%.2f dropped=%llu\n",
                             static_cast<unsigned long long>(s_logWrites), static_cast<unsigned long long>(s_logBytes),
                             s_logWrites ? s_logWriteNs / 1e6 / s_logWrites : 0.0, s_logMaxNs / 1e6,
                             static_cast<unsigned long long>(s_logDropped));
                s_logMaxNs = 0;
            }
        }
        logDrain(true);
    }

    // true: stdout/stderr now go to the ring; false: the caller falls back to the plain SD file
    static bool logStart()
    {
        s_logFd = open("sdmc:/switch/gow/log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (s_logFd < 0)
            return false;
        mutexInit(&s_logLock);
        std::memset(&s_logDevoptab, 0, sizeof(s_logDevoptab));
        s_logDevoptab.name = "gowlog";
        s_logDevoptab.structSize = sizeof(int);
        s_logDevoptab.open_r = logOpen;
        s_logDevoptab.close_r = logClose;
        s_logDevoptab.write_r = logWrite;
        if (AddDevice(&s_logDevoptab) < 0 || !std::freopen("gowlog:/log", "w", stdout))
        {
            close(s_logFd);
            s_logFd = -1;
            return false;
        }
        dup2(fileno(stdout), fileno(stderr));
        // priority 0x3B (below every game thread), core 0
        if (R_SUCCEEDED(threadCreate(&s_logThread, logThreadMain, nullptr, nullptr, 0x10000, 0x3B, 0)) &&
            R_SUCCEEDED(threadStart(&s_logThread)))
            s_logThreadUp = true;
        return true;
    }

    static void logStop()
    {
        if (s_logThreadUp)
        {
            s_logStop = true;
            threadWaitForExit(&s_logThread);
            threadClose(&s_logThread);
            s_logThreadUp = false;
        }
        logDrain(true);
    }

    // GOW_PC_SAMPLE=<hz>: wall-time PC sampling of named threads (GOW_PC_SAMPLE_THREADS, comma list, default GSWorker =
    // the VIF1/VU1 thread): a thread on core GOW_PC_SAMPLE_CORE (default 0) pauses each one, reads its PC
    // (svcGetThreadContext3) and lets it run again. Every 5 s the histogram of PC offsets in the ELF is appended to
    // sdmc:/switch/gow/pcs_<name>.txt ("w <ms since start> <samples> <failed>", "<offset hex> <count>" and caller lines); map them with
    // tools/switch_pcprof.py against the build's ps2EntryRunner.elf. Unlike the replay harness this sees the live A57:
    // cache misses, other cores' memory traffic, the code outside the VU1 programs. Each sample stops the thread ~10 us.
    // open-addressed counts by a non-zero 64-bit key
    struct PcHist
    {
        static constexpr u32 kSize = 16384;
        u64 keys[kSize];
        u32 counts[kSize];
        u32 distinct;
        void add(u64 key)
        {
            for (u32 h = static_cast<u32>((key * 0x9E3779B97F4A7C15ull) >> 50);; h = (h + 1u) & (kSize - 1u))
                if (keys[h] == key || keys[h] == 0u)
                {
                    if (keys[h] == 0u)
                    {
                        if (distinct >= kSize - kSize / 8u) // nearly full: drop rather than probe forever
                            return;
                        ++distinct;
                        keys[h] = key;
                    }
                    ++counts[h];
                    return;
                }
        }
        void clear()
        {
            std::memset(keys, 0, sizeof(keys));
            std::memset(counts, 0, sizeof(counts));
            distinct = 0;
        }
    };
    struct PcSampled
    {
        Handle handle;
        char name[16];
        PcHist pcs;
        // samples stopped in a system call (waits, mutex arbitration): the svc number and the first two return
        // addresses found on the thread's stack (words in the code whose previous instruction is a BL / BLR; a scan,
        // so a stale frame can show up), "c <svc> <ra1> <ra2> <count>" lines: who waits or contends
        PcHist callers;
        u32 samples, failed;
    };
    static PcSampled *s_pcSampled[4];
    static int s_pcSampledCount = 0;
    static Mutex s_pcLock;
    static Thread s_pcThread;
    static bool s_pcThreadUp = false;

    static void pcSamplerMain(void *arg)
    {
        const u64 periodNs = 1000000000ull / reinterpret_cast<uintptr_t>(arg);
        const u64 base = moduleBase();
        u64 textEnd = base;
        {
            MemoryInfo info{};
            u32 page = 0;
            if (R_SUCCEEDED(svcQueryMemory(&info, &page, base)))
                textEnd = info.addr + info.size;
        }
        const auto isCall = [&](u64 ra) {
            if (ra < base + 4u || ra >= textEnd || (ra & 3u) != 0u)
                return false;
            const u32 insn = *reinterpret_cast<const u32 *>(ra - 4u);
            return (insn & 0xFC000000u) == 0x94000000u || (insn & 0xFFFFFC1Fu) == 0xD63F0000u; // BL / BLR
        };
        const u64 t0 = armGetSystemTick();
        u64 lastWrite = t0;
        for (;;)
        {
            svcSleepThread(static_cast<s64>(periodNs));
            mutexLock(&s_pcLock);
            for (int i = 0; i < s_pcSampledCount; ++i)
            {
                PcSampled &t = *s_pcSampled[i];
                ThreadContext ctx;
                if (R_FAILED(svcSetThreadActivity(t.handle, ThreadActivity_Paused)))
                {
                    ++t.failed;
                    continue;
                }
                const Result rc = svcGetThreadContext3(&ctx, t.handle);
                if (R_SUCCEEDED(rc) && ctx.pc.x >= base + 4u && ctx.pc.x < textEnd)
                {
                    // in a system call: pc is at or after the svc instruction
                    u32 svcInsn = *reinterpret_cast<const u32 *>(ctx.pc.x);
                    if ((svcInsn & 0xFFE0001Fu) != 0xD4000001u)
                        svcInsn = *reinterpret_cast<const u32 *>(ctx.pc.x - 4u);
                    if ((svcInsn & 0xFFE0001Fu) == 0xD4000001u)
                    {
                        u64 ra[2] = {};
                        int found = 0;
                        const u64 *stack = reinterpret_cast<const u64 *>(ctx.sp & ~7ull);
                        for (int k = 0; k < 96 && found < 2; ++k) // the stack is ours and the thread is paused
                            if (isCall(stack[k]))
                                ra[found++] = stack[k] - base;
                        t.callers.add((static_cast<u64>((svcInsn >> 5) & 0xFFu) << 56) | ((ra[0] & 0xFFFFFFFu) << 28) |
                                      (ra[1] & 0xFFFFFFFu) | 1ull << 63);
                    }
                }
                svcSetThreadActivity(t.handle, ThreadActivity_Runnable);
                if (R_FAILED(rc))
                {
                    ++t.failed;
                    continue;
                }
                t.pcs.add(static_cast<u32>(ctx.pc.x - base) | 1u); // odd: never 0 (the free marker)
                ++t.samples;
            }
            mutexUnlock(&s_pcLock);
            const u64 now = armGetSystemTick();
            if (armTicksToNs(now - lastWrite) < 5000000000ull)
                continue;
            lastWrite = now;
            mutexLock(&s_pcLock);
            for (int i = 0; i < s_pcSampledCount; ++i)
            {
                PcSampled &t = *s_pcSampled[i];
                char path[64];
                std::snprintf(path, sizeof(path), "sdmc:/switch/gow/pcs_%s.txt", t.name);
                if (std::FILE *f = std::fopen(path, "a"))
                {
                    std::fprintf(f, "w %llu %u %u\n", static_cast<unsigned long long>(armTicksToNs(now - t0) / 1000000u), t.samples, t.failed);
                    for (u32 h = 0; h < PcHist::kSize; ++h)
                        if (t.pcs.keys[h] != 0u)
                            std::fprintf(f, "%x %u\n", static_cast<u32>(t.pcs.keys[h]) & ~1u, t.pcs.counts[h]);
                    for (u32 h = 0; h < PcHist::kSize; ++h)
                        if (const u64 k = t.callers.keys[h])
                            std::fprintf(f, "c %x %x %x %u\n", static_cast<u32>((k >> 56) & 0x7Fu), static_cast<u32>((k >> 28) & 0xFFFFFFFu),
                                         static_cast<u32>(k & 0xFFFFFFFu), t.callers.counts[h]);
                    std::fclose(f);
                }
                t.pcs.clear();
                t.callers.clear();
                t.samples = t.failed = 0;
            }
            mutexUnlock(&s_pcLock);
        }
    }

    // ThreadNaming.h, from the named thread itself
    void ps2xSwitchPcSampleRegister(const char *name)
    {
        const char *hz = std::getenv("GOW_PC_SAMPLE");
        if (!hz || std::atoi(hz) <= 0)
            return;
        const char *list = std::getenv("GOW_PC_SAMPLE_THREADS");
        if (!list)
            list = "GSWorker";
        const size_t n = std::strlen(name);
        bool wanted = false;
        for (const char *p = list; *p;)
        {
            const char *comma = std::strchr(p, ',');
            const size_t len = comma ? static_cast<size_t>(comma - p) : std::strlen(p);
            wanted = wanted || (len == n && std::strncmp(p, name, n) == 0);
            p += len + (comma ? 1u : 0u);
        }
        if (!wanted)
            return;
        static bool started = false;
        if (!started)
        {
            started = true;
            mutexInit(&s_pcLock);
            const char *coreVar = std::getenv("GOW_PC_SAMPLE_CORE");
            const int core = coreVar && coreVar[0] >= '0' && coreVar[0] <= '2' ? coreVar[0] - '0' : 0;
            // priority 0x20: above the game threads, so the sampler wakes on time on a busy core (it sleeps otherwise)
            s_pcThreadUp = R_SUCCEEDED(threadCreate(&s_pcThread, pcSamplerMain, reinterpret_cast<void *>(static_cast<uintptr_t>(std::atoi(hz))),
                                                    nullptr, 0x8000, 0x20, core)) &&
                           R_SUCCEEDED(threadStart(&s_pcThread));
            std::fprintf(stderr, "[pc-sample] %s Hz on core %d%s\n", hz, core, s_pcThreadUp ? "" : " (thread failed)");
        }
        mutexLock(&s_pcLock);
        if (s_pcSampledCount < 4)
        {
            PcSampled *t = static_cast<PcSampled *>(std::calloc(1, sizeof(PcSampled)));
            t->handle = threadGetCurHandle();
            std::snprintf(t->name, sizeof(t->name), "%s", name);
            s_pcSampled[s_pcSampledCount++] = t;
            std::fprintf(stderr, "[pc-sample] thread %s\n", name);
        }
        mutexUnlock(&s_pcLock);
    }

    void userAppInit(void)
    {
        // the installed title (NSP) carries the extracted disc in its RomFS (romfs:/disc-usa/...); an NRO has none and
        // reads it from the SD card. The SD folder holds what is written: log, memory card, settings.
        romfsInit();
        mkdir("sdmc:/switch", 0777);
        mkdir("sdmc:/switch/gow", 0777);
        if (R_SUCCEEDED(socketInitializeDefault()))
            s_nxlinkSocket = nxlinkStdio();
        if (s_nxlinkSocket < 0)
        {
            // the previous run's log survives one more launch (a crash, then a restart, keeps the crash log)
            std::remove("sdmc:/switch/gow/log_prev.txt");
            std::rename("sdmc:/switch/gow/log.txt", "sdmc:/switch/gow/log_prev.txt");
            // one file descriptor for both streams (two opens of the file would overwrite each other's lines): the log
            // ring, else the SD file itself
            if (!logStart() && std::freopen("sdmc:/switch/gow/log.txt", "w", stdout))
                dup2(fileno(stdout), fileno(stderr));
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            std::setvbuf(stderr, nullptr, _IONBF, 0);
        }
        // sdmc:/switch/gow/env.txt: KEY=VALUE lines (# comments) applied first, like the `set` lines of play.cmd
        if (std::FILE *env = std::fopen("sdmc:/switch/gow/env.txt", "r"))
        {
            char line[512];
            while (std::fgets(line, sizeof(line), env))
            {
                char *end = line + std::strcspn(line, "\r\n");
                *end = '\0';
                char *equals = std::strchr(line, '=');
                if (line[0] == '#' || !equals)
                    continue;
                *equals = '\0';
                setenv(line, equals + 1, 1);
                std::fprintf(stderr, "[switch] env %s=%s\n", line, equals + 1);
            }
            std::fclose(env);
        }
        setenv("PS2X_GS_WORKER", "1", 0);
        setenv("GOW_GS_FINISH_ASINCRONO", "1", 0);
        setenv("GOW_MC0", "sdmc:/switch/gow/play.mcd", 0);
#if defined(PS2X_DK)
        // deko3d renderer (docs/switch-deko3d.md): the PS2's own 640x448 render targets, upscaled to the 720p screen on the
        // GPU by the bicubic + sharpen present pass. Console 2026-10-19, gameplay: 1x 19-28 fps, 1.6x (720 lines) 12-15 fps.
        // env.txt PS2X_GS_HW_SCALE=1.6 renders at 720p, GOW_SCALER=bilinear presents with one tap.
        setenv("PS2X_GS_HW_SCALE", "1", 0);
#else
        // one bilinear tap to present (the bicubic + sharpen pass costs GPU time the Tegra lacks, and crashes Citron)
        setenv("GOW_SCALER", "bilinear", 0);
        // native PS2 resolution (640x448 targets): the console shows 720p, and the 2x targets (up to 2048x2048) cost the
        // Tegra four times the fill and readback for detail the screen cannot show
        setenv("PS2X_GS_HW_SCALE", "1", 0);
#endif
        // the SoundMix thread takes batches of 32 samples: on the A57 the least IOP time left on core 1 of 128/64/32
        // (docs/sound.md "Mixer thread", console table)
        setenv("GOW_SOUND_BATCH", "32", 0);
        // the IOP runs inline on the EE thread: on the console the IopRun thread (core 0, next to the deko3d thread)
        // fell behind and the EE waited ~0.8-1.0 s per 5 s for its lead in fights, slower than inline (same build,
        // bot fight: 22.6-26 fps vs 26-31 after the heavy part; docs/switch-deko3d.md "Draw count"); env.txt
        // GOW_IOP_THREAD=1 turns it on
        setenv("GOW_IOP_THREAD", "0", 0);
        // VU1 native blocks with traces and direct chaining: on the console (A57) the title menu runs 37-40 fps vs 31 without
        // them (2 runs, docs/vu1-gpu.md); the fights are not worse (16-30 vs 15-24, scenes vary); env.txt =0 turns them off
        setenv("GOW_VU1N_TRACES", "1", 0);
        setenv("GOW_VU1N_DIRECT", "1", 0);
        // the plain-float kernels for the hot vertex loops (ps2_vu1_hle.inl, docs/vu1-gpu.md): replay benchmark on the console
        // fight 128.6 -> 108.4 ms, menu 146.9 -> 115.2 ms (6000 warm runs), menu 38-40 -> 44-46 fps; verified against the exact
        // path on the recordings (tolerances in docs/vu1-gpu.md); env.txt GOW_VU1_HLE=0 turns them off
        setenv("GOW_VU1_HLE", "1", 0);
        // speed lines in every log (a few lines every 5 s), so a log from the console says where the time goes
        setenv("GOW_PERF_DIAG", "1", 0);
        setenv("GOW_PERF_FRAME_PC", "0x001837B8", 0);
        setenv("GOW_DELTA_DIAG", "1", 0);
        // GPU diagnostics are opt-in again (env.txt GOW_GS_HW_STATS=1 / GOW_GS_GPU_TIME=1 / GOW_GPU_BISECT=1): the bisect
        // phases draw wrong pictures on purpose, and a timer query per draw plus glGetError per batch cost the GL thread
        // time; the [gow-perf]/[gs-gl]/[gs-hw-cpu] lines above say where the time goes without them
        startClocks();
    }

    void userAppExit(void)
    {
        stopClocks();
        std::fflush(stdout);
        std::fflush(stderr);
        logStop();
        if (s_nxlinkSocket >= 0)
            close(s_nxlinkSocket);
        socketExit();
        romfsExit();
    }
}

#endif // PLATFORM_SWITCH
