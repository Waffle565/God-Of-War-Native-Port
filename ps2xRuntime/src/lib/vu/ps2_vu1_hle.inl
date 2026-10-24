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
    VU1FlagRing *clipRing; // clip flags (likewise)
    uint32_t *workingClip; // the clip flag chain (VU1Interpreter::m_workingClip)
};

using KernelFn = uint32_t (*)(Ctx &c, uint32_t entry);

struct Kernel
{
    const char *name;
    uint64_t regionHash;
    uint32_t regionPairs;
    uint32_t entryOff[2]; // byte offsets of the entry blocks (second: ~0u when only one)
    KernelFn fn;
    bool keepsQ = false; // runs no DIV/EFU: an FDIV/EFU entry in flight stays valid (else the kernel's Q replaces it)
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
    // f1, f2, f5, f6, f7, f10 keep their other lanes through the loop and every use inside it reads only the lanes the loop
    // writes (bc<0..2>, xyz stores, xy converts): the loop runs on the full values and blends the kept lanes back once at
    // exit (six fewer 128-bit blends per vertex). f8 / f9 keep z, which f10's product reads: they stay blended.
    const __m128 e1 = f1, e2 = f2, e5 = f5, e6 = f6, e7 = f7, e10 = f10;
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
            f7 = st;
            f6 = ftoi<4>(f5);
            vi2 = sx(vi2 + vi4);
            acc = madd(acc, m20, bc<0>(f2));
            f1 = ldq(d, vi2);
            acc = madd(acc, m21, bc<1>(f2));
            vi5 = sx(vi5 - 1);
            f4 = madd(acc, m22, bc<2>(f2));
            stq<7>(d, vi3 + 1, f10);
            acc = _mm_add_ps(m23, _mm_setzero_ps());
            vi3 = sx(vi3 + vi4);
            f5 = _mm_mul_ps(f3, _mm_set1_ps(q));
            stq<7>(d, vi3, f6);
            f2 = itof<4>(f1);
            f10 = _mm_mul_ps(f8, _mm_set1_ps(q));
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
            f7 = st;
            f6 = ftoi<4>(f5);
            vi2 = sx(vi2 + vi4);
            acc = madd(acc, m20, bc<0>(f2));
            f1 = ldq(d, vi2);
            acc = madd(acc, m21, bc<1>(f2));
            vi5 = sx(vi5 - 1);
            f3 = madd(acc, m22, bc<2>(f2));
            stq<7>(d, vi3 + 1, f10);
            acc = _mm_add_ps(m23, _mm_setzero_ps());
            vi3 = sx(vi3 + vi4);
            f5 = _mm_mul_ps(f4, _mm_set1_ps(q));
            stq<7>(d, vi3, f6);
            f2 = itof<4>(f1);
            f10 = _mm_mul_ps(f9, _mm_set1_ps(q));
            q = rcpw(f3);
            cyc += 9u;
            if (vi5 == 0)
            {
                next = c.base + 0xA0u; // B's fall-through: jalr vi15, vi14 (returns to A)
                break;
            }
        }
    }
    f1 = bl<7>(e1, f1); f2 = bl<7>(e2, f2); f5 = bl<7>(e5, f5); f6 = bl<7>(e6, f6); f7 = bl<3>(e7, f7); f10 = bl<7>(e10, f10);
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
    // f1, f7, f9 keep their other lanes through the loop, read only in the lanes it writes (bc<0..2>, xy converts, xyz
    // stores): no blends inside, the kept lanes blended back at exit. f3's w lane never leaves the pass (p16 writes all
    // of f3 before any read of w), so its two blends go too. Six fewer 128-bit blends per vertex.
    const __m128 e1 = f1, e7 = f7, e9 = f9;
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
            f7 = st;
        }
        f5 = itof<0>(f5);                                                  // p2
        vi2 = sx(vi2 + vi4);
        acc = bl<7>(acc, _mm_mul_ps(m27, bc<0>(f4)));                     // p3
        f3 = ldq(d, vi2 - 2);
        acc = bl<7>(acc, madd(acc, m28, bc<1>(f4)));                       // p4
        const float qn = rcpw(f2);
        acc = bl<8>(acc, _mm_add_ps(f4, bc<2>(f4)));                       // p5
        f1 = ldq(d, vi2);
        f4 = _mm_add_ps(acc, _mm_mul_ps(m29, _mm_set1_ps(1.0f)));          // p6
        f3 = itof<15>(f3);                                      // p7
        acc = _mm_add_ps(m31, _mm_setzero_ps());                           // p8
        f1 = itof<4>(f1);                                       // p9
        f4 = madd(acc, f4, f5);                                            // p10
        f5 = ldq(d, vi2 - 1);
        q = qn;                                                            // p11
        f2 = bl<7>(f2, _mm_mul_ps(f2, _mm_set1_ps(q)));
        f9 = _mm_mul_ps(f8, _mm_set1_ps(q));                    // p12
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
    f1 = bl<7>(e1, f1); f7 = bl<3>(e7, f7); f9 = bl<7>(e9, f9);
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
    // f3, f5, f10 keep their other lanes through the loop, read only in the lanes it writes: no blends inside, the kept
    // lanes blended back at exit; f1's two partial writes (w from vi1, xyz loaded) are one blend, f2's two (ST*Q, ftoi4)
    // one on the product. Six fewer 128-bit blends per vertex.
    const __m128 e3 = f3, e5 = f5, e10 = f10;
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
            f10 = st;
        }
        f2 = madd(acc, m14, bc<2>(f1));                                   // q1
        {                                                                  // q2
            const __m128 n = ldq(d, vi2 + 2);
            f7 = _mm_max_ps(f6, m11);
            f3 = n;
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
        f3 = itof<15>(f3);                                      // q8
        acc = bl<7>(acc, _mm_mul_ps(m27, bc<0>(f7)));                     // q9
        vi1 = sx(vi1 + vi1);
        acc = bl<7>(acc, madd(acc, m28, bc<1>(f7)));                       // q10
        acc = bl<8>(acc, _mm_add_ps(f7, bc<2>(f7)));                       // q11
        const float qn = rcpw(f2);
        f7 = _mm_add_ps(acc, _mm_mul_ps(m29, _mm_set1_ps(1.0f)));          // q12
        acc = _mm_mul_ps(m24, bc<0>(f3));                                  // q13
        f1 = bl<7>(_mm_castsi128_ps(_mm_set1_epi32(vi1)), ldq(d, vi2 + 4)); // q12's w (vi1), q13's xyz
        acc = madd(acc, m25, bc<1>(f3));                                   // q14
        f6 = madd(acc, m26, bc<2>(f3));                                    // q15
        f4 = ldq(d, vi2 + 3);
        acc = _mm_add_ps(m31, _mm_setzero_ps());                           // q16
        f7 = madd(acc, f8, f7);                                            // q17
        f1 = itof<4>(f1);                                                  // q18
        q = qn;                                                            // q19
        f5 = _mm_mul_ps(f9, _mm_set1_ps(q));
        vi2 = sx(vi2 + vi4);
        const __m128 f2q = _mm_mul_ps(f2, _mm_set1_ps(q));                   // q20 (xyz into f2 with q24)
        f7 = _mm_min_ps(f7, I);                                            // q21
        vi1 = vi3;
        f8 = itof<0>(f4);                                                  // q22
        acc = _mm_mul_ps(m16, bc<0>(f3));                                  // q23
        f2 = bl<7>(f2, ftoi<4>(f2q));                                       // q24
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
    f3 = bl<7>(e3, f3); f5 = bl<7>(e5, f5); f10 = bl<3>(e10, f10);
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

// --- kernel: batch-front scatter loops (the skinned module's per-batch set-up: qwords to the offsets of a packed list) ----
// Head (pair 0): itof12 vf12, vf11 | mtir vi1, vf11x; lqi.<Ld> vf1, (vi2++); ibgtz vi1 -> tail; mr32 vf11, vf11 (delay);
// pair 4: ftoi4 vf12, vf12 | ibeq vi1, vi0 -> exit; nop; then vf11 = mr32(ftoi4(itof12(vf12))) (the next 16-bit offsets of
// the packed lanes) and the tail; tail (pair 9): [iadd vi3, vi1, vi12 when Based]; b -> head; sq.<St> vf1, 0(vi3 | vi1).
// No FMAC flags are written (ITOF/FTOI/MR32 leave MAC and status alone) and no Q: values are exact by construction (the
// native blocks' own helpers). Cycles as the native blocks count them: each block's idle-pipeline issue table (stalls
// inside a block only, 4-cycle FMAC / MR32 latency; none across block starts): head block 4, tail 3 (Based) / 2,
// pair-4 block 2, reload block 12 / 11, i.e. positive pass 7 / 6, reload pass 18 / 17, exit pass 6 (head to exit pc). Entries: the tail (0x48, after a positive head) and
// pair 4 (0x20, after a non-positive head). Exit pc: head + 0x60 (Based) / + 0x58.
template <int Ld, bool Based>
inline uint32_t kScatter(Ctx &c, uint32_t entry)
{
    VU1State &s = c.s;
    uint8_t *d = c.data;
    __m128 f1 = ldv(s, 1), f11 = ldv(s, 11), f12 = ldv(s, 12);
    int32_t vi1 = s.vi[1], vi2 = s.vi[2], vi3 = s.vi[3];
    const int32_t vi12 = s.vi[12];
    uint64_t cyc = c.cycle;
    const uint32_t exitPc = c.base + (Based ? 0x60u : 0x58u);
    const auto store = [&]() {
        if constexpr (Based)
        {
            vi3 = sx(vi1 + vi12);
            stq<Ld>(d, vi3, f1);
        }
        else
            stq<Ld>(d, vi1, f1);
    };
    // the reload pass from pair 4 on (vi1 < 0): returns true at the exit (vi1 == 0)
    const auto pair4 = [&]() {
        f12 = ftoi<4>(f12);
        if ((vi1 & 0xFFFF) == 0)
            return true;
        f11 = itof<12>(f12);
        f11 = ftoi<4>(f11);
        f11 = _mm_shuffle_ps(f11, f11, _MM_SHUFFLE(0, 3, 2, 1));
        store();
        return false;
    };
    uint32_t next = c.base; // the head
    if (entry == 0u) // tail: the store of a positive pass
    {
        store();
        cyc += Based ? 3u : 2u;
    }
    else if (pair4())
    {
        cyc += 2u;
        next = exitPc;
    }
    else
        cyc += Based ? 14u : 13u;
    for (uint32_t n = 0; next != exitPc && n < 65536u; ++n)
    {
        f12 = itof<12>(f11);
        vi1 = vu1n::laneBits16<0>(f11);
        if constexpr (Ld == 15)
            f1 = ldq(d, static_cast<uint16_t>(vi2));
        else
            f1 = bl<Ld>(f1, ldq(d, static_cast<uint16_t>(vi2)));
        vi2 = sx(vi2 + 1);
        f11 = _mm_shuffle_ps(f11, f11, _MM_SHUFFLE(0, 3, 2, 1));
        if (static_cast<int16_t>(vi1) > 0)
        {
            store();
            cyc += Based ? 7u : 6u;
        }
        else if (pair4())
        {
            cyc += 6u;
            next = exitPc;
        }
        else
            cyc += Based ? 18u : 17u;
    }
    stv(s, 1, f1); stv(s, 11, f11); stv(s, 12, f12);
    s.vi[1] = vi1;
    s.vi[2] = vi2;
    if constexpr (Based)
        s.vi[3] = vi3;
    c.cycle = cyc;
    return next;
}

// --- kernel: clip-flag culling loop of the vertex module (0cd8..0e10, 40 pairs; per vertex of a strip) -------------------
// Blocks as the native code splits them (cycles = their timing tables, the flag-reading blocks with their entry stalls):
//   B1 0cd8 (5, idle): vf17 = vf16, vf16 = vf15, ACC.xy = 0, ACC.z = vf22w, vf15.w = vf19.w + vf29.z, vi1 = vertex w
//      word, vf31 = vertex xyz, vi8 = vi10 & vi11; ibeq vi5 -> 0e18 (strip done; the kernel's exit).
//   B2 0d00 (7 pairs, issue 2..8: waits for ACC.z): vf30/vf15.xyz = ACC + vf29 * vf22/vf23, vf18/vf19.xy = ACC - ...,
//      fmand vi14 = 16 & the MAC flags of B1's vf15.w (the newest writer visible at its cycle), clipw vf30 and vf15
//      against vf15w, vi5 -= 1; ibeq vi14 -> path B.
//   path A 0d38 (3 pairs, issue 1, 5, 9: vf19.x for mtir, then the ACC chain), fcget = B2's second clip, then 0d78 (2,
//      idle): vf29 = ACC + vf26 * vf31z (the next vertex), vi8 &= vi10; ibltz vi1 -> B1.
//   path B 0d50 (7 pairs, issue 0, 1, 4, 8, 12, 16, 17: mfir -> mtir, then the ACC chain): clipw vf18 / vf19, vi10 =
//      3055 - (its second clip & 0xFFF), the same transform to vf29; ibltz vi1 -> B1.
//   vi1 >= 0 (a vertex that closes a triangle): 0d88 (4) vi9 = (vi9 | vi10) & vi3; vi8 != 0: reject 0df8 (4, ADC word
//      0x8001); else 0da8 (2) vi9 == 0: accept; else partial 0db8 (8: vi1 - 16384 into the w word, 929.z |= 2). All -> B1.
// Values use the native blocks' exact helpers (the _x variants: normalised operands, maddc, clamped ADD results); the
// MAC / clip writers of the last passes go into the flag rings with their issue cycles at the exit.
// Cycles: the flag READS above follow the blocks' own entry timing (they are exact: replay state, MAC and clip flags
// identical), but the cycle COUNT per pass uses B2 = 7 and path A = 9 (their idle tables, as the native traces run them)
// and the other lengths above; the native traces still count fewer cycles (fight.rec: +71 k VU cycles over 302 calls,
// ~3 % of the loop's cycles, never fewer than native). Like the vertex kernels, not cycle-exact.
inline uint32_t kCullClip(Ctx &c, uint32_t entry)
{
    VU1State &s = c.s;
    uint8_t *d = c.data;
    int32_t vi5 = s.vi[5];
    if (static_cast<int16_t>(vi5) < 0)
        return kDecline;
    __m128 f15 = ldv(s, 15), f16 = ldv(s, 16), f17 = ldv(s, 17), f18 = ldv(s, 18), f19 = ldv(s, 19), f29 = ldv(s, 29),
           f30 = ldv(s, 30), f31 = ldv(s, 31), acc = _mm_loadu_ps(s.acc);
    const __m128 f0 = ldv(s, 0), n22 = vu1n::nrm(ldv(s, 22)), n23 = vu1n::nrm(ldv(s, 23)), n24 = vu1n::nrm(ldv(s, 24)), n25 = vu1n::nrm(ldv(s, 25)),
                 n26 = vu1n::nrm(ldv(s, 26)), n27 = vu1n::nrm(ldv(s, 27));
    int32_t vi1 = s.vi[1], vi2 = s.vi[2], vi3 = s.vi[3], vi8 = s.vi[8], vi9 = s.vi[9], vi10 = s.vi[10], vi11 = s.vi[11],
            vi14 = s.vi[14];
    const int32_t vi4 = s.vi[4];
    uint32_t clip = *c.workingClip;
    uint64_t cyc = c.cycle;
    // the newest clip writers (issue cycle, flags), the newest five pushed to the ring at the exit; rings of 8 indexed
    // with & 7 (a % 5 per writer was ~1/4 of this kernel's samples on the console). MAC: the kernel always leaves from B1,
    // whose five FMACs are the newest five writers then, so only the last B1's are kept (raw: the ring computes the flags
    // when something reads them); the other blocks' MAC writers never reach the ring
    uint64_t clipAt[8];
    uint32_t clipV[8], clipN = 0u;
    __m128 b1r[5];
    uint64_t b1at = 0u;
    const auto clp = [&](uint64_t at, uint32_t v) { clipAt[clipN & 7u] = at, clipV[clipN & 7u] = v, ++clipN, clip = v; };
    const auto qd = [&](int32_t qw) { return d + ((static_cast<uint32_t>(qw) * 16u) & 0x3FFFu); };
    // the generated blocks' fast variants (plain SSE, no operand normalisation / clamp) where their entry checks pass
    // (vu1n_gen.py Bounds: the same results there); limits per lane (x, y, z, w) on the magnitude bits
    // lanes over the limit as a mask (OR several, test once: movemask is ~10 instructions under sse2neon)
    const auto overm = [](__m128 v, uint32_t lx, uint32_t ly, uint32_t lz, uint32_t lw) {
        return _mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7FFFFFFF)),
                               _mm_set_epi32(static_cast<int>(lw), static_cast<int>(lz), static_cast<int>(ly), static_cast<int>(lx)));
    };
    const auto anyv = [](__m128i m) {
#if defined(__aarch64__)
        return vmaxvq_u32(vreinterpretq_u32_s32(vreinterpretq_s32_m128i(m))) != 0u;
#else
        return _mm_movemask_epi8(m) != 0;
#endif
    };
    const auto over = [&](__m128 v, uint32_t lx, uint32_t ly, uint32_t lz, uint32_t lw) { return anyv(overm(v, lx, ly, lz, lw)); };
    constexpr uint32_t A = 0x7fffffffu, F = 0x7f7fffffu, M = 0x5d7fffffu, C = 0x5f7fffffu;
    const auto clipf = [](__m128 v, __m128 t, uint32_t prev) { // vu1n::clipValv on in-range operands
        const __m128 w = _mm_and_ps(_mm_shuffle_ps(t, t, _MM_SHUFFLE(3, 3, 3, 3)), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
        const __m128 nw = _mm_xor_ps(w, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))));
        const __m128i gt = _mm_castps_si128(_mm_cmpgt_ps(v, w));
        const __m128i lt = _mm_castps_si128(_mm_cmplt_ps(v, nw));
