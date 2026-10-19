// GOW-Port: native VU1 backend runtime (included at the end of ps2_vu1_core.cpp). See include/runtime/ps2_vu1_native.h.
#include "runtime/ps2_vu1_native.h"

#include <memory>
#include <unordered_map>

// GOW_VU1N_BLOCKPROF=<file> (with GOW_VU1N_REPLAY): per dispatched block pc, runs and VU cycles of the warm replay
// passes, written at the end of the replay ("pc runs cycles"); finds the hot paths of the microprograms (docs/vu1-hle.md)
static std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> *g_vu1nBlockProf = nullptr;
static bool g_vu1nTraceThis = false;
// with GOW_VU1N_BLOCKPROF: dispatches from block pc a to block pc b ("e a b count" lines; run with GOW_VU1N_DIRECT=0
// GOW_VU1N_TRACES=0 so that every edge outside the generated loops is a dispatch): vu1n_gen.py --profile picks the
// traces' likely successors from them
static std::unordered_map<uint32_t, uint64_t> *g_vu1nEdgeProf = nullptr;

namespace
{
    struct NativeEntry
    {
        VU1NativeFn fn;
        uint32_t pairs;
        const VU1TailDesc *tail;
        bool readsEntryFlags; // reads MAC/clip flags written before the block (vu1n_gen.py: bit 16 of pairs)
        std::vector<uint8_t> cyc; // issue cycle of each pair (block entry = 0), filled on first use
    };
    std::unordered_map<uint64_t, NativeEntry> &nativeRegistry()
    {
        static std::unordered_map<uint64_t, NativeEntry> registry;
        return registry;
    }
    bool isBranchLower(uint32_t lower)
    {
        switch (lower >> 25)
        {
        case 0x20: case 0x21: case 0x24: case 0x25: case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            return true;
        default:
            return false;
        }
    }
    // Same rule as tools/vu1/vu1n_gen.py: pairs from pc up to and including the delay slot of the first branch, the pair
    // after an E bit, or a D/T pair; at most 64 pairs; an all-zero pair is invalid (extent 0).
    uint32_t nativeExtent(const uint8_t *code, uint32_t size, uint32_t pc, uint32_t *words)
    {
        uint32_t n = 0;
        for (uint32_t p = pc; p + 8u <= size && n < 64u; p += 8u)
        {
            uint32_t lower, upper;
            std::memcpy(&lower, code + p, 4);
            std::memcpy(&upper, code + p + 4, 4);
            if (lower == 0u && upper == 0u)
                return n == 0u ? 0u : n;
            words[n * 2u] = lower;
            words[n * 2u + 1u] = upper;
            ++n;
            const bool iBit = (upper >> 31) != 0u;
            if (((upper >> 28) & 1u) || ((upper >> 27) & 1u))
                break;
            if ((!iBit && isBranchLower(lower)) || ((upper >> 30) & 1u))
            {
                uint32_t dl, du;
                if (p + 16u <= size)
                {
                    std::memcpy(&dl, code + p + 8u, 4);
                    std::memcpy(&du, code + p + 12u, 4);
                    if (!(dl == 0u && du == 0u))
                    {
                        words[n * 2u] = dl;
                        words[n * 2u + 1u] = du;
                        ++n;
                    }
                }
                break;
            }
        }
        return n;
    }
}

void vu1nRegisterBlock(uint64_t key, VU1NativeFn fn, uint32_t pairs, const VU1TailDesc *tail)
{
    nativeRegistry()[key] = {fn, pairs & 0xFFFFu, tail, (pairs & 0x10000u) != 0u, {}};
}

struct VU1TraceEntry
{
    uint64_t key;
    VU1NativeFn fn;
    bool readsEntryFlags;
    uint32_t parts;
    const uint16_t *pcs, *counts;
    const uint64_t *keys;
    const VU1TailDesc *tail;
    std::vector<uint8_t> idle;     // the parts' idle tables one after the other (filled on first use)
    std::vector<uint16_t> pairPcs; // pc of every pair
    bool tooLong = false;          // the idle tables exceed kTraceMaxCycles: not used
};
namespace
{
    // Traces end within this many cycles on idle tables, so that with part 0's entry stalls (below the latencies, < 128)
    // every issue cycle still fits the uint8_t timing tables.
    constexpr uint32_t kTraceMaxCycles = 120u;
    std::unordered_map<uint64_t, std::vector<std::unique_ptr<VU1TraceEntry>>> &traceRegistry() // by part 0's block key
    {
        static std::unordered_map<uint64_t, std::vector<std::unique_ptr<VU1TraceEntry>>> registry;
        return registry;
    }
    const bool g_vu1nTracesOff = std::getenv("GOW_VU1N_TRACES") && std::atoi(std::getenv("GOW_VU1N_TRACES")) == 0;
    // GOW_VU1N_TRACE_PC=<hex pc>: only the traces starting there (diagnostics: finding a trace that changes results);
    // GOW_VU1N_TRACE_ENTRY=0: no traces whose part 0 reads entry flags
    const long g_vu1nTracePc = std::getenv("GOW_VU1N_TRACE_PC") ? std::strtol(std::getenv("GOW_VU1N_TRACE_PC"), nullptr, 16) : -1;
    const bool g_vu1nTraceEntryOff = std::getenv("GOW_VU1N_TRACE_ENTRY") && std::atoi(std::getenv("GOW_VU1N_TRACE_ENTRY")) == 0;
}

void vu1nRegisterTrace(uint64_t key, VU1NativeFn fn, uint32_t flags, uint32_t parts, const uint16_t *pcs, const uint16_t *counts,
                       const uint64_t *keys, const VU1TailDesc *tail)
{
    auto te = std::make_unique<VU1TraceEntry>();
    te->key = key;
    te->fn = fn;
    te->readsEntryFlags = (flags & 0x10000u) != 0u;
    te->parts = parts;
    te->pcs = pcs;
    te->counts = counts;
    te->keys = keys;
    te->tail = tail;
    traceRegistry()[keys[0]].push_back(std::move(te));
}

// Issue cycles of the pairs of a block as the exact engine would see them (operand/pipeline stalls included), assuming
// every pipeline is idle at block entry. Flag readers use them to decide which earlier FMAC/CLIP result is visible.
void VU1Interpreter::nativeTiming(const uint8_t *code, uint32_t pc, uint32_t count, std::vector<uint8_t> &cyc)
{
    VU1Interpreter t(*this);
    constexpr uint64_t base = 1u << 20;
    t.m_cycle = base;
    for (auto &r : t.m_vfReady)
        r.fill(0u);
    t.m_viReady.fill(0u);
    t.m_accReady.fill(0u);
    t.m_fdiv.valid = false;
    for (auto &e : t.m_efu)
        e.valid = false;
    t.m_efuResourceReady = 0;
    t.m_xgkick.active = false;
    cyc.assign(count, 0);
    for (uint32_t i = 0; i < count; ++i)
    {
        const DecodedInstructionPair d = t.decodeInstructionPair(code, pc + i * 8u);
        const uint64_t ready = t.calculatePairReadyCycle(d);
        if (ready > t.m_cycle)
            t.m_cycle = ready;
        cyc[i] = static_cast<uint8_t>(std::min<uint64_t>(t.m_cycle - base, 255u));
        t.markPairWrites(d);
        if (d.lowerUsage.pipeline == PipelineFdiv && !d.lowerUsage.waitQ)
        {
            t.m_fdiv.valid = true;
            t.m_fdiv.readyCycle = t.m_cycle + d.lowerUsage.latency;
        }
        else if (d.lowerUsage.pipeline == PipelineEfu && !d.lowerUsage.waitP)
        {
            t.m_efu[0].valid = true;
            t.m_efu[0].readyCycle = t.m_cycle + d.lowerUsage.latency;
            t.m_efuResourceReady = t.m_cycle + (d.lowerUsage.latency > 0u ? d.lowerUsage.latency - 1u : 0u);
        }
        ++t.m_cycle;
    }
}

void VU1NativeAccess::callLower(VU1NativeCtx &n, uint32_t pc, uint32_t lower, uint32_t upper)
{
    VU1Interpreter &vu = *n.vu;
    auto &c = *static_cast<VU1Interpreter::StepContext *>(n.context);
    vu.m_state.pc = pc;
    vu.execLower(lower, c.vuData, c.dataSize, *c.gs, c.memory, upper);
    VU1NativeAccess::commitValuesKeepTiming(vu);
    VU1NativeAccess::importFlags(vu); // the callout's flag writes join the native blocks' ones
}

