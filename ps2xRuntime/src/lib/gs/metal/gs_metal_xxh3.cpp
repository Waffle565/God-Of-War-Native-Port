// GOW-Port: XXH3_64bits (seed 0, default secret), scalar. The hash PCSX2 names replacement textures with (GSXXH.h calls
// xxHash's XXH3_64bits / XXH3_64bits_update, whose result does not depend on the SIMD path or on streaming).
// Algorithm and secret from xxHash by Yann Collet (BSD 2-Clause, https://github.com/Cyan4973/xxHash), xxhash.h:
// XXH3_len_0to16_64b, XXH3_len_17to128_64b, XXH3_len_129to240_64b, XXH3_hashLong_64b_internal.
#include "gs_metal_texrepl.h"

#include <cstring>

namespace
{
    constexpr uint64_t kP32_1 = 0x9E3779B1u, kP32_2 = 0x85EBCA77u, kP32_3 = 0xC2B2AE3Du;
    constexpr uint64_t kP64_1 = 0x9E3779B185EBCA87ull, kP64_2 = 0xC2B2AE3D27D4EB4Full, kP64_3 = 0x165667B19E3779F9ull;
    constexpr uint64_t kP64_4 = 0x85EBCA77C2B2AE63ull, kP64_5 = 0x27D4EB2F165667C5ull;
    constexpr uint64_t kMx1 = 0x165667919E3779F9ull, kMx2 = 0x9FB21C651E98DF25ull;

    alignas(64) const uint8_t kSecret[192] = {
        0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
        0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
        0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
        0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
        0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
        0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
        0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
        0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
        0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
        0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
        0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
        0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
    };

    inline uint64_t rd64(const uint8_t *p) { uint64_t v; std::memcpy(&v, p, 8); return v; } // little-endian host
    inline uint32_t rd32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    inline uint64_t rotl(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }
    inline uint64_t fold(uint64_t a, uint64_t b)
    {
        const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
        return static_cast<uint64_t>(p) ^ static_cast<uint64_t>(p >> 64);
    }
    inline uint64_t avalanche(uint64_t h) { h ^= h >> 37; h *= kMx1; return h ^ (h >> 32); }
    inline uint64_t avalanche64(uint64_t h)
    {
        h ^= h >> 33; h *= kP64_2; h ^= h >> 29; h *= kP64_3; return h ^ (h >> 32);
    }
    inline uint64_t mix16(const uint8_t *in, const uint8_t *sec) { return fold(rd64(in) ^ rd64(sec), rd64(in + 8) ^ rd64(sec + 8)); }

    uint64_t upTo16(const uint8_t *in, size_t len)
    {
        if (len > 8)
        {
            const uint64_t lo = rd64(in) ^ (rd64(kSecret + 24) ^ rd64(kSecret + 32));
            const uint64_t hi = rd64(in + len - 8) ^ (rd64(kSecret + 40) ^ rd64(kSecret + 48));
            return avalanche(len + __builtin_bswap64(lo) + hi + fold(lo, hi));
        }
        if (len >= 4)
        {
            const uint64_t v = (rd32(in + len - 4) + (static_cast<uint64_t>(rd32(in)) << 32)) ^ (rd64(kSecret + 8) ^ rd64(kSecret + 16));
            uint64_t h = v ^ rotl(v, 49) ^ rotl(v, 24);
            h *= kMx2;
            h ^= (h >> 35) + len;
            h *= kMx2;
            return h ^ (h >> 28);
        }
        if (len)
        {
            const uint32_t combined = (static_cast<uint32_t>(in[0]) << 16) | (static_cast<uint32_t>(in[len >> 1]) << 24) | in[len - 1] |
                                      (static_cast<uint32_t>(len) << 8);
            return avalanche64(combined ^ static_cast<uint64_t>(rd32(kSecret) ^ rd32(kSecret + 4)));
        }
        return avalanche64(rd64(kSecret + 56) ^ rd64(kSecret + 64));
    }

    void stripe(uint64_t *acc, const uint8_t *in, const uint8_t *sec)
    {
        for (int i = 0; i < 8; ++i)
        {
            const uint64_t v = rd64(in + i * 8), k = v ^ rd64(sec + i * 8);
            acc[i ^ 1] += v;
            acc[i] += (k & 0xFFFFFFFFu) * (k >> 32);
        }
    }

    uint64_t hashLong(const uint8_t *in, size_t len)
    {
        uint64_t acc[8] = {kP32_3, kP64_1, kP64_2, kP64_3, kP64_4, kP32_2, kP64_5, kP32_1};
        constexpr size_t stripesPerBlock = (sizeof(kSecret) - 64) / 8, blockLen = 64 * stripesPerBlock;
        const size_t blocks = (len - 1) / blockLen;
        for (size_t n = 0; n < blocks; ++n)
        {
            for (size_t s = 0; s < stripesPerBlock; ++s)
                stripe(acc, in + n * blockLen + s * 64, kSecret + s * 8);
            for (int i = 0; i < 8; ++i) // scramble
            {
                uint64_t a = acc[i];
                a ^= a >> 47;
                a ^= rd64(kSecret + sizeof(kSecret) - 64 + i * 8);
                acc[i] = a * kP32_1;
            }
        }
        const size_t stripes = ((len - 1) - blockLen * blocks) / 64;
        for (size_t s = 0; s < stripes; ++s)
            stripe(acc, in + blocks * blockLen + s * 64, kSecret + s * 8);
        stripe(acc, in + len - 64, kSecret + sizeof(kSecret) - 64 - 7);
        uint64_t result = len * kP64_1;
        for (int i = 0; i < 4; ++i)
            result += fold(acc[2 * i] ^ rd64(kSecret + 11 + 16 * i), acc[2 * i + 1] ^ rd64(kSecret + 11 + 16 * i + 8));
        return avalanche(result);
    }
}

uint64_t gsrepl::xxh3_64(const void *data, size_t len)
{
    const uint8_t *in = static_cast<const uint8_t *>(data);
    if (len <= 16)
        return upTo16(in, len);
    if (len <= 128)
    {
        uint64_t acc = len * kP64_1;
        if (len > 32)
        {
            if (len > 64)
            {
                if (len > 96)
                    acc += mix16(in + 48, kSecret + 96) + mix16(in + len - 64, kSecret + 112);
                acc += mix16(in + 32, kSecret + 64) + mix16(in + len - 48, kSecret + 80);
            }
            acc += mix16(in + 16, kSecret + 32) + mix16(in + len - 32, kSecret + 48);
        }
        acc += mix16(in, kSecret) + mix16(in + len - 16, kSecret + 16);
        return avalanche(acc);
    }
    if (len <= 240)
    {
        uint64_t acc = len * kP64_1;
        for (int i = 0; i < 8; ++i)
            acc += mix16(in + 16 * i, kSecret + 16 * i);
        uint64_t end = mix16(in + len - 16, kSecret + 136 - 17);
        acc = avalanche(acc);
        for (size_t i = 8; i < len / 16; ++i)
            end += mix16(in + 16 * i, kSecret + 16 * (i - 8) + 3);
        return avalanche(acc + end);
    }
    return hashLong(in, len);
}
