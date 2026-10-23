#ifndef PS2_PERF_H
#define PS2_PERF_H

// GOW-Port: perfil opcional de tiempo transcurrido exclusivo, separado por hilo.
// Los tiempos incluyen esperas del sistema; no representan muestras de uso de CPU.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>

namespace ps2_perf
{
    // GsQueueWait: the GS frontend blocked on a full GL-thread queue (or a drain); counted apart from Gs so the log shows
    // whether the frontend or the GL thread behind it is the limit
    enum class Bucket : size_t { Ee, Vu, Gs, Iop, GuestWait, Upload, HostWait, GsQueueWait, Count, None = Count };
    constexpr size_t bucketCount = static_cast<size_t>(Bucket::Count);

    // Contabilidad con reloj inyectable para comprobar el anidamiento sin sleeps.
    struct Accounting
    {
        Bucket active = Bucket::None;
        uint64_t since = 0;

        struct Charge { Bucket bucket; uint64_t nanos; };
        Charge switchTo(Bucket next, uint64_t now)
        {
            const Charge charge{active, now >= since ? now - since : 0u};
            active = next;
            since = now;
            return charge;
        }
    };

    inline bool enabled()
    {
        static const bool on = [] {
            const char *value = std::getenv("GOW_PERF_DIAG");
            return value && std::strcmp(value, "1") == 0;
        }();
        return on;
    }
    inline bool countFrames()
    {
        static const bool on = [] {
            const char *value = std::getenv("GOW_PERF_DIAG");
            return value && (std::strcmp(value, "1") == 0 || std::strcmp(value, "frames") == 0);
        }();
        return on;
    }
    inline std::array<std::atomic<uint64_t>, bucketCount> nanos{};
    inline std::atomic<uint64_t> guestFlips{0}, hostFrames{0};
    inline thread_local Accounting accounting;
    inline uint64_t nowNanos()
    {
#if defined(__SWITCH__) && defined(__aarch64__)
        // the system counter (19.2 MHz, shared by all cores) instead of clock_gettime: the VU1 thread takes the time
        // twice per microprogram run (~1.4 % of its samples on the console, GOW_PC_SAMPLE 2026-10-23)
        uint64_t ticks;
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks)); // as libnx armGetSystemTick (cntvct_el0 traps at EL0)
        return ticks * 625u / 12u;