void VU1NativeAccess::callUpper(VU1NativeCtx &n, uint32_t pc, uint32_t upper)
{
    VU1Interpreter &vu = *n.vu;
    vu.m_state.pc = pc;
    vu.execUpper(upper);
    VU1NativeAccess::commitValuesKeepTiming(vu);
    VU1NativeAccess::importFlags(vu); // the callout's flag writes join the native blocks' ones
}

void VU1NativeAccess::xgkick(VU1NativeCtx &n, uint32_t qwordAddress)
{
    VU1Interpreter &vu = *n.vu;
    if (vu.m_unit != VU1Interpreter::Unit::VU1 || !vu.m_activeVuData || vu.m_activeVuDataSize < 16u)
        return;
    vu.m_xgkick.reset();
    vu.m_xgkick.active = true;
    vu.m_xgkick.sourceAddress = (qwordAddress * 16u) % vu.m_activeVuDataSize;
    vu.m_xgkick.cycleCredit = 0x40000000u; // the whole packet is read at once (PCSX2 does the same)
    vu.m_xgkick.issueCycle = vu.m_cycle;
    // Fast path: walk the GIFtags in VU memory, then copy the whole packet with at most two memcpy (wrap). Same bytes and
    // end state as progressXgkick's qword loop; anything unusual (bad format, oversize, xgkick trace on) takes that loop.
    const uint32_t size = vu.m_activeVuDataSize;
    if (!g_xgkickTrace && (size & (size - 1u)) == 0u)
    {
        constexpr uint32_t kMax = VU1Interpreter::XgkickPipeline::kBufferSize;
        const uint32_t src = vu.m_xgkick.sourceAddress;
        uint32_t total = 0u;
        bool ok = false;
        for (;;)
        {
            const uint32_t at = (src + total) & (size - 1u);
            if (total > kMax - 16u || at + 8u > size)
                break;
            uint64_t tagLo;
            std::memcpy(&tagLo, vu.m_activeVuData + at, 8);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
                break;
            if (tagBytes > kMax - total)
                break;
            total += static_cast<uint32_t>(tagBytes);
            if ((tagLo >> 15) & 1u)
            {
                ok = true;
                break;
            }
        }
        if (ok)
        {
            const uint32_t first = src & (size - 1u);
            const uint32_t head = std::min(total, size - first);
            vu.m_xgkick.copiedBytes = total;
            vu.m_xgkick.totalBytes = total;
            vu.m_xgkick.currentTagEnd = total;
            vu.m_xgkick.currentTagEop = true;
            vu.m_xgkick.cycleCredit = 0x40000000u - (total / 16u) * 2u + 1u;
            if (head == total)
            {
                // the packet is contiguous in VU memory and is handed on before the next VU instruction: no copy here
                // (the GS stage queue copies it once, ~170 MB/s in gameplay)
                vu.finishXgkick(vu.m_activeVuData + first);
                return;
            }
            uint8_t *dst = vu.m_xgkick.packet.data();
            std::memcpy(dst, vu.m_activeVuData + first, head);
            for (uint32_t done = head; done < total;)
            {
                const uint32_t chunk = std::min(total - done, size);
                std::memcpy(dst + done, vu.m_activeVuData, chunk);
                done += chunk;
            }
            vu.finishXgkick();
            return;
        }
    }
    vu.progressXgkick();
}

// Native entry (and after an exact callout inside a block): the exact engine's flag pipeline moves into the rings
// (status / sticky updates, not modelled by the native blocks, are applied at once like before).
void VU1NativeAccess::importFlags(VU1Interpreter &vu)
{
    VU1Interpreter::NativeState &ns = *vu.m_native;
    if (!ns.flagsInRings)
    {
        ns.macRing.reset(vu.m_state.mac);
        ns.clipRing.reset(vu.m_state.clip);
        ns.clipRing.clip = true;
        ns.flagsInRings = true;
    }
    while (vu.m_flagCount != 0u)
    {
        auto &e = vu.m_flagPipeline[vu.m_flagHead];
        vu.m_flagHead = (vu.m_flagHead + 1u) % VU1Interpreter::kMaxFlagEntries;
        --vu.m_flagCount;
        if (e.writesMac)
            ringPush(ns.macRing, e.issueCycle, e.mac);
        if (e.writesClip)
            ringPush(ns.clipRing, e.issueCycle, e.clip);
        if (e.writesStatus)
        {
            const uint32_t current = e.status & 0xFu;
            vu.m_state.status = (vu.m_state.status & 0xFF0u) | current | ((current | e.extraSticky) << 6);
        }
        if (e.writesSticky)
            vu.m_state.status = (vu.m_state.status & 0x03Fu) | (e.status & 0xFC0u);
        e.valid = false;
    }
}

// The exact engine continues: visible flags become its committed values, writers still in flight its flag pipeline.
void VU1NativeAccess::exportFlags(VU1Interpreter &vu)
{
    VU1Interpreter::NativeState &ns = *vu.m_native;
    if (!ns.flagsInRings)
        return;
    ns.flagsInRings = false;
    vu.m_state.mac = ringAt(ns.macRing, vu.m_cycle);
    vu.m_state.clip = ringAt(ns.clipRing, vu.m_cycle);
    // pending writers of both rings (tail records included), oldest first
    struct P { uint64_t issue; uint32_t value; bool clip; };
    P pending[32];
    uint32_t count = 0;
    for (const auto *r : {&ns.macRing, &ns.clipRing})
        for (uint32_t k = 4u; k >= 1u; --k)
        {
            const uint32_t i = VU1FlagRing::back(r->next, k);
            if (r->issue[i] + VU1Interpreter::kFmacLatency > vu.m_cycle)
            {
                pending[count++] = {r->issue[i], r->valueOf(i), r == &ns.clipRing};
                if (const VU1FlagTail *t = r->tail[i])
                {
                    const uint32_t n = r->clip ? t->desc->clipCount : t->desc->macCount;
                    for (uint32_t j = 0; j < n; ++j)
                    {
                        const uint8_t pair = r->clip ? t->desc->clipPair[j] : t->desc->macPair[j];
                        const uint64_t issue = t->start + t->cyc[pair];
                        if (issue + VU1Interpreter::kFmacLatency > vu.m_cycle)
                            pending[count++] = {issue, r->clip ? t->c[j] : vu1n::macValRt(t->q[j], t->desc->macMask[j]), r->clip};
                    }
                }
            }
        }
    std::sort(pending, pending + count, [](const P &a, const P &b) { return a.issue < b.issue; });
    for (uint32_t k = 0; k < count; ++k)
    {
        if (vu.m_flagCount >= VU1Interpreter::kMaxFlagEntries)
            break;
        auto &e = vu.m_flagPipeline[(vu.m_flagHead + vu.m_flagCount) % VU1Interpreter::kMaxFlagEntries];
        ++vu.m_flagCount;
        e.mac = pending[k].clip ? 0u : pending[k].value;
        e.clip = pending[k].clip ? pending[k].value : 0u;
        e.status = e.extraSticky = 0u;
        e.writesMac = !pending[k].clip;
        e.writesClip = pending[k].clip;
        e.writesStatus = e.writesSticky = false;
        e.valid = true;
        e.issueCycle = pending[k].issue;
        e.readyCycle = pending[k].issue + VU1Interpreter::kFmacLatency;
    }
}

// Values of results in flight are written at once (a reader would stall until they are ready anyway, so it sees the
// same value), but their timing state stays: the flag queue, and FDIV/EFU marked busy until their ready cycle.
void VU1NativeAccess::commitValuesKeepTiming(VU1Interpreter &vu)
{
    const uint32_t flagHead = vu.m_flagHead, flagCount = vu.m_flagCount;
    const VU1Interpreter::ScalarPipelineEntry fdiv = vu.m_fdiv;
    const auto efu = vu.m_efu;
    vu.m_flagCount = 0u;
    vu.commitEverythingNow();
    vu.m_flagHead = flagHead;
    vu.m_flagCount = flagCount;
    vu.m_fdiv = fdiv;
    vu.m_efu = efu;
    if (vu.m_fdiv.valid)
        vu.m_nextCommitCycle = std::min(vu.m_nextCommitCycle, vu.m_fdiv.readyCycle);
    for (const VU1Interpreter::ScalarPipelineEntry &e : vu.m_efu)
        if (e.valid)
            vu.m_nextCommitCycle = std::min(vu.m_nextCommitCycle, e.readyCycle);
}

