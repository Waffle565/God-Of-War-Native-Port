// GOW-Port: native VU1 backend. Basic blocks of the game's VU1 microcode are translated ahead of time (tools/vu1/vu1n_gen.py)
// into straight-line SSE functions that use immediate, timing-free semantics: the VU interlocks on every hazard, so the
// *values* a program computes never depend on cycle timing; only the MAC/CLIP flag registers have a visible latency, and
// the generator keeps their order. The exact engine (VU1Interpreter) stays as fallback for blocks that were not translated
// and as the oracle for GOW_VU1N_VERIFY.
#ifndef PS2_VU1_NATIVE_H
#define PS2_VU1_NATIVE_H

#include "runtime/ps2_vu1.h"

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cstring>
#if defined(USE_SSE2NEON)
#include "sse2neon.h" // GOW-Port: ARM (Switch)
#else
#include <immintrin.h>
#endif

class PS2Memory;

namespace vu1n
{
    inline uint32_t macValRt(__m128 r, uint32_t m);
}

// The flag writers of a native block that only matter to a read less than kFmacLatency cycles after its exit: all MAC /
// clip writers but the newest whose successor lies in the block's last three pairs (vu1n_gen.py). The descriptor is
// static (pairs, DEST masks in SSE order); the block stores the values (MAC: the unclamped FMAC result) in its record.
struct VU1TailDesc
{
    uint8_t macCount, clipCount;
    uint8_t macPair[3], macMask[3], clipPair[3];
};
struct VU1FlagTail
{
    const VU1TailDesc *desc;
    const uint8_t *cyc; // issue cycles of the block's pairs (relative to start)
    uint64_t start;
    __m128 q[3];
    uint32_t c[3];
};

// MAC / clip flags while native blocks run: the last four writers (issue cycle, value) in slots next-1 .. next-4 (newest
// first) and, in slot next-5, the writer pushed out last (visible before them; its issue cycle is not looked at). The VU
// issues at most one FMAC / CLIP per cycle, so four writers cover the whole latency window (kFmacLatency). Eight slots: a
// push overwrites the slot two pushes behind the one it pushes out, so nothing is copied. A MAC writer pushed by a native
// block keeps its FMAC result (raw, DEST mask in SSE order): the flags are computed only when something reads them; mask
// 0 = value holds the flags. A native block pushes only its newest writer, with its tail record: the older writers there
// are visible before it.
struct VU1FlagRing
{
    static constexpr uint32_t kSlots = 8u, kWrap = kSlots - 1u;
    __m128 raw[kSlots] = {};
    uint64_t issue[kSlots] = {};
    uint32_t value[kSlots] = {};
    uint32_t mask[kSlots] = {};
    const VU1FlagTail *tail[kSlots] = {};
    uint32_t next = 0; // slot of the next push
    bool clip = false;
    void reset(uint32_t value0)
    {
        for (uint32_t k = 0; k < kSlots; ++k)
            value[k] = value0, mask[k] = 0u, tail[k] = nullptr, issue[k] = 0u;
        next = 0u;
    }
    uint32_t valueOf(uint32_t k) const { return mask[k] ? vu1n::macValRt(raw[k], mask[k]) : value[k]; }
    static uint32_t back(uint32_t next, uint32_t k) { return (next + kSlots - k) & kWrap; } // k-th newest (1 = newest)
};

struct VU1NativeCtx
{
    VU1State *s = nullptr;
    uint8_t *data = nullptr;
    uint32_t mask = 0u; // dataSize - 1
    VU1Interpreter *vu = nullptr;
    void *context = nullptr; // VU1Interpreter::StepContext *
    const uint8_t *cyc = nullptr; // issue cycle of every pair of the running block, from the exact engine's stall model
    uint64_t start = 0;           // exact-engine cycle at which the running block's pair 0 issues
    VU1FlagRing *macRing = nullptr, *clipRing = nullptr;
    VU1FlagTail *tail = nullptr; // the running block's tail record (blocks with a VU1TailDesc)
    const void *slot = nullptr;  // the running block's VU1Interpreter::NativeState::Slot (chainTo)
    uint32_t cycId = 0;          // its timing id (finishBlockTiming)
    uint64_t budgetEnd = 0;      // the run's cycle budget (StepContext::budgetEnd)
};

// Return value of a native block: next pc in the low 16 bits; kEnded when the program finished; kBail when the exact
// engine must continue at the returned pc (D/T halt enabled, ...).
constexpr uint32_t kVu1nEnded = 0x80000000u;
constexpr uint32_t kVu1nBail = 0x40000000u;
constexpr uint32_t kVu1nFinished = 0x20000000u; // a chained block already finished its timing (chainTo)
using VU1NativeFn = uint32_t (*)(VU1NativeCtx &);

// Registered by the generated tables: key = FNV-1a over (pc, block words), see vu1n::blockKey. pairs: block length in the
// low 16 bits, bit 16 set when the block reads MAC/clip flags written before it (it then needs exact entry timing).
void vu1nRegisterBlock(uint64_t key, VU1NativeFn fn, uint32_t pairs, const VU1TailDesc *tail);

