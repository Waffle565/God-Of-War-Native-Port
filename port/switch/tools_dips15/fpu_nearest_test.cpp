// fpu_nearest_test.cpp (sw-dips15): ps2FpuDivideNearest / ps2FpuSqrtNearest (FPCR-free AArch64 form) against the FPCR-switch
// reference, with FPCR in round-toward-zero + flush-to-zero like the EE thread. fnew.inc = the header text from
// 'inline float ps2FpuBitsToFloat' to the next '#else' of ps2xRuntime/include/ps2_runtime_macros.h. In WSL:
//   aarch64-linux-gnu-g++ -O2 -static -std=c++17 fpu_nearest_test.cpp -o t && qemu-aarch64 ./t <seed> <pairs>
// prints div_bad / sqrt_bad (0 = identical) and exits non-zero on any difference.
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <random>
#include "fnew.inc"
static float refDiv(float a, float b) {
    uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("msr fpcr, %2" : "+w"(a), "+w"(b) : "r"(fpcr & ~(3ull << 22)));
    float r; __asm__ volatile("fdiv %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
    __asm__ volatile("msr fpcr, %1" : "+w"(r) : "r"(fpcr)); return r; }
static float refSqrt(float a) {
    uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("msr fpcr, %1" : "+w"(a) : "r"(fpcr & ~(3ull << 22)));
    float r; __asm__ volatile("fsqrt %s0, %s1" : "=w"(r) : "w"(a));
    __asm__ volatile("msr fpcr, %1" : "+w"(r) : "r"(fpcr)); return r; }
static float fb(uint32_t u){float f; memcpy(&f,&u,4); return f;}
static uint32_t bf(float f){uint32_t u; memcpy(&u,&f,4); return u;}
int main(int argc, char**argv) {
    uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr = (fpcr & ~(3ull<<22)) | (3ull<<22) | (1ull<<24); // RZ + FZ
    __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
    std::mt19937_64 rng(argc > 1 ? atoi(argv[1]) : 1);
    long n = argc > 2 ? atol(argv[2]) : 20000000, bad = 0, badS = 0, fastD = 0;
    for (long i = 0; i < n; ++i) {
        uint64_t r = rng();
        uint32_t ua = (uint32_t)r, ub = (uint32_t)(r >> 32);
        if (i & 1) { // bias exponents near each other / edges
            ua = (ua & 0x807FFFFFu) | ((uint32_t)(rng() % 256) << 23);
            ub = (ub & 0x807FFFFFu) | ((uint32_t)(rng() % 256) << 23);
        }
        if (i % 7 == 0) ub = (ub & 0xFF800000u) | (rng() & 3); // tiny mantissas: exact quotients
        if (i % 11 == 0) ua = (ua & 0xFF800000u) | ((rng() & 7) << 20);
        float a = fb(ua), b = fb(ub);
        if (std::isnan(a) || std::isnan(b)) continue;
        float x = ps2FpuDivideNearest(a, b), y = refDiv(a, b);
        if (bf(x) != bf(y) && !(std::isnan(x) && std::isnan(y))) { if (bad++ < 10) printf("DIV %08x/%08x new %08x ref %08x\n", ua, ub, bf(x), bf(y)); }
        float s = fabsf(a);
        float sx = ps2FpuSqrtNearest(s), sy = refSqrt(s);
        if (bf(sx) != bf(sy) && !(std::isnan(sx) && std::isnan(sy))) { if (badS++ < 10) printf("SQRT %08x new %08x ref %08x\n", bf(s), bf(sx), bf(sy)); }
    }
    // squares: x = k*k exact and neighbours
    for (uint32_t k = 1; k < 5000000; ++k) { float f = (float)k; float q = f * f; for (int d = -1; d <= 1; ++d) { float s = fb(bf(q) + d); float sx = ps2FpuSqrtNearest(s), sy = refSqrt(s); if (bf(sx) != bf(sy)) { if (badS++ < 20) printf("SQRT2 %08x\n", bf(s)); } } }
    printf("n=%ld div_bad=%ld sqrt_bad=%ld\n", n, bad, badS);
    return bad || badS;
}
