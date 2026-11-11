// GOW-Port: texture replacement names compatible with PCSX2 texture packs (textures/<serial>/replacements).
// The name of a texture is computed exactly as PCSX2 does it, so existing packs load unchanged. Ported from PCSX2
// (GPL-3.0+, (c) 2002-2026 PCSX2 Dev Team): pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp (TextureName, file name
// formats), GSTextureCache.cpp (HashCacheKey::Create, HashTextureLevel, SourceRegion::Create, PaletteKeyHash),
// GSClut.cpp (Read32: the palette that is hashed), GSBlock.h (Expand16to32 / Expand24to32), GSTables.cpp (block tables).
// Plain C++ (no Metal): the Metal side is gs_metal_texrepl_store.mm.
#pragma once

#include "runtime/gs/gs_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gsrepl
{
    // XXH3_64bits (seed 0, default secret): the hash PCSX2 uses for texture data and palettes (GSXXH.h)
    uint64_t xxh3_64(const void *data, size_t len);

    // PCSX2's TextureName: data hash, palette hash (paletted formats only), region size, packed TEX0/TEXA bits, mip level
    struct Name
    {
        uint64_t texHash = 0, clutHash = 0;
        uint32_t regionW = 0, regionH = 0;
        uint32_t bits = 0; // PSM:6 TW:4 TH:4 (unused):1 TA0:8 AEM:1 TA1:8
        uint32_t mip = 0;
        bool operator==(const Name &o) const
        {
            return texHash == o.texHash && clutHash == o.clutHash && regionW == o.regionW && regionH == o.regionH && bits == o.bits && mip == o.mip;
        }
    };
    struct NameHash
    {
        size_t operator()(const Name &n) const
        {
            uint64_t h = n.texHash * 0x9E3779B97F4A7C15ull ^ n.clutHash;
            h = h * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(n.regionW) | (static_cast<uint64_t>(n.regionH) << 32));
            h = h * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(n.bits) | (static_cast<uint64_t>(n.mip) << 32));
            return static_cast<size_t>(h ^ (h >> 29));
        }
    };

    bool hasPalette(uint32_t psm);
    bool hashable(uint32_t psm);      // colour formats PCSX2 hashes (not depth)
    uint32_t formatClass(uint32_t psm); // GSLocalMemory psm_t::fmt: 0 32 bit, 1 24 bit, 2 16 bit, 3 8 bit, 4 4 bit
    // PCSX2 file name without extension: "<tex>-<clut>-r<W>x<H>-<bits>[-mip<N>]" (hex without leading zeros, bits %08x)
    std::string fileStem(const Name &n);
    // a replacement file name (any extension) in one of PCSX2's formats, including the old 64-bit region form
    bool parseFileName(const char *file, Name &out);

    // SourceRegion of PCSX2 (64 bits: min/max x in the low half, min/max y in the high half), from CLAMP region modes
    uint64_t regionFromClamp(const GSTex0Reg &tex0, uint64_t clamp);

    // TEX0 hash input of one level (PCSX2 HashTextureLevel): the raw 256-byte blocks in GS memory order, or for small
    // textures, partial formats (CT24/CT16/T8H/T4HL/T4HH) and regions the expanded texels (indices / RGBA8 with TEXA)
    void appendLevel(const uint8_t *vram, uint32_t tbp0, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, const GSTexaReg &texa,
                     uint64_t region, std::vector<uint8_t> &out);

    // the hashed palette (GSClut::Read32 into m_buff32): 16 or 256 RGBA words from the CLUT buffer of the texture store
    uint64_t clutHash(const std::array<uint16_t, 512> &clut, const GSTex0Reg &tex0, const GSTexaReg &texa);

    // mip levels PCSX2 adds to the hash when hardware mipmapping is active (TEX1.MXL > 0, MMIN 2..5): 0 = base only
    uint32_t mipLevels(const GSContext &c);

    // the full name of the texture a draw samples (false: a format PCSX2 does not hash). `mips`: also hash levels 1..mips
    // (PCSX2 with mipmapping active). `clut`: the texture store's CLUT buffer (paletted formats). `scratch`: hash input.
    bool nameFor(const uint8_t *vram, const GSDrawState &s, const std::array<uint16_t, 512> &clut, uint32_t mips,
                 std::vector<uint8_t> &scratch, Name &out);
}
