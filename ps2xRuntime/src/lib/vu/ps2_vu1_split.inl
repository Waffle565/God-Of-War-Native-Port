// GOW-Port: GOW_VU1_SPLIT - data-parallel vertex loops (included by ps2_vu1_native.inl, before runNative).
//
// 99 % of the VU1 time is one vertex loop that carries only the previous vertex and pipelined loads from one pass to the
// next (docs/vu1-hle.md). For a long loop the VU1 thread (worker) hands the second half to a helper thread:
// - probe: the worker breaks the dispatched loop function after each block (budget = now + 1; chainTo hands the block
//   back, exactly as at any budget end) until the block pcs repeat with period P (1-3 blocks) and equal VU cycles and VI
//   deltas over two periods: dC cycles per period, VI deltas, a down counter (VI with delta < 0) gives the periods left R;
// - plan: k = (R + warm + lat) / 2 periods from now is the take-over point (cycle Tk = now + k dC). The helper starts
//   `warm` periods before it from the worker's present state with the linear VI registers advanced and every absolute
//   cycle shifted by (k - warm) dC; the other registers are wrong at first and are recomputed by the warm-up passes;
// - the helper (own VU1Interpreter, own NativeState in specMode: no timing-cache writes, no exact-engine callouts) runs
//   on its own copy of VU data memory to its first block boundary at or after Tk, snapshots its whole state (registers,
//   cycle, FDIV/EFU, flag rings, recent history), then runs to the loop exit (or the run's budget) with memory logging:
//   words read before written (VU1SpecMem::log) and words written;
// - the worker runs on to Tk itself, compares its state with the helper's snapshot (equal state => the helper's
//   continuation is the sequential one), waits for the helper, checks every logged read against the real memory (which
//   is the memory at Tk: the worker waits there), copies the written words and takes the helper's final state over.
// Any difference, a stalled helper (preempted), a timing-cache change on the worker meanwhile or a callout cancels the
// split: the worker simply goes on. Results are bit-exact by construction (replay harness: docs/vu1-hle.md).
// Off by default (GOW_VU1_SPLIT=1): the loops are short (~25 vertices, ~5 us on the A57) and the probe, the state
// copies and the hand-off cost more than the helper saves (console replay benchmark +4..24 %, docs/vu1-hle.md).
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "ThreadNaming.h"

namespace
{
    inline void vu1SplitPause()
    {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
    inline uint32_t vu1SplitEnv(const char *name, uint32_t def)
    {
        const char *v = std::getenv(name);
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : def;
    }
}

void vu1nSpecRead(const VU1NativeCtx &n, uint32_t addr, uint32_t words)
{
    VU1SpecMem &m = *n.spec;
    for (uint32_t c = 0; c < 4u; ++c)
    {
        if (!(words & (1u << c)))
            continue;
        const uint32_t w = (addr >> 2) + c;
        const uint64_t bit = 1ull << (w & 63u);
        if ((m.written[w >> 6] | m.logged[w >> 6]) & bit)
            continue;
        m.logged[w >> 6] |= bit;
        uint32_t v;
        std::memcpy(&v, n.data + w * 4u, 4);
        if (m.count < VU1SpecMem::kLog)
            m.log[m.count] = {w, v};
        ++m.count; // > kLog: overflow, the split is dropped
    }
}

void vu1nSpecWrite(const VU1NativeCtx &n, uint32_t addr, uint32_t words)
{
    VU1SpecMem &m = *n.spec;
    for (uint32_t c = 0; c < 4u; ++c)
        if (words & (1u << c))
        {
            const uint32_t w = (addr >> 2) + c;
            m.written[w >> 6] |= 1ull << (w & 63u);
        }
}

// Everything of a VU1 interpreter + native state that the native blocks read or write between two block boundaries.
// Rings and history in logical order (newest first); absolute cycles as they are.
struct VU1SplitSnap
{
    using NS = VU1Interpreter::NativeState;
    static constexpr uint32_t kHist = 8u;
    VU1State st;
    uint64_t cycle = 0, nextCommit = 0, efuResourceReady = 0;
    VU1Interpreter::ScalarPipelineEntry fdiv{};
    std::array<VU1Interpreter::ScalarPipelineEntry, 2> efu{};
    uint32_t workingClip = 0;
    struct Ring
    {
        __m128 raw[8];
        uint64_t issue[8];
        uint32_t value[8], mask[8];
        int32_t tail[8]; // history index (newest = 0) of the tail record, -1 none
    } mac, clip;
    NS::Hist hist[kHist];
    uint32_t histN = 0, histCount = 0;

    static uint32_t histPos(const NS &ns, uint32_t j) { return (ns.histNext + NS::kHist - 1u - j) % NS::kHist; }