// First use of a block: decoded pairs, the lanes it reads before writing them (with the idle-schedule cycle of that
// first read) and the last write of every lane it writes.
void VU1NativeAccess::prepareBlockTiming(VU1Interpreter &vu, VU1Interpreter::NativeState::BlockTiming &bt, const uint8_t *code, uint32_t pc, uint32_t count,
                                        const uint8_t *idle, const uint16_t *pairPcs, uint32_t readPairs)
{
    using NS = VU1Interpreter::NativeState;
    bt.idle = idle;
    bt.dec.clear();
    bt.scalarPairs.clear();
    bool read[NS::kLanes] = {}, written[NS::kLanes] = {};
    bool scalarWritten[3] = {};
    uint32_t &maxLat = vu.m_native->maxLat;
    const auto onRead = [&](uint32_t lane, uint8_t at) {
        if (read[lane] || written[lane])
            return;
        read[lane] = true;
        bt.readMask[lane >> 6] |= 1ull << (lane & 63u);
        bt.readAt[lane] = at;
    };
    const auto onWrite = [&](uint32_t lane, uint32_t pair, uint32_t lat) {
        written[lane] = true;
        bt.writeMask[lane >> 6] |= 1ull << (lane & 63u);
        bt.lastPair[lane] = static_cast<uint8_t>(pair);
        bt.lastLat[lane] = static_cast<uint8_t>(lat);
        maxLat = std::max(maxLat, lat);
    };
    const auto onScalarRead = [&](uint32_t k, uint8_t at) {
        if (scalarWritten[k] || (bt.scalarRead & (1u << k)))
            return;
        bt.scalarRead = static_cast<uint8_t>(bt.scalarRead | (1u << k));
        bt.scalarAt[k] = at;
    };
    for (uint32_t i = 0; i < count; ++i)
    {
        const VU1Interpreter::DecodedInstructionPair d = vu.decodeInstructionPair(code, pairPcs ? pairPcs[i] : pc + i * 8u);
        bt.dec.push_back(d);
        const uint8_t at = idle[i];
        // traces: only part 0's reads (the other parts run on their idle tables, see BlockTiming::simCount)
        for (const VU1Interpreter::InstructionUsage *u : {&d.upperUsage, &d.lowerUsage})
        {
            if (i >= readPairs)
                break;
            for (uint32_t k = 0; k < u->vfReadCount; ++k)
                for (uint32_t c = 0; c < 4u; ++c)
                    if (u->vfRead[k].lanes & (1u << (3u - c)))
                        onRead(u->vfRead[k].reg * 4u + c, at);
            for (uint32_t r = 1; r < 16u; ++r)
                if (u->viRead & (1u << r))
                    onRead(128u + r, at);
            for (uint32_t c = 0; c < 4u; ++c)
                if (u->accRead & (1u << (3u - c)))
                    onRead(144u + c, at);
        }
        if (i < readPairs && (d.lowerUsage.pipeline == VU1Interpreter::PipelineFdiv || d.lowerUsage.waitQ))
            onScalarRead(0, at);
        if (i < readPairs && d.lowerUsage.pipeline == VU1Interpreter::PipelineEfu)
            onScalarRead(1, at);
        if (i < readPairs && d.lowerUsage.waitP)
            onScalarRead(2, at);
        // writes, in markPairWrites' order (lower VF, upper VF, VI, ACC)
        const auto vfWrite = [&](const VU1Interpreter::InstructionUsage &u, bool lower) {
            const VU1Interpreter::VfAccess w = u.vfWrite;
            if (w.reg == 0u || (lower && d.suppressedLowerVf == w.reg))
                return;
            const uint32_t lat = u.vfLatency != 0u ? u.vfLatency : u.latency;
            for (uint32_t c = 0; c < 4u; ++c)
                if (w.lanes & (1u << (3u - c)))
                    onWrite(w.reg * 4u + c, i, lat);
        };
        vfWrite(d.lowerUsage, true);
        vfWrite(d.upperUsage, false);
        for (uint32_t r = 1; r < 16u; ++r)
            if (d.lowerUsage.viWrite & (1u << r))
                onWrite(128u + r, i, d.lowerUsage.viLatency != 0u ? d.lowerUsage.viLatency : d.lowerUsage.latency);
        for (uint32_t c = 0; c < 4u; ++c)
            if (d.upperUsage.accWrite & (1u << (3u - c)))
                onWrite(144u + c, i, VU1Interpreter::kAccForwardLatency);
        const bool fdivOp = d.lowerUsage.pipeline == VU1Interpreter::PipelineFdiv && !d.lowerUsage.waitQ;
        const bool efuOp = d.lowerUsage.pipeline == VU1Interpreter::PipelineEfu && !d.lowerUsage.waitP;
        if (fdivOp || efuOp)
        {
            bt.scalarPairs.push_back(static_cast<uint8_t>(i));
            scalarWritten[fdivOp ? 0 : 1] = true;
            if (efuOp)
                scalarWritten[2] = true;
        }
    }
}

static inline uint64_t &vu1nLaneWord(VU1Interpreter &vu, uint32_t lane)
{
    return VU1NativeAccess::laneWord(vu, lane);
}

uint64_t &VU1NativeAccess::laneWord(VU1Interpreter &vu, uint32_t lane)
{
    if (lane < 128u)
        return vu.m_vfReady[lane >> 2][lane & 3u];
    if (lane < 144u)
        return vu.m_viReady[lane - 128u];
    return vu.m_accReady[lane - 144u];
}

// Ready cycles of lanes from the blocks in `hist`, newest first: calls ready(lane, cycle) for every lane of `want` whose
// newest writer is one of them and can still be in flight at `now` (cycle 0: surely ready), and removes those lanes from
// `want`. Lanes left in `want` are ready by `now` unless arraysReadyBy > now: then they hold the exact engine's values.
// Blocks that ended maxLat or more cycles before `now` (and all older ones) hold nothing in flight any more.
template <typename F>
void VU1NativeAccess::walkHistory(VU1Interpreter::NativeState &ns, uint64_t now, uint64_t want[3], F &&ready)
{
    using NS = VU1Interpreter::NativeState;
    const uint32_t n = std::min(ns.histCount, NS::kHist);
    uint32_t k = 0;
    for (; k < n; ++k)
    {
        const NS::Hist &h = ns.hist[(ns.histNext + NS::kHist - 1u - k) % NS::kHist];
        if (h.end + ns.maxLat <= now)
            break;
        for (uint32_t w = 0; w < 3u; ++w)
        {
            const uint64_t lanes = want[w] & h.bt->writeMask[w];
            want[w] &= ~lanes;
            for (uint64_t bits = lanes; bits != 0u; bits &= bits - 1u)
            {
                const uint32_t lane = w * 64u + static_cast<uint32_t>(std::countr_zero(bits));
                ready(lane, h.start + h.cyc[h.bt->lastPair[lane]] + h.bt->lastLat[lane]);
            }
        }
    }
    if (ns.arraysReadyBy <= now)
        return;
    // Shortly after native entry: lanes written by the older blocks are ready, the others keep the exact engine's values.
    // All blocks since entry are still in the ring here (they span less than the exact engine's latencies, far below kHist).
    for (; k < n; ++k)
    {
        const NS::Hist &h = ns.hist[(ns.histNext + NS::kHist - 1u - k) % NS::kHist];
        for (uint32_t w = 0; w < 3u; ++w)
        {
            const uint64_t lanes = want[w] & h.bt->writeMask[w];
            want[w] &= ~lanes;
            for (uint64_t bits = lanes; bits != 0u; bits &= bits - 1u)
                ready(w * 64u + static_cast<uint32_t>(std::countr_zero(bits)), 0u);
        }
    }
}

// Before the exact engine looks at its readiness arrays: lanes the native blocks wrote that may still be in flight get
// their real ready cycle (the others are ready by now, as every stale value is); the history starts over.
void VU1NativeAccess::syncInflight(VU1Interpreter &vu)
{
    VU1Interpreter::NativeState &ns = *vu.m_native;
    uint64_t want[3] = {~0ull, ~0ull, (1ull << (VU1Interpreter::NativeState::kLanes - 128u)) - 1u};
    uint64_t readyBy = ns.arraysReadyBy;
    walkHistory(ns, vu.m_cycle, want, [&](uint32_t lane, uint64_t ready) {
        vu1nLaneWord(vu, lane) = ready;
        readyBy = std::max(readyBy, ready);
    });
    ns.arraysReadyBy = readyBy;
    ns.histCount = 0u;
}

