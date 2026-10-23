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
#include <sys/stat.h>
#include <unistd.h>

extern "C"
{
    // Crashes (data aborts, bad jumps): libnx calls this on its own stack instead of ending the process silently. The
    // log gets the exception and the registers as offsets in the ELF (addr2line -e ps2EntryRunner.elf <offset>).
    extern char __start__;
    alignas(16) u8 __nx_exception_stack[0x8000];
    u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

    void __libnx_exception_handler(ThreadExceptionDump *ctx)
    {
        const u64 base = reinterpret_cast<u64>(&__start__);
        std::fprintf(stderr, "[switch] EXCEPTION desc=%u pc=%#lx (elf+%#lx) lr=%#lx (elf+%#lx) far=%#lx sp=%#lx esr=%#x\n",
                     ctx->error_desc, ctx->pc.x, ctx->pc.x - base, ctx->lr.x, ctx->lr.x - base, ctx->far.x, ctx->sp.x, ctx->esr);
        for (int i = 0; i < 29; i += 4)
            std::fprintf(stderr, "[switch]   x%-2d=%#lx x%-2d=%#lx x%-2d=%#lx x%-2d=%#lx\n", i, ctx->cpu_gprs[i].x, i + 1,
                         i + 1 < 29 ? ctx->cpu_gprs[i + 1].x : 0ul, i + 2, i + 2 < 29 ? ctx->cpu_gprs[i + 2].x : 0ul, i + 3,
                         i + 3 < 29 ? ctx->cpu_gprs[i + 3].x : 0ul);
        std::fflush(stderr);
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
    static u32 s_cpuHzWanted = 0;
    static u32 s_cpuHzBefore = 0;
    static Thread s_clockThread;
    static UEvent s_clockStop;

    static bool cpuClockGet(u32 *hz)
    {
        if (hosversionAtLeast(8, 0, 0))
        {
            ClkrstSession session;
            if (R_FAILED(clkrstOpenSession(&session, PcvModuleId_CpuBus, 3)))
                return false;
            const bool ok = R_SUCCEEDED(clkrstGetClockRate(&session, hz));
            clkrstCloseSession(&session);
            return ok;
        }
        return R_SUCCEEDED(pcvGetClockRate(PcvModule_CpuBus, hz));
    }

    static Result cpuClockSet(u32 hz)
    {
        if (hosversionAtLeast(8, 0, 0))
        {
            ClkrstSession session;
            Result rc = clkrstOpenSession(&session, PcvModuleId_CpuBus, 3);
            if (R_SUCCEEDED(rc))
            {
                rc = clkrstSetClockRate(&session, hz);
                clkrstCloseSession(&session);
            }
            return rc;
        }
        return pcvSetClockRate(PcvModule_CpuBus, hz);
    }

    static void clockKeeper(void *)
    {
        // waitSingle returns 0 when the stop event fires, a timeout result every 2 s otherwise
        while (R_FAILED(waitSingle(waiterForUEvent(&s_clockStop), 2000000000ull)))
        {
            u32 now = 0;
            if (cpuClockGet(&now) && now != s_cpuHzWanted)
                cpuClockSet(s_cpuHzWanted);
        }
    }

    static void startCpuClock()
    {
        const char *value = std::getenv("GOW_CPU_MHZ");
        const u32 mhz = value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 1785u;
        if (mhz == 0u)
            return;
        const bool clkrst = hosversionAtLeast(8, 0, 0);
        if (R_FAILED(clkrst ? clkrstInitialize() : pcvInitialize()))
            return;
        cpuClockGet(&s_cpuHzBefore);
        s_cpuHzWanted = mhz * 1000000u;
        const Result rc = cpuClockSet(s_cpuHzWanted);
        u32 now = 0;
        cpuClockGet(&now);
        std::fprintf(stderr, "[switch] cpu clock %u -> %u Hz (rc=%x)\n", s_cpuHzBefore, now, rc);
        ueventCreate(&s_clockStop, false);
        if (R_FAILED(threadCreate(&s_clockThread, clockKeeper, nullptr, nullptr, 0x4000, 0x3B, -2)) ||
            R_FAILED(threadStart(&s_clockThread)))
            s_clockThread = {};
    }

    static void stopCpuClock()
    {
        if (s_cpuHzWanted == 0u)
            return;
        if (s_clockThread.handle)
        {
            ueventSignal(&s_clockStop);
            threadWaitForExit(&s_clockThread);
            threadClose(&s_clockThread);
        }
        if (s_cpuHzBefore != 0u)
            cpuClockSet(s_cpuHzBefore);
        if (hosversionAtLeast(8, 0, 0))
            clkrstExit();
        else
            pcvExit();
        s_cpuHzWanted = 0;
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
            // one file descriptor for both streams (two opens of the file would overwrite each other's lines)
            if (std::freopen("sdmc:/switch/gow/log.txt", "w", stdout))
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
        startCpuClock();
    }

    void userAppExit(void)
    {
        stopCpuClock();
        std::fflush(stdout);
        std::fflush(stderr);
        if (s_nxlinkSocket >= 0)
            close(s_nxlinkSocket);
        socketExit();
        romfsExit();
    }
}

#endif // PLATFORM_SWITCH
