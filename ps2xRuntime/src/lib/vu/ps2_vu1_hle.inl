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
    VU1FlagRing *macRing; // MAC flags (entry reads through it; a kernel pushes its last writers)
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
    if (vi5 <= 0) // the loop would wrap the 16-bit counter: left to the native blocks
        return kDecline;
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

// --- kernel: lit vertices (one per pass, self-loop of 22 pairs) ---------------------------------------------------------
// Region (24 pairs): the loop block and its fall-through (jalr vi15, vi14 into the batch switch, which returns to it).
// Input vertex (4 qwords from vi2 + 1): ST ints, normal (ints, 15 fractional bits), RGBA ints, XYZ ints.
// Output (from vi3 + 1): STQ, -, RGBA (2^23-biased floats: the colour in the mantissa, clamped at 255), XYZ ints.
// Lighting: ACC = vf16..18 * normal (three directional lights), max with vf11 (ambient floor), colour matrix
// vf27/vf28 (+ vf29), times the vertex colour plus the 2^23 bias vf31, min 2^23 + 255.
inline uint32_t kLitSt(Ctx &c, uint32_t)
{
    VU1State &s = c.s;
    uint8_t *d = c.data;
    __m128 f1 = ldv(s, 1), f2 = ldv(s, 2), f3 = ldv(s, 3), f4 = ldv(s, 4), f5 = ldv(s, 5), f7 = ldv(s, 7), f8 = ldv(s, 8),
           f9 = ldv(s, 9), acc = _mm_loadu_ps(s.acc);
    const __m128 m11 = ldv(s, 11), m16 = ldv(s, 16), m17 = ldv(s, 17), m18 = ldv(s, 18), m20 = ldv(s, 20), m21 = ldv(s, 21),
                 m22 = ldv(s, 22), m23 = ldv(s, 23), m27 = ldv(s, 27), m28 = ldv(s, 28), m29 = ldv(s, 29), m31 = ldv(s, 31);
    uint32_t ibits = 0x4B0000FFu;
    float iv;
    std::memcpy(&iv, &ibits, 4);
    const __m128 I = _mm_set1_ps(iv);
    float q = s.q;
    int32_t vi2 = s.vi[2], vi3 = s.vi[3], vi5 = s.vi[5];
    if (vi5 <= 0) // the loop would wrap the 16-bit counter: left to the native blocks
        return kDecline;
    const int32_t vi4 = s.vi[4];
    uint64_t cyc = c.cycle;
    do
    {
        f2 = madd(acc, m22, bc<2>(f1));                                   // p0
        {                                                                  // p1
            const __m128 st = ldq(d, vi2 + 1);
            f8 = bl<3>(f8, itof<12>(f7));
            f7 = bl<3>(f7, st);
        }
        f5 = itof<0>(f5);                                                  // p2
        vi2 = sx(vi2 + vi4);
        acc = bl<7>(acc, _mm_mul_ps(m27, bc<0>(f4)));                     // p3
        f3 = bl<7>(f3, ldq(d, vi2 - 2));
        acc = bl<7>(acc, madd(acc, m28, bc<1>(f4)));                       // p4
        const float qn = rcpw(f2);
        acc = bl<8>(acc, _mm_add_ps(f4, bc<2>(f4)));                       // p5
        f1 = bl<7>(f1, ldq(d, vi2));
        f4 = _mm_add_ps(acc, _mm_mul_ps(m29, _mm_set1_ps(1.0f)));          // p6
        f3 = bl<7>(f3, itof<15>(f3));                                      // p7
        acc = _mm_add_ps(m31, _mm_setzero_ps());                           // p8
        f1 = bl<7>(f1, itof<4>(f1));                                       // p9
        f4 = madd(acc, f4, f5);                                            // p10
        f5 = ldq(d, vi2 - 1);
        q = qn;                                                            // p11
        f2 = bl<7>(f2, _mm_mul_ps(f2, _mm_set1_ps(q)));
        f9 = bl<7>(f9, _mm_mul_ps(f8, _mm_set1_ps(q)));                    // p12
        acc = _mm_mul_ps(m16, bc<0>(f3));                                  // p13
        acc = madd(acc, m17, bc<1>(f3));                                   // p14
        f2 = bl<7>(f2, ftoi<4>(f2));                                       // p15
        vi5 = sx(vi5 - 1);
        f3 = madd(acc, m18, bc<2>(f3));                                    // p16
        f4 = _mm_min_ps(f4, I);                                            // p17
        stq<7>(d, vi3 + 1, f9);
        acc = _mm_add_ps(m23, _mm_setzero_ps());                           // p18
        vi3 = sx(vi3 + vi4);
        acc = madd(acc, m20, bc<0>(f1));                                   // p19
        stq<7>(d, vi3, f2);
        acc = madd(acc, m21, bc<1>(f1));                                   // p20
        stq<15>(d, vi3 - 1, f4);                                           // p21 (the store reads vf4 before the MAX)
        f4 = _mm_max_ps(f3, m11);
        cyc += 22u;
    } while (vi5 != 0);
    stv(s, 1, f1); stv(s, 2, f2); stv(s, 3, f3); stv(s, 4, f4); stv(s, 5, f5); stv(s, 7, f7); stv(s, 8, f8); stv(s, 9, f9);
    _mm_storeu_ps(s.acc, acc);
    s.q = q;
    s.i = iv;
    s.vi[2] = vi2;
    s.vi[3] = vi3;
    s.vi[5] = vi5;
    c.cycle = cyc;
    return c.base + 22u * 8u; // fall-through: jalr vi15, vi14 (batch switch, returns to the loop)
}

