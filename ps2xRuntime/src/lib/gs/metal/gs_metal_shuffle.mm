// GOW-Port: channel shuffles of GSMetalBackend, port of the "channel shuffles" section of gs_hw_backend.cpp (kShuffleFs
// explains the GS side: 8 px sprites through a 16 bit view of a 32 bit target are halfword moves between owner pixels).
#include "gs_metal_impl.h"

using namespace gsmtl;

namespace
{
    uint32_t fbmsk16(uint32_t m)
    {
        return ((m >> 3) & 0x1Fu) | ((m >> 6) & 0x3E0u) | ((m >> 9) & 0x7C00u) | ((m >> 16) & 0x8000u);
    }
}

const std::vector<uint16_t> &GSMetalBackend::Impl::inverseFor(uint32_t ownerPsm)
{
    std::vector<uint16_t> &inv = ownerInverse[ownerPsm];
    if (inv.empty())
    {
        const GSSwizzle::Format &f = GSSwizzle::GetFormat(ownerPsm);
        inv.assign(2048u, 0);
        for (uint32_t e = 0; e < 2048u; ++e)
            inv[f.table[e]] = static_cast<uint16_t>(e);
    }
    return inv;
}

// pixel (x, y) of a 16 bit view (psm/base/bw) -> pixel and halfword of the 32 bit owner laid out as ownerPsm at fbp/fbw
bool GSMetalBackend::Impl::viewToOwner(uint32_t viewPsm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t ownerPsm,
                                       uint32_t fbp, uint32_t fbw, uint32_t &ox, uint32_t &oy, uint32_t &half)
{
    const uint32_t byte = GSSwizzle::Locate(GSSwizzle::GetFormat(viewPsm), base, bw, x, y).byte;
    const uint32_t word = byte >> 2, first = fbp * 2048u;
    if (word < first)
        return false;
    const uint32_t rel = word - first, page = rel >> 11u, e = inverseFor(ownerPsm)[rel & 2047u];
    half = (byte >> 1) & 1u;
    ox = (page % fbw) * 64u + (e & 63u);
    oy = (page / fbw) * 32u + (e >> 6u);
    return true;
}

// an 8x8 unit of the view at (x, y) -> an 8x8 owner block at (ox, oy), one halfword (checked at the far corner)
bool GSMetalBackend::Impl::unitToOwner(uint32_t viewPsm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t ownerPsm,
                                       uint32_t fbp, uint32_t fbw, uint32_t &ox, uint32_t &oy, uint32_t &half)
{
    uint32_t ex, ey, eh;
    return viewToOwner(viewPsm, base, bw, x, y, ownerPsm, fbp, fbw, ox, oy, half) &&
           viewToOwner(viewPsm, base, bw, x + 7u, y + 7u, ownerPsm, fbp, fbw, ex, ey, eh) && eh == half && ex == ox + 7u && ey == oy + 7u;
}