struct VU1Interpreter::NativeState
{
    uint64_t generation = ~0ull;
    // The micro memory the slots were set up from: when the code generation changes, only slots whose extent (block
    // pairs plus the pair after it, which ends the extent scan) overlaps a changed pair are set up again.
    uint8_t code[16384];
    bool codeValid = false;
    // Exact issue cycles of a block from the pipeline state at its entry, so results and flags in flight from the previous
    // block (native or exact) stall and become visible exactly as in the exact engine. Pipeline lanes: VF reg*4+comp
    // (0-127), VI 128+reg, ACC 144+comp. While native blocks run, no per-lane state is kept: the recent blocks are
    // remembered (`hist`) and a lane's ready cycle is derived from its newest writer only when a block that needs exact
    // timing enters or the exact engine takes over (its readiness arrays are synced then).
    static constexpr uint32_t kLanes = 148;
    struct BlockTiming
    {
        std::vector<DecodedInstructionPair> dec;
        uint64_t readMask[3] = {};   // lanes read before the block writes them
        uint64_t writeMask[3] = {};  // lanes the block writes
        uint8_t readAt[kLanes] = {}; // idle-schedule issue cycle of the first such read
        uint8_t lastPair[kLanes] = {}, lastLat[kLanes] = {}; // last write of each written lane: pair and latency
        uint8_t scalarRead = 0;      // bit 0 FDIV, 1 EFU unit, 2 WAITP read before the block starts one
        uint8_t scalarAt[3] = {};
        std::vector<uint8_t> scalarPairs; // pairs that start an FDIV / EFU operation
        const uint8_t *idle = nullptr;
        bool readsEntryFlags = false; // reads flags written before it: exact entry timing (others use `idle`)
        struct Cached { std::vector<uint16_t> sig; std::vector<uint8_t> cyc; uint32_t id = 0; };
        std::vector<Cached> cache; // timings for in-flight entry states seen so far (loops repeat the same few)
        uint32_t next = 0;
    };
    std::unordered_map<uint64_t, BlockTiming> timings;
    // Per pc slot. The first cache line holds what the dispatch loop touches for every block; the entry-timing memo
    // (only blocks that read entry flags use it) is in the second.
    struct alignas(64) Slot
    {
        VU1NativeFn fn = nullptr; // null: no native block at this pc
        const uint8_t *idle = nullptr;
        BlockTiming *bt = nullptr;
        const VU1TailDesc *tail = nullptr;
        uint16_t pairs = 0;
        uint16_t idleEnd = 0; // idle[pairs - 1] + 1: the block's length in cycles with an idle pipeline
        bool known = false;
        bool readsEntryFlags = false;
        bool memoable = false;   // its entry timing does not depend on FDIV/EFU state
        uint8_t scalarCount = 0; // FDIV / EFU operations it starts (kScalarMany: more than fit in `scalar`)
        struct Scalar { uint8_t pair, latency; bool fdiv; } scalar[2] = {};
        // Entry timing by predecessor (blocks that read entry flags): when the block before ran for maxLat cycles or more
        // and nothing from the exact engine is still in flight, that block and its timing table alone decide the entry
        // state; the timing found for it is reused.
        alignas(64) const BlockTiming *memoPred = nullptr;
        const uint8_t *memoCyc = nullptr;
        uint32_t memoPredId = 0, memoId = 0;
    };
    static_assert(sizeof(Slot) == 128);
    static constexpr uint8_t kScalarMany = 0xFF;
    Slot slots[2048];
    // Native blocks run since native entry (or the last sync), newest last; histCount counts them all (the ring keeps the
    // last kHist). A write is ready less than maxLat cycles after its block ended, so only blocks that ended less than
    // maxLat cycles ago can still hold a lane in flight.
    // cycId: 0 = the block's idle table, else the id of the cache slot (slots are reused, ids are not)
    struct Hist { const BlockTiming *bt; const uint8_t *cyc; uint64_t start, end; uint32_t cycId; VU1FlagTail tail; };
    static constexpr uint32_t kHist = 64;
    Hist hist[kHist];
    uint32_t histNext = 0, histCount = 0;
    uint32_t maxLat = 0;
    uint32_t nextCycId = 1;
    // Every lane of the exact engine's readiness arrays is ready by arraysReadyBy (the arrays are cleared at every program
    // start; recomputed at native entry when the exact engine ran since the last native exit at exitCycle).
    uint64_t arraysReadyBy = 0, exitCycle = ~0ull;
    std::vector<uint16_t> sig; // scratch: (lane << 8 | slack) of the in-flight lanes a block reads
    using FlagRing = VU1FlagRing;
    FlagRing macRing, clipRing;
    bool flagsInRings = false; // the rings hold the flag state (set at native entry, cleared when the exact engine continues)
    bool enabled = std::getenv("GOW_VU1_NATIVE_OFF") == nullptr;
    unsigned verifyLeft = std::getenv("GOW_VU1N_VERIFY") ? static_cast<unsigned>(std::atoi(std::getenv("GOW_VU1N_VERIFY"))) : 0u;
    bool diag = std::getenv("GOW_VU1N_DIAG") != nullptr;
    unsigned long long nativePairs = 0, nativeBlocks = 0, missBlocks = 0, nativeRuns = 0, codeChanges = 0, slotSetups = 0;
    std::unordered_map<uint32_t, unsigned> missPcs, missHits;
    unsigned limit = ~0u, count = 0; // verify bisection: native blocks allowed in this run
    VU1State snap;                   // state on entry of block number `limit` (verify bisection)
    std::vector<uint32_t> trace;     // pcs of the native blocks executed while bisecting
};