// --- kernel: skinned lit vertices (two matrices blended by a per-vertex weight; self-loop of 31 pairs) ----------------
// Region (33 pairs): the loop block and its fall-through (jr vi14 into the batch switch, which returns to the loop).
// Input as lit_st, the weight in the XYZ qword's w (16 bits, doubled, 4 fractional bits). Position M_B * p +
// w * (M_A * p) with M_A = vf12..15, M_B = vf20..23; normal lighting vf24..26 * n blended the same way with
// vf16..18 (the game takes the next vertex's weight there: kept as is).
inline uint32_t kSkinLitSt(Ctx &c, uint32_t)
{
    VU1State &s = c.s;
    uint8_t *d = c.data;
    __m128 f1 = ldv(s, 1), f2 = ldv(s, 2), f3 = ldv(s, 3), f4 = ldv(s, 4), f5 = ldv(s, 5), f6 = ldv(s, 6), f7 = ldv(s, 7),
           f8 = ldv(s, 8), f9 = ldv(s, 9), f10 = ldv(s, 10), acc = _mm_loadu_ps(s.acc);
    const __m128 m11 = ldv(s, 11), m12 = ldv(s, 12), m13 = ldv(s, 13), m14 = ldv(s, 14), m15 = ldv(s, 15), m16 = ldv(s, 16),
                 m17 = ldv(s, 17), m18 = ldv(s, 18), m20 = ldv(s, 20), m21 = ldv(s, 21), m22 = ldv(s, 22), m23 = ldv(s, 23),
                 m24 = ldv(s, 24), m25 = ldv(s, 25), m26 = ldv(s, 26), m27 = ldv(s, 27), m28 = ldv(s, 28), m29 = ldv(s, 29),
                 m31 = ldv(s, 31);
    uint32_t ibits = 0x4B0000FFu;
    float iv;
    std::memcpy(&iv, &ibits, 4);
    const __m128 I = _mm_set1_ps(iv);
    float q = s.q;
    int32_t vi1 = s.vi[1], vi2 = s.vi[2], vi3 = s.vi[3], vi5 = s.vi[5];
    if (vi5 <= 0)
        return kDecline;
    const int32_t vi4 = s.vi[4];
    uint64_t cyc = c.cycle;
    do
    {
        {                                                                  // q0
            const __m128 st = ldq(d, vi2 + 1);
            f9 = bl<3>(f9, itof<12>(f10));
            f10 = bl<3>(f10, st);
        }
        f2 = madd(acc, m14, bc<2>(f1));                                   // q1
        {                                                                  // q2
            const __m128 n = ldq(d, vi2 + 2);
            f7 = _mm_max_ps(f6, m11);
            f3 = bl<7>(f3, n);
        }
        acc = _mm_add_ps(m23, _mm_setzero_ps());                           // q3
        acc = madd(acc, m20, bc<0>(f1));                                   // q4
        {
            uint32_t w;
            std::memcpy(&w, d + qa(vi2 + 4) + 12, 4);
            vi1 = static_cast<int16_t>(w & 0xFFFFu);
        }
        acc = madd(acc, m21, bc<1>(f1));                                   // q5
        acc = madd(acc, m22, bc<2>(f1));                                   // q6
        f2 = madd(acc, f2, bc<3>(f1));                                     // q7
        f3 = bl<7>(f3, itof<15>(f3));                                      // q8
        acc = bl<7>(acc, _mm_mul_ps(m27, bc<0>(f7)));                     // q9
        vi1 = sx(vi1 + vi1);
        acc = bl<7>(acc, madd(acc, m28, bc<1>(f7)));                       // q10
        acc = bl<8>(acc, _mm_add_ps(f7, bc<2>(f7)));                       // q11
        const float qn = rcpw(f2);
        f7 = _mm_add_ps(acc, _mm_mul_ps(m29, _mm_set1_ps(1.0f)));          // q12
        f1 = bl<8>(f1, _mm_castsi128_ps(_mm_set1_epi32(vi1)));
        acc = _mm_mul_ps(m24, bc<0>(f3));                                  // q13
        f1 = bl<7>(f1, ldq(d, vi2 + 4));
        acc = madd(acc, m25, bc<1>(f3));                                   // q14
        f6 = madd(acc, m26, bc<2>(f3));                                    // q15
        f4 = ldq(d, vi2 + 3);
        acc = _mm_add_ps(m31, _mm_setzero_ps());                           // q16
        f7 = madd(acc, f8, f7);                                            // q17
        f1 = itof<4>(f1);                                                  // q18
        q = qn;                                                            // q19
        f5 = bl<7>(f5, _mm_mul_ps(f9, _mm_set1_ps(q)));
        vi2 = sx(vi2 + vi4);
        f2 = bl<7>(f2, _mm_mul_ps(f2, _mm_set1_ps(q)));                    // q20
        f7 = _mm_min_ps(f7, I);                                            // q21
        vi1 = vi3;
        f8 = itof<0>(f4);                                                  // q22
        acc = _mm_mul_ps(m16, bc<0>(f3));                                  // q23
        f2 = bl<7>(f2, ftoi<4>(f2));                                       // q24
        stq<7>(d, vi1 + 1, f5);
        acc = madd(acc, m17, bc<1>(f3));                                   // q25
        acc = madd(acc, m18, bc<2>(f3));                                   // q26
        stq<15>(d, vi1 + 3, f7);
        f6 = madd(acc, f6, bc<3>(f1));                                     // q27
        vi5 = sx(vi5 - 1);
        acc = _mm_add_ps(m15, _mm_setzero_ps());                           // q28
        stq<7>(d, vi1 + 4, f2);
        acc = madd(acc, m12, bc<0>(f1));                                   // q29
        acc = madd(acc, m13, bc<1>(f1));                                   // q30
        vi3 = sx(vi3 + vi4);
        cyc += 31u;
    } while (vi5 != 0);
    stv(s, 1, f1); stv(s, 2, f2); stv(s, 3, f3); stv(s, 4, f4); stv(s, 5, f5); stv(s, 6, f6); stv(s, 7, f7);
    stv(s, 8, f8); stv(s, 9, f9); stv(s, 10, f10);
    _mm_storeu_ps(s.acc, acc);
    s.q = q;
    s.i = iv;
    s.vi[1] = vi1;
    s.vi[2] = vi2;
    s.vi[3] = vi3;
    s.vi[5] = vi5;
    c.cycle = cyc;
    return c.base + 31u * 8u; // fall-through: jr vi14 (batch switch, returns to the loop)
}