    bool capture(const VU1Interpreter &vu)
    {
        const NS &ns = *vu.m_native;
        st = vu.m_state;
        cycle = vu.m_cycle;
        nextCommit = vu.m_nextCommitCycle;
        fdiv = vu.m_fdiv;
        efu = vu.m_efu;
        efuResourceReady = vu.m_efuResourceReady > cycle ? vu.m_efuResourceReady : 0u;
        workingClip = vu.m_workingClip;
        histCount = ns.histCount;
        histN = std::min<uint32_t>(std::min<uint32_t>(ns.histCount, NS::kHist), kHist);
        for (uint32_t j = 0; j < histN; ++j)
            hist[j] = ns.hist[histPos(ns, j)];
        // older entries than the ones kept must not be in flight (walkHistory would look at them)
        if (ns.histCount > histN && histN != 0u && hist[histN - 1u].end + ns.maxLat > cycle)
            return false;
        const auto ring = [&](const VU1FlagRing &r, Ring &o) {
            for (uint32_t k = 1; k <= 8u; ++k)
            {
                const uint32_t i = VU1FlagRing::back(r.next, k);
                o.raw[k - 1u] = r.raw[i];
                o.issue[k - 1u] = r.issue[i];
                o.value[k - 1u] = r.value[i];
                o.mask[k - 1u] = r.mask[i];
                o.tail[k - 1u] = -1;
                if (k <= 4u && r.tail[i]) // only the four newest tails are ever looked at (ringAt)
                {
                    int32_t found = -1;
                    for (uint32_t j = 0; j < histN; ++j)
                        if (&ns.hist[histPos(ns, j)].tail == r.tail[i])
                        {
                            found = static_cast<int32_t>(j);
                            break;
                        }
                    if (found < 0)
                        return false;
                    o.tail[k - 1u] = found;
                }
            }
            return true;
        };
        return ring(ns.macRing, mac) && ring(ns.clipRing, clip);
    }

    void restore(VU1Interpreter &vu) const
    {
        NS &ns = *vu.m_native;
        vu.m_state = st;
        vu.m_cycle = cycle;
        vu.m_nextCommitCycle = nextCommit;
        vu.m_fdiv = fdiv;
        vu.m_efu = efu;
        if (efuResourceReady != 0u || vu.m_efuResourceReady > cycle)
            vu.m_efuResourceReady = efuResourceReady;
        vu.m_workingClip = workingClip;
        for (uint32_t j = 0; j < histN; ++j)
            ns.hist[histPos(ns, j)] = hist[j];
        ns.histCount = histN;
        const auto ring = [&](VU1FlagRing &r, const Ring &o) {
            r.next = 0u;
            for (uint32_t k = 1; k <= 8u; ++k)
            {
                const uint32_t i = (8u - k) & VU1FlagRing::kWrap;
                r.raw[i] = o.raw[k - 1u];
                r.issue[i] = o.issue[k - 1u];
                r.value[i] = o.value[k - 1u];
                r.mask[i] = o.mask[k - 1u];
                r.tail[i] = o.tail[k - 1u] >= 0 ? &ns.hist[histPos(ns, static_cast<uint32_t>(o.tail[k - 1u]))].tail : nullptr;
            }
        };
        ring(ns.macRing, mac);
        ring(ns.clipRing, clip);
        ns.flagsInRings = true;
    }

    void shift(uint64_t d)
    {
        cycle += d;
        if (nextCommit != ~0ull)
            nextCommit += d;
        if (fdiv.valid)
            fdiv.readyCycle += d;
        for (auto &e : efu)
            if (e.valid)
                e.readyCycle += d;
        if (efuResourceReady != 0u)
            efuResourceReady += d;
        for (uint32_t j = 0; j < histN; ++j)
        {
            hist[j].start += d;
            hist[j].end += d;
            hist[j].tail.start += d;
        }
        for (Ring *r : {&mac, &clip})
            for (auto &i : r->issue)
                i += d;
    }

