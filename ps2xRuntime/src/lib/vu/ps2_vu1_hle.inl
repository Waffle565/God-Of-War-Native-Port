// GOW-Port: GOW_VU1_HLE - the hot vertex loops of God of War's VU1 microprogram as plain float C++ (docs/vu1-gpu.md).
// Included by ps2_vu1_native.inl before runNative.
//
// The game's main VU1 program spends ~93 % of its cycles in a few software-pipelined vertex loops (self-loops and
// two-block cycles: transform, perspective divide, ST*Q, lighting, culling). The native blocks run them with the exact
// cycle model (history entry, flag rings, FDIV timing per pass) and exact float semantics. A kernel here runs all the
// remaining passes of such a loop at once in plain float (round toward zero, as the native blocks), without the cycle
// model: same results except for float rounding details (not bit-exact, compared with tolerances, tools in
// docs/vu1-gpu.md), then hands back at the loop's fall-through pc with the registers as the loop leaves them.
//
// Matching: a kernel names its loop region (pairs from the region start) by FNV-1a 64 of its bytes, and the entry
// blocks inside it by their offset. The modules are position independent and the game loads them at different addresses,
// so a dispatch pc is checked once per code generation (VU1HleState::tried) against every kernel's region.
#include <atomic>

namespace vu1hle
{
constexpr uint32_t kDecline = ~0u;

struct Ctx
{
    VU1State &s;
    uint8_t *data;   // VU1 data memory (16 KB)
    uint64_t &cycle; // VU cycle counter (advanced by the passes run)
    uint64_t budgetEnd;
    uint32_t base; // region start pc
};

using KernelFn = uint32_t (*)(Ctx &c, uint32_t entry);

struct Kernel
{
    const char *name;
    uint64_t regionHash;
    uint32_t regionPairs;
    uint32_t entryOff[2]; // byte offsets of the entry blocks (second: ~0u when only one)
    KernelFn fn;
};

// --- plain float helpers (SSE; sse2neon on the Switch) -----------------------------------------------------------------
inline uint32_t qa(int32_t qw) { return (static_cast<uint32_t>(qw) & 0x3FFu) * 16u; }
inline __m128 ldq(const uint8_t *d, int32_t qw) { return _mm_loadu_ps(reinterpret_cast<const float *>(d + qa(qw))); }
template <int M> // SSE blend mask: x = 1, y = 2, z = 4, w = 8
inline void stq(uint8_t *d, int32_t qw, __m128 v)
{
    float *p = reinterpret_cast<float *>(d + qa(qw));
    if constexpr (M == 15)
        _mm_storeu_ps(p, v);
    else
        _mm_storeu_ps(p, _mm_blend_ps(_mm_loadu_ps(p), v, M));
}
template <int M>
inline __m128 bl(__m128 a, __m128 b) { return _mm_blend_ps(a, b, M); } // lanes M from b
template <int C>
inline __m128 bc(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(C, C, C, C)); }
inline __m128 madd(__m128 acc, __m128 a, __m128 b) { return _mm_add_ps(acc, _mm_mul_ps(a, b)); }
template <int Shift>
inline __m128 itof(__m128 raw) { return vu1n::itofv<Shift>(raw); }
template <int Shift>
inline __m128 ftoi(__m128 v) { return vu1n::ftoiv<Shift>(v); }
inline int32_t sx(int32_t v) { return static_cast<int16_t>(v); }
inline float rcpw(__m128 v) // DIV Q, vf0w, v.w (1 / w; w == 0: +-FLT_MAX)
{
    const float w = vu1n::lane<3>(v);
    return w == 0.0f ? (std::signbit(w) ? -FLT_MAX : FLT_MAX) : 1.0f / w;
}
inline __m128 ldv(const VU1State &s, int r) { return _mm_loadu_ps(s.vf[r]); }
inline void stv(VU1State &s, int r, __m128 v) { _mm_storeu_ps(s.vf[r], v); }

