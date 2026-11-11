// GOW-Port: PCSX2 texture replacement names (see gs_metal_texrepl.h for the PCSX2 sources this follows). The bytes that
// are hashed must be the ones PCSX2 hashes, so the block walk, the expansion rules and the palette layout are PCSX2's,
// not the renderer's own decode (which differs, e.g. CT16 is expanded with bit replication there).
#include "gs_metal_texrepl.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace
{
    // GSTables.cpp block swizzle tables (8 columns x 4 rows / 4 columns x 8 rows)
    const uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
    const uint8_t kBlock16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
    const uint8_t kBlock16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};

    // the GSLocalMemory::psm_t fields the hash uses
    struct Psm
    {
        bool valid = false;
        uint32_t bsX = 8, bsY = 8;       // block size in texels
        uint32_t pgShX = 6, pgShY = 5;   // page size (log2)
        uint32_t blShX = 3, blShY = 3;   // block size (log2)
        int table = 32;                  // 32 (also T8: same table), 16, 16S (161), 4 (T4: the 16 table)
        bool fullMask = true;            // fmsk == 0xFFFFFFFF
        uint32_t pal = 0;
        uint32_t fmt = 0;
    };

    Psm psmInfo(uint32_t psm)
    {
        Psm p;
        p.valid = true;
        switch (psm)
        {
        case GS_PSM_CT32: break;
        case GS_PSM_CT24: p.fullMask = false; p.fmt = 1; break;
        case GS_PSM_CT16: case GS_PSM_CT16S:
            p.bsX = 16; p.pgShY = 6; p.blShX = 4; p.table = psm == GS_PSM_CT16 ? 16 : 161; p.fullMask = false; p.fmt = 2; break;
        case GS_PSM_T8: p.bsX = p.bsY = 16; p.pgShX = 7; p.pgShY = 6; p.blShX = p.blShY = 4; p.pal = 256; p.fmt = 3; break;
        case GS_PSM_T4: p.bsX = 32; p.bsY = 16; p.pgShX = p.pgShY = 7; p.blShX = 5; p.blShY = 4; p.table = 4; p.pal = 16; p.fmt = 4; break;
        case GS_PSM_T8H: p.fullMask = false; p.pal = 256; p.fmt = 3; break;
        case GS_PSM_T4HL: case GS_PSM_T4HH: p.fullMask = false; p.pal = 16; p.fmt = 4; break;
        default: p.valid = false; break; // depth formats are depth sources in PCSX2 (never hashed)
        }
        return p;
    }

    uint32_t swizzle(const Psm &p, uint32_t bx, uint32_t by)
    {
        switch (p.table)
        {
        case 16: case 4: return kBlock16[by & 7][bx & 3];
        case 161: return kBlock16S[by & 7][bx & 3];
        default: return kBlock32[by & 3][bx & 7];
        }
    }

    int32_t minX(uint64_t r) { return static_cast<int16_t>(r); }
    int32_t maxX(uint64_t r) { return static_cast<int16_t>((r >> 16) & 0xFFFFu); }
    int32_t minY(uint64_t r) { return static_cast<int16_t>((r >> 32) & 0xFFFFu); }
    int32_t maxY(uint64_t r) { return static_cast<int16_t>(r >> 48); }
    bool hasX(uint64_t r) { return static_cast<uint32_t>(r) != 0u; }
    bool hasY(uint64_t r) { return static_cast<uint32_t>(r >> 32) != 0u; }
    void setX(uint64_t &r, int32_t lo, int32_t hi) { r |= static_cast<uint64_t>(static_cast<uint16_t>(lo)) | (static_cast<uint64_t>(static_cast<uint16_t>(hi)) << 16); }
    void setY(uint64_t &r, int32_t lo, int32_t hi) { r |= (static_cast<uint64_t>(static_cast<uint16_t>(lo)) << 32) | (static_cast<uint64_t>(static_cast<uint16_t>(hi)) << 48); }

    // one expanded texel as psm.rtx / psm.rtxP write it: palette index, or RGBA8 with TEXA (Expand24to32, Expand16to32)
    uint32_t expandedTexel(const uint8_t *vram, uint32_t psm, uint32_t tbp0, uint32_t tbw, uint32_t x, uint32_t y, const GSTexaReg &texa)
    {
        using namespace GSMem;
        uint8_t *m = const_cast<uint8_t *>(vram);
        switch (psm)
        {
        case GS_PSM_CT32: return ReadCT32(m, tbp0, tbw, x, y);
        case GS_PSM_CT24:
        {
            const uint32_t c = ReadCT24(m, tbp0, tbw, x, y) & 0x00FFFFFFu;
            return c | ((texa.aem && c == 0u) ? 0u : static_cast<uint32_t>(texa.ta0) << 24);
        }
        case GS_PSM_CT16: case GS_PSM_CT16S:
        {
            const uint32_t c = (psm == GS_PSM_CT16 ? ReadCT16(m, tbp0, tbw, x, y) : ReadCT16S(m, tbp0, tbw, x, y)) & 0xFFFFu;
            const uint32_t a = (c & 0x8000u) ? texa.ta1 : texa.ta0;
            return ((c & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) << 11) | (((c >> 10) & 0x1Fu) << 19) | ((texa.aem && c == 0u) ? 0u : a << 24);
        }
        case GS_PSM_T8: return ReadP8(m, tbp0, tbw, x, y) & 0xFFu;
        case GS_PSM_T4: return ReadP4(m, tbp0, tbw, x, y) & 0x0Fu;
        case GS_PSM_T8H: return ReadCT32(m, tbp0, tbw, x, y) >> 24;
        case GS_PSM_T4HL: return (ReadCT32(m, tbp0, tbw, x, y) >> 24) & 0x0Fu;
        case GS_PSM_T4HH: return ReadCT32(m, tbp0, tbw, x, y) >> 28;
        default: return 0u;
        }
    }
}