// Callouts into the exact engine for the rare instructions (defined in ps2_vu1_core.cpp).
struct VU1NativeAccess
{
    // Flags across block boundaries, through the flag rings (cycle = start + rel): the MAC / clip value
    // a reader issued at rel sees, the clip chain value CLIP shifts (newest, in flight included), and queueing a writer
    // issued at rel (visible kFmacLatency later).
    static uint32_t macAt(VU1NativeCtx &n, uint32_t rel);
    static uint32_t clipAt(VU1NativeCtx &n, uint32_t rel);
    static uint32_t clipChain(VU1NativeCtx &n);
    static void pushClip(VU1NativeCtx &n, uint32_t rel, uint32_t clip);
    static uint32_t ringAt(const VU1Interpreter::NativeState::FlagRing &r, uint64_t at);
    static void ringPush(VU1Interpreter::NativeState::FlagRing &r, uint64_t issue, uint32_t value);
    static void pushMacRaw(VU1NativeCtx &n, uint32_t rel, __m128 raw, uint32_t mask, const VU1FlagTail *tail);
    static void pushClipTail(VU1NativeCtx &n, uint32_t rel, uint32_t clip, const VU1FlagTail *tail);
    // The newest writer of ring slot k's tail record visible at `at`, if any.
    static bool tailVisible(const VU1FlagRing &r, uint32_t k, uint64_t at, uint32_t &value);
    static void importFlags(VU1Interpreter &vu);
    static void exportFlags(VU1Interpreter &vu);
    // Pipeline timing across native blocks (see VU1Interpreter::NativeState::BlockTiming)
    static void commitValuesKeepTiming(VU1Interpreter &vu);
    static void prepareBlockTiming(VU1Interpreter &vu, VU1Interpreter::NativeState::BlockTiming &bt, const uint8_t *code,
                                   uint32_t pc, uint32_t count, const uint8_t *idle);
    static const uint8_t *blockTiming(VU1Interpreter &vu, VU1Interpreter::NativeState::Slot &sl, uint32_t &cycId);
    static void scalarStart(VU1Interpreter &vu, uint64_t issue, uint32_t latency, bool fdiv);
    static void commitDue(VU1Interpreter &vu); // commitReadyPipelines after a native block (FDIV/EFU-only fast path)
    static VU1Interpreter::NativeState::Slot &slotFor(VU1Interpreter &vu, const uint8_t *code, uint32_t codeSize, uint32_t pc);
    static void codeChanged(VU1Interpreter::NativeState &ns, const uint8_t *code); // micro memory (16 KB) rewritten
    static uint64_t &laneWord(VU1Interpreter &vu, uint32_t lane);
    template <typename F>
    static void walkHistory(VU1Interpreter::NativeState &ns, uint64_t now, uint64_t want[3], F &&ready);
    static void syncInflight(VU1Interpreter &vu);
    static void finishBlockTiming(VU1Interpreter &vu, const VU1Interpreter::NativeState::Slot &sl, const uint8_t *cyc, uint32_t cycId,
                                  uint64_t start);
    static uint32_t chainTo(VU1NativeCtx &n, uint32_t pc, VU1NativeFn expected);
    static void callLower(VU1NativeCtx &n, uint32_t pc, uint32_t lower, uint32_t upper);
    static void callUpper(VU1NativeCtx &n, uint32_t pc, uint32_t upper);
    static void xgkick(VU1NativeCtx &n, uint32_t qwordAddress);
    // GOW_VU1N_RECORD / GOW_VU1N_REPLAY (bit-exact A/B of generator changes, see ps2_vu1_native.inl)
    static void recordReplay(VU1Interpreter &vu, void *context, uint32_t maxCycles);
    static uint64_t silentNativeHash(VU1Interpreter &vu, void *context);
};


// The VU arithmetic must not be reassociated, contracted or assumed finite (the runtime builds with /fp:fast).
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#endif

#if defined(_MSC_VER)
#define VU1N_INLINE __forceinline
#define VU1N_NOINLINE __declspec(noinline)
#else
#define VU1N_INLINE inline __attribute__((always_inline))
#define VU1N_NOINLINE __attribute__((noinline))
#endif
// --- flags across native blocks (inline: the generated blocks call these at every exit / entry-flag read) -------------
// Flags keep their latency across native blocks, and blocks that read older flags get their exact issue cycles from the
// pipeline state at entry (character shadows; docs/vu1-native-flags.md).
// A writer issued at cycle c is visible to readers from c + kFmacLatency, exactly as in the exact engine's flag pipeline.
inline bool VU1NativeAccess::tailVisible(const VU1FlagRing &r, uint32_t k, uint64_t at, uint32_t &out)
{
    const VU1FlagTail *t = r.tail[k];
    if (!t)
        return false;
    const uint32_t count = r.clip ? t->desc->clipCount : t->desc->macCount;
    for (uint32_t j = count; j-- > 0u;) // newest first
    {
        const uint8_t pair = r.clip ? t->desc->clipPair[j] : t->desc->macPair[j];
        if (t->start + t->cyc[pair] + VU1Interpreter::kFmacLatency <= at)
        {
            out = r.clip ? t->c[j] : vu1n::macValRt(t->q[j], t->desc->macMask[j]);
            return true;
        }
    }
    return false;
}

VU1N_INLINE uint32_t VU1NativeAccess::ringAt(const VU1Interpreter::NativeState::FlagRing &r, uint64_t at)
{
    for (uint32_t k = 1; k <= 4u; ++k)
    {
        const uint32_t i = VU1FlagRing::back(r.next, k); // newest first
        if (r.issue[i] + VU1Interpreter::kFmacLatency <= at)
            return r.valueOf(i);
        uint32_t v;
        if (r.tail[i] && tailVisible(r, i, at, v))
            return v;
    }
    return r.valueOf(VU1FlagRing::back(r.next, 5u));
}

VU1N_INLINE void VU1NativeAccess::ringPush(VU1Interpreter::NativeState::FlagRing &r, uint64_t issue, uint32_t value)
{
    // the oldest of the four leaves (it becomes slot next-5): newer writers hide it from every later reader once visible
    r.issue[r.next] = issue;
    r.value[r.next] = value;
    r.mask[r.next] = 0u;
    r.tail[r.next] = nullptr;
    r.next = (r.next + 1u) & VU1FlagRing::kWrap;
}

VU1N_INLINE uint32_t VU1NativeAccess::macAt(VU1NativeCtx &n, uint32_t rel) { return ringAt(*n.macRing, n.start + rel); }
VU1N_INLINE uint32_t VU1NativeAccess::clipAt(VU1NativeCtx &n, uint32_t rel) { return ringAt(*n.clipRing, n.start + rel); }
VU1N_INLINE uint32_t VU1NativeAccess::clipChain(VU1NativeCtx &n) { return n.vu->m_workingClip; }
VU1N_INLINE void VU1NativeAccess::pushMacRaw(VU1NativeCtx &n, uint32_t rel, __m128 raw, uint32_t mask, const VU1FlagTail *tail)
{
    VU1FlagRing &r = *n.macRing;
    r.issue[r.next] = n.start + rel;
    r.raw[r.next] = raw;
    r.mask[r.next] = mask;
    r.tail[r.next] = tail;
    r.next = (r.next + 1u) & VU1FlagRing::kWrap;
}
VU1N_INLINE void VU1NativeAccess::pushClipTail(VU1NativeCtx &n, uint32_t rel, uint32_t clip, const VU1FlagTail *tail)
{
    n.vu->m_workingClip = clip;
    VU1FlagRing &r = *n.clipRing;
    r.issue[r.next] = n.start + rel;
    r.value[r.next] = clip;
    r.mask[r.next] = 0u;
    r.tail[r.next] = tail;
    r.next = (r.next + 1u) & VU1FlagRing::kWrap;
}
VU1N_INLINE void VU1NativeAccess::pushClip(VU1NativeCtx &n, uint32_t rel, uint32_t clip)
{
    n.vu->m_workingClip = clip;
    ringPush(*n.clipRing, n.start + rel, clip);
}