// --- kernel: unlit vertices, ST/Q + transform + perspective (two-block cycle, two vertices per cycle) ------------------
// Region (20 pairs): block A (9 pairs), A's fall-through (jalr into the batch switch, 2 pairs), block B (9 pairs).
// Input vertex (vi4 qwords from vi2 + 1): ST ints (xy, 12 fractional bits), RGBA, XYZ ints (4 fractional bits).
// Output (from vi3 + 1): STQ (xyz), -, XYZ ints (screen, 4 fractional bits); w lanes and RGBA are left alone.
// Both blocks: lq ST -> vf7 (converted one block later), lq XYZ -> vf1, itof4 -> vf2, ACC chain M(vf20..23) * vf2,
// DIV Q = 1 / w in the delay slot, mulq one block later, ftoi4 and the stores one block after that.
inline uint32_t kUnlitSt(Ctx &c, uint32_t entry)
{
    VU1State &s = c.s;
    uint8_t *d = c.data;
    __m128 f1 = ldv(s, 1), f2 = ldv(s, 2), f3 = ldv(s, 3), f4 = ldv(s, 4), f5 = ldv(s, 5), f6 = ldv(s, 6), f7 = ldv(s, 7),
           f8 = ldv(s, 8), f9 = ldv(s, 9), f10 = ldv(s, 10), acc = _mm_loadu_ps(s.acc);
    const __m128 m20 = ldv(s, 20), m21 = ldv(s, 21), m22 = ldv(s, 22), m23 = ldv(s, 23);
    float q = s.q;
    int32_t vi2 = s.vi[2], vi3 = s.vi[3], vi5 = s.vi[5];
    const int32_t vi4 = s.vi[4];
    uint64_t cyc = c.cycle;
    uint32_t next;
    bool atB = entry == 1u;
    static int dbg = std::getenv("GOW_VU1_HLE_DBG") ? std::atoi(std::getenv("GOW_VU1_HLE_DBG")) : 0;
    if (dbg > 0)
    {
        --dbg;
        std::fprintf(stderr, "[vu1-hle-dbg] unlit_st entry=%u base=%x vi2=%d vi3=%d vi4=%d vi5=%d pc=%x\n", entry, c.base, vi2, vi3, vi4, vi5, s.pc);
    }
    for (;;)
    {
        if (!atB)
        {
            // A: f9/f4 set, Q from B's DIV (vf3)
            const __m128 st = ldq(d, vi2 + 1);
            f9 = bl<3>(f9, itof<12>(f7));
            f7 = bl<3>(f7, st);
            f6 = bl<7>(f6, ftoi<4>(f5));
            vi2 = sx(vi2 + vi4);
            acc = madd(acc, m20, bc<0>(f2));
            f1 = bl<7>(f1, ldq(d, vi2));
            acc = madd(acc, m21, bc<1>(f2));
            vi5 = sx(vi5 - 1);
            f4 = madd(acc, m22, bc<2>(f2));
            stq<7>(d, vi3 + 1, f10);
            acc = _mm_add_ps(m23, _mm_setzero_ps());
            vi3 = sx(vi3 + vi4);
            f5 = bl<7>(f5, _mm_mul_ps(f3, _mm_set1_ps(q)));
            stq<7>(d, vi3, f6);
            f2 = bl<7>(f2, itof<4>(f1));
            f10 = bl<7>(f10, _mm_mul_ps(f8, _mm_set1_ps(q)));
            q = rcpw(f4);
            cyc += 9u;
            if (vi5 == 0)
            {
                next = c.base + 0x48u; // A's fall-through: jalr vi15, vi14 (batch switch, returns to B)
                break;
            }
        }
        atB = false;
        // B: f8/f3 set, Q from A's DIV (vf4)
        {
            const __m128 st = ldq(d, vi2 + 1);
            f8 = bl<3>(f8, itof<12>(f7));
            f7 = bl<3>(f7, st);
            f6 = bl<7>(f6, ftoi<4>(f5));
            vi2 = sx(vi2 + vi4);
            acc = madd(acc, m20, bc<0>(f2));
            f1 = bl<7>(f1, ldq(d, vi2));
            acc = madd(acc, m21, bc<1>(f2));
            vi5 = sx(vi5 - 1);
            f3 = madd(acc, m22, bc<2>(f2));
            stq<7>(d, vi3 + 1, f10);
            acc = _mm_add_ps(m23, _mm_setzero_ps());
            vi3 = sx(vi3 + vi4);
            f5 = bl<7>(f5, _mm_mul_ps(f4, _mm_set1_ps(q)));
            stq<7>(d, vi3, f6);
            f2 = bl<7>(f2, itof<4>(f1));
            f10 = bl<7>(f10, _mm_mul_ps(f9, _mm_set1_ps(q)));
            q = rcpw(f3);
            cyc += 9u;
            if (vi5 == 0)
            {
                next = c.base + 0xA0u; // B's fall-through: jalr vi15, vi14 (returns to A)
                break;
            }
        }
    }
    stv(s, 1, f1); stv(s, 2, f2); stv(s, 3, f3); stv(s, 4, f4); stv(s, 5, f5); stv(s, 6, f6); stv(s, 7, f7);
    stv(s, 8, f8); stv(s, 9, f9); stv(s, 10, f10);
    _mm_storeu_ps(s.acc, acc);
    s.q = q;
    s.vi[2] = vi2;
    s.vi[3] = vi3;
    s.vi[5] = vi5;
    c.cycle = cyc;
    return next;
}