// Issue cycles (relative to vu.m_cycle) of the block's pairs: the idle table unless a lane it reads is still in flight
// past that read; then the exact engine's stall model, run once per distinct (lane, slack) set and cached.
const uint8_t *VU1NativeAccess::blockTiming(VU1Interpreter &vu, VU1Interpreter::NativeState::Slot &sl, uint32_t &cycId)
{
    using NS = VU1Interpreter::NativeState;
    NS &ns = *vu.m_native;
    NS::BlockTiming &bt = *sl.bt;
    const uint64_t start = vu.m_cycle;
    const NS::Hist *pred = ns.histCount ? &ns.hist[(ns.histNext + NS::kHist - 1u) % NS::kHist] : nullptr;
    // (a memo hit is taken by entryTiming before this is called)
    const bool memoOk = pred && (sl.memoable || ns.chainFlags) && pred->end == start && pred->end - pred->start >= ns.maxLat &&
                        ns.arraysReadyBy <= start;
    const NS::BlockTiming *predBt = memoOk ? pred->bt : nullptr;
    const uint32_t predId = memoOk ? pred->cycId : 0u;
    const uint32_t scalar = memoOk && !sl.memoable ? scalarKey(vu, bt, start) : 0u;
    const auto remember = [&](const uint8_t *cyc, uint32_t id) {
        if (predBt)
        {
            sl.memoPred[1] = sl.memoPred[0], sl.memoPredId[1] = sl.memoPredId[0], sl.memoCyc[1] = sl.memoCyc[0];
            sl.memoId[1] = sl.memoId[0], sl.memoScalar[1] = sl.memoScalar[0];
            sl.memoPred[0] = predBt, sl.memoPredId[0] = predId, sl.memoCyc[0] = cyc, sl.memoId[0] = id, sl.memoScalar[0] = scalar;
        }
        cycId = id;
        return cyc;
    };
    uint64_t hot[3] = {};
    uint8_t slack[NS::kLanes];
    const auto note = [&](uint32_t lane, uint64_t ready) {
        const uint64_t readAt = start + bt.readAt[lane];
        if (ready > readAt)
        {
            hot[lane >> 6] |= 1ull << (lane & 63u);
            slack[lane] = static_cast<uint8_t>(std::min<uint64_t>(ready - readAt, 255u));
        }
    };
    uint64_t want[3] = {bt.readMask[0], bt.readMask[1], bt.readMask[2]};
    walkHistory(ns, start, want, note);
    if (ns.arraysReadyBy > start) // not written by native blocks since the exact engine ran: its readiness arrays
        for (uint32_t w = 0; w < 3u; ++w)
            for (uint64_t bits = want[w]; bits != 0u; bits &= bits - 1u)
            {
                const uint32_t lane = w * 64u + static_cast<uint32_t>(std::countr_zero(bits));
                note(lane, vu1nLaneWord(vu, lane));
            }
    ns.sig.clear();
    for (uint32_t w = 0; w < 3u; ++w)
        for (uint64_t bits = hot[w]; bits != 0u; bits &= bits - 1u)
        {
            const uint32_t lane = w * 64u + static_cast<uint32_t>(std::countr_zero(bits));
            ns.sig.push_back(static_cast<uint16_t>((lane << 8) | slack[lane]));
        }
    if (bt.scalarRead)
    {
        uint64_t ready[3] = {vu.m_fdiv.valid ? vu.m_fdiv.readyCycle : 0u, vu.m_efuResourceReady, 0u};
        for (const VU1Interpreter::ScalarPipelineEntry &e : vu.m_efu)
            if (e.valid)
                ready[2] = std::max(ready[2], e.readyCycle);
        for (uint32_t k = 0; k < 3u; ++k)
            if ((bt.scalarRead & (1u << k)) && ready[k] > start + bt.scalarAt[k])
                ns.sig.push_back(static_cast<uint16_t>(((200u + k) << 8) | std::min<uint64_t>(ready[k] - start - bt.scalarAt[k], 255u)));
    }
    if (ns.sig.empty())
        return remember(bt.idle, 0u);
    for (const auto &c : bt.cache)
        if (c.sig == ns.sig)
            return remember(c.cyc.data(), c.id);
    // simulate on the exact engine's pipeline state (synced; this also drops the history, whose timing tables may be
    // cache slots replaced below), then restore it
    syncInflight(vu);
    const uint64_t keepCycle = vu.m_cycle;
    const auto vfReady = vu.m_vfReady;
    const auto viReady = vu.m_viReady;
    const auto accReady = vu.m_accReady;
    const VU1Interpreter::ScalarPipelineEntry fdiv = vu.m_fdiv;
    const auto efu = vu.m_efu;
    const uint64_t efuResource = vu.m_efuResourceReady;
    const bool xgkick = vu.m_xgkick.active;
    vu.m_xgkick.active = false;
    std::vector<uint8_t> cyc(bt.dec.size());
    const size_t simN = bt.simCount ? bt.simCount : bt.dec.size();
    for (size_t i = 0; i < simN; ++i)
    {
        const VU1Interpreter::DecodedInstructionPair &d = bt.dec[i];
        const uint64_t ready = vu.calculatePairReadyCycle(d);
        if (ready > vu.m_cycle)
            vu.m_cycle = ready;
        cyc[i] = static_cast<uint8_t>(std::min<uint64_t>(vu.m_cycle - keepCycle, 255u));
        vu.markPairWrites(d);
        if (d.lowerUsage.pipeline == VU1Interpreter::PipelineFdiv && !d.lowerUsage.waitQ)
        {
            vu.m_fdiv.valid = true;
            vu.m_fdiv.readyCycle = vu.m_cycle + d.lowerUsage.latency;
        }
        else if (d.lowerUsage.pipeline == VU1Interpreter::PipelineEfu && !d.lowerUsage.waitP)
        {
            vu.m_efu[0].valid = true;
            vu.m_efu[0].readyCycle = vu.m_cycle + d.lowerUsage.latency;
            vu.m_efuResourceReady = vu.m_cycle + (d.lowerUsage.latency > 0u ? d.lowerUsage.latency - 1u : 0u);
        }
        ++vu.m_cycle;
    }
    for (size_t i = simN; i < cyc.size(); ++i) // a trace's later parts: their idle tables after part 0
        cyc[i] = static_cast<uint8_t>(std::min<uint32_t>(cyc[simN - 1u] + 1u + bt.rest[i - simN], 255u));
    vu.m_cycle = keepCycle;
    vu.m_vfReady = vfReady;
    vu.m_viReady = viReady;
    vu.m_accReady = accReady;
    vu.m_fdiv = fdiv;
    vu.m_efu = efu;
    vu.m_efuResourceReady = efuResource;
    vu.m_xgkick.active = xgkick;
    NS::BlockTiming::Cached &slot = bt.cache.size() < 16u ? bt.cache.emplace_back() : bt.cache[bt.next++ % 16u];
    slot.sig = ns.sig;
    slot.cyc = std::move(cyc);
    slot.id = ns.nextCycId++;
    for (NS::Slot &other : ns.slots) // may point into the cache slot replaced here
        if (other.bt == &bt)
            other.memoCyc[0] = other.memoCyc[1] = nullptr;
    return remember(slot.cyc.data(), slot.id);
}

// After a native block: it joins the history (its results that outlive it are found there when needed); FDIV/EFU stay
// busy until their ready cycle.
// An FDIV / EFU operation a native block started at `issue`: busy until its ready cycle.

// The micro memory has a new generation (VIF MPG, EE writes): slots whose extent scan read a changed pair are set up again
// on their next use; the others keep their block (the game swaps one microprogram while the rest stays resident).
void VU1NativeAccess::codeChanged(VU1Interpreter::NativeState &ns, const uint8_t *code)
{
    using NS = VU1Interpreter::NativeState;
    constexpr uint32_t kPairs = sizeof(ns.code) / 8u;
    static_assert(kPairs == std::size(ns.slots));
    ++ns.codeChanges;
    if (!ns.codeValid)
    {
        for (NS::Slot &sl : ns.slots)
            sl.known = false, sl.fn = nullptr, sl.key = 0u;
        std::memcpy(ns.code, code, sizeof(ns.code));
        ns.codeValid = true;
        return;
    }
    // changedBefore[i]: changed pairs among 0..i-1 (prefix count), so a slot's range test is one subtraction
    thread_local uint16_t changedBefore[kPairs + 1];
    uint32_t changed = 0;
    for (uint32_t i = 0; i < kPairs; ++i)
    {
        changedBefore[i] = static_cast<uint16_t>(changed);
        changed += std::memcmp(ns.code + i * 8u, code + i * 8u, 8u) != 0;
    }
    changedBefore[kPairs] = static_cast<uint16_t>(changed);
    if (changed == 0u)
        return;
    for (uint32_t i = 0; i < kPairs; ++i)
    {
        NS::Slot &sl = ns.slots[i];
        if (!sl.known)
            continue;
        bool hit = false;
        if (sl.trace)
            for (uint32_t k = 0; k < sl.trace->parts && !hit; ++k)
            {
                const uint32_t b = sl.trace->pcs[k] >> 3, e = std::min<uint32_t>(kPairs, b + sl.trace->counts[k] + 1u);
                hit = changedBefore[e] != changedBefore[b];
            }
        else
        {
            const uint32_t end = std::min<uint32_t>(kPairs, i + sl.pairs + 1u); // the pair after the block ends the scan
            hit = changedBefore[end] != changedBefore[i];
        }
        if (hit)
            sl.known = false, sl.fn = nullptr, sl.key = 0u; // chainDirect compares only the key
    }
    std::memcpy(ns.code, code, sizeof(ns.code));
}