#if defined(__aarch64__)
        const uint32x4_t bits = vorrq_u32(vandq_u32(vreinterpretq_u32_m128i(gt), (uint32x4_t){1u, 4u, 16u, 0u}),
                                          vandq_u32(vreinterpretq_u32_m128i(lt), (uint32x4_t){2u, 8u, 32u, 0u}));
        const uint32_t f = vaddvq_u32(bits);
#else
        const uint32_t g = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(gt))), l = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(lt)));
        const uint32_t f = (g & 1u) | ((l & 1u) << 1) | ((g & 2u) << 1) | ((l & 2u) << 2) | ((g & 4u) << 2) | ((l & 4u) << 3);
#endif
        return ((prev << 6) | f) & 0xFFFFFFu;
    };
    // the per-call part of the blocks' checks (constant registers)
    const __m128 r22 = ldv(s, 22), r23 = ldv(s, 23), r24 = ldv(s, 24), r25 = ldv(s, 25), r26 = ldv(s, 26), r27 = ldv(s, 27);
    const bool constOk = !over(r22, M, M, M, M) && !over(r23, M, M, M, M) && !over(r24, M, M, M, M) && !over(r25, M, M, M, M) &&
                         !over(r26, M, M, M, M) && !over(r27, C, C, C, A) && !over(f0, C, A, A, A);
    uint32_t next;
    // entry 1 (0d00): B1 ran natively (the strip's first pass after the prologue); B2's fmand reads the ring as the
    // block does (its pair 3 at entry + 5)
    bool atB2 = entry == 1u;
    uint32_t m15w = atB2 ? VU1NativeAccess::ringAt(*c.macRing, cyc + 5u) : 0u;
    for (;;)
    {
      __m128 r;
      if (!atB2)
      {
        // B1
        const __m128 n19 = vu1n::nrm(f19), n29e = vu1n::nrm(f29);
        r = _mm_add_ps(vu1n::nrm(f16), bc<0>(f0));
        b1r[0] = r;
        f17 = vu1n::clampv(r);
        {
            uint32_t w;
            std::memcpy(&w, qd(vi2 - 1) + 12, 4);
            vi1 = static_cast<int16_t>(w & 0xFFFFu);
        }
        r = _mm_add_ps(vu1n::nrm(f15), bc<0>(f0));
        b1r[1] = r;
        f16 = vu1n::clampv(r);
        vi2 = vu1n::sx16(vi2 + vi4);
        r = _mm_sub_ps(f0, f0);
        b1r[2] = r;
        acc = _mm_blend_ps(acc, vu1n::clampv(r), 3);
        f31 = _mm_blend_ps(f31, _mm_loadu_ps(reinterpret_cast<const float *>(qd(vi2 - 1))), 7);
        const bool done = static_cast<int16_t>(vi5) == 0;
        r = _mm_add_ps(f0, bc<3>(n22));
        b1r[3] = r;
        acc = _mm_blend_ps(acc, vu1n::clampv(r), 4);
        r = _mm_add_ps(n19, bc<2>(n29e));
        m15w = vu1n::macValRt(r, 8u);
        b1r[4] = r;
        f15 = _mm_blend_ps(f15, vu1n::clampv(r), 8);
        vi8 = vi10 & vi11;
        b1at = cyc;
        cyc += 5u;
        if (done)
        {
            next = c.base + 0x140u; // 0e18
            break;
        }
      }
        atB2 = false;
        // B2 (issue 2..8)
        {
            const bool fast = constOk && !anyv(_mm_or_si128(_mm_or_si128(overm(f15, A, A, A, F), overm(f30, A, A, A, F)),
                                                             _mm_or_si128(overm(f29, M, M, M, M), overm(acc, C, C, C, C))));
            const __m128 n29 = fast ? f29 : vu1n::nrm(f29);
            __m128 q;
            if (fast)
                q = r = _mm_add_ps(acc, _mm_mul_ps(f29, r22));
            else
                r = vu1n::maddc<false>(acc, n29, n22, q);
            f30 = _mm_blend_ps(f30, r, 7);
            vi9 = vi10 | vi11;
            f31 = _mm_blend_ps(f31, vu1n::itofv<4>(f31), 7);
            vi11 = vi10;
            if (fast)
                q = r = _mm_add_ps(acc, _mm_mul_ps(f29, r23));
            else
                r = vu1n::maddc<false>(acc, n29, n23, q);
            f15 = _mm_blend_ps(f15, r, 7);
            if (fast)
                q = r = _mm_sub_ps(acc, _mm_mul_ps(f29, r22));
            else
                r = vu1n::maddc<true>(acc, n29, n22, q);
            f18 = _mm_blend_ps(f18, r, 3);
            vi14 = static_cast<int32_t>(m15w & 16u);
            if (fast)
                q = r = _mm_sub_ps(acc, _mm_mul_ps(f29, r23));
            else
                r = vu1n::maddc<true>(acc, n29, n23, q);
            f19 = _mm_blend_ps(f19, r, 3);
            vi5 = vu1n::sx16(vi5 - 1);
            const uint32_t c5 = fast ? clipf(f30, f15, clip) : vu1n::clipValv(vu1n::nrm(f30), vu1n::nrm(f15), clip);
            clp(cyc + 7u, c5);
            clp(cyc + 8u, fast ? clipf(f15, f15, c5) : vu1n::clipValv(vu1n::nrm(f15), vu1n::nrm(f15), c5));
            vi3 = 63;
            cyc += 7u;
        }
        const __m128 n31 = vu1n::nrm(f31);
        const bool pathOk = constOk && !anyv(_mm_or_si128(overm(f31, M, M, M, A), overm(acc, A, A, A, C)));
        if (static_cast<int16_t>(vi14) != 0)
        {
            // path A (issue 1, 5, 9), then 0d78 (idle)
            const bool fast = pathOk;
            __m128 q;
            r = _mm_add_ps(fast ? r27 : n27, bc<0>(f0));
            acc = _mm_blend_ps(acc, fast ? r : vu1n::clampv(r), 7);
            vi10 = vu1n::laneBits16<0>(f19);
            if (fast)
                q = r = _mm_add_ps(acc, _mm_mul_ps(r24, bc<0>(f31)));
            else
                r = vu1n::maddc<false>(acc, n24, bc<0>(n31), q);
            acc = _mm_blend_ps(acc, r, 7);
            if (fast)
                q = r = _mm_add_ps(acc, _mm_mul_ps(r25, bc<1>(f31)));
            else
                r = vu1n::maddc<false>(acc, n25, bc<1>(n31), q);
            acc = _mm_blend_ps(acc, r, 7);
            vi10 = static_cast<int32_t>(clip & 0x0FFFu);
            cyc += 9u;
            r = vu1n::maddc<false>(acc, n26, bc<2>(n31), q);
            f29 = _mm_blend_ps(f29, r, 7);
            vi8 = vi8 & vi10;
            cyc += 2u;
        }
        else
        {
            // path B (issue 0, 1, 4, 8, 12, 16, 17)
            const bool fast = pathOk && !anyv(_mm_or_si128(overm(f15, F, F, F, F), _mm_or_si128(overm(f18, F, F, F, F), overm(f19, F, F, F, F))));
            __m128 q;
            const uint32_t c0 = fast ? clipf(f18, f15, clip) : vu1n::clipValv(vu1n::nrm(f18), vu1n::nrm(f15), clip);
            clp(cyc, c0);
            f29 = _mm_blend_ps(f29, vu1n::fromInt(vu1n::sx16(vi10)), 1);
            const uint32_t c1 = fast ? clipf(f19, f15, c0) : vu1n::clipValv(vu1n::nrm(f19), vu1n::nrm(f15), c0);
            clp(cyc + 1u, c1);
            vi14 = 3055;
            r = _mm_add_ps(fast ? r27 : n27, bc<0>(f0));
            acc = _mm_blend_ps(acc, fast ? r : vu1n::clampv(r), 7);
            vi10 = vu1n::laneBits16<0>(f29);
            if (fast)
                q = r = _mm_add_ps(acc, _mm_mul_ps(r24, bc<0>(f31)));
            else
                r = vu1n::maddc<false>(acc, n24, bc<0>(n31), q);
            acc = _mm_blend_ps(acc, r, 7);
            vi10 = static_cast<int32_t>(c1 & 0x0FFFu);
            if (fast)
                q = r = _mm_add_ps(acc, _mm_mul_ps(r25, bc<1>(f31)));
            else
                r = vu1n::maddc<false>(acc, n25, bc<1>(n31), q);
            acc = _mm_blend_ps(acc, r, 7);
            vi10 = vu1n::sx16(vi14 - vi10);
            if (fast)
                q = r = _mm_add_ps(acc, _mm_mul_ps(r26, bc<2>(f31)));
            else
                r = vu1n::maddc<false>(acc, n26, bc<2>(n31), q);
            f29 = _mm_blend_ps(f29, r, 7);
            vi8 = vi8 & vi10;
            cyc += 18u;
        }
        if (static_cast<int16_t>(vi1) < 0)
            continue;
        // 0d88: the triangle's clip codes
        vi9 = (vi9 | vi10) & vi3;
        const bool reject = static_cast<int16_t>(vi8) != 0;
        vi3 = vu1n::sx16(-32767);
        cyc += 4u;
        const auto stW = [&](int32_t qw, int comp, int32_t v) {
            const uint32_t w = static_cast<uint16_t>(v & 0xFFFF);
            std::memcpy(qd(qw) + comp * 4, &w, 4);
        };
        if (reject) // 0df8: ADC 0x8001 into the vertex's w word
        {
            stW(vi2 - vi4 - 1, 3, vi3);
            cyc += 4u;
            continue;
        }
        const bool accept = static_cast<int16_t>(vi9) == 0; // 0da8
        vi1 = vu1n::sx16(vi1 - 16384);
        cyc += 2u;
        if (accept)
            continue;
        // 0db8: partially outside: the w word with bit 14 cleared, 929.z |= 2
        {
            uint32_t w;
            std::memcpy(&w, qd(929) + 8, 4);
            vi8 = static_cast<int16_t>(w & 0xFFFFu);
        }
        stW(vu1n::sx16(vi2 - vi4) - 1, 3, vi1);
        vi1 = 2;
        vi8 = vi8 | vi1;
        stW(929, 2, vi8);
        cyc += 8u;
    }
    static constexpr uint32_t kB1Mask[5] = {15u, 15u, 3u, 4u, 8u};
    for (uint32_t k = 0; k < 5u; ++k)
    {
        VU1FlagRing &rg = *c.macRing;
        rg.issue[rg.next] = b1at + k;
        rg.raw[rg.next] = b1r[k];
        rg.mask[rg.next] = kB1Mask[k];
        rg.tail[rg.next] = nullptr;
        rg.next = (rg.next + 1u) & VU1FlagRing::kWrap;
    }
    for (uint32_t k = clipN > 5u ? clipN - 5u : 0u; k < clipN; ++k)
        VU1NativeAccess::ringPush(*c.clipRing, clipAt[k & 7u], clipV[k & 7u]);
    *c.workingClip = clip;
    stv(s, 15, f15); stv(s, 16, f16); stv(s, 17, f17); stv(s, 18, f18); stv(s, 19, f19); stv(s, 29, f29); stv(s, 30, f30);
    stv(s, 31, f31);
    _mm_storeu_ps(s.acc, acc);
    s.vi[1] = vi1; s.vi[2] = vi2; s.vi[3] = vi3; s.vi[5] = vi5; s.vi[8] = vi8; s.vi[9] = vi9; s.vi[10] = vi10;
    s.vi[11] = vi11; s.vi[14] = vi14;
    c.cycle = cyc;
    return next;
}