bool gsrepl::hasPalette(uint32_t psm) { return psmInfo(psm & 63u).pal > 0u; }
bool gsrepl::hashable(uint32_t psm) { return psmInfo(psm & 63u).valid; }
uint32_t gsrepl::formatClass(uint32_t psm) { return psmInfo(psm & 63u).fmt; }

std::string gsrepl::fileStem(const Name &n)
{
    char buf[128];
    const bool pal = hasPalette(n.bits & 63u), region = n.regionW != 0u || n.regionH != 0u;
    int len;
    if (region && pal)
        len = std::snprintf(buf, sizeof(buf), "%" PRIx64 "-%" PRIx64 "-r%ux%u-%08x", n.texHash, n.clutHash, n.regionW, n.regionH, n.bits);
    else if (region)
        len = std::snprintf(buf, sizeof(buf), "%" PRIx64 "-r%ux%u-%08x", n.texHash, n.regionW, n.regionH, n.bits);
    else if (pal)
        len = std::snprintf(buf, sizeof(buf), "%" PRIx64 "-%" PRIx64 "-%08x", n.texHash, n.clutHash, n.bits);
    else
        len = std::snprintf(buf, sizeof(buf), "%" PRIx64 "-%08x", n.texHash, n.bits);
    if (n.mip > 0u && len > 0)
        std::snprintf(buf + len, sizeof(buf) - static_cast<size_t>(len), "-mip%u", n.mip);
    return buf;
}