    static bool sameState(const VU1State &a, const VU1State &b)
    {
        return std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 && std::memcmp(a.vi, b.vi, sizeof(a.vi)) == 0 &&
               std::memcmp(a.acc, b.acc, sizeof(a.acc)) == 0 && std::memcmp(&a.q, &b.q, 4) == 0 &&
               std::memcmp(&a.p, &b.p, 4) == 0 && std::memcmp(&a.i, &b.i, 4) == 0 && a.r == b.r && a.pc == b.pc &&
               a.mac == b.mac && a.clip == b.clip && a.status == b.status && a.ebit == b.ebit &&
               a.haltAfterDelaySlot == b.haltAfterDelaySlot && a.dBitEnabled == b.dBitEnabled && a.tBitEnabled == b.tBitEnabled &&
               a.stoppedByD == b.stoppedByD && a.stoppedByT == b.stoppedByT && a.top == b.top && a.itop == b.itop &&
               a.branchPending == b.branchPending && a.branchTarget == b.branchTarget && a.branchDelay == b.branchDelay;
    }
    static bool sameScalar(const VU1Interpreter::ScalarPipelineEntry &a, const VU1Interpreter::ScalarPipelineEntry &b)
    {
        if (a.valid != b.valid)
            return false;
        return !a.valid || (a.readyCycle == b.readyCycle && std::memcmp(&a.value, &b.value, 4) == 0 && a.statusDi == b.statusDi);
    }
    static bool sameTail(const VU1FlagTail &a, const VU1FlagTail &b, bool clipRing)
    {
        if (a.desc != b.desc || a.cyc != b.cyc || a.start != b.start)
            return false;
        const uint32_t n = clipRing ? a.desc->clipCount : a.desc->macCount;
        for (uint32_t j = 0; j < n; ++j)
            if (clipRing ? a.c[j] != b.c[j]
                         : vu1n::macValRt(a.q[j], a.desc->macMask[j]) != vu1n::macValRt(b.q[j], b.desc->macMask[j]))
                return false;
        return true;
    }
    // equal state as far as anything later can tell
    // 0: equal; else the first part that differs (GOW_VU1_SPLIT_DIAG)
    static int same(const VU1SplitSnap &a, const VU1SplitSnap &b, uint32_t maxLat)
    {
        if (std::memcmp(a.st.vf, b.st.vf, sizeof(a.st.vf)) != 0)
            return 1;
        if (std::memcmp(a.st.vi, b.st.vi, sizeof(a.st.vi)) != 0)
            return 2;
        if (!sameState(a.st, b.st))
            return 3;
        if (a.cycle != b.cycle)
            return 4;
        if (a.nextCommit != b.nextCommit || !sameScalar(a.fdiv, b.fdiv) || !sameScalar(a.efu[0], b.efu[0]) ||
            !sameScalar(a.efu[1], b.efu[1]) || a.efuResourceReady != b.efuResourceReady)
            return 5;
        if (a.workingClip != b.workingClip || (a.histCount != 0u) != (b.histCount != 0u))
            return 6;
        const auto window = [&](const VU1SplitSnap &s) {
            uint32_t w = 0;
            while (w < s.histN && (w == 0u || s.hist[w].end + maxLat > s.cycle))
                ++w;
            return w;
        };
        const uint32_t wa = window(a);
        if (wa != window(b))
            return 7;
        for (uint32_t j = 0; j < wa; ++j)
        {
            const NS::Hist &x = a.hist[j], &y = b.hist[j];
            if (x.bt != y.bt || x.cyc != y.cyc || x.start != y.start || x.end != y.end || x.cycId != y.cycId)
                return 8;
        }
        // what a reader at the snapshot cycle or later sees (ringAt / exportFlags): newest first, the flag value of each
        // writer still in flight (issue cycle, tail record) down to the first one already visible, whose value is the
        // last word (its issue cycle no longer matters: a run of the loop that writes no clip flags shifts nothing)
        const auto ring = [&](const Ring &x, const Ring &y, bool clipRing) {
            for (uint32_t k = 0; k < 5u; ++k)
            {
                const bool visX = k == 4u || x.issue[k] + VU1Interpreter::kFmacLatency <= a.cycle;
                const bool visY = k == 4u || y.issue[k] + VU1Interpreter::kFmacLatency <= b.cycle;
                if (visX != visY)
                    return false;
                const uint32_t vx = x.mask[k] ? vu1n::macValRt(x.raw[k], x.mask[k]) : x.value[k];
                const uint32_t vy = y.mask[k] ? vu1n::macValRt(y.raw[k], y.mask[k]) : y.value[k];
                if (vx != vy)
                    return false;
                if (visX)
                    return true;
                if (x.issue[k] != y.issue[k] || x.tail[k] != y.tail[k])
                    return false;
                if (x.tail[k] >= 0 && !sameTail(a.hist[x.tail[k]].tail, b.hist[y.tail[k]].tail, clipRing))
                    return false;
            }
            return true;
        };
        if (!ring(a.mac, b.mac, false))
            return 9;
        return ring(a.clip, b.clip, true) ? 0 : 10;
    }
};

struct VU1Split
{
    using NS = VU1Interpreter::NativeState;
    enum : uint32_t { kIdle, kPosted, kWarm, kSnap, kDone, kFailed };
    enum : uint32_t { kStop, kExit, kFail };