#else
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    }
    inline void switchTo(Bucket next)
    {
        const auto charge = accounting.switchTo(next, nowNanos());
        if (charge.bucket != Bucket::None)
            nanos[static_cast<size_t>(charge.bucket)].fetch_add(charge.nanos, std::memory_order_relaxed);
    }
    class Scope
    {
        bool on;
        Bucket previous = Bucket::None;
    public:
        explicit Scope(Bucket bucket) : on(enabled())
        {
            if (on) { previous = accounting.active; switchTo(bucket); }
        }
        ~Scope() { if (on) switchTo(previous); }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
    };
    // GOW-Port: GOW_DELTA_DIAG=1 = game-speed meter (idea from KIexster's ps2recomp-game-speed.patch).
    // On every entry to GOW_PERF_FRAME_PC (vid::Flip, once per game frame) it adds the game's own frame
    // step (float at 0x29C64C in SCUS-97399, seconds) and once per wall second prints
    // [gow-speed] ... speed=<game seconds / wall seconds>: 1.00 = real time, >1 fast, <1 slow motion.
    // Guest code runs one function at a time, so the plain statics need no lock.
    inline bool deltaDiag()
    {
        static const bool on = [] {
            const char *value = std::getenv("GOW_DELTA_DIAG");
            return value && std::strcmp(value, "1") == 0;
        }();
        return on;
    }
    inline void gameSpeedFrame(const uint8_t *rdram)
    {
        static uint64_t windowStart = nowNanos(), frames = 0;
        static double gameSeconds = 0.0, minDelta = 1e9, maxDelta = 0.0;
        float delta = 0.0f;
        std::memcpy(&delta, rdram + 0x29C64Cu, sizeof(delta));
        if (delta > 0.0f && delta < 1.0f)
        {
            gameSeconds += delta;
            if (delta < minDelta) minDelta = delta;
            if (delta > maxDelta) maxDelta = delta;
        }
        // T0 ticks the game measured since its previous frame (0x29C670), in tenths of an NTSC field (9610 ticks).
        static uint32_t hist[24] = {};
        int32_t ticks = 0;
        std::memcpy(&ticks, rdram + 0x29C670u, sizeof(ticks));
        ++hist[std::min<uint32_t>(23u, static_cast<uint32_t>(std::max(0, ticks)) / 961u)];
        ++frames;
        const uint64_t now = nowNanos();
        const double wall = static_cast<double>(now - windowStart) * 1e-9;
        if (wall < 1.0) return;
        std::string line;
        for (uint32_t i = 0; i < 24u; ++i)
            if (hist[i]) line += " " + std::to_string(i) + ":" + std::to_string(hist[i]), hist[i] = 0;
        std::fprintf(stderr, "[gow-speed-hist] field/10 counts:%s\n", line.c_str());
        std::fprintf(stderr, "[gow-speed] frames=%llu wall=%.2f game=%.3f speed=%.3f delta_min=%.4f delta_max=%.4f\n",
                     static_cast<unsigned long long>(frames), wall, gameSeconds, gameSeconds / wall,
                     frames && maxDelta > 0.0 ? minDelta : 0.0, maxDelta);
        windowStart = now; frames = 0; gameSeconds = 0.0; minDelta = 1e9; maxDelta = 0.0;
    }
    inline void guestEntry(uint32_t pc, const uint8_t *rdram = nullptr)
    {
        static const bool frames = countFrames(), speed = deltaDiag();
        if (!frames && !speed) return;
        static const uint32_t framePc = [] {
            const char *value = std::getenv("GOW_PERF_FRAME_PC");
            return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 0u;
        }();
        // Solo entradas a la función elegida: las reanudaciones no cuentan como cuadros nuevos.
        if (framePc == 0u || pc != framePc) return;
        if (frames) guestFlips.fetch_add(1, std::memory_order_relaxed);
        if (speed && rdram) gameSpeedFrame(rdram);
    }
    // GOW-Port: split of the GS worker thread (VIF1 + VU1) under GOW_PERF_DIAG, printed by the worker as [gsw-split]
    // every 5 s: whole jobs, VU1 microprogram runs (MSCAL/MSCNT callback), XGKICK hand-off to the GIF/GS stage inside
    // those runs, and the rest (VIF commands and unpacks, DIRECT, GIF copies) = job - vu. With PS2X_VU1_ASYNC=1 vu_ms
    // is on the VU1 thread, vif_ms is not meaningful, and async_wait_ms is the worker waiting for the VU1 thread.
    namespace gsw
    {
        inline std::atomic<uint64_t> jobNs{0}, vuNs{0}, kickNs{0}, runs{0}, kicks{0}, jobs{0}, asyncWaitNs{0}, asyncWaits{0};
        // EE side: DMA source chains walked and copied by PS2Memory::sendDmaChain before the worker job is queued (VU1
        // cannot start on a chain until this is done), printed as [gsw-chain]
        inline std::atomic<uint64_t> chainNs{0}, chains{0}, chainBytes{0}, chainMaxNs{0};
        inline void reportFromWorker()
        {
            static uint64_t last = nowNanos();
            static uint64_t before[8] = {};
            const uint64_t now = nowNanos();
            if (now - last < 5000000000ull) return;
            const uint64_t cur[8] = {jobNs.load(), vuNs.load(), kickNs.load(), runs.load(), kicks.load(), jobs.load(), asyncWaitNs.load(), asyncWaits.load()};
            uint64_t d[8];
            for (int i = 0; i < 8; ++i) { d[i] = cur[i] - before[i]; before[i] = cur[i]; }
            std::fprintf(stderr, "[gsw-split] window=%.2f job_ms=%.1f vu_ms=%.1f kick_ms=%.1f vif_ms=%.1f runs=%llu kicks=%llu jobs=%llu async_wait_ms=%.1f async_waits=%llu\n",
                         (now - last) * 1e-9, d[0] * 1e-6, d[1] * 1e-6, d[2] * 1e-6, (d[0] > d[1] ? d[0] - d[1] : 0) * 1e-6,
                         (unsigned long long)d[3], (unsigned long long)d[4], (unsigned long long)d[5], d[6] * 1e-6,
                         (unsigned long long)d[7]);
            static uint64_t chainBefore[3] = {};
            const uint64_t chainCur[3] = {chainNs.load(), chains.load(), chainBytes.load()};
            uint64_t c[3];
            for (int i = 0; i < 3; ++i) { c[i] = chainCur[i] - chainBefore[i]; chainBefore[i] = chainCur[i]; }
            std::fprintf(stderr, "[gsw-chain] window=%.2f chain_ms=%.1f chains=%llu chain_kb=%llu max_chain_ms=%.2f\n", (now - last) * 1e-9,
                         c[0] * 1e-6, (unsigned long long)c[1], (unsigned long long)(c[2] >> 10), chainMaxNs.exchange(0) * 1e-6);
            last = now;
        }
    }
    inline void report()
    {
        if (!countFrames()) return;
        // Se llama Ãºnicamente desde el hilo de presentaciÃ³n. No lee RAM del juego.
        const uint64_t now = nowNanos();
        static uint64_t first = now, last = now;
        static std::array<uint64_t, bucketCount> before{};
        static uint64_t previousFlips = 0, previousFrames = 0;
        if (now - last < 5000000000ull) return;
        const double seconds = static_cast<double>(now - last) * 1e-9;
        std::array<double, bucketCount> ms{};
        for (size_t i = 0; i < bucketCount; ++i)
        {
            const uint64_t total = nanos[i].load(std::memory_order_relaxed);
            ms[i] = static_cast<double>(total - before[i]) * 1e-6;
            before[i] = total;
        }
        const uint64_t flips = guestFlips.load(std::memory_order_relaxed);
        const uint64_t frames = hostFrames.load(std::memory_order_relaxed);
        std::fprintf(stderr, "[gow-perf] t=%.2f window=%.2f ee_ms=%.2f vu_ms=%.2f gs_ms=%.2f iop_ms=%.2f guest_wait_ms=%.2f upload_ms=%.2f host_wait_ms=%.2f guest_flip_hz=%.2f host_hz=%.2f gsq_wait_ms=%.2f\n",
            (now - first) * 1e-9, seconds, ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], ms[6],
            (flips - previousFlips) / seconds, (frames - previousFrames) / seconds, ms[7]);
        previousFlips = flips; previousFrames = frames; last = now;
    }
}
#endif