// --- pipeline timing between native blocks (inline: runNative and the self-loops of the generated blocks) ----------
VU1N_INLINE void VU1NativeAccess::scalarStart(VU1Interpreter &vu, uint64_t issue, uint32_t latency, bool fdiv)
{
    if (fdiv)
    {
        vu.m_fdiv.valid = true;
        vu.m_fdiv.readyCycle = issue + latency;
        vu.m_fdiv.value = vu.m_state.q;
        vu.m_fdiv.statusDi = vu.m_state.status & 0x30u;
        vu.m_nextCommitCycle = std::min(vu.m_nextCommitCycle, vu.m_fdiv.readyCycle);
        return;
    }
    // EFU instructions run through callLower, which queued their result at the block's start cycle: move the newest
    // entry to the pair's real issue cycle
    VU1Interpreter::ScalarPipelineEntry *newest = nullptr;
    for (VU1Interpreter::ScalarPipelineEntry &e : vu.m_efu)
        if (e.valid && (!newest || e.readyCycle >= newest->readyCycle))
            newest = &e;
    if (newest)
    {
        newest->readyCycle = issue + latency;
        vu.m_nextCommitCycle = std::min(vu.m_nextCommitCycle, newest->readyCycle);
    }
    vu.m_efuResourceReady = issue + (latency > 0u ? latency - 1u : 0u);
}