// The dispatch slot of pc, set up on first use (native block lookup, timing tables).
VU1Interpreter::NativeState::Slot &VU1NativeAccess::slotFor(VU1Interpreter &vu, const uint8_t *code, uint32_t codeSize, uint32_t pc)
{
    VU1Interpreter::NativeState &ns = *vu.m_native;
    VU1Interpreter::NativeState::Slot &sl = ns.slots[pc >> 3];
    if (!sl.known)
    {
        uint32_t words[128];
        ++ns.slotSetups;
        const uint32_t count = nativeExtent(code, codeSize, pc, words);
        sl = {};
        sl.known = true;
        sl.pairs = static_cast<uint16_t>(count);
        if (count != 0u)
        {
            const auto it = nativeRegistry().find(vu1n::blockKey(pc, words, count * 2u));
            if (it != nativeRegistry().end())
            {
                if (it->second.cyc.empty())
                    vu.nativeTiming(code, pc, count, it->second.cyc);
                VU1Interpreter::NativeState::BlockTiming &bt = ns.timings[it->first];
                if (bt.dec.empty())
                    prepareBlockTiming(vu, bt, code, pc, count, it->second.cyc.data());
                bt.readsEntryFlags = it->second.readsEntryFlags;
                sl.fn = it->second.fn;
                sl.key = it->first;
                sl.idle = it->second.cyc.data();
                sl.idleEnd = static_cast<uint16_t>(sl.idle[count - 1u] + 1u);
                sl.bt = &bt;
                sl.readsEntryFlags = bt.readsEntryFlags;
                sl.memoable = bt.scalarRead == 0u;
                sl.tail = it->second.tail;
                if (bt.scalarPairs.size() > std::size(sl.scalar))
                    sl.scalarCount = VU1Interpreter::NativeState::kScalarMany;
                else
                    for (const uint8_t i : bt.scalarPairs)
                        sl.scalar[sl.scalarCount++] = {i, static_cast<uint8_t>(bt.dec[i].lowerUsage.latency),
                                                       bt.dec[i].lowerUsage.pipeline == VU1Interpreter::PipelineFdiv};
                // a trace starting with this block whose other parts are in place replaces it
                const auto tr = traceRegistry().find(it->first);
                if (tr != traceRegistry().end() && !g_vu1nTracesOff)
                    for (const auto &te : tr->second)
                        if (setupTrace(vu, sl, code, codeSize, *te))
                            break;
            }
            else if (ns.diag)
                ++ns.missPcs[pc];
        }
    }
    return sl;
}

// The scalar (FDIV / EFU) starts of a slot from its timing data.
void VU1NativeAccess::slotScalars(VU1Interpreter::NativeState::Slot &sl, const VU1Interpreter::NativeState::BlockTiming &bt)
{
    sl.scalarCount = 0u;
    if (bt.scalarPairs.size() > std::size(sl.scalar))
        sl.scalarCount = VU1Interpreter::NativeState::kScalarMany;
    else
        for (const uint8_t i : bt.scalarPairs)
            sl.scalar[sl.scalarCount++] = {i, static_cast<uint8_t>(bt.dec[i].lowerUsage.latency),
                                           bt.dec[i].lowerUsage.pipeline == VU1Interpreter::PipelineFdiv};
}

// A trace at sl (its part 0 set up there already): when every other part holds the block it was generated from, the slot
// runs the trace function with the parts' idle tables one after the other, and its prefix blocks (parts 0..k, the side
// exits) get their own timing data. Same history, flags and cycles as the parts run one by one (docs/vu1-hle.md).
bool VU1NativeAccess::setupTrace(VU1Interpreter &vu, VU1Interpreter::NativeState::Slot &sl, const uint8_t *code, uint32_t codeSize,
                                 const VU1TraceEntry &cte)
{
    using NS = VU1Interpreter::NativeState;
    NS &ns = *vu.m_native;
    VU1TraceEntry &te = const_cast<VU1TraceEntry &>(cte);
    if (te.tooLong || (g_vu1nTracePc >= 0 && te.pcs[0] != g_vu1nTracePc) || (g_vu1nTraceEntryOff && te.readsEntryFlags))
        return false;
    for (uint32_t k = 1; k < te.parts; ++k)
    {
        uint32_t words[128];
        const uint32_t count = nativeExtent(code, codeSize, te.pcs[k], words);
        if (count != te.counts[k] || vu1n::blockKey(te.pcs[k], words, count * 2u) != te.keys[k])
            return false;
        if (nativeRegistry().find(te.keys[k]) == nativeRegistry().end())
            return false;
    }
    if (te.idle.empty())
    {
        uint32_t base = 0u;
        for (uint32_t k = 0; k < te.parts; ++k)
        {
            NativeEntry &pe = nativeRegistry().find(te.keys[k])->second;
            if (pe.cyc.empty())
                vu.nativeTiming(code, te.pcs[k], te.counts[k], pe.cyc);
            for (uint32_t j = 0; j < te.counts[k]; ++j)
            {
                te.idle.push_back(static_cast<uint8_t>(std::min<uint32_t>(base + pe.cyc[j], 255u)));
                te.pairPcs.push_back(static_cast<uint16_t>(te.pcs[k] + 8u * j));
            }
            base += pe.cyc[te.counts[k] - 1u] + 1u;
        }
        if (base > kTraceMaxCycles)
        {
            te.tooLong = true;
            return false;
        }
    }
    const uint32_t n0 = te.counts[0], total = static_cast<uint32_t>(te.idle.size());
    const auto prep = [&](uint64_t key, uint32_t count) -> NS::BlockTiming & {
        NS::BlockTiming &bt = ns.timings[key];
        if (bt.dec.empty())
        {
            prepareBlockTiming(vu, bt, code, te.pcs[0], count, te.idle.data(), te.pairPcs.data(), n0);
            bt.simCount = n0;
            bt.rest.clear();
            for (uint32_t i = n0; i < count; ++i)
                bt.rest.push_back(static_cast<uint8_t>(te.idle[i] - (te.idle[n0 - 1u] + 1u)));
            bt.readsEntryFlags = te.readsEntryFlags;
        }
        return bt;
    };
    NS::BlockTiming &bt = prep(te.key, total);
    std::vector<NS::Slot> &parts = ns.traceParts[te.key];
    if (parts.empty())
    {
        parts.resize(te.parts - 1u);
        uint32_t c = 0u;
        for (uint32_t k = 0; k + 1u < te.parts; ++k)
        {
            c += te.counts[k];
            NS::BlockTiming &pbt = prep(te.key ^ (0x9E3779B97F4A7C15ull * (k + 1u)), c);
            NS::Slot &p = parts[k];
            p.known = true;
            p.pairs = static_cast<uint16_t>(c);
            p.idle = te.idle.data();
            p.idleEnd = static_cast<uint16_t>(te.idle[c - 1u] + 1u);
            p.bt = &pbt;
            slotScalars(p, pbt);
        }
    }
    sl.fn = te.fn;
    sl.idle = te.idle.data();
    sl.idleEnd = static_cast<uint16_t>(te.idle[total - 1u] + 1u);
    sl.bt = &bt;
    sl.pairs = static_cast<uint16_t>(total);
    sl.readsEntryFlags = te.readsEntryFlags;
    sl.memoable = bt.scalarRead == 0u;
    sl.tail = te.tail;
    slotScalars(sl, bt);
    sl.trace = &te;
    sl.parts = parts.data();
    return true;
}

void VU1Interpreter::commitEverythingNow()
{
    const uint64_t keep = m_cycle;
    m_cycle = ~0ull >> 1;
    m_nextCommitCycle = 0u;
    commitReadyPipelines();
    commitReadyFlags();
    m_cycle = keep;
    m_nextCommitCycle = ~0ull;
}