bool GSMetalBackend::Impl::tryShuffle(const GSDrawState &s, const GSPrimitiveBatch &b)
{
    const GSContext &c = s.context;
    const uint8_t tpsm = c.tex0.psm;
    const bool zTex = tpsm == 0x32 || tpsm == 0x3A;
    if (s.prim.type != GS_PRIM_SPRITE || !s.prim.tme || !s.prim.fst || s.prim.abe || !(family16(tpsm) || zTex))
        { shuffleWhy = 1; return false; }
    if ((c.test & 1u) != 0u && ((c.test >> 1) & 7u) != 1u)
        { shuffleWhy = 2; return false; }
    if (((c.test >> 14) & 1u) != 0u)
        { shuffleWhy = 3; return false; }
    {
        const GSVertex &v = b.vertices[1];
        const bool identity = c.tex0.tfx == 1 || (c.tex0.tfx == 0 && v.r == 128 && v.g == 128 && v.b == 128 && (!c.tex0.tcc || v.a == 128));
        if (!identity)
            { shuffleWhy = 4; return false; }
    }
    const uint32_t m16 = fbmsk16(c.frame.fbmsk);
    const uint32_t lowByte = m16 & 0xFFu, highByte = m16 & 0xFF00u;
    if ((lowByte != 0u && lowByte != 0xFFu) || (highByte != 0u && highByte != 0xFF00u) || m16 == 0xFFFFu)
        { shuffleWhy = 5; return false; }
    RenderTarget *dst = findTarget(c.frame.fbp);
    if (dst && family16(dst->psm) && dst->usedW != 0u)
        { shuffleWhy = 6; return false; }
    if ((c.tex0.tbp0 % 32u) != 0u)
        { shuffleWhy = 7; return false; }
    RenderTarget *src = nullptr;
    bool srcDepth = false;
    if (zTex)
        for (RenderTarget &t : targets)
            if (t.depth && t.zbp == c.tex0.tbp0 / 32u && t.fbp != t.zbp)
            {
                src = &t;
                srcDepth = true;
            }
    if (!src)
        if (RenderTarget *t = findTarget(c.tex0.tbp0 / 32u))
            if (!family16(t->psm) && t->usedW != 0u)
                src = t;
    if (!src)
        { shuffleWhy = 8; return false; }
    const GSVertex &a = b.vertices[0], &e = b.vertices[1];
    const float ofx = c.xyoffset.ofx / 16.0f, ofy = c.xyoffset.ofy / 16.0f;
    const float fx0 = std::min(a.x, e.x) - ofx, fx1 = std::max(a.x, e.x) - ofx, fy0 = std::min(a.y, e.y) - ofy, fy1 = std::max(a.y, e.y) - ofy;
    const auto whole = [](float v) { return std::fabs(v - std::round(v)) < 1e-3f && v >= 0.0f; };
    if (!whole(fx0) || !whole(fx1) || !whole(fy0) || !whole(fy1))
        { shuffleWhy = 9; return false; }
    const int x0 = static_cast<int>(std::lround(fx0)), x1 = static_cast<int>(std::lround(fx1));
    const int y0 = static_cast<int>(std::lround(fy0)), y1 = static_cast<int>(std::lround(fy1));
    const GSVertex &left = (a.x <= e.x) ? a : e;
    const GSVertex &top = (a.y <= e.y) ? a : e;
    const int du = static_cast<int>(std::floor(left.u / 16.0f)) - x0, dv = static_cast<int>(std::floor(top.v / 16.0f)) - y0;
    const int uSpan = static_cast<int>(std::lround(std::fabs(static_cast<float>(a.u) - static_cast<float>(e.u)) / 16.0f));
    const int vSpan = static_cast<int>(std::lround(std::fabs(static_cast<float>(a.v) - static_cast<float>(e.v)) / 16.0f));
    if (((x0 | x1 | y0 | y1 | du | dv) & 7) != 0 || x1 <= x0 || y1 <= y0 || uSpan != x1 - x0 || vSpan != y1 - y0)
        { shuffleWhy = 10; return false; }
    if (x0 + du < 0 || y0 + dv < 0 || x0 < static_cast<int>(c.scissor.x0) || y0 < static_cast<int>(c.scissor.y0) ||
        x1 - 1 > static_cast<int>(c.scissor.x1) || y1 - 1 > static_cast<int>(c.scissor.y1))
        { shuffleWhy = 11; return false; }
    if (!dst)
    {
        // creating a target may move the others: nothing queued may point at them, src is found again
        flushShuffle();
        const size_t srcIndex = static_cast<size_t>(src - targets.data());
        dst = &target(c.frame.fbp, c.frame.fbw);
        src = &targets[srcIndex];
    }
    const uint32_t srcOwnerPsm = srcDepth ? 0x30u : 0x00u, srcFbp = srcDepth ? src->zbp : src->fbp;
    std::vector<ShuffleVertex> quads[4];
    uint32_t maxX = 0, maxY = 0;
    for (int y = y0; y < y1; y += 8)
        for (int x = x0; x < x1; x += 8)
        {
            uint32_t dx, dy, hd, sx, sy, hs;
            if (!unitToOwner(c.frame.psm, c.frame.fbp * 32u, c.frame.fbw, x, y, 0x00u, dst->fbp, dst->fbw, dx, dy, hd) ||
                !unitToOwner(tpsm, c.tex0.tbp0, c.tex0.tbw, x + du, y + dv, srcOwnerPsm, srcFbp, src->fbw, sx, sy, hs))
                { shuffleWhy = 12; return false; }
            if (dx + 8u > dst->pxW || dy + 8u > dst->pxH || sx + 8u > src->pxW || sy + 8u > src->pxH)
                { shuffleWhy = 13; return false; }
            if (src == dst && !srcDepth && (sx != dx || sy != dy))
                { shuffleWhy = 14; return false; }
            const float ox = static_cast<float>(sx) - static_cast<float>(dx), oy = static_cast<float>(sy) - static_cast<float>(dy);
            const float X0 = static_cast<float>(dx), Y0 = static_cast<float>(dy), X1 = X0 + 8.0f, Y1 = Y0 + 8.0f;
            std::vector<ShuffleVertex> &q = quads[hs * 2u + hd];
            q.insert(q.end(), {{X0, Y0, ox, oy}, {X1, Y0, ox, oy}, {X0, Y1, ox, oy}, {X1, Y0, ox, oy}, {X1, Y1, ox, oy}, {X0, Y1, ox, oy}});
            maxX = std::max(maxX, dx + 8u);
            maxY = std::max(maxY, dy + 8u);
        }
    if (havePending)
        flushPending();
    if (haveShuffle && (!sameState(shuffleState, s) || shuffleDst != dst || shuffleSrc != src || shuffleSrcZ != srcDepth))
        flushShuffle();
    if (!haveShuffle)
    {
        haveShuffle = true;
        shuffleState = s;
        shuffleDst = dst;
        shuffleSrc = src;
        shuffleSrcZ = srcDepth;
    }
    for (int i = 0; i < 4; ++i)
        shuffleQuads[i].insert(shuffleQuads[i].end(), quads[i].begin(), quads[i].end());
    dst->usedW = std::max(dst->usedW, std::min(maxX, dst->pxW));
    dst->usedH = std::max(dst->usedH, std::min(maxY, dst->pxH));
    ++stats.shuffles;
    return true;
}

