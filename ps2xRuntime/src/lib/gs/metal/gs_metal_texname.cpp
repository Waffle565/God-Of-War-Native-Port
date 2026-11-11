// GOW-Port: the PCSX2 replacement name of a draw's texture (HashCacheKey::Create + CreateTextureName of PCSX2, see
// gs_metal_texrepl.h): data hash over the base level (and the mip levels when mipmapping is active), palette hash, region,
// packed TEX0/TEXA bits.
#include "gs_metal_texrepl.h"

#include <algorithm>

namespace
{
    // GSClut::Read32 into m_buff32: CT32/CT24 palettes are the raw words of the CLUT buffer (T8: 16-entry rows from
    // CSA*16, clamped at entry 240), CT16 ones are expanded with TEXA (Expand16: x << 3, alpha TA0/TA1, AEM on 0)
    uint32_t readPalette(const std::array<uint16_t, 512> &clut, const GSTex0Reg &t, const GSTexaReg &texa, uint32_t out[256])
    {
        const uint32_t psm = t.psm & 63u;
        const uint32_t count = (psm == GS_PSM_T8 || psm == GS_PSM_T8H) ? 256u : 16u;
        if (t.cpsm == GS_PSM_CT16 || t.cpsm == GS_PSM_CT16S)
        {
            const uint32_t base = static_cast<uint32_t>(t.csa) << 4;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t c = clut[(base + i) & 511u];
                const uint32_t a = (c & 0x8000u) ? texa.ta1 : texa.ta0;
                out[i] = ((c & 0x1Fu) << 3) | ((c & 0x3E0u) << 6) | ((c & 0x7C00u) << 9) | ((texa.aem && c == 0u) ? 0u : a << 24);
            }
            return count;
        }
        const uint32_t offset = (static_cast<uint32_t>(t.csa) & 15u) << 4;
        for (uint32_t i = 0; i < count; i += 16u)
        {
            const uint32_t row = count == 256u ? std::min(i + offset, 240u) : offset;
            for (uint32_t j = 0; j < 16u; ++j)
                out[i + j] = static_cast<uint32_t>(clut[row + j]) | (static_cast<uint32_t>(clut[row + j + 256u]) << 16);
        }
        return count;
    }

    // GSState::GetTex0Layer for level 1..6, with the MIPTBP1 the GS computes itself when TEX1.MTBA is set (GSState TEX0
    // handler: only for colour formats 32..1024 (512 for 32 bit layouts) wide)
    GSTex0Reg mipLayer(const GSContext &c, uint32_t level)
    {
        GSTex0Reg t = c.tex0;
        const uint32_t psm = t.psm & 63u;
        const uint32_t fmt = gsrepl::formatClass(psm);
        const uint32_t bpp = (fmt == 2u) ? 16u : (fmt == 3u && psm == GS_PSM_T8) ? 8u : (fmt == 4u && psm == GS_PSM_T4) ? 4u : 32u;
        const uint32_t maxTex = bpp < 32u ? 10u : 9u;
        uint64_t mip1 = c.miptbp1;
        if (((c.tex1 >> 9) & 1u) && t.tw >= 5u && t.tw <= maxTex)
        {
            uint32_t bp = t.tbp0, bw = std::max(1u, (1u << t.tw) >> 6);
            uint32_t size = ((1u << t.tw) * (1u << t.tw) * (bpp >> 2)) >> 9;
            mip1 = 0;
            for (uint32_t i = 0; i < 3u; ++i)
            {
                bp += size;
                bw = std::max(bw >> 1, 1u);
                size = std::max(size >> 2, 1u);
                mip1 |= (static_cast<uint64_t>(bp & 0x3FFFu) | (static_cast<uint64_t>(bw & 0x3Fu) << 14)) << (20u * i);
            }
        }
        const uint64_t reg = level <= 3u ? mip1 : c.miptbp2;
        const uint32_t shift = 20u * ((level - 1u) % 3u);
        t.tbp0 = static_cast<uint32_t>((reg >> shift) & 0x3FFFu);
        t.tbw = static_cast<uint8_t>((reg >> (shift + 14u)) & 0x3Fu);
        t.th = t.th <= level ? 0u : static_cast<uint8_t>(t.th - level);
        t.tw = t.tw <= level ? 0u : static_cast<uint8_t>(t.tw - level);
        return t;
    }

    uint64_t regionForLevel(uint64_t r, uint32_t level)
    {
        const auto lo16 = [](uint64_t v, int s) { return static_cast<int32_t>(static_cast<int16_t>((v >> s) & 0xFFFFu)); };
        uint64_t out = 0;
        if (static_cast<uint32_t>(r) != 0u)
        {
            const int32_t a = lo16(r, 0) >> level, w = std::max((lo16(r, 16) - lo16(r, 0)) >> level, 1);
            out |= static_cast<uint64_t>(static_cast<uint16_t>(a)) | (static_cast<uint64_t>(static_cast<uint16_t>(a + w)) << 16);
        }
        if (static_cast<uint32_t>(r >> 32) != 0u)
        {
            const int32_t a = lo16(r, 32) >> level, h = std::max((lo16(r, 48) - lo16(r, 32)) >> level, 1);
            out |= (static_cast<uint64_t>(static_cast<uint16_t>(a)) << 32) | (static_cast<uint64_t>(static_cast<uint16_t>(a + h)) << 48);
        }
        return out;
    }
}