// --- kernel: position-only strips with winding culling (self-loop of 13 pairs; the shadow volumes and others) ---------
// Region (19 pairs): prologue (vi6 = 0x8080, I = 2^20, vf7.z = 2^20), the loop block, its fall-through (3 pairs:
// vi8 = 3, jalr into the batch switch, which returns to the prologue).
// Input vertex (vi4 qwords from vi2): XYZ ints (4 fractional bits) with a flag word in w (bit 15: no kick, bit 0: the
// expected winding). Output (vi3): XYZ ints (screen, z = 2^20 - z/w) and the ADC word in w: 0 (draw) when the MAC
// flags of the 2D cross product of the last two edges equal the expected winding (0x80: negative, 0: positive), else
// 0xFFFF. The FMEQ reads the cross product of the pass before (4 cycles back across the loop branch); Swap: the
// module of the other winding (the operands of MULAy/MSUBy exchanged).
template <bool Swap>
inline uint32_t kCull(Ctx &c, uint32_t)
{
    VU1State &s = c.s;
    uint8_t *d = c.data;
    __m128 f1 = ldv(s, 1), f2 = ldv(s, 2), f3 = ldv(s, 3), f4 = ldv(s, 4), f5 = ldv(s, 5), f11 = ldv(s, 11), f27 = ldv(s, 27),
           f28 = ldv(s, 28), acc = _mm_loadu_ps(s.acc);
    const __m128 m7 = ldv(s, 7), m20 = ldv(s, 20), m21 = ldv(s, 21), m22 = ldv(s, 22), m23 = ldv(s, 23);
    float q = s.q;
    int32_t vi1 = s.vi[1], vi2 = s.vi[2], vi3 = s.vi[3], vi5 = s.vi[5], vi7 = s.vi[7], vi8 = s.vi[8];
    if (vi5 <= 0)
        return kDecline;
    const int32_t vi4 = s.vi[4], vi6 = s.vi[6];
    uint64_t cyc = c.cycle;
    // the MAC flags the first pass's FMEQ sees (r2, idle timing): written before the loop
    uint32_t mac = VU1NativeAccess::ringAt(*c.macRing, cyc + 2u);
    uint32_t mac11 = 0u, mac12 = 0u;
    do
    {
        acc = _mm_add_ps(m23, _mm_setzero_ps());                           // r0
        vi2 = sx(vi2 + vi4);
        acc = madd(acc, m20, bc<0>(f2));                                   // r1
        f1 = ldq(d, vi2);
        f5 = bl<7>(f5, _mm_mul_ps(f3, _mm_set1_ps(q)));                    // r2
        vi8 = (mac & 0xFFFFu) == static_cast<uint32_t>(vi8 & 0xFFFF) ? 1 : 0;
        acc = madd(acc, m21, bc<1>(f2));                                   // r3
        vi8 = sx(vi8 - 1);
        f3 = madd(acc, m22, bc<2>(f2));                                    // r4
        vi3 = sx(vi3 + vi4);
        f28 = bl<3>(f28, _mm_add_ps(f27, _mm_setzero_ps()));               // r5
        {
            const uint32_t w = static_cast<uint16_t>(vi8);
            std::memcpy(d + qa(vi3) + 12, &w, 4);
        }
        f5 = bl<4>(f5, _mm_sub_ps(m7, f5));                                // r6
        vi5 = sx(vi5 - 1);
        f27 = bl<3>(f27, _mm_sub_ps(f5, f11));                             // r7
        stq<7>(d, vi3, f4);
        f2 = bl<7>(f2, itof<4>(f1));                                       // r8
        const float qn = rcpw(f3);
        f4 = bl<7>(f4, ftoi<4>(f5));                                       // r9
        vi8 = vi7 & vi6;
        __m128 r;                                                          // r10, r11
        if constexpr (!Swap)
        {
            acc = bl<1>(acc, _mm_mul_ps(f28, bc<1>(f27)));
            r = _mm_sub_ps(acc, _mm_mul_ps(f27, bc<1>(f28)));
        }
        else
        {
            acc = bl<1>(acc, _mm_mul_ps(f27, bc<1>(f28)));
            r = _mm_sub_ps(acc, _mm_mul_ps(f28, bc<1>(f27)));
        }
        vi7 = sx(vi1 + 127);
        mac11 = vu1n::macValRt(r, 1u);
        f11 = bl<3>(f11, _mm_add_ps(f5, _mm_setzero_ps()));               // r12
        mac12 = vu1n::macValRt(f11, 3u);
        vi1 = static_cast<int16_t>(static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(_mm_shuffle_ps(f1, f1, _MM_SHUFFLE(3, 3, 3, 3))))) & 0xFFFFu);
        q = qn; // the DIV result is there 7 cycles later (r2 of the next pass)
        mac = mac11;
        cyc += 13u;
    } while (vi5 != 0);
    // the last two MAC writers for readers after the loop (msuby in r11, addx in r12)
    VU1NativeAccess::ringPush(*c.macRing, cyc - 2u, mac11);
    VU1NativeAccess::ringPush(*c.macRing, cyc - 1u, mac12);
    stv(s, 1, f1); stv(s, 2, f2); stv(s, 3, f3); stv(s, 4, f4); stv(s, 5, f5); stv(s, 11, f11); stv(s, 27, f27);
    stv(s, 28, f28);
    _mm_storeu_ps(s.acc, acc);
    s.q = q;
    s.vi[1] = vi1;
    s.vi[2] = vi2;
    s.vi[3] = vi3;
    s.vi[5] = vi5;
    s.vi[7] = vi7;
    s.vi[8] = vi8;
    c.cycle = cyc;
    return c.base + 0x18u + 13u * 8u; // fall-through: vi8 = 3, jalr vi15, vi14 (batch switch, returns to the prologue)
}