inline constexpr Kernel kKernels[] = {
    {"unlit_st", 0x2e8bdf18116c01cdull, 20u, {0x00u, 0x58u}, kUnlitSt},
    {"lit_st", 0x7c187fbcd9b8ba29ull, 24u, {0x00u, ~0u}, kLitSt},
    {"skin_lit_st", 0xb8138c8401f6f173ull, 33u, {0x00u, ~0u}, kSkinLitSt},
    {"cull", 0xe0b2f1a1fa9e29fbull, 19u, {0x18u, ~0u}, kCull<false>},
    {"cull_swap", 0x4e6af90f38ca205bull, 19u, {0x18u, ~0u}, kCull<true>},
    {"skin_lit28", 0x5181a0b08a6d0989ull, 28u, {0x00u, ~0u}, kSkinLit28},
    {"unlit11", 0xa2eaa96b750883ceull, 11u, {0x00u, ~0u}, kUnlit11},
    {"scat_b", 0x30421310a686ffb1ull, 12u, {0x48u, 0x20u}, kScatter<15, true>, true},
    {"scat_bw", 0xfb0e4fb24245d179ull, 12u, {0x48u, 0x20u}, kScatter<8, true>, true},
    {"scat", 0x8fb4c370967d9ff8ull, 11u, {0x48u, 0x20u}, kScatter<15, false>, true},
    {"scat_w", 0x8b442df4a19fae58ull, 11u, {0x48u, 0x20u}, kScatter<8, false>, true},
    {"cull_clip", 0x0b72be7897fd2d28ull, 40u, {0x00u, 0x28u}, kCullClip, true},
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
// namespace scope (zero-initialised before anything runs): no guard check per kernel call
inline Stats g_stats{};
inline Stats &stats() { return g_stats; }
// The counters have one writer (the VU1 thread's dispatch; report() drains them on the same thread): a relaxed load and
// store, not a read-modify-write (an LDXR/STXR loop per kernel call on the A57, ~1 % of the console's VU1 thread)
inline void statAdd(std::atomic<uint64_t> &a, uint64_t v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }
// GOW_VU1_HLE_TIME, read on first use into a plain byte (-1 = not read yet)
inline bool timeKernels()
{
    static int8_t on = -1;
    if (on < 0)
        on = std::getenv("GOW_VU1_HLE_TIME") != nullptr ? 1 : 0;
    return on != 0;
}
// "[vu1-hle] window=5s <kernel>=<calls>/<VU cycles> ..." every 5 s with GOW_PERF_DIAG or GOW_VU1_HLE_STATS (force: now)
inline void report(bool force = false)
{
    // called at every runNative exit: no static guard, and the clock is read only every 64th call (one runNative ~10-30 us)
    static int8_t want = -1;
    if (want < 0)
        want = (std::getenv("GOW_PERF_DIAG") || std::getenv("GOW_VU1_HLE_STATS")) ? 1 : 0;
    if (!want && !force)
        return;
    static uint32_t callsSinceClock = 0;
    if (!force && (++callsSinceClock & 63u) != 0u)
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
    // per kernel: the bytes of a region that matched its hash; a region equal to them matches without the byte-wise FNV
    // (a remembered kernel is re-confirmed at every code change near its pc: the FNV of up to 320 bytes, one dependent
    // multiply per byte, was most of hleMatch's samples). Other bytes still go through the hash: the same decisions.
    std::vector<uint8_t> region[vu1hle::kKernelCount > 0 ? vu1hle::kKernelCount : 1];
};