bool VU1Interpreter::runNative(StepContext &context)
{
    if (m_unit != Unit::VU1 || context.vuData == nullptr || context.codeSize != PS2_VU1_CODE_SIZE)
        return false;
    if (!m_native)
        m_native = new NativeState();
    NativeState &ns = *m_native;
    if (!ns.enabled)
        return false;
    if (m_state.branchPending || m_state.ebit || m_state.haltAfterDelaySlot || m_stopRequested)
        return false;

    const uint64_t generation = context.memory ? context.memory->getVU1CodeGeneration() : 0u;
    if (generation != ns.generation)
    {
        ns.generation = generation;
        VU1NativeAccess::codeChanged(ns, context.vuCode);
    }

    const uint64_t savedFpMode = vu_fpmode::get();
    bool armed = false; // pending exact-engine pipelines are committed only when a native block really runs
    VU1NativeCtx ctx;
    ctx.s = &m_state;
    ctx.data = context.vuData;
    ctx.mask = context.dataSize - 1u;
    ctx.vu = this;
    ctx.context = &context;
    ctx.macRing = &ns.macRing;
    ctx.clipRing = &ns.clipRing;
    ctx.budgetEnd = context.budgetEnd;

    bool handled = false;
    // per-block bookkeeping kept in registers across the block calls (the blocks are opaque calls: anything in memory is
    // reloaded after each one): diagnostics counters added to ns on exit, the verify bisection limit fixed for the run
    const bool bisecting = ns.limit != ~0u;
    const bool diag = ns.diag; // the pair count is only printed by GOW_VU1N_DIAG
    uint64_t blocks = 0u, pairs = 0u;
    uint32_t edgeFrom = 0u; // GOW_VU1N_BLOCKPROF: the block dispatched before
    for (;;)
    {
        if (m_stopRequested || m_cycle >= context.budgetEnd)
        {
            handled = true;
            break;
        }
        const uint32_t pc = m_state.pc & 0x3FF8u;
        if (pc + 8u > context.codeSize)
            break;
        NativeState::Slot &sl = ns.slots[pc >> 3];
        if (!sl.known)
            VU1NativeAccess::slotFor(*this, context.vuCode, context.codeSize, pc);
        VU1NativeFn fn = sl.fn;
        if (!fn)
        {
            ++ns.missBlocks;
            if (ns.diag)
                ++ns.missHits[pc];
            break;
        }
        if (!armed)
        {
            // Results in flight from the exact engine: values now, timing kept (blockTiming / the flag queue), so the
            // blocks stall on them and see their flags at the same cycles as the exact engine.
            commitReadyPipelines();
            commitReadyFlags();
            VU1NativeAccess::commitValuesKeepTiming(*this);
            VU1NativeAccess::importFlags(*this);
            ns.histCount = 0u; // the exact engine's readiness arrays hold the state in flight
            if (m_cycle != ns.exitCycle) // the exact engine ran since the last native exit
            {
                uint64_t readyBy = 0u;
                for (const auto &r : m_vfReady)
                    for (const uint64_t c : r)
                        readyBy = std::max(readyBy, c);
                for (const uint64_t c : m_viReady)
                    readyBy = std::max(readyBy, c);
                for (const uint64_t c : m_accReady)
                    readyBy = std::max(readyBy, c);
                ns.arraysReadyBy = readyBy;
            }
            ++ns.nativeRuns;
            vu_fpmode::set((savedFpMode & ~vu_fpmode::kRoundMask) | vu_fpmode::kRoundZero | vu_fpmode::kFlushZero);
            armed = true;
        }
        if (bisecting)
        {
            if (ns.count >= ns.limit)
                break;
            if (ns.count + 1u == ns.limit)
                ns.snap = m_state;
            ++ns.count;
            ns.trace.push_back(pc);
        }
        const uint64_t start = m_cycle;
        // Values never depend on timing, only flag reads do. A block that reads flags written before it gets its exact
        // issue cycles from the pipeline state at entry (the stalls decide which of those flags it sees); the others use
        // their idle-pipeline table and only hand their results and flags in flight on.
        uint32_t cycId = 0u;
        const uint8_t *cyc = sl.readsEntryFlags ? VU1NativeAccess::entryTiming(*this, sl, cycId) : sl.idle;
        ctx.cyc = cyc;
        ctx.start = start;
        ctx.slot = &sl;
        ctx.cycId = cycId;
        ctx.chainLeft = bisecting ? 0u : ns.chainMax;
        if (sl.tail)
        {
            VU1FlagTail &t = ns.hist[ns.histNext].tail;
            t.desc = sl.tail;
            t.cyc = cyc;
            t.start = start;
            ctx.tail = &t;
        }
        const uint32_t result = fn(ctx);
        if (g_vu1nBlockProf) // GOW_VU1N_BLOCKPROF (replay): VU cycles per dispatched block pc, chained blocks included
        {
            auto &e = (*g_vu1nBlockProf)[pc];
            ++e.first;
            e.second += m_cycle - start;
            if (g_vu1nEdgeProf && blocks != 0u)
                ++(*g_vu1nEdgeProf)[(edgeFrom << 16) | pc];
            edgeFrom = pc;
            if (g_vu1nTraceThis) // GOW_VU1N_TRACEREC=<record>: the dispatch sequence of that record
                std::fprintf(stderr, "[vu1n-trace] %x +%llu -> %x\n", pc, static_cast<unsigned long long>(m_cycle - start), result & 0x3FFFu);
        }
        // ctx describes the block the function returns from: chained blocks (VU1NativeAccess::chainTo) may have run after
        // the one dispatched here, and one that stopped the chain has finished its own timing (kVu1nFinished)
        const NativeState::Slot &last = *static_cast<const NativeState::Slot *>(ctx.slot);
        if (!(result & kVu1nFinished))
        {
            VU1NativeAccess::finishBlockTiming(*this, last, ctx.cyc, ctx.cycId, ctx.start);
            if (m_cycle >= m_nextCommitCycle)
                VU1NativeAccess::commitDue(*this); // FDIV/EFU entries kept for timing retire when ready (values are written already)
        }
        if (diag)
            pairs += last.pairs;
        ++blocks;
        m_state.pc = result & 0x3FFFu;
        if (result & kVu1nEnded)
        {
            context.programEnded = true;
            handled = true;
            break;
        }
        if (result & kVu1nBail)
            break;
    }
    ns.nativePairs += pairs;
    ns.nativeBlocks += blocks;
    m_state.cycles = m_cycle; // only read outside the blocks (diagnostics)
    if (armed)
    {
        VU1NativeAccess::syncInflight(*this); // the exact engine continues: its readiness arrays become current
        VU1NativeAccess::exportFlags(*this);
        ns.exitCycle = m_cycle;
    }
    vu_fpmode::set(savedFpMode);
    if (ns.diag)
    {
        static auto last = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5))
        {
            last = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[vu1n] nativePairs=%llu nativeBlocks=%llu missBlocks=%llu nativeEntries=%llu distinctMissPcs=%zu codeChanges=%llu slotSetups=%llu\n",
                         ns.nativePairs, ns.nativeBlocks, ns.missBlocks, ns.nativeRuns, ns.missPcs.size(), ns.codeChanges, ns.slotSetups);
            ns.nativePairs = ns.nativeBlocks = ns.missBlocks = ns.nativeRuns = ns.codeChanges = ns.slotSetups = 0;
            std::vector<std::pair<unsigned, uint32_t>> top;
            for (const auto &kv : ns.missHits)
                top.emplace_back(kv.second, kv.first);
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 40u; ++i)
                std::fprintf(stderr, "[vu1n-miss] pc=%x hits=%u\n", top[i].second, top[i].first);
            ns.missHits.clear();
        }
    }
    return handled;
}

// GOW_VU1N_RECORD=<file> [GOW_VU1N_RECORD_N=4000, GOW_VU1N_RECORD_STRIDE=7, GOW_VU1N_RECORD_SKIP=0 (runs skipped first, to
// record a later part of a zone run; ~76k runs per second in gameplay)]: every STRIDE-th VU1 program run, the input
// (VU1State, micro memory, data memory) and a bit-exact hash of the native result (state, data memory, GIF packets) are
// appended to <file>. GOW_VU1N_REPLAY=<file>: on the first VU1 run, every record is executed by this build's native
// blocks and its hash compared with the recorded one -> "[vu1n-replay] N records, M differ". Proves that a change of the
// generator / runtime keeps the output bit-identical.
namespace
{
    struct Vu1nRecord
    {
        uint32_t magic;
        uint32_t maxCycles;
        uint64_t hash;
        VU1State state;
        uint8_t code[PS2_VU1_CODE_SIZE];
        uint8_t data[16384];
    };
    uint64_t g_vu1nReplayNs = 0; // time spent in the native runs of the replay (GOW_VU1N_REPLAY_REPS timing)
    inline uint64_t fnvBytes(uint64_t h, const void *p, size_t n)
    {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i)
            h = (h ^ b[i]) * 1099511628211ull;
        return h;
    }
}