// GSTextureReplacements::ParseReplacementName: the same sscanf patterns in the same order
bool gsrepl::parseFileName(const char *file, Name &n)
{
    n = {};
    char dot = 0;
    unsigned long long region = 0;
    const auto done = [&n] { n.bits &= ~(1u << 14); return true; }; // RemoveUnusedBits: the old TCC bit
    if (std::sscanf(file, "%" SCNx64 "-%" SCNx64 "-r%ux%u-%08x%c", &n.texHash, &n.clutHash, &n.regionW, &n.regionH, &n.bits, &dot) == 6 && dot == '.')
        return done();
    n = {};
    if (std::sscanf(file, "%" SCNx64 "-r%ux%u-%08x%c", &n.texHash, &n.regionW, &n.regionH, &n.bits, &dot) == 5 && dot == '.')
        return done();
    n = {};
    if (std::sscanf(file, "%" SCNx64 "-%" SCNx64 "-r%llx-%08x%c", &n.texHash, &n.clutHash, &region, &n.bits, &dot) == 5 && dot == '.')
    {
        n.regionW = static_cast<uint32_t>(maxX(region) - minX(region));
        n.regionH = static_cast<uint32_t>(maxY(region) - minY(region));
        return done();
    }
    n = {};
    if (std::sscanf(file, "%" SCNx64 "-r%llx-%08x%c", &n.texHash, &region, &n.bits, &dot) == 4 && dot == '.')
    {
        n.regionW = static_cast<uint32_t>(maxX(region) - minX(region));
        n.regionH = static_cast<uint32_t>(maxY(region) - minY(region));
        return done();
    }
    n = {};
    if (std::sscanf(file, "%" SCNx64 "-%" SCNx64 "-%08x%c", &n.texHash, &n.clutHash, &n.bits, &dot) == 4 && dot == '.')
        return done();
    n = {};
    if (std::sscanf(file, "%" SCNx64 "-%08x%c", &n.texHash, &n.bits, &dot) == 3 && dot == '.')
        return done();
    return false;
}

// SourceRegion::Create (CLAMP region modes only; PCSX2's transfer-size guess for CLAMP/CLAMP draws is not reproduced)
uint64_t gsrepl::regionFromClamp(const GSTex0Reg &tex0, uint64_t clamp)
{
    const uint32_t wms = clamp & 3u, wmt = (clamp >> 2) & 3u;
    const uint32_t minu = (clamp >> 4) & 0x3FFu, maxu = (clamp >> 14) & 0x3FFu, minv = (clamp >> 24) & 0x3FFu, maxv = (clamp >> 34) & 0x3FFu;
    const uint32_t tw = 1u << (tex0.tw & 15u), th = 1u << (tex0.th & 15u);
    uint64_t r = 0;
    if (wms == 2u && maxu >= minu)
    {
        if (maxu - minu + 1u < tw || maxu >= tw)
            setX(r, static_cast<int32_t>(minu), static_cast<int32_t>(maxu + 1u));
    }
    else if (wms == 3u && minu != 0u)
    {
        const uint32_t rw = ((minu | maxu) - maxu) + 1u;
        if (rw < tw || (maxu != 0u && rw <= tw))
            setX(r, static_cast<int32_t>(maxu), static_cast<int32_t>((minu | maxu) + 1u));
    }
    if (wmt == 2u && maxv >= minv)
    {
        if (maxv - minv + 1u < th || maxv >= th)
            setY(r, static_cast<int32_t>(minv), static_cast<int32_t>(maxv + 1u));
    }
    else if (wmt == 3u && minv != 0u)
    {
        const uint32_t rh = ((minv | maxv) - maxv) + 1u;
        if (rh < th || (maxv != 0u && rh <= th))
            setY(r, static_cast<int32_t>(maxv), static_cast<int32_t>((minv | maxv) + 1u));
    }
    return r;
}