// --- kernel: skinned lit vertices without the perspective stage (self-loop of 28 pairs, the title menu's main loop) ----
// Region (28 pairs): the loop block (its delay slot is the last pair); the fall-through is `jr vi14` into the batch switch.
// Same inputs as skin_lit_st (ST, normal, RGBA, XYZ + weight in w, doubled) but no perspective divide: the output is
// STQ.xy, the position xyz as floats (M_B * p + w * (M_A * p)) and the lit colour (clamped at 255.0).
inline uint32_t kSkinLit28(Ctx &c, uint32_t)
{
    VU1State &s = c.s;
    uint8_t *d = c.data;
    __m128 f1 = ldv(s, 1), f2 = ldv(s, 2), f3 = ldv(s, 3), f4 = ldv(s, 4), f6 = ldv(s, 6), f7 = ldv(s, 7), f8 = ldv(s, 8),
           f9 = ldv(s, 9), f10 = ldv(s, 10), acc = _mm_loadu_ps(s.acc);
    const __m128 m11 = ldv(s, 11), m12 = ldv(s, 12), m13 = ldv(s, 13), m14 = ldv(s, 14), m15 = ldv(s, 15), m16 = ldv(s, 16),
                 m17 = ldv(s, 17), m18 = ldv(s, 18), m20 = ldv(s, 20), m21 = ldv(s, 21), m22 = ldv(s, 22), m23 = ldv(s, 23),
                 m24 = ldv(s, 24), m25 = ldv(s, 25), m26 = ldv(s, 26), m27 = ldv(s, 27), m28 = ldv(s, 28), m29 = ldv(s, 29),
                 m31 = ldv(s, 31);
    const uint32_t ibits = 0x437F8000u; // 255.0
    float iv;
    std::memcpy(&iv, &ibits, 4);
    const __m128 I = _mm_set1_ps(iv);
    const __m128 z = _mm_setzero_ps();
    int32_t vi1 = s.vi[1], vi2 = s.vi[2], vi3 = s.vi[3], vi5 = s.vi[5];
    if (vi5 <= 0) // the loop would wrap the 16-bit counter: left to the native blocks
        return kDecline;
    const int32_t vi4 = s.vi[4];
    uint64_t cyc = c.cycle;
    do
    {
        f9 = bl<3>(f9, itof<12>(f10));                                     // p0
        f10 = bl<3>(f10, ldq(d, vi2 + 1));
        f2 = bl<7>(f2, madd(acc, m14, bc<2>(f1)));                         // p1
        f7 = _mm_max_ps(f6, m11);                                          // p2
        f3 = bl<7>(f3, ldq(d, vi2 + 2));
        acc = bl<7>(acc, _mm_add_ps(m23, z));                              // p3
        acc = bl<7>(acc, madd(acc, m20, bc<0>(f1)));                       // p4
        {
            uint32_t w;
            std::memcpy(&w, d + qa(vi2 + 4) + 12, 4);
            vi1 = static_cast<int16_t>(w & 0xFFFFu);
        }
        acc = bl<7>(acc, madd(acc, m21, bc<1>(f1)));                       // p5
        acc = bl<7>(acc, madd(acc, m22, bc<2>(f1)));                       // p6
        f2 = madd(acc, f2, bc<3>(f1));                                     // p7
        f3 = bl<7>(f3, itof<15>(f3));                                      // p8
        acc = bl<7>(acc, _mm_mul_ps(m27, bc<0>(f7)));                      // p9
        vi1 = sx(vi1 + vi1);
        acc = bl<7>(acc, madd(acc, m28, bc<1>(f7)));                       // p10
        acc = bl<8>(acc, _mm_add_ps(f7, bc<2>(f7)));                       // p11
        f7 = _mm_add_ps(acc, _mm_mul_ps(m29, _mm_set1_ps(1.0f)));          // p12
        f1 = bl<8>(f1, _mm_castsi128_ps(_mm_set1_epi32(vi1)));
        acc = _mm_mul_ps(m24, bc<0>(f3));                                  // p13
        f1 = bl<7>(f1, ldq(d, vi2 + 4));
        acc = madd(acc, m25, bc<1>(f3));                                   // p14
        f6 = madd(acc, m26, bc<2>(f3));                                    // p15
        f4 = ldq(d, vi2 + 3);
        acc = _mm_add_ps(m31, z);                                          // p16
        f7 = madd(acc, f8, f7);                                            // p17
        f1 = itof<4>(f1);                                                  // p18
        f8 = itof<0>(f4);                                                  // p19
        vi2 = sx(vi2 + vi4);
        acc = _mm_mul_ps(m16, bc<0>(f3));                                  // p20
        f7 = _mm_min_ps(f7, I);                                            // p21
        stq<7>(d, vi3 + 1, f9);
        acc = madd(acc, m17, bc<1>(f3));                                   // p22
        acc = madd(acc, m18, bc<2>(f3));                                   // p23
        stq<7>(d, vi3 + 4, f2);
        f6 = madd(acc, f6, bc<3>(f1));                                     // p24
        vi5 = sx(vi5 - 1);
        acc = bl<7>(acc, _mm_add_ps(m15, z));                              // p25
        stq<15>(d, vi3 + 3, f7);
        acc = bl<7>(acc, madd(acc, m12, bc<0>(f1)));                       // p26
        acc = bl<7>(acc, madd(acc, m13, bc<1>(f1)));                       // p27
        vi3 = sx(vi3 + vi4);
        cyc += 28u;
    } while (vi5 != 0);
    stv(s, 1, f1); stv(s, 2, f2); stv(s, 3, f3); stv(s, 4, f4); stv(s, 6, f6); stv(s, 7, f7); stv(s, 8, f8); stv(s, 9, f9);
    stv(s, 10, f10);
    _mm_storeu_ps(s.acc, acc);
    s.i = iv;
    s.vi[1] = vi1;
    s.vi[2] = vi2;
    s.vi[3] = vi3;
    s.vi[5] = vi5;
    c.cycle = cyc;
    return c.base + 28u * 8u; // fall-through: jr vi14 (batch switch)
}