// commitReadyPipelines once m_cycle reached m_nextCommitCycle after a native block. The blocks write their values
// directly and queue only FDIV/EFU entries (kept for timing, scalarStart). While no other queue holds anything (the
// usual case; otherwise commitReadyPipelines runs), only those entries can be due: retired here with the same effects
// and the same next commit cycle as commitReadyPipelines, without its call and its scan of the other queues.
VU1N_INLINE void VU1NativeAccess::commitDue(VU1Interpreter &vu)
{
    if ((vu.m_storePending | vu.m_vfPending | vu.m_viPending | vu.m_accPending) != 0u || vu.m_flagCount != 0u)
    {
        vu.commitReadyPipelines();
        return;
    }
    uint64_t next = ~0ull;
    if (vu.m_fdiv.valid)
    {
        if (vu.m_fdiv.readyCycle <= vu.m_cycle)
        {
            vu.m_state.q = vu.m_fdiv.value;
            const uint32_t currentDi = vu.m_fdiv.statusDi & 0x30u;
            vu.m_state.status = (vu.m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            vu.m_fdiv.valid = false;
        }
        else
            next = vu.m_fdiv.readyCycle;
    }
    for (VU1Interpreter::ScalarPipelineEntry &entry : vu.m_efu)
    {
        if (!entry.valid)
            continue;
        if (entry.readyCycle <= vu.m_cycle)
        {
            vu.m_state.p = entry.value;
            entry.valid = false;
        }
        else
            next = std::min(next, entry.readyCycle);
    }
    vu.m_nextCommitCycle = next;
}

// After a native block: it joins the history (its results that outlive it are found there when needed); FDIV/EFU stay
// busy until their ready cycle.
VU1N_INLINE void VU1NativeAccess::finishBlockTiming(VU1Interpreter &vu, const VU1Interpreter::NativeState::Slot &sl, const uint8_t *cyc,
                                                     uint32_t cycId, uint64_t start)
{
    using NS = VU1Interpreter::NativeState;
    NS &ns = *vu.m_native;
    // the idle table's length is in the slot: most blocks never touch their table's cache line here
    const uint64_t end = start + (cyc == sl.idle ? sl.idleEnd : cyc[sl.pairs - 1u] + 1u);
    NS::Hist &h = ns.hist[ns.histNext];
    h.bt = sl.bt;
    h.cyc = cyc;
    h.start = start;
    h.end = end;
    h.cycId = cycId;
    ns.histNext = (ns.histNext + 1u) % NS::kHist;
    ++ns.histCount;
    vu.m_cycle = end;
    if (sl.scalarCount == 0u)
        return;
    if (sl.scalarCount != NS::kScalarMany)
    {
        for (uint32_t k = 0; k < sl.scalarCount; ++k)
            scalarStart(vu, start + cyc[sl.scalar[k].pair], sl.scalar[k].latency, sl.scalar[k].fdiv);
        return;
    }
    const NS::BlockTiming &bt = *sl.bt;
    for (const uint8_t i : bt.scalarPairs)
    {
        const VU1Interpreter::DecodedInstructionPair &d = bt.dec[i];
        scalarStart(vu, start + cyc[i], d.lowerUsage.latency, d.lowerUsage.pipeline == VU1Interpreter::PipelineFdiv);
    }
}


// Chained blocks: the generated code puts the blocks of a microprogram that branch to each other into one function
// (vu1n_gen.py regions), with the registers kept in locals, and goes from a block straight on to its successor when
// that successor's slot holds the expected function (the words at its pc are the ones it was generated from). Between the
// two blocks chainTo does what runNative does between two blocks: the finished block's timing and history entry, the
// due FDIV/EFU retirement, the stop / budget check, and the successor's start cycle, timing table and tail record. Only
// successors with an idle-pipeline timing table are entered (no entry-flag reads), and only while nothing from the exact
// engine is pending (else commitDue would run commitReadyPipelines, which writes registers the block holds in locals).
// Returns 0: nothing done (runNative finishes the block), 1: successor set up, 2: block finished, runNative dispatches
// the successor (the block returns with kVu1nFinished).
VU1N_INLINE uint32_t VU1NativeAccess::chainTo(VU1NativeCtx &n, uint32_t pc, VU1NativeFn expected)
{
    using NS = VU1Interpreter::NativeState;
    VU1Interpreter &vu = *n.vu;
    NS &ns = *vu.m_native;
    const NS::Slot &sl = *static_cast<const NS::Slot *>(n.slot);
    if (ns.limit != ~0u || (vu.m_storePending | vu.m_vfPending | vu.m_viPending | vu.m_accPending) != 0u || vu.m_flagCount != 0u)
        return 0u;
    finishBlockTiming(vu, sl, n.cyc, n.cycId, n.start);
    if (vu.m_cycle >= vu.m_nextCommitCycle)
        commitDue(vu);
    const NS::Slot &next = ns.slots[(pc & 0x3FF8u) >> 3];
    if (vu.m_stopRequested || vu.m_cycle >= n.budgetEnd || next.fn != expected || !next.known || next.readsEntryFlags)
        return 2u;
    ++ns.nativeBlocks; // runNative counts the block the function returns from
    ns.nativePairs += sl.pairs;
    n.slot = &next;
    n.start = vu.m_cycle;
    n.cyc = next.idle;
    n.cycId = 0u;
    if (next.tail)
    {
        VU1FlagTail &t = ns.hist[ns.histNext].tail;
        t.desc = next.tail;
        t.cyc = next.idle;
        t.start = n.start;
        n.tail = &t;
    }
    return 1u;
}

namespace vu1n
{
    VU1N_INLINE uint64_t fnv(uint64_t h, uint32_t w)
    {
        for (int i = 0; i < 4; ++i)
            h = (h ^ ((w >> (i * 8)) & 0xFFu)) * 1099511628211ull;
        return h;
    }
    VU1N_INLINE uint64_t blockKey(uint32_t pc, const uint32_t *words, uint32_t count)
    {
        uint64_t h = fnv(1469598103934665603ull, pc);
        for (uint32_t i = 0; i < count; ++i)
            h = fnv(h, words[i]);
        return h;
    }

    // lane masks: DEST field order (x = 8) -> SSE blend/movemask order (x = 1).
    constexpr int kRev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

    VU1N_INLINE __m128 ld(const VU1State &s, int r) { return _mm_loadu_ps(s.vf[r]); }
    // Operand view of a register: Inf/NaN (exponent 255) act as +-FLT_MAX, as in the exact engine's normalizeOperand. The register
    // itself keeps its raw bits (integer data travels through VF registers).
    VU1N_INLINE __m128 nrm(__m128 v)
    {
#if defined(__aarch64__)
        // NEON, for every operand: the magnitude bits as an unsigned minimum with FLT_MAX's (exponent 255 is exactly what
        // exceeds 0x7F7FFFFF), the sign kept by one BSL: BIC, UMIN, BSL
        const uint32x4_t b = vreinterpretq_u32_f32(v);
        const uint32x4_t sign = vdupq_n_u32(0x80000000u);
        return vreinterpretq_f32_u32(vbslq_u32(sign, b, vminq_u32(vbicq_u32(b, sign), vdupq_n_u32(0x7F7FFFFFu))));
#else
        const __m128i b = _mm_castps_si128(v);
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i bad = _mm_cmpeq_epi32(_mm_and_si128(b, expMask), expMask);
        const __m128i fixed = _mm_or_si128(_mm_and_si128(b, _mm_set1_epi32(static_cast<int>(0x80000000u))), _mm_set1_epi32(0x7F7FFFFF));
        return _mm_castsi128_ps(_mm_blendv_epi8(b, fixed, bad));
#endif
    }
    VU1N_INLINE __m128 ldn(const VU1State &s, int r) { return nrm(_mm_loadu_ps(s.vf[r])); }
    VU1N_INLINE __m128 ldAcc(const VU1State &s) { return _mm_loadu_ps(s.acc); }
    template <int M> // M in SSE order
    VU1N_INLINE void stf(float *p, __m128 v)
    {
        if constexpr (M == 15)
            _mm_storeu_ps(p, v);
        else if constexpr (M != 0)
            _mm_storeu_ps(p, _mm_blend_ps(_mm_loadu_ps(p), v, M));
    }
    template <int C>
    VU1N_INLINE __m128 bc(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(C, C, C, C)); }
    VU1N_INLINE __m128 clampv(__m128 r)
    {
#if defined(__aarch64__)
        // ARMv8 FMAXNM/FMINNM: one instruction each, and the x86 result for a quiet NaN in r (maxps returns its second
        // operand, FMAXNM the number), which is all arithmetic can produce; sse2neon's precise min/max (the Switch
        // build's setting) needs a compare and a select per call, and this runs after every FMAC
        return vminnmq_f32(vmaxnmq_f32(r, vdupq_n_f32(-FLT_MAX)), vdupq_n_f32(FLT_MAX));
#else
        return _mm_min_ps(_mm_max_ps(r, _mm_set1_ps(-FLT_MAX)), _mm_set1_ps(FLT_MAX));
#endif
    }
#if defined(__aarch64__)
    // NEON: the MAC bits of all lanes (DEST order: x = bit 3 of each nibble) from the lane masks with one ADDV. Through
    // sse2neon each of the three _mm_movemask_ps below is a shift, a variable shift and an ADDV, for every FMAC.
    // Big: round-toward-zero's largest finite value also counts as overflow (macVal), not only exponent 255 (macFlags).
    template <bool Big>
    VU1N_INLINE uint32_t macBitsNeon(__m128 r)
    {
        static constexpr uint32_t kZero[4] = {0x8u, 0x4u, 0x2u, 0x1u};
        static constexpr uint32_t kSign[4] = {0x80u, 0x40u, 0x20u, 0x10u};
        static constexpr uint32_t kOver[4] = {0x8000u, 0x4000u, 0x2000u, 0x1000u};
        const uint32x4_t b = vreinterpretq_u32_f32(r);
        const uint32x4_t mag = vandq_u32(b, vdupq_n_u32(0x7FFFFFFFu));
        const uint32x4_t expMask = vdupq_n_u32(0x7F800000u);
        uint32x4_t over = vceqq_u32(vandq_u32(b, expMask), expMask);
        if constexpr (Big)
            over = vorrq_u32(over, vceqq_u32(mag, vdupq_n_u32(0x7F7FFFFFu)));
        const uint32x4_t zero = vceqq_u32(mag, vdupq_n_u32(0u));
        const uint32x4_t sign = vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(b), 31));
        const uint32x4_t bits = vorrq_u32(vorrq_u32(vandq_u32(zero, vld1q_u32(kZero)), vandq_u32(sign, vld1q_u32(kSign))),
                                          vandq_u32(over, vld1q_u32(kOver)));
        return vaddvq_u32(bits);
    }
    // the MAC bits a DEST mask (SSE order) keeps
    VU1N_INLINE uint32_t macDestBits(uint32_t m) { return static_cast<uint32_t>(kRev4[m & 15u]) * 0x1011u; }
