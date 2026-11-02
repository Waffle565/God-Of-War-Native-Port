// wrtest.cpp: GSMem::WriteRun (the run version) against PixelStorageTraits<psm>::Write per pixel, all formats.
// Build: g++ -O2 -std=c++20 -I<rt>/include -I<rt>/include/runtime -I<rt>/src/lib wrtest.cpp
#include "gs/ps2_gs_memory.cpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace GSMem
{
    template <PixelStorageMode psm, typename Table, typename Fetch>
    void refRunT(const Table &table, u8 *data, u32 bp, u32 bw, u32 x, u32 y, u32 count, Fetch fetch)
    {
        using Packed = typename PixelStorageTraits<psm>::PackedT;
        for (u32 i = 0; i < count; ++i)
            PixelStorageTraits<psm>::Write(table, data, bp, bw, x + i, y, static_cast<Packed>(fetch(i)));
    }
    void RefRun(u8 *data, u32 psm, u32 bp, u32 bw, u32 x, u32 y, const u8 *src, u32 srcPixel, u32 count)
    {
        auto f32 = [src, srcPixel](u32 i) { u32 v; std::memcpy(&v, src + (srcPixel + i) * 4u, 4u); return v; };
        auto f24 = [src, srcPixel](u32 i) { const u8 *p = src + (srcPixel + i) * 3u; return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16); };
        auto f16 = [src, srcPixel](u32 i) { u16 v; std::memcpy(&v, src + (srcPixel + i) * 2u, 2u); return u32(v); };
        auto f8 = [src, srcPixel](u32 i) { return u32(src[srcPixel + i]); };
        auto f4 = [src, srcPixel](u32 i) { const u32 n = srcPixel + i; return u32(src[n >> 1] >> ((n & 1u) * 4u)) & 0x0Fu; };
        switch (static_cast<PixelStorageMode>(psm & 0x3Fu))
        {
        case C32: refRunT<C32>(PageTableC32, data, bp, bw, x, y, count, f32); break;
        case Z32: refRunT<Z32>(PageTableZ32, data, bp, bw, x, y, count, f32); break;
        case C24: refRunT<C24>(PageTableC32, data, bp, bw, x, y, count, f24); break;
        case Z24: refRunT<Z24>(PageTableZ32, data, bp, bw, x, y, count, f24); break;
        case C16: refRunT<C16>(PageTableC16, data, bp, bw, x, y, count, f16); break;
        case C16S: refRunT<C16S>(PageTableC16S, data, bp, bw, x, y, count, f16); break;
        case Z16: refRunT<Z16>(PageTableZ16, data, bp, bw, x, y, count, f16); break;
        case Z16S: refRunT<Z16S>(PageTableZ16S, data, bp, bw, x, y, count, f16); break;
        case P8: refRunT<P8>(PageTableP8, data, bp, bw, x, y, count, f8); break;
        case P8H: refRunT<P8H>(PageTableC32, data, bp, bw, x, y, count, f8); break;
        case P4: refRunT<P4>(PageTableP4, data, bp, bw, x, y, count, f4); break;
        case P4HL: refRunT<P4HL>(PageTableC32, data, bp, bw, x, y, count, f4); break;
        case P4HH: refRunT<P4HH>(PageTableC32, data, bp, bw, x, y, count, f4); break;
        default: break;
        }
    }
}

int main(int argc, char **argv)
{
    using namespace GSMem;
    InitLookupTables();
    const PixelStorageMode psms[] = {C32, Z32, C24, Z24, C16, C16S, Z16, Z16S, P8, P8H, P4, P4HL, P4HH};
    const u32 iters = argc > 1 ? std::atoi(argv[1]) : 20000;
    std::mt19937_64 rng(12345);
    std::vector<u8> a(4u << 20), b(4u << 20), src(1u << 16);
    long long pixels = 0, bad = 0;
    for (PixelStorageMode pm : psms)
    {
        const u32 psm = static_cast<u32>(pm);
        for (auto &v : a) v = static_cast<u8>(rng());
        b = a;
        for (u32 it = 0; it < iters; ++it)
        {
            for (auto &v : src) v = static_cast<u8>(rng());
            const u32 bp = rng() % 16384u, bw = 1u + rng() % 32u;
            // x: anywhere in the buffer width and beyond (wraps), y up to 2047, runs up to 2048 pixels
            const u32 x = rng() % 2048u, y = rng() % 2048u, n = 1u + (rng() % 4 == 0 ? rng() % 2048u : rng() % 160u);
            const u32 sp = rng() % 64u;
            WriteRun(a.data(), psm, bp, bw, x, y, src.data(), sp, n);
            RefRun(b.data(), psm, bp, bw, x, y, src.data(), sp, n);
            pixels += n;
            if ((it & 1023u) == 0u && a != b)
            {
                ++bad;
                std::printf("psm %x: mismatch by iteration %u\n", psm, it);
                b = a;
            }
        }
        if (a != b)
        {
            ++bad;
            std::printf("psm %x: final mismatch\n", psm);
        }
    }
    std::printf("pixels %lld, mismatching formats %lld\n", pixels, bad);
    return bad != 0;
}