// --- kernel: unlit vertices with ST, plain transform (self-loop of 11 pairs, Q from the previous pass) -----------------
// Region (11 pairs). Input: ST ints (xy, 12 fractional bits) at vi2 + 1, XYZ ints at vi2, a qword at vi2 - 1;
// output at vi3: ST*Q, position*Q, a converted qword; the DIV is vf0w / vf1z of this pass, used by the next one.
inline uint32_t kUnlit11(Ctx &c, uint32_t)
{
    VU1State &s = c.s;
    uint8_t *d = c.data;
    __m128 f1 = ldv(s, 1), f2 = ldv(s, 2), f3 = ldv(s, 3), f4 = ldv(s, 4), f5 = ldv(s, 5), f6 = ldv(s, 6), f7 = ldv(s, 7),
           f8 = ldv(s, 8), f9 = ldv(s, 9), f10 = ldv(s, 10), acc = _mm_loadu_ps(s.acc);
    const __m128 m20 = ldv(s, 20), m21 = ldv(s, 21), m22 = ldv(s, 22), m23 = ldv(s, 23);
    const __m128 z = _mm_setzero_ps();
    float q = s.q;
    int32_t vi2 = s.vi[2], vi3 = s.vi[3], vi5 = s.vi[5];
    if (vi5 <= 0)
        return kDecline;
    const int32_t vi4 = s.vi[4];
    uint64_t cyc = c.cycle;
    do
    {
        f8 = bl<3>(f8, _mm_add_ps(f7, z));                                 // p0
        f7 = bl<3>(f7, ldq(d, vi2 + 1));
        acc = bl<7>(acc, _mm_mul_ps(m21, bc<2>(f1)));                      // p1
        vi2 = sx(vi2 + vi4);
        f2 = bl<7>(f2, _mm_sub_ps(acc, _mm_mul_ps(m20, f2)));             // p2
        f1 = bl<7>(f1, ldq(d, vi2));
        f6 = _mm_add_ps(f5, z);                                            // p3
        vi5 = sx(vi5 - 1);
        f4 = bl<7>(f4, ftoi<4>(f3));                                       // p4
        stq<7>(d, vi3 + 1, f9);
        f9 = bl<7>(f9, _mm_mul_ps(f8, _mm_set1_ps(q)));                    // p5
        vi3 = sx(vi3 + vi4);
        f3 = bl<7>(f3, _mm_mul_ps(f2, _mm_set1_ps(q)));                    // p6
        f5 = ldq(d, vi2 - 1);
        acc = bl<4>(acc, _mm_add_ps(z, bc<3>(m22)));                       // p7
        {
            const float w = vu1n::lane<2>(f1);
            q = w == 0.0f ? (std::signbit(w) ? -FLT_MAX : FLT_MAX) : 1.0f / w;
        }
        acc = bl<3>(acc, _mm_sub_ps(z, z));                                // p8
        stq<7>(d, vi3, f4);
        f2 = bl<7>(f2, madd(acc, f1, m23));                                // p9
        {                                                                  // p10
            const __m128 old = f10;
            f10 = ftoi<0>(f6);
            stq<15>(d, vi3 - 1, old);
        }
        cyc += 11u;
    } while (vi5 != 0);
    stv(s, 1, f1); stv(s, 2, f2); stv(s, 3, f3); stv(s, 4, f4); stv(s, 5, f5); stv(s, 6, f6); stv(s, 7, f7); stv(s, 8, f8);
    stv(s, 9, f9); stv(s, 10, f10);
    _mm_storeu_ps(s.acc, acc);
    s.q = q;
    s.vi[2] = vi2;
    s.vi[3] = vi3;
    s.vi[5] = vi5;
    c.cycle = cyc;
    return c.base + 11u * 8u; // fall-through: jr vi14 (batch switch)
}