uint64_t VU1NativeAccess::silentNativeHash(VU1Interpreter &vu, void *contextPtr)
{
    auto &context = *static_cast<VU1Interpreter::StepContext *>(contextPtr);
    const bool silentBefore = vu.m_verifySilent;
    vu.m_verifySilent = true;
    g_vuVerifyCapture = true;
    g_vuVerifyPackets.clear();
    VU1Interpreter::StepContext c = context;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;)
    {
        if (vu.runNative(c))
            break;
        if (!vu.stepInterpreted(c))
            break;
    }
    g_vu1nReplayNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    vu.flushPipelines();
    g_vuVerifyCapture = false;
    vu.m_verifySilent = silentBefore;
    const VU1State &s = vu.m_state;
    uint64_t h = 1469598103934665603ull;
    h = fnvBytes(h, s.vf, sizeof(s.vf));
    h = fnvBytes(h, s.vi, sizeof(s.vi));
    h = fnvBytes(h, s.acc, sizeof(s.acc));
    h = fnvBytes(h, &s.q, 4);
    h = fnvBytes(h, &s.p, 4);
    h = fnvBytes(h, &s.i, 4);
    h = fnvBytes(h, &s.mac, 4);
    h = fnvBytes(h, &s.clip, 4);
    h = fnvBytes(h, &s.pc, sizeof(s.pc));
    const uint8_t ended = c.programEnded ? 1u : 0u;
    h = fnvBytes(h, &ended, 1);
    h = fnvBytes(h, c.vuData, c.dataSize);
    h = fnvBytes(h, g_vuVerifyPackets.data(), g_vuVerifyPackets.size());
    return h;
}