uint64_t gsrepl::clutHash(const std::array<uint16_t, 512> &clut, const GSTex0Reg &tex0, const GSTexaReg &texa)
{
    uint32_t words[256];
    const uint32_t count = readPalette(clut, tex0, texa, words);
    return xxh3_64(words, count * sizeof(uint32_t));
}

uint32_t gsrepl::mipLevels(const GSContext &c)
{
    const uint32_t mxl = static_cast<uint32_t>((c.tex1 >> 2) & 7u), mmin = static_cast<uint32_t>((c.tex1 >> 6) & 7u);
    return (mxl > 0u && mmin >= 2u && mmin <= 5u) ? std::min(mxl, 6u) : 0u;
}

bool gsrepl::nameFor(const uint8_t *vram, const GSDrawState &s, const std::array<uint16_t, 512> &clut, uint32_t mips,
                     std::vector<uint8_t> &scratch, Name &out)
{
    const GSTex0Reg &t = s.context.tex0;
    const uint32_t psm = t.psm & 63u;
    if (!vram || !hashable(psm))
        return false;
    const uint64_t region = regionFromClamp(t, s.context.clamp);
    scratch.clear();
    appendLevel(vram, t.tbp0, t.tbw, psm, t.tw & 15u, t.th & 15u, s.texa, region, scratch);
    for (uint32_t level = 1; level <= mips; ++level)
    {
        const GSTex0Reg m = mipLayer(s.context, level);
        appendLevel(vram, m.tbp0, m.tbw, psm, m.tw & 15u, m.th & 15u, s.texa, regionForLevel(region, level), scratch);
    }
    out = {};
    out.texHash = xxh3_64(scratch.data(), scratch.size());
    const bool pal = hasPalette(psm);
    out.clutHash = pal ? clutHash(clut, t, s.texa) : 0u;
    const auto lo16 = [&region](int sh) { return static_cast<int32_t>(static_cast<int16_t>((region >> sh) & 0xFFFFu)); };
    out.regionW = static_cast<uint16_t>(lo16(16) - lo16(0));
    out.regionH = static_cast<uint16_t>(lo16(48) - lo16(32));
    out.bits = psm | ((t.tw & 15u) << 6) | ((t.th & 15u) << 10);
    if (!pal && formatClass(psm) > 0u) // TEXA is part of the key only for CT24 / CT16 (HashCacheKey::Create)
        out.bits |= (static_cast<uint32_t>(s.texa.ta0) << 15) | ((s.texa.aem ? 1u : 0u) << 23) | (static_cast<uint32_t>(s.texa.ta1) << 24);
    return true;
}