inline constexpr Kernel kKernels[] = {
    {"unlit_st", 0x2e8bdf18116c01cdull, 20u, {0x00u, 0x58u}, kUnlitSt},
    {"lit_st", 0x7c187fbcd9b8ba29ull, 24u, {0x00u, ~0u}, kLitSt},
    {"skin_lit_st", 0xb8138c8401f6f173ull, 33u, {0x00u, ~0u}, kSkinLitSt},
    {"cull", 0xe0b2f1a1fa9e29fbull, 19u, {0x18u, ~0u}, kCull<false>},
    {"cull_swap", 0x4e6af90f38ca205bull, 19u, {0x18u, ~0u}, kCull<true>},
    {"skin_lit28", 0x5181a0b08a6d0989ull, 28u, {0x00u, ~0u}, kSkinLit28},
    {"unlit11", 0xa2eaa96b750883ceull, 11u, {0x00u, ~0u}, kUnlit11},
};
constexpr uint32_t kKernelCount = sizeof(kKernels) / sizeof(kKernels[0]);
// the reach of a match around its pc (VU1NativeAccess::codeChanged resets only the pcs a code change can affect)
constexpr uint32_t kMaxRegionPairs = [] {
    uint32_t m = 0;
    for (const Kernel &k : kKernels)
        m = k.regionPairs > m ? k.regionPairs : m;
    return m;
}();
constexpr uint32_t kMaxEntryPairs = [] {
    uint32_t m = 0;
    for (const Kernel &k : kKernels)
        for (const uint32_t off : k.entryOff)
            m = off != ~0u && off / 8u > m ? off / 8u : m;
    return m;
}();