#endif
    // MAC register after an FMAC: per-lane zero / sign / overflow (underflow is not tracked); lanes outside DEST are 0.
    template <int M>
    VU1N_INLINE void macFlags(VU1State &s, __m128 r)
    {
#if defined(__aarch64__)
        s.mac = macBitsNeon<false>(r) & macDestBits(M);
#else
        const __m128i b = _mm_castps_si128(r);
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const int z = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(b, _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128()))) & M;
        const int sg = _mm_movemask_ps(r) & M;
        const int o = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(b, expMask), expMask))) & M;
        s.mac = static_cast<uint32_t>(kRev4[z] | (kRev4[sg] << 4) | (kRev4[o] << 12));
#endif
    }
    // acc +/- a*b. The exact engine evaluates it unrounded, so a product that overflows float can still cancel against ACC:
    // when the float result is not finite the lanes are redone in double (out of line: rare, and keeping it out lets the
    // fast path inline into the generated blocks; MSVC x64 passes __m128 arguments of a real call through memory).
    template <bool Sub>
    VU1N_NOINLINE __m128 maddWide(__m128 acc, __m128 a, __m128 b)
    {
        alignas(16) float fa[4], fb[4], fc[4], out[4];
        _mm_store_ps(fa, acc);
        _mm_store_ps(fb, a);
        _mm_store_ps(fc, b);
        for (int i = 0; i < 4; ++i)
        {
            const double x = Sub ? static_cast<double>(fa[i]) - static_cast<double>(fb[i]) * static_cast<double>(fc[i])
                                 : static_cast<double>(fa[i]) + static_cast<double>(fb[i]) * static_cast<double>(fc[i]);
            out[i] = x > static_cast<double>(FLT_MAX) ? HUGE_VALF : x < -static_cast<double>(FLT_MAX) ? -HUGE_VALF : static_cast<float>(x);
        }
        return _mm_load_ps(out);
    }
    template <bool Sub>
    VU1N_INLINE __m128 madd(__m128 acc, __m128 a, __m128 b)
    {
        const __m128 p = _mm_mul_ps(a, b);
        __m128 r = Sub ? _mm_sub_ps(acc, p) : _mm_add_ps(acc, p);
        // round-toward-zero saturates an overflowing product to FLT_MAX instead of infinity: any lane at/over FLT_MAX in the product
        // or the sum needs the wide evaluation
#if defined(__aarch64__)
        // NEON: the larger magnitude of product and sum per lane, its maximum against the limit (one compare in a general
        // register), and the wide evaluation inline in double lanes: no call, so the block keeps its vector registers
        const uint32x4_t sign = vdupq_n_u32(0x80000000u);
        const uint32x4_t mag = vmaxq_u32(vbicq_u32(vreinterpretq_u32_f32(p), sign), vbicq_u32(vreinterpretq_u32_f32(r), sign));
        if (__builtin_expect(vmaxvq_u32(mag) <= 0x7F7FFFFEu, 1))
            return r;
        const float64x2_t aLo = vcvt_f64_f32(vget_low_f32(acc)), aHi = vcvt_high_f64_f32(acc);
        const float64x2_t pLo = vmulq_f64(vcvt_f64_f32(vget_low_f32(a)), vcvt_f64_f32(vget_low_f32(b)));
        const float64x2_t pHi = vmulq_f64(vcvt_high_f64_f32(a), vcvt_high_f64_f32(b));
        const float64x2_t xLo = Sub ? vsubq_f64(aLo, pLo) : vaddq_f64(aLo, pLo);
        const float64x2_t xHi = Sub ? vsubq_f64(aHi, pHi) : vaddq_f64(aHi, pHi);
        const float32x4_t narrow = vcvt_high_f32_f64(vcvt_f32_f64(xLo), xHi);
        const float64x2_t big = vdupq_n_f64(static_cast<double>(FLT_MAX));
        const uint32x4_t over = vcombine_u32(vmovn_u64(vcagtq_f64(xLo, big)), vmovn_u64(vcagtq_f64(xHi, big)));
        // beyond FLT_MAX: infinity of the result's sign (maddWide's HUGE_VALF), else the float conversion
        const uint32x4_t inf = vorrq_u32(vandq_u32(vreinterpretq_u32_f32(narrow), sign), vdupq_n_u32(0x7F800000u));
        return vreinterpretq_f32_u32(vbslq_u32(over, inf, vreinterpretq_u32_f32(narrow)));
#else
        const __m128i lim = _mm_set1_epi32(0x7F7FFFFE);
        const __m128i hot = _mm_or_si128(_mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(p), _mm_set1_epi32(0x7FFFFFFF)), lim),
                                         _mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7FFFFFFF)), lim));
        if (_mm_movemask_epi8(hot) == 0)
            return r;
        return maddWide<Sub>(acc, a, b);
#endif
    }
    // macVal with the DEST mask (SSE order) at run time: flags of a MAC writer read after its block ended
    inline uint32_t macValRt(__m128 r, uint32_t m)
    {
#if defined(__aarch64__)
        return macBitsNeon<true>(r) & macDestBits(m);
#else
        const __m128i b = _mm_castps_si128(r);
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const int z = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(b, _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128()))) & m;
        const int sg = _mm_movemask_ps(r) & m;
        const __m128i big = _mm_cmpeq_epi32(_mm_and_si128(b, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF));
        const int o = _mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(_mm_cmpeq_epi32(_mm_and_si128(b, expMask), expMask), big))) & m;
        return static_cast<uint32_t>(kRev4[z] | (kRev4[sg] << 4) | (kRev4[o] << 12));