    // configuration (env)
    uint32_t minCyc = vu1SplitEnv("GOW_VU1_SPLIT_MIN", 600u);    // VU cycles left in the loop at the probe
    uint32_t warm = vu1SplitEnv("GOW_VU1_SPLIT_WARM", 5u);       // warm-up periods of the helper
    uint32_t lat = vu1SplitEnv("GOW_VU1_SPLIT_LAT", 2u);         // periods added for the helper's start latency
    uint64_t stallNs = vu1SplitEnv("GOW_VU1_SPLIT_STALLUS", 40u) * 1000ull;
    uint64_t spinNs = vu1SplitEnv("GOW_VU1_SPLIT_SPINUS", 2000u) * 1000ull;
    static constexpr uint32_t kProbeMax = 64u;
    uint32_t probeMax = std::min(std::max(vu1SplitEnv("GOW_VU1_SPLIT_PROBE", 24u), 7u), kProbeMax); // block boundaries looked at
    bool sync = vu1SplitEnv("GOW_VU1_SPLIT_SYNC", 0u) != 0u;     // helper job run inline at the post (deterministic test)
    bool stats = std::getenv("GOW_VU1_SPLIT_STATS") != nullptr || ps2_perf::enabled();

    // helper
    VU1Interpreter *hvu = nullptr;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> waiting{false};
    std::atomic<uint32_t> jobSeq{0};
    std::atomic<uint32_t> state{kIdle};
    std::atomic<uint32_t> progress{0};
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> helperNs{0};

    // job (written by the worker before jobSeq is raised)
    VU1SplitSnap jobStart;
    uint32_t loopPc[4] = {}, loopN = 0;
    NS::Slot loopSlot[4];
    uint64_t Tk = 0, budgetEnd = 0, chunk = 0;
    uint32_t maxBlocks = 0, maxLat = 0, dataSize = 0;
    bool chainFlags = true;
    uint32_t chainMax = 0;
    const uint8_t *srcData = nullptr;
    // qword ranges of data memory the helper copies (around the loop's pointer registers; everything when unsure).
    // Whatever it reads elsewhere is stale: a warm-up read shows in the state check, a later one in the read log.
    struct Range { uint32_t first, count; } ranges[6];
    uint32_t rangeN = 0;
    VU1Interpreter::StepContext dummy{};

    // results (helper -> worker after kSnap / kDone)
    VU1SplitSnap snapK, fin;
    VU1SpecMem mem;
    alignas(64) uint8_t priv[16384];
    uint32_t helperBlocks = 0;

    // worker side
    int phase = 0;
    NS *probeNs = nullptr;
    uint32_t probeIdx = 0; // slot index of the probed dispatch (NS::splitLast / splitSkip)
    uint64_t probeStart = 0;
    uint32_t nb = 0;
    struct Bound { uint32_t pc; int32_t vi[16]; uint64_t cycle; } b[kProbeMax];
    uint32_t epochAtPost = 0, expectPc = 0, planBlocks = 0;

    // statistics (worker thread)
    uint64_t sProbe = 0, sPlan = 0, sReach = 0, sAdopt = 0, sOff = 0, sNotStarted = 0, sStall = 0, sHelperFail = 0,
             sMismatch = 0, sMem = 0, sEpoch = 0, sNoPeriod = 0, sShort = 0, sBlocksSaved = 0, sWaitNs = 0;
    uint64_t lastReport = 0;
    uint32_t reportTick = 0;

    static void specAbort(VU1Interpreter &vu)
    {
        vu.m_native->specFail = true;
        vu.m_native->limit = 0u; // chainTo refuses: the block hands back to the helper's dispatch loop
    }

    static VU1Split *engineFor(VU1Interpreter &vu)
    {
        NS &ns = *vu.m_native;
        if (ns.splitMode < 0)
        {
            const char *v = std::getenv("GOW_VU1_SPLIT");
            // off by default: on the console it costs more than it saves (docs/vu1-hle.md); GOW_VU1_SPLIT=1 turns it on
            ns.splitMode = (v && std::atoi(v) != 0) ? 1 : 0;
            if (ns.specMode || ns.verifyLeft != 0u || std::getenv("GOW_VU1N_BLOCKPROF") || std::getenv("GOW_VU1N_TRACEREC"))
                ns.splitMode = 0;
            if (ns.splitMode)
            {
                VU1Split *sp = new VU1Split();
                sp->hvu = new VU1Interpreter(VU1Interpreter::Unit::VU1);
                sp->hvu->m_native = new NS();
                sp->hvu->m_native->specMode = true;
                sp->hvu->m_native->splitMode = 0;
                ns.split = sp;
                if (!sp->sync)
                    new std::thread([sp] { sp->helperMain(); }); // never joined (libnx has no pthread_detach)
                std::fprintf(stderr, "[vu1-split] on (min %u cycles, warm %u, lat %u, %s)\n", sp->minCyc, sp->warm, sp->lat,
                             sp->sync ? "inline helper" : "helper thread VU1Split");
            }
        }
        return ns.splitMode ? ns.split : nullptr;
    }