void gsrepl::appendLevel(const uint8_t *vram, uint32_t tbp0, uint32_t tbw, uint32_t psm, uint32_t twLog, uint32_t thLog,
                         const GSTexaReg &texa, uint64_t region, std::vector<uint8_t> &out)
{
    const Psm p = psmInfo(psm);
    if (!p.valid)
        return;
    // GS textures are at most 1024x1024 (TW/TH > 10 are invalid; PCSX2 clamps them too): no multi-GB hash buffers
    twLog = std::min(twLog, 10u);
    thLog = std::min(thLog, 10u);
    const int32_t tw = hasX(region) ? maxX(region) - minX(region) : (1 << twLog);
    const int32_t th = hasY(region) ? maxY(region) - minY(region) : (1 << thLog);
    // GetRect, then aligned outwards to the block size (ralign<Align_Outside>(bs))
    const int32_t rl = hasX(region) ? minX(region) : 0, rt = hasY(region) ? minY(region) : 0;
    const int32_t rr = hasX(region) ? maxX(region) : tw, rb = hasY(region) ? maxY(region) : th;
    const int32_t bx = static_cast<int32_t>(p.bsX), by = static_cast<int32_t>(p.bsY);
    const auto down = [](int32_t v, int32_t a) { return v >= 0 ? v / a * a : -((-v + a - 1) / a * a); };
    const int32_t bl = down(rl, bx), bt = down(rt, by), br = down(rr + bx - 1, bx), bb = down(rb + by - 1, by);
    if (tw < bx || th < by || !p.fullMask || maxX(region) > 0 || minY(region) > 0)
    {
        // expanded texels of the block rectangle in a buffer with PCSX2's pitch (VectorAlign: 32 bytes), then rows of tw
        const bool pal = p.pal > 0u;
        const uint32_t bpp = pal ? 1u : 4u;
        const uint32_t pitch = ((static_cast<uint32_t>(std::max(br, 0)) * bpp) + 31u) & ~31u;
        const uint32_t rows = static_cast<uint32_t>(std::max(bb - bt, 0));
        std::vector<uint8_t> tmp(static_cast<size_t>(pitch) * rows + 64u, 0u);
        for (int32_t y = bt; y < bb; ++y)
            for (int32_t x = bl; x < br; ++x)
            {
                if (x < 0 || y < 0)
                    continue;
                const uint32_t v = expandedTexel(vram, psm, tbp0, tbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y), texa);
                uint8_t *d = tmp.data() + static_cast<size_t>(y - bt) * pitch + static_cast<size_t>(x - bl) * bpp;
                if (pal)
                    *d = static_cast<uint8_t>(v);
                else
                    std::memcpy(d, &v, 4u);
            }
        // PCSX2 offsets the start by (rect.left - block_rect.left) bytes, unscaled, for both layouts
        const size_t start = static_cast<size_t>(pitch) * static_cast<size_t>(rt - bt) + static_cast<size_t>(rl - bl);
        const size_t rowSize = static_cast<size_t>(std::max(tw, 0)) * bpp;
        for (int32_t y = 0; y < th; ++y)
        {
            const size_t at = start + static_cast<size_t>(y) * pitch;
            if (at + rowSize > tmp.size())
                break;
            out.insert(out.end(), tmp.begin() + static_cast<ptrdiff_t>(at), tmp.begin() + static_cast<ptrdiff_t>(at + rowSize));
        }
        return;
    }
    // raw blocks in GSOffset::BNHelper order: rows of blocks, each row left to right, wrapping at 16384 blocks
    const int32_t bwPg = static_cast<int32_t>(tbw >> (p.pgShX - 6u));
    const int32_t pageMaskX = (1 << (p.pgShX - p.blShX)) - 1, pageMaskY = (1 << (p.pgShY - p.blShY)) - 1;
    int32_t baseBp = static_cast<int32_t>(tbp0) + ((bt >> (p.pgShY - 5)) & ~0x1f) * bwPg + ((bl >> (p.pgShX - 5)) & ~0x1f);
    const int32_t right = br >> p.blShX, bottom = bb >> p.blShY;
    for (int32_t y = bt >> p.blShY; y < bottom;)
    {
        int32_t bp = baseBp;
        for (int32_t x = bl >> p.blShX; x < right;)
        {
            const uint32_t block = static_cast<uint32_t>(bp + static_cast<int32_t>(swizzle(p, static_cast<uint32_t>(x), static_cast<uint32_t>(y)))) % 16384u;
            out.insert(out.end(), vram + block * 256u, vram + block * 256u + 256u);
            if (!(++x & pageMaskX))
                bp += 32;
        }
        if (!(++y & pageMaskY))
            baseBp += 32 * bwPg;
    }
}