#endif
    }
    template <int M>
    VU1N_INLINE uint32_t macVal(__m128 r)
    {
#if defined(__aarch64__)
        return macBitsNeon<true>(r) & macDestBits(M);
#else
        const __m128i b = _mm_castps_si128(r);
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const int z = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(b, _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128()))) & M;
        const int sg = _mm_movemask_ps(r) & M;
        // round-toward-zero turns an overflow into the largest finite value, so that counts as overflow too
        const __m128i big = _mm_cmpeq_epi32(_mm_and_si128(b, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF));
        const int o = _mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(_mm_cmpeq_epi32(_mm_and_si128(b, expMask), expMask), big))) & M;
        return static_cast<uint32_t>(kRev4[z] | (kRev4[sg] << 4) | (kRev4[o] << 12));
#endif
    }
    VU1N_INLINE uint32_t clipVal(const VU1State &s, int fs, int ft, uint32_t prev)
    {
        alignas(16) float v[4], t[4];
        _mm_store_ps(v, ldn(s, fs));
        _mm_store_ps(t, ldn(s, ft));
        const float w = std::fabs(t[3]);
        uint32_t f = 0;
        if (v[0] > w) f |= 0x01u;
        if (v[0] < -w) f |= 0x02u;
        if (v[1] > w) f |= 0x04u;
        if (v[1] < -w) f |= 0x08u;
        if (v[2] > w) f |= 0x10u;
        if (v[2] < -w) f |= 0x20u;
        return ((prev << 6) | f) & 0xFFFFFFu;
    }
    VU1N_INLINE float qOperand(const VU1State &s) { return s.q; }

    // --- integer / memory helpers -------------------------------------------------------------------------------
    VU1N_INLINE uint32_t qaddr(const VU1NativeCtx &n, int32_t qw) { return (static_cast<uint32_t>(qw) * 16u) & n.mask; }
    VU1N_INLINE __m128 ldq(const VU1NativeCtx &n, uint32_t addr) { return _mm_loadu_ps(reinterpret_cast<const float *>(n.data + addr)); }
    template <int M>
    VU1N_INLINE void stq(VU1NativeCtx &n, uint32_t addr, __m128 v) { stf<M>(reinterpret_cast<float *>(n.data + addr), v); }
    VU1N_INLINE int32_t sx16(int32_t v) { return static_cast<int16_t>(v); }
    VU1N_INLINE int32_t ldWord(const VU1NativeCtx &n, uint32_t addr, int comp)
    {
        uint32_t v;
        std::memcpy(&v, n.data + addr + comp * 4, 4);
        return static_cast<int16_t>(v & 0xFFFFu);
    }
    VU1N_INLINE void stWords(VU1NativeCtx &n, uint32_t addr, int32_t value, int destMask)
    {
        const uint32_t w = static_cast<uint16_t>(value & 0xFFFF);
        for (int c = 0; c < 4; ++c)
            if (destMask & (8 >> c))
                std::memcpy(n.data + addr + c * 4, &w, 4);
    }
    VU1N_INLINE __m128 fromInt(int32_t v)
    {
        const float f = [&] { float x; std::memcpy(&x, &v, 4); return x; }();
        return _mm_set1_ps(f);
    }
    VU1N_INLINE int32_t compBits(const VU1State &s, int reg, int comp)
    {
        uint32_t v;
        std::memcpy(&v, &s.vf[reg][comp], 4);
        return static_cast<int16_t>(v & 0xFFFFu);
    }

    // --- FMAC special ops ----------------------------------------------------------------------------------------
    template <int Shift, int M>
    VU1N_INLINE void itof(VU1State &s, int fs, int ft)
    {
        const __m128 v = _mm_cvtepi32_ps(_mm_castps_si128(ld(s, fs))); // integer bits: raw
        __m128 r = v;
        if constexpr (Shift == 4) r = _mm_mul_ps(v, _mm_set1_ps(1.0f / 16.0f));
        if constexpr (Shift == 12) r = _mm_mul_ps(v, _mm_set1_ps(1.0f / 4096.0f));
        if constexpr (Shift == 15) r = _mm_mul_ps(v, _mm_set1_ps(1.0f / 32768.0f));
        stf<M>(s.vf[ft], r);
    }
    template <int Shift, int M>
    VU1N_INLINE void ftoi(VU1State &s, int fs, int ft)
    {
        __m128 v = ldn(s, fs);
        constexpr float scale = Shift == 0 ? 1.0f : Shift == 4 ? 16.0f : Shift == 12 ? 4096.0f : 32768.0f;
        v = _mm_mul_ps(clampv(v), _mm_set1_ps(scale));
#if defined(__aarch64__)
        // FCVTZS saturates (>= 2^31 -> INT_MAX, <= -2^31 -> INT_MIN) and clampv leaves no NaN: floatToInt's result
        // without sse2neon's x86 fix-up of cvtt and the blend below
        stf<M>(s.vf[ft], vreinterpretq_f32_s32(vcvtq_s32_f32(v)));
#else
        // saturate like floatToInt: >= 2^31 -> INT_MAX (cvtt gives INT_MIN for out of range)
        const __m128 big = _mm_cmpge_ps(v, _mm_set1_ps(2147483648.0f));
        __m128i i = _mm_cvttps_epi32(v);
        i = _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(i), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), big));
        stf<M>(s.vf[ft], _mm_castsi128_ps(i));