void VU1NativeAccess::recordReplay(VU1Interpreter &vu, void *contextPtr, uint32_t maxCycles)
{
    auto &context = *static_cast<VU1Interpreter::StepContext *>(contextPtr);
    static const char *recPath = std::getenv("GOW_VU1N_RECORD");
    static const char *repPath = std::getenv("GOW_VU1N_REPLAY");
    if ((!recPath && !repPath) || context.dataSize != 16384u || context.codeSize != PS2_VU1_CODE_SIZE)
        return;
    if (repPath)
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        FILE *f = std::fopen(repPath, "rb");
        if (!f)
        {
            std::fprintf(stderr, "[vu1n-replay] cannot open %s\n", repPath);
            return;
        }
        auto rec = std::make_unique<Vu1nRecord>();
        VU1Interpreter saved(vu);
        unsigned total = 0, differ = 0;
        unsigned warmDiffer = 0; // warm passes (block cache, entry-timing memos warm): hash or VU cycles unlike the cold pass
        uint64_t coldCycles = 0;
        static const unsigned reps = std::getenv("GOW_VU1N_REPLAY_REPS") ? static_cast<unsigned>(std::atoi(std::getenv("GOW_VU1N_REPLAY_REPS"))) : 1u;
        auto scratch = std::make_unique<Vu1nRecord>();
        uint64_t timedNs = 0;
        // GOW_VU1N_REPLAY_LIVEIN=1 (consecutive recording, STRIDE=1): what a run takes from the run before it. Per state
        // field: "dead" = poisoning it at the start leaves the result unchanged (not read before written); "stale" = the
        // value from the previous run's start (what a run started before its predecessor ended would see) leaves the
        // result unchanged. "mem" = the result is unchanged when the bytes the previous run wrote (and the VIF did not
        // rewrite since) hold their values from before that run. Prints the table at the end.
        static const bool liveIn = std::getenv("GOW_VU1N_REPLAY_LIVEIN") != nullptr;
        constexpr int kLiveFields = 32 + 16 + 8; // vf0-31, vi0-15, acc q p i r mac clip status
        struct LiveCount { unsigned dead = 0, stale = 0, n = 0; };
        std::vector<LiveCount> live(kLiveFields);
        unsigned liveAllStale = 0, liveMem = 0, liveBoth = 0, livePairs = 0;
        auto prevRec = std::make_unique<Vu1nRecord>();
        auto prevOut = std::make_unique<Vu1nRecord>();
        bool havePrev = false;
        const auto runOnce = [&](const VU1State &st, const uint8_t *data, uint8_t *out) -> uint64_t
        {
            vu = saved;
            vu.m_state = st;
            std::memcpy(out, data, 16384u);
            VU1Interpreter::StepContext c = context;
            c.vuCode = rec->code;
            c.vuData = out;
            c.budgetEnd = vu.m_cycle + rec->maxCycles;
            c.programEnded = false;
            vu.m_activeVuData = out;
            vu.m_activeVuDataSize = 16384u;
            return silentNativeHash(vu, &c);
        };
        const auto setField = [](VU1State &d, const VU1State &src, int f, bool poison)
        {
            const uint32_t pv = 0x7F7FFFF1u; // a large float / odd integer nobody computes
            auto put = [&](void *dst, const void *from, size_t n)
            {
                if (poison)
                    for (size_t i = 0; i < n; i += 4)
                        std::memcpy(static_cast<uint8_t *>(dst) + i, &pv, 4);
                else
                    std::memcpy(dst, from, n);
            };
            if (f < 32)
                put(d.vf[f], src.vf[f], 16);
            else if (f < 48)
                put(&d.vi[f - 32], &src.vi[f - 32], 4);
            else
                switch (f - 48)
                {
                case 0: put(d.acc, src.acc, 16); break;
                case 1: put(&d.q, &src.q, 4); break;
                case 2: put(&d.p, &src.p, 4); break;
                case 3: put(&d.i, &src.i, 4); break;
                case 4: put(&d.r, &src.r, 4); break;
                case 5: put(&d.mac, &src.mac, 4); break;
                case 6: put(&d.clip, &src.clip, 4); break;
                default: put(&d.status, &src.status, 4); break;
                }
        };
        while (std::fread(rec.get(), sizeof(Vu1nRecord), 1, f) == 1)
        {
            ++total;
            for (unsigned rep = 0; rep < std::max(reps, 1u); ++rep)
            {
                vu = saved;
                vu.m_state = rec->state;
                if (rep == 0u)
                    vu.m_native->generation = ~0ull - 1u; // micro memory is not the game's: forget the block cache
                if (rep == 1u && !g_vu1nBlockProf && std::getenv("GOW_VU1N_BLOCKPROF"))
                    g_vu1nBlockProf = new std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>(),
                    g_vu1nEdgeProf = new std::unordered_map<uint32_t, uint64_t>();
                g_vu1nTraceThis = rep == 1u && std::getenv("GOW_VU1N_TRACEREC") &&
                                  total == static_cast<unsigned>(std::atoi(std::getenv("GOW_VU1N_TRACEREC")));
                std::memcpy(scratch->data, rec->data, sizeof(rec->data));
                VU1Interpreter::StepContext c = context;
                c.vuCode = rec->code;
                c.vuData = scratch->data;
                c.budgetEnd = vu.m_cycle + rec->maxCycles;
                c.programEnded = false;
                vu.m_activeVuData = scratch->data;
                vu.m_activeVuDataSize = 16384u;
                g_vu1nReplayNs = 0;
                const uint64_t cycleStart = vu.m_cycle;
                const uint64_t h = silentNativeHash(vu, &c);
                if (rep == 0u)
                    coldCycles = vu.m_cycle - cycleStart;
                else if (h != rec->hash || vu.m_cycle - cycleStart != coldCycles)
                    ++warmDiffer;
                if (rep != 0u)
                {
                    timedNs += g_vu1nReplayNs; // warm block cache only
                    // GOW_VU1N_REPLAY_PROFILE=<file>: per record and warm pass "record startPc vuCycles ns" (which
                    // programs cost what, docs/vu1-hle.md)
                    static FILE *profile = std::getenv("GOW_VU1N_REPLAY_PROFILE") ? std::fopen(std::getenv("GOW_VU1N_REPLAY_PROFILE"), "w") : nullptr;
                    if (profile)
                        std::fprintf(profile, "%u %x %llu %llu\n", total, rec->state.pc,
                                     static_cast<unsigned long long>(vu.m_cycle - cycleStart),
                                     static_cast<unsigned long long>(g_vu1nReplayNs));
                }
                else
                {
                    if (liveIn && rec->state.pc < 0x40u)
                    {
                        auto tmp = std::make_unique<Vu1nRecord>();
                        const uint64_t ref = h;
                        VU1State allStale = rec->state;
                        for (int fi = 0; fi < kLiveFields; ++fi)
                        {
                            VU1State st = rec->state;
                            setField(st, rec->state, fi, true);
                            const bool dead = runOnce(st, rec->data, tmp->data) == ref;
                            live[fi].dead += dead ? 1u : 0u;
                            if (havePrev)
                            {
                                st = rec->state;
                                setField(st, prevRec->state, fi, false);
                                live[fi].stale += runOnce(st, rec->data, tmp->data) == ref ? 1u : 0u;
                                if (!dead)
                                    setField(allStale, prevRec->state, fi, false);
                            }
                            ++live[fi].n;
                        }
                        if (havePrev)
                        {
                            ++livePairs;
                            // every live field from the previous run's start
                            const bool okStale = runOnce(allStale, rec->data, tmp->data) == ref;
                            liveAllStale += okStale ? 1u : 0u;
                            // memory: bytes the previous run wrote, not rewritten since, back to their old value
                            auto spec = std::make_unique<Vu1nRecord>();
                            std::memcpy(spec->data, rec->data, 16384u);
                            for (uint32_t b = 0; b < 16384u; ++b)
                                if (prevOut->data[b] != prevRec->data[b] && rec->data[b] == prevOut->data[b])
                                    spec->data[b] = prevRec->data[b];
                            // reference result (state + data + packets) of the real run
                            auto refOut = std::make_unique<Vu1nRecord>();
                            runOnce(rec->state, rec->data, refOut->data);
                            const VU1State refState = vu.m_state;
                            const std::vector<uint8_t> refPk = g_vuVerifyPackets;
                            runOnce(rec->state, spec->data, tmp->data);
                            // bytes the speculative run left alone still hold the old values: the merge puts the
                            // previous run's results there (what a real implementation would do), then compare
                            unsigned readDiff = 0;
                            for (uint32_t b = 0; b < 16384u; ++b)
                                if (spec->data[b] != rec->data[b] && tmp->data[b] == spec->data[b])
                                    tmp->data[b] = rec->data[b];
                            for (uint32_t b = 0; b < 16384u; ++b)
                                readDiff += tmp->data[b] != refOut->data[b] ? 1u : 0u;
                            const bool okMem = readDiff == 0u && g_vuVerifyPackets == refPk &&
                                               std::memcmp(vu.m_state.vf, refState.vf, sizeof(refState.vf)) == 0 &&
                                               std::memcmp(vu.m_state.vi, refState.vi, sizeof(refState.vi)) == 0 &&
                                               vu.m_state.mac == refState.mac && vu.m_state.clip == refState.clip &&
                                               vu.m_state.pc == refState.pc;
                            static unsigned shown = 0;
                            if (!okMem && shown++ < 8u)
                            {
                                uint32_t first = 0, last = 0, nrev = 0;
                                for (uint32_t b = 0; b < 16384u; ++b)
                                    if (spec->data[b] != rec->data[b])
                                    {
                                        if (!nrev++) first = b;
                                        last = b;
                                    }
                                std::fprintf(stderr, "[vu1n-livein] mem record %u: reverted %u bytes qw %u..%u, data diff %u, packets %s\n",
                                             total, nrev, first / 16u, last / 16u, readDiff,
                                             g_vuVerifyPackets == refPk ? "same" : "differ");
                            }
                            liveMem += okMem ? 1u : 0u;
                            liveBoth += 0u;
                        }
                        // this run's own output, for the next record
                        runOnce(rec->state, rec->data, prevOut->data);
                        std::memcpy(prevRec.get(), rec.get(), sizeof(Vu1nRecord));
                        havePrev = true;
                    }
                    else if (liveIn)
                        havePrev = false;
                    if (h != rec->hash && differ++ < 20u)
                        std::fprintf(stderr, "[vu1n-replay] record %u startPc=%x differs\n", total, rec->state.pc);
                    // GOW_VU1N_REPLAY_DUMP=<file>: every record's result (state, data memory, GIF packets), to compare
                    // two builds (e.g. x86 and ARM) record by record
                    static FILE *dump = std::getenv("GOW_VU1N_REPLAY_DUMP") ? std::fopen(std::getenv("GOW_VU1N_REPLAY_DUMP"), "wb") : nullptr;
                    if (dump)
                    {
                        const uint32_t packetBytes = static_cast<uint32_t>(g_vuVerifyPackets.size());
                        std::fwrite(&vu.m_state, sizeof(VU1State), 1, dump);
                        std::fwrite(scratch->data, sizeof(scratch->data), 1, dump);
                        std::fwrite(&packetBytes, 4, 1, dump);
                        std::fwrite(g_vuVerifyPackets.data(), 1, packetBytes, dump);
                        const uint8_t ended = c.programEnded ? 1u : 0u;
                        std::fwrite(&h, 8, 1, dump);
                        std::fwrite(&rec->hash, 8, 1, dump);
                        std::fwrite(&ended, 1, 1, dump);
                    }
                }
            }
        }
        if (reps > 1u)
            std::fprintf(stderr, "[vu1n-replay] timing: %.3f ms for %u warm runs\n", timedNs / 1e6, total * (reps - 1u));
        if (liveIn)
        {
            static const char *names[8] = {"acc", "q", "p", "i", "r", "mac", "clip", "status"};
            for (int fi = 0; fi < kLiveFields; ++fi)
            {
                char nm[16];
                if (fi < 32)
                    std::snprintf(nm, sizeof(nm), "vf%d", fi);
                else if (fi < 48)
                    std::snprintf(nm, sizeof(nm), "vi%d", fi - 32);
                else
                    std::snprintf(nm, sizeof(nm), "%s", names[fi - 48]);
                std::fprintf(stderr, "[vu1n-livein] %-6s runs=%u dead=%u stale_ok=%u\n", nm, live[fi].n, live[fi].dead, live[fi].stale);
            }
            std::fprintf(stderr, "[vu1n-livein] pairs=%u all_live_stale_ok=%u mem_ok=%u both_ok=%u\n", livePairs, liveAllStale,
                         liveMem, liveBoth);
        }
        if (g_vu1nBlockProf)
        {
            if (FILE *bp = std::fopen(std::getenv("GOW_VU1N_BLOCKPROF"), "w"))
            {
                for (const auto &kv : *g_vu1nBlockProf)
                    std::fprintf(bp, "%x %llu %llu\n", kv.first, static_cast<unsigned long long>(kv.second.first),
                                 static_cast<unsigned long long>(kv.second.second));
                for (const auto &kv : *g_vu1nEdgeProf)
                    std::fprintf(bp, "e %x %x %llu\n", kv.first >> 16, kv.first & 0xFFFFu, static_cast<unsigned long long>(kv.second));
                std::fclose(bp);
            }
            delete g_vu1nBlockProf;
            g_vu1nBlockProf = nullptr;
            delete g_vu1nEdgeProf;
            g_vu1nEdgeProf = nullptr;
        }
        std::fclose(f);
        vu = saved;
        vu.m_native->generation = ~0ull - 1u;
        std::fprintf(stderr, "[vu1n-replay] %u records, %u differ (warm passes: %u differ)\n", total, differ, warmDiffer);
        return;
    }
    static const unsigned limit = std::getenv("GOW_VU1N_RECORD_N") ? static_cast<unsigned>(std::atoi(std::getenv("GOW_VU1N_RECORD_N"))) : 4000u;
    static const unsigned stride = std::getenv("GOW_VU1N_RECORD_STRIDE") ? static_cast<unsigned>(std::atoi(std::getenv("GOW_VU1N_RECORD_STRIDE"))) : 7u;
    static const unsigned long long skip = std::getenv("GOW_VU1N_RECORD_SKIP") ? std::strtoull(std::getenv("GOW_VU1N_RECORD_SKIP"), nullptr, 10) : 0ull;
    static unsigned long long seen = 0;
    static unsigned written = 0;
    if (written >= limit || seen++ < skip || ((seen - skip) % stride) != 0u)
        return;
    static FILE *f = std::fopen(recPath, "wb");
    if (!f)
        return;
    auto rec = std::make_unique<Vu1nRecord>();
    rec->magic = 0x4E315556u;
    rec->maxCycles = maxCycles;
    rec->state = vu.m_state;
    std::memcpy(rec->code, context.vuCode, PS2_VU1_CODE_SIZE);
    std::memcpy(rec->data, context.vuData, 16384u);
    VU1Interpreter saved(vu);
    // same input as the replay: copies of code/data
    VU1Interpreter::StepContext c = context;
    auto code = std::make_unique<Vu1nRecord>();
    std::memcpy(code->code, rec->code, PS2_VU1_CODE_SIZE);
    std::memcpy(code->data, rec->data, 16384u);
    c.vuCode = code->code;
    c.vuData = code->data;
    c.budgetEnd = vu.m_cycle + maxCycles;
    vu.m_native->generation = ~0ull - 1u;
    vu.m_activeVuData = code->data;
    rec->hash = silentNativeHash(vu, &c);
    vu = saved;
    vu.m_native->generation = ~0ull - 1u;
    std::fwrite(rec.get(), sizeof(Vu1nRecord), 1, f);
    if (++written == limit)
        std::fclose(f), std::fprintf(stderr, "[vu1n-record] %u records written\n", written);
    else if (written % 500u == 0u)
        std::fflush(f);
}