    // ---- worker ----------------------------------------------------------------------------------------------------
    uint64_t before(VU1Interpreter &vu, NS::Slot &sl, uint64_t budget)
    {
        if (phase == 2)
            return std::min(budget, Tk);
        if (phase == 1)
            return std::min(budget, vu.m_cycle + 1u);
        NS &lns = *vu.m_native;
        const uint32_t idx = static_cast<uint32_t>(&sl - lns.slots) & 2047u;
        if (lns.splitLast[idx] < minCyc)
            return budget;
        if (lns.splitSkip[idx] != 0u)
        {
            --lns.splitSkip[idx]; // cool-down after a probe that found no loop
            return budget;
        }
        const uint32_t s = state.load(std::memory_order_acquire);
        if (s != kIdle)
        {
            if (s != kDone && s != kFailed)
                return budget; // the helper is still on a cancelled job
            cancel.store(false, std::memory_order_relaxed);
            state.store(kIdle, std::memory_order_relaxed);
        }
        const NS &ns = *vu.m_native;
        if (ns.arraysReadyBy > vu.m_cycle || ns.limit != ~0u || (vu.m_storePending | vu.m_vfPending | vu.m_viPending | vu.m_accPending) != 0u ||
            vu.m_flagCount != 0u || vu.m_xgkick.active || !ns.flagsInRings)
            return budget;
        phase = 1;
        probeNs = &lns;
        probeIdx = idx;
        probeStart = vu.m_cycle;
        nb = 0;
        ++sProbe;
        return std::min(budget, vu.m_cycle + 1u);
    }

    void giveUp(uint64_t learned)
    {
        phase = 0;
        if (probeNs)
            probeNs->splitLast[probeIdx] = static_cast<uint16_t>(std::min<uint64_t>(learned, 65535u));
    }

    void after(VU1Interpreter &vu, NS::Slot &sl, uint64_t start, VU1Interpreter::StepContext &c)
    {
        if (phase == 0)
        {
            vu.m_native->splitLast[static_cast<uint32_t>(&sl - vu.m_native->slots) & 2047u] =
                static_cast<uint16_t>(std::min<uint64_t>(vu.m_cycle - start, 65535u));
            report();
            return;
        }
        if (phase == 2)
        {
            if (vu.m_cycle >= Tk)
                settle(vu, c);
            return;
        }
        Bound &x = b[nb++];
        x.pc = vu.m_state.pc & 0x3FFFu;
        std::memcpy(x.vi, vu.m_state.vi, sizeof(x.vi));
        x.cycle = vu.m_cycle;
        for (uint32_t P = 1; P <= 3u; ++P)
        {
            if (nb < 2u * P + 1u)
                break;
            const uint32_t i = nb - 1u;
            const Bound &b0 = b[i - 2u * P], &b1 = b[i - P], &b2 = b[i];
            if (b2.pc != b1.pc || b1.pc != b0.pc)
                continue;
            const uint64_t dC = b2.cycle - b1.cycle;
            if (dC == 0u || dC != b1.cycle - b0.cycle)
                continue;
            plan(vu, c, P, dC);
            return;
        }
        if (nb >= probeMax)
        {
            ++sNoPeriod;
            giveUp(0u);
            probeNs->splitSkip[probeIdx] = 64u; // not a loop head: probed again after 64 long dispatches
        }
    }