#endif
    }
    template <int M>
    VU1N_INLINE void absOp(VU1State &s, int fs, int ft)
    {
        stf<M>(s.vf[ft], _mm_and_ps(ldn(s, fs), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))));
    }
    VU1N_INLINE void clipOp(VU1State &s, int fs, int ft)
    {
        const float *v = s.vf[fs];
        const float w = std::fabs(s.vf[ft][3]);
        uint32_t f = 0;
        if (v[0] > w) f |= 0x01u;
        if (v[0] < -w) f |= 0x02u;
        if (v[1] > w) f |= 0x04u;
        if (v[1] < -w) f |= 0x08u;
        if (v[2] > w) f |= 0x10u;
        if (v[2] < -w) f |= 0x20u;
        s.clip = ((s.clip << 6) | f) & 0xFFFFFFu;
    }

    // --- Q unit --------------------------------------------------------------------------------------------------
    VU1N_INLINE float qnorm(float x)
    {
        uint32_t b;
        std::memcpy(&b, &x, 4);
        const uint32_t e = (b >> 23) & 0xFFu;
        if (e == 0u) b &= 0x80000000u;
        else if (e == 255u) b = (b & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&x, &b, 4);
        return x;
    }
    VU1N_INLINE float qfinish(float x)
    {
        uint32_t b;
        std::memcpy(&b, &x, 4);
        const uint32_t e = (b >> 23) & 0xFFu;
        if (e == 0u) b &= 0x80000000u;
        else if (e == 255u) b = (b & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&x, &b, 4);
        return x;
    }
    VU1N_INLINE void divOp(VU1State &s, int fs, int fsf, int ft, int ftf)
    {
        const float num = qnorm(s.vf[fs][fsf]), den = qnorm(s.vf[ft][ftf]);
        float r;
        if (den == 0.0f)
            r = (std::signbit(num) != std::signbit(den)) ? -FLT_MAX : FLT_MAX;
        else
            r = num / den;
        s.q = qfinish(r);
    }
    VU1N_INLINE void sqrtOp(VU1State &s, int ft, int ftf) { s.q = qfinish(std::sqrt(std::fabs(qnorm(s.vf[ft][ftf])))); }
    VU1N_INLINE void rsqrtOp(VU1State &s, int fs, int fsf, int ft, int ftf)
    {
        const float num = qnorm(s.vf[fs][fsf]);
        const float den = std::sqrt(std::fabs(qnorm(s.vf[ft][ftf])));
        float r;
        if (den != 0.0f)
            r = num / den;
        else
            r = std::signbit(num) ? -FLT_MAX : FLT_MAX;
        s.q = qfinish(r);
    }

    // --- register-file-in-locals variants (generated blocks keep VF/VI/ACC/Q/I in C++ locals; same semantics as above) ---
    inline uint32_t qa(uint32_t mask, int32_t qw) { return (static_cast<uint32_t>(qw) * 16u) & mask; }
    inline __m128 ldm(const uint8_t *mem, uint32_t addr) { return _mm_loadu_ps(reinterpret_cast<const float *>(mem + addr)); }
    template <int M>
    inline void stm(uint8_t *mem, uint32_t addr, __m128 v) { stf<M>(reinterpret_cast<float *>(mem + addr), v); }
    inline int32_t ldWordm(const uint8_t *mem, uint32_t addr, int comp)
    {
        uint32_t v;
        std::memcpy(&v, mem + addr + comp * 4, 4);
        return static_cast<int16_t>(v & 0xFFFFu);
    }
    inline void stWordsm(uint8_t *mem, uint32_t addr, int32_t value, int destMask)
    {
        const uint32_t w = static_cast<uint16_t>(value & 0xFFFF);
        for (int c = 0; c < 4; ++c)
            if (destMask & (8 >> c))
                std::memcpy(mem + addr + c * 4, &w, 4);
    }
    template <int C> // raw lane (bits preserved)
    inline float lane(__m128 v)
    {
        uint32_t b = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(_mm_shuffle_ps(v, v, _MM_SHUFFLE(C, C, C, C)))));
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    template <int C>
    inline int32_t laneBits16(__m128 v)
    {
        const uint32_t b = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(_mm_shuffle_ps(v, v, _MM_SHUFFLE(C, C, C, C)))));
        return static_cast<int16_t>(b & 0xFFFFu);
    }
    template <int Shift>
    inline __m128 itofv(__m128 raw)
    {
        const __m128 v = _mm_cvtepi32_ps(_mm_castps_si128(raw));
        if constexpr (Shift == 4) return _mm_mul_ps(v, _mm_set1_ps(1.0f / 16.0f));
        if constexpr (Shift == 12) return _mm_mul_ps(v, _mm_set1_ps(1.0f / 4096.0f));
        if constexpr (Shift == 15) return _mm_mul_ps(v, _mm_set1_ps(1.0f / 32768.0f));
        return v;
    }
    template <int Shift>
    inline __m128 ftoiv(__m128 normalized)
    {
        constexpr float scale = Shift == 0 ? 1.0f : Shift == 4 ? 16.0f : Shift == 12 ? 4096.0f : 32768.0f;
        const __m128 v = _mm_mul_ps(clampv(normalized), _mm_set1_ps(scale));
#if defined(__aarch64__)
        return vreinterpretq_f32_s32(vcvtq_s32_f32(v)); // FCVTZS saturates like floatToInt (see ftoi)
#endif
        const __m128 big = _mm_cmpge_ps(v, _mm_set1_ps(2147483648.0f));
        const __m128i i = _mm_cvttps_epi32(v);
        return _mm_blendv_ps(_mm_castsi128_ps(i), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), big);
    }
    inline __m128 absv(__m128 normalized) { return _mm_and_ps(normalized, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))); }
    inline uint32_t clipValv(__m128 vn, __m128 tn, uint32_t prev)
    {
        alignas(16) float v[4], t[4];
        _mm_store_ps(v, vn);
        _mm_store_ps(t, tn);
        const float w = std::fabs(t[3]);
        uint32_t f = 0;
        if (v[0] > w) f |= 0x01u;
        if (v[0] < -w) f |= 0x02u;
        if (v[1] > w) f |= 0x04u;
        if (v[1] < -w) f |= 0x08u;
        if (v[2] > w) f |= 0x10u;
        if (v[2] < -w) f |= 0x20u;
        return ((prev << 6) | f) & 0xFFFFFFu;
    }
    inline float divv(float fsRaw, float ftRaw)
    {
        const float num = qnorm(fsRaw), den = qnorm(ftRaw);
        float r;
        if (den == 0.0f)
            r = (std::signbit(num) != std::signbit(den)) ? -FLT_MAX : FLT_MAX;
        else
            r = num / den;
        return qfinish(r);
    }
    inline float sqrtv(float ftRaw) { return qfinish(std::sqrt(std::fabs(qnorm(ftRaw)))); }
    inline float rsqrtv(float fsRaw, float ftRaw)
    {
        const float num = qnorm(fsRaw);
        const float den = std::sqrt(std::fabs(qnorm(ftRaw)));
        float r;
        if (den != 0.0f)
            r = num / den;
        else
            r = std::signbit(num) ? -FLT_MAX : FLT_MAX;
        return qfinish(r);
    }
}

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif

#endif