void GSMetalBackend::Impl::flushShuffle()
{
    if (!haveShuffle)
        return;
    haveShuffle = false;
    RenderTarget &dst = *shuffleDst, &src = *shuffleSrc;
    const GSContext &c = shuffleState.context;
    ++dst.version;
    dst.lastDrawSeq = ++writeSeq;
    const uint32_t m16 = fbmsk16(c.frame.fbmsk);
    const bool lo = (m16 & 0xFFu) == 0u, hi = (m16 & 0xFF00u) == 0u;
    id<MTLTexture> source = sampleable(shuffleSrcZ ? src.depth : src.color, dst);
    beginPass(dst);
    [enc setScissorRect:MTLScissorRect{0, 0, dst.glW, dst.glH}];
    if (dst.depth)
        setDss(depthState(kAlways, false));
    ShUniforms u{};
    u.srcZ = shuffleSrcZ ? 1 : 0;
    u.zMax = zMaxFor(c.zbuf.psm);
    u.scaleX = scaleX;
    u.scaleY = scaleY;
    u.texa[0] = shuffleState.texa.ta0 >> 7;
    u.texa[1] = shuffleState.texa.ta1 >> 7;
    u.texa[2] = shuffleState.texa.aem ? 1 : 0;
    const VsUniforms vu{1.0f / dst.pxW, 1.0f / dst.pxH};
    [enc setVertexBytes:&vu length:sizeof(vu) atIndex:1];
    [enc setFragmentTexture:shuffleSrcZ ? dummyColor : source atIndex:0];
    [enc setFragmentTexture:shuffleSrcZ ? source : dummyDepth atIndex:1];
    for (uint32_t i = 0; i < 4; ++i)
    {
        std::vector<ShuffleVertex> &q = shuffleQuads[i];
        if (q.empty())
            continue;
        u.hs = static_cast<int32_t>(i >> 1);
        u.hd = static_cast<int32_t>(i & 1u);
        const uint8_t mask = u.hd == 0 ? static_cast<uint8_t>((lo ? kMaskR : 0) | (hi ? kMaskG : 0))
                                       : static_cast<uint8_t>((lo ? kMaskB : 0) | (hi ? kMaskA : 0));
        setPso(pipelineFor(Pso::Shuffle, mask, dst.depth != nil));
        NSUInteger offset = 0;
        id<MTLBuffer> vb = upload(q.data(), q.size() * sizeof(ShuffleVertex), offset);
        if (!vb) // out of memory (upload logs it)
        {
            q.clear();
            continue;
        }
        [enc setVertexBuffer:vb offset:offset atIndex:0];
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:q.size()];
        q.clear();
    }
    ++stats.batches;
}