inline uint64_t fnv(const uint8_t *p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// The match result at pc depends only on pc and the pairs [pc - kMaxEntryPairs, pc + kMaxRegionPairs): a 64-bit hash of
// them, eight bytes at a time (VU1HleState::byKey; ~10x cheaper than the byte-wise FNV of every kernel's region)
inline uint64_t windowKey(const uint8_t *code, uint32_t codeSize, uint32_t pc)
{
    const uint32_t lo = pc >= kMaxEntryPairs * 8u ? pc - kMaxEntryPairs * 8u : 0u;
    const uint32_t hi = std::min<uint32_t>(codeSize, pc + kMaxRegionPairs * 8u);
    uint64_t h = 0x9E3779B97F4A7C15ull ^ pc;
    for (uint32_t a = lo; a + 8u <= hi; a += 8u)
    {
        uint64_t w;
        std::memcpy(&w, code + a, 8u);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
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
    std::atomic<uint64_t> ns[kKernelCount > 0 ? kKernelCount : 1]; // wall time inside the kernel (GOW_PERF_DIAG)
};
inline uint64_t nowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
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
        n += std::snprintf(buf + n, sizeof(buf) - n, " %s=%llu/%llu/%.0fms", kKernels[k].name,
                           static_cast<unsigned long long>(stats().calls[k].exchange(0)),
                           static_cast<unsigned long long>(stats().cycles[k].exchange(0)),
                           stats().ns[k].exchange(0) / 1e6);
    std::fprintf(stderr, "%s\n", buf);
}
} // namespace vu1hle