    static int32_t d16(int32_t a, int32_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a) - static_cast<uint16_t>(b)); }

    void plan(VU1Interpreter &vu, VU1Interpreter::StepContext &c, uint32_t P, uint64_t dC)
    {
        const NS &ns = *vu.m_native;
        const uint32_t i = nb - 1u;
        int32_t lin[16] = {};
        uint64_t R = ~0ull;
        for (uint32_t r = 1; r < 16u; ++r)
        {
            const int32_t d1 = d16(b[i].vi[r], b[i - P].vi[r]), d0 = d16(b[i - P].vi[r], b[i - 2u * P].vi[r]);
            if (d1 != d0)
                continue;
            lin[r] = d1;
            const int32_t v = static_cast<int16_t>(b[i].vi[r]);
            if (d1 < 0 && -d1 <= static_cast<int32_t>(2u * P) && v > 0)
                R = std::min<uint64_t>(R, static_cast<uint64_t>(v / -d1));
        }
        if (R == ~0ull || R * dC < minCyc || R < warm + lat + 4u)
        {
            ++sShort;
            giveUp(R == ~0ull ? 0u : R * dC);
            return;
        }
        const uint64_t k = (R + warm + lat) / 2u, m = k - warm;
        Tk = vu.m_cycle + k * dC;
        if (Tk >= c.budgetEnd || !jobStart.capture(vu))
        {
            ++sShort;
            giveUp(0u);
            return;
        }
        jobStart.shift(m * dC);
        for (uint32_t r = 1; r < 16u; ++r)
            if (lin[r] != 0)
                jobStart.st.vi[r] = static_cast<int16_t>(static_cast<uint16_t>(jobStart.st.vi[r] + static_cast<int32_t>(m) * lin[r]));
        loopN = 0;
        for (uint32_t j = i - P + 1u; j <= i; ++j)
        {
            bool seen = false;
            for (uint32_t q = 0; q < loopN; ++q)
                seen |= loopPc[q] == b[j].pc;
            if (!seen && loopN < 4u)
            {
                loopPc[loopN] = b[j].pc & 0x3FF8u;
                loopSlot[loopN] = ns.slots[loopPc[loopN] >> 3];
                ++loopN;
            }
        }
        budgetEnd = c.budgetEnd;
        chunk = 32u * dC;
        maxBlocks = static_cast<uint32_t>((R - m + 4u) * P + 8u);
        maxLat = ns.maxLat;
        chainFlags = ns.chainFlags;
        chainMax = ns.chainMax;
        srcData = c.vuData;
        dataSize = c.dataSize;
        {
            const uint32_t qws = dataSize / 16u;
            const int64_t periods = static_cast<int64_t>(R - m) + 2;
            rangeN = 0;
            bool all = false;
            for (uint32_t r = 1; r < 16u && !all; ++r)
            {
                if (lin[r] == 0)
                    continue;
                const int64_t a = static_cast<uint16_t>(jobStart.st.vi[r]);
                const int64_t e = a + periods * lin[r];
                const int64_t lo = std::min(a, e) - 8, hi = std::max(a, e) + 8;
                if (rangeN == 6u || hi - lo >= static_cast<int64_t>(qws))
                    all = true;
                else
                    ranges[rangeN++] = {static_cast<uint32_t>(((lo % qws) + qws) % qws), static_cast<uint32_t>(hi - lo)};
            }
            if (all)
                ranges[0] = {0u, qws}, rangeN = 1;
        }
        epochAtPost = ns.timingEpoch;
        expectPc = b[i].pc;
        planBlocks = static_cast<uint32_t>((R - k) * P);
        ++sPlan;
        giveUp(R * dC);
        phase = 2;
        state.store(kPosted, std::memory_order_relaxed);
        progress.store(0u, std::memory_order_relaxed);
        if (sync)
        {
            runJob();
            return;
        }
        jobSeq.fetch_add(1u, std::memory_order_seq_cst);
        if (waiting.load(std::memory_order_seq_cst))
        {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_one();
        }
    }

    bool waitFor(uint32_t want)
    {
        uint32_t lastProg = progress.load(std::memory_order_relaxed);
        uint64_t t0 = 0;
        for (uint32_t n = 0;; ++n)
        {
            const uint32_t s = state.load(std::memory_order_acquire);
            if (s == kFailed)
            {
                ++sHelperFail;
                return false;
            }
            if (s >= want)
                return true;
            if (s == kPosted && !sync)
            {
                ++sNotStarted; // asleep or preempted: not worth waiting for
                return false;
            }
            vu1SplitPause();
            if ((n & 31u) == 0u)
            {
                const uint64_t t = ps2_perf::nowNanos();
                const uint32_t p = progress.load(std::memory_order_relaxed);
                if (t0 == 0u || p != lastProg)
                    lastProg = p, t0 = t;
                else if (t - t0 > stallNs)
                {
                    ++sStall;
                    return false;
                }
            }
        }
    }

    void settle(VU1Interpreter &vu, VU1Interpreter::StepContext &c)
    {
        phase = 0;
        ++sReach;
        const uint64_t w0 = stats ? ps2_perf::nowNanos() : 0u;
        const auto drop = [&](uint64_t &counter) {
            ++counter;
            cancel.store(true, std::memory_order_relaxed);
            if (w0)
                sWaitNs += ps2_perf::nowNanos() - w0;
        };
        if (vu.m_cycle != Tk || (vu.m_state.pc & 0x3FFFu) != expectPc)
            return drop(sOff);
        uint64_t none = 0;
        if (!waitFor(kSnap))
            return drop(none);
        VU1SplitSnap &w = *wsnap;
        if (!w.capture(vu))
            return drop(sMismatch);
        if (const int why = VU1SplitSnap::same(w, snapK, vu.m_native->maxLat))
        {
            static int diagLeft = static_cast<int>(vu1SplitEnv("GOW_VU1_SPLIT_DIAG", 0u));
            if (diagLeft > 0)
            {
                --diagLeft;
                std::fprintf(stderr, "[vu1-split] mismatch %d at pc=%x cycle=%llu/%llu:", why, w.st.pc, (unsigned long long)w.cycle,
                             (unsigned long long)snapK.cycle);
                for (int r = 0; r < 32; ++r)
                    if (std::memcmp(w.st.vf[r], snapK.st.vf[r], 16) != 0)
                    {
                        uint32_t x[4], y[4];
                        std::memcpy(x, w.st.vf[r], 16);
                        std::memcpy(y, snapK.st.vf[r], 16);
                        std::fprintf(stderr, " vf%d=%08x,%08x,%08x,%08x/%08x,%08x,%08x,%08x", r, x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]);
                    }
                for (int r = 0; r < 16; ++r)
                    if (w.st.vi[r] != snapK.st.vi[r])
                        std::fprintf(stderr, " vi%d=%d/%d", r, w.st.vi[r], snapK.st.vi[r]);
                std::fprintf(stderr, "\n");
            }
            return drop(sMismatch);
        }
        if (!waitFor(kDone))
            return drop(none);
        if (vu.m_native->timingEpoch != epochAtPost)
            return drop(sEpoch);
        if (mem.count > VU1SpecMem::kLog)
            return drop(sMem);
        const uint8_t *real = c.vuData;
        for (uint32_t e = 0; e < mem.count; ++e)
            if (std::memcmp(real + mem.log[e].word * 4u, &mem.log[e].value, 4) != 0)
                return drop(sMem);
        fin.restore(vu);
        for (uint32_t q = 0; q < 64u; ++q)
            for (uint64_t bits = mem.written[q]; bits != 0u; bits &= bits - 1u)
            {
                const uint32_t word = q * 64u + static_cast<uint32_t>(std::countr_zero(bits));
                std::memcpy(c.vuData + word * 4u, priv + word * 4u, 4);
            }
        ++sAdopt;
        sBlocksSaved += planBlocks;
        state.store(kIdle, std::memory_order_relaxed);
        if (w0)
            sWaitNs += ps2_perf::nowNanos() - w0;
    }

    void runExit()
    {
        if (phase == 2)
            cancel.store(true, std::memory_order_relaxed);
        phase = 0;
    }

    void report(bool force = false)
    {
        if (!stats || (!force && (++reportTick & 4095u) != 0u))
            return;
        const uint64_t now = ps2_perf::nowNanos();
        if (lastReport == 0u)
            lastReport = now;
        if (!force && now - lastReport < 5000000000ull)
            return;
        std::fprintf(stderr,
                     "[vu1-split] window=%.2f probes=%llu plans=%llu reached=%llu adopted=%llu blocks_saved=%llu no_period=%llu short=%llu "
                     "off=%llu not_started=%llu stall=%llu helper_fail=%llu mismatch=%llu mem=%llu epoch=%llu wait_ms=%.1f helper_ms=%.1f "
                     "helper_state=%u jobs=%u\n",
                     (now - lastReport) * 1e-9, (unsigned long long)sProbe, (unsigned long long)sPlan, (unsigned long long)sReach,
                     (unsigned long long)sAdopt, (unsigned long long)sBlocksSaved, (unsigned long long)sNoPeriod,
                     (unsigned long long)sShort, (unsigned long long)sOff, (unsigned long long)sNotStarted, (unsigned long long)sStall,
                     (unsigned long long)sHelperFail, (unsigned long long)sMismatch, (unsigned long long)sMem, (unsigned long long)sEpoch,
                     sWaitNs * 1e-6, helperNs.exchange(0u) * 1e-6, state.load(std::memory_order_relaxed),
                     jobSeq.load(std::memory_order_relaxed));
        sProbe = sPlan = sReach = sAdopt = sOff = sNotStarted = sStall = sHelperFail = sMismatch = sMem = sEpoch = sNoPeriod = sShort =
            sBlocksSaved = sWaitNs = 0;
        lastReport = now;
    }

    // ---- helper ----------------------------------------------------------------------------------------------------
    VU1SplitSnap *wsnap = new VU1SplitSnap(); // worker's scratch snapshot

    void helperMain()
    {
        ThreadNaming::SetCurrentThreadName("VU1Split");
        vu_fpmode::set((vu_fpmode::get() & ~vu_fpmode::kRoundMask) | vu_fpmode::kRoundZero | vu_fpmode::kFlushZero);
        uint32_t seen = 0;
        for (;;)
        {
            const uint64_t until = ps2_perf::nowNanos() + spinNs;
            for (uint32_t n = 0; jobSeq.load(std::memory_order_acquire) == seen; ++n)
            {
#if defined(__SWITCH__)
                std::this_thread::yield(); // Horizon does not time-slice one priority: let the core's other threads run
#else
                vu1SplitPause();
#endif
                if ((n & 63u) == 0u && ps2_perf::nowNanos() >= until)
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    waiting.store(true, std::memory_order_seq_cst);
                    cv.wait(lock, [&] { return jobSeq.load(std::memory_order_seq_cst) != seen; });
                    waiting.store(false, std::memory_order_relaxed);
                    break;
                }
            }
            seen = jobSeq.load(std::memory_order_acquire);
            runJob();
        }
    }

    // runNative's dispatch loop on the helper's state, restricted to the loop's slots
    uint32_t drive(VU1NativeCtx &ctx, uint64_t stop, uint32_t &blocks)
    {
        VU1Interpreter &vu = *hvu;
        NS &ns = *vu.m_native;
        for (;;)
        {
            if (cancel.load(std::memory_order_relaxed))
                return kFail;
            const uint64_t chunkStop = std::min(stop, vu.m_cycle + chunk);
            while (vu.m_cycle < chunkStop)
            {
                const uint32_t pc = vu.m_state.pc & 0x3FF8u;
                NS::Slot &sl = ns.slots[pc >> 3];
                if (!sl.fn)
                    return kExit;
                if (++blocks > maxBlocks)
                    return kFail;
                const uint64_t start = vu.m_cycle;
                uint32_t cycId = 0u;
                const uint8_t *cyc = sl.readsEntryFlags ? VU1NativeAccess::entryTiming(vu, sl, cycId) : sl.idle;
                ctx.cyc = cyc;
                ctx.start = start;
                ctx.slot = &sl;
                ctx.cycId = cycId;
                ctx.budgetEnd = chunkStop;
                ctx.chainLeft = ns.chainMax;
                if (sl.tail)
                {
                    VU1FlagTail &t = ns.hist[ns.histNext].tail;
                    t.desc = sl.tail;
                    t.cyc = cyc;
                    t.start = start;
                    ctx.tail = &t;
                }
                const uint32_t result = sl.fn(ctx);
                const NS::Slot &last = *static_cast<const NS::Slot *>(ctx.slot);
                if (!(result & kVu1nFinished))
                {
                    VU1NativeAccess::finishBlockTiming(vu, last, ctx.cyc, ctx.cycId, ctx.start);
                    if (vu.m_cycle >= vu.m_nextCommitCycle)
                        VU1NativeAccess::commitDue(vu);
                }
                vu.m_state.pc = result & 0x3FFFu;
                if ((result & (kVu1nEnded | kVu1nBail)) || ns.specFail)
                    return kFail;
            }
            progress.store(blocks, std::memory_order_relaxed);
            if (vu.m_cycle >= stop)
                return kStop;
        }
    }

    void runJob()
    {
        const uint64_t t0 = stats ? ps2_perf::nowNanos() : 0u;
        state.store(kWarm, std::memory_order_relaxed);
        VU1Interpreter &vu = *hvu;
        NS &ns = *vu.m_native;
        // the worker writes on meanwhile: whatever is read past Tk is checked
        const uint32_t qws = dataSize / 16u;
        for (uint32_t q = 0; q < rangeN; ++q)
        {
            const uint32_t first = ranges[q].first, count = ranges[q].count, head = std::min(count, qws - first);
            std::memcpy(priv + first * 16u, srcData + first * 16u, head * 16u);
            if (count > head)
                std::memcpy(priv, srcData, (count - head) * 16u);
        }
        ns.histNext = 0u;
        jobStart.restore(vu);
        vu.m_storePending = vu.m_vfPending = vu.m_viPending = vu.m_accPending = 0u;
        vu.m_flagCount = 0u;
        vu.m_stopRequested = false;
        vu.m_xgkick.active = false;
        vu.m_activeVuData = priv;
        vu.m_activeVuDataSize = dataSize;
        ns.specFail = false;
        ns.limit = ~0u;
        ns.arraysReadyBy = 0u;
        ns.maxLat = maxLat;
        ns.chainFlags = chainFlags;
        ns.chainMax = chainMax;
        for (uint32_t q = 0; q < loopN; ++q)
            ns.slots[loopPc[q] >> 3] = loopSlot[q];
        VU1NativeCtx ctx;
        ctx.s = &vu.m_state;
        ctx.data = priv;
        ctx.mask = dataSize - 1u;
        ctx.vu = &vu;
        ctx.context = &dummy;
        ctx.macRing = &ns.macRing;
        ctx.clipRing = &ns.clipRing;
        uint32_t blocks = 0;
        bool ok = drive(ctx, Tk, blocks) == kStop && snapK.capture(vu);
        if (ok)
        {
            state.store(kSnap, std::memory_order_release);
            std::memset(mem.written, 0, sizeof(mem.written));
            std::memset(mem.logged, 0, sizeof(mem.logged));
            mem.count = 0u;
            ctx.spec = &mem;
            const uint32_t r = drive(ctx, budgetEnd, blocks);
            ok = r != kFail && fin.capture(vu);
        }
        helperBlocks = blocks;
        for (uint32_t q = 0; q < loopN; ++q)
            ns.slots[loopPc[q] >> 3] = NS::Slot{};
        if (t0)
            helperNs.fetch_add(ps2_perf::nowNanos() - t0, std::memory_order_relaxed);
        state.store(ok ? kDone : kFailed, std::memory_order_release);
    }
};