inline const Kernel kKernels[] = {
    {"unlit_st", 0x2e8bdf18116c01cdull, 20u, {0x00u, 0x58u}, kUnlitSt},
};
constexpr uint32_t kKernelCount = sizeof(kKernels) / sizeof(kKernels[0]);

inline uint64_t fnv(const uint8_t *p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// GOW_VU1_HLE=1 turns the kernels on; GOW_VU1_HLE_MASK=<hex>: only the kernels whose bit is set (A/B per kernel)
inline bool enabled()
{
    static const bool on = std::getenv("GOW_VU1_HLE") && std::atoi(std::getenv("GOW_VU1_HLE")) != 0;
    return on;
}
inline uint32_t kernelMask()
{
    static const uint32_t m = std::getenv("GOW_VU1_HLE_MASK") ? static_cast<uint32_t>(std::strtoul(std::getenv("GOW_VU1_HLE_MASK"), nullptr, 16)) : ~0u;
    return m;
}

struct Stats
{
    std::atomic<uint64_t> calls[kKernelCount > 0 ? kKernelCount : 1];
    std::atomic<uint64_t> cycles[kKernelCount > 0 ? kKernelCount : 1];
};
inline Stats &stats()
{
    static Stats st{};
    return st;
}
// "[vu1-hle] window=5s <kernel>=<calls>/<VU cycles> ..." every 5 s with GOW_PERF_DIAG or GOW_VU1_HLE_STATS (force: now)
inline void report(bool force = false)
{
    static const bool want = std::getenv("GOW_PERF_DIAG") || std::getenv("GOW_VU1_HLE_STATS");
    if (!want && !force)
        return;
    static std::atomic<int64_t> lastMs{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t prev = lastMs.load(std::memory_order_relaxed);
    if (prev == 0 && !force)
    {
        lastMs.store(now, std::memory_order_relaxed);
        return;
    }
    if (!force && now - prev < 5000)
        return;
    if (!lastMs.compare_exchange_strong(prev, now))
        return;
    if (prev == 0)
        prev = now;
    char buf[1024];
    int n = std::snprintf(buf, sizeof(buf), "[vu1-hle] window=%.1fs", (now - prev) / 1000.0);
    for (uint32_t k = 0; k < kKernelCount && n < static_cast<int>(sizeof(buf)) - 64; ++k)
        n += std::snprintf(buf + n, sizeof(buf) - n, " %s=%llu/%llu", kKernels[k].name,
                           static_cast<unsigned long long>(stats().calls[k].exchange(0)),
                           static_cast<unsigned long long>(stats().cycles[k].exchange(0)));
    std::fprintf(stderr, "%s\n", buf);
}
} // namespace vu1hle

// Per VU1 interpreter: which pcs start a kernel (kernel index + 1 and entry in the low bit pair, 0 none), checked once
// per slot and code generation.
struct VU1HleState
{
    uint8_t tried[2048] = {};
    uint8_t kernel[2048] = {}; // (index + 1) << 1 | entry
};