// Per VU1 interpreter: which pcs start a kernel (kernel index + 1 and entry in the low bit pair, 0 none), checked once
// per slot and code change near it. The game streams the same modules into its slots again and again: `byKey` keeps the
// result per windowKey (pc + every pair a match can read) so code seen before is not matched again; a remembered kernel
// is still confirmed by its region hash before it runs (a 64-bit key collision then only costs speed).
struct VU1HleState
{
    uint8_t tried[2048] = {};
    uint8_t kernel[2048] = {}; // (index + 1) << 1 | entry
    uint8_t stop[2048];        // !tried || kernel (NativeState::hleStop: chained blocks stop there)
    // byKey: open-addressed (key 0 = free; a key of 0 is stored as 1, a collision that only costs speed)
    std::vector<uint64_t> keys = std::vector<uint64_t>(4096u);
    std::vector<uint8_t> vals = std::vector<uint8_t>(4096u);
    uint32_t count = 0;
    VU1HleState() { std::memset(stop, 1, sizeof(stop)); }
    // the value slot of key (inserted with value 0xFF = unknown when new)
    uint8_t &byKey(uint64_t key)
    {
        key = key ? key : 1u;
        for (;;)
        {
            const size_t mask = keys.size() - 1u;
            for (size_t h = static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;; h = (h + 1u) & mask)
            {
                if (keys[h] == key)
                    return vals[h];
                if (keys[h] != 0u)
                    continue;
                if ((count + 1u) * 2u > keys.size())
                    break;
                ++count;
                keys[h] = key;
                vals[h] = 0xFFu;
                return vals[h];
            }
            std::vector<uint64_t> ok(keys.size() * 2u);
            std::vector<uint8_t> ov(keys.size() * 2u);
            ok.swap(keys);
            ov.swap(vals);
            const size_t mask2 = keys.size() - 1u;
            for (size_t i = 0; i < ok.size(); ++i)
                if (ok[i] != 0u)
                    for (size_t h = static_cast<size_t>((ok[i] * 0x9E3779B97F4A7C15ull) >> 40) & mask2;; h = (h + 1u) & mask2)
                        if (keys[h] == 0u)
                        {
                            keys[h] = ok[i];
                            vals[h] = ov[i];
                            break;
                        }
        }
    }
    void reset(uint32_t i) { tried[i] = kernel[i] = 0u, stop[i] = 1u; }
};
