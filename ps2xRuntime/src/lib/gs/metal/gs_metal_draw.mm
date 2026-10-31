// GOW-Port: drawing in GSMetalBackend: primitives to triangles (lines, sprites with their sampled texel range and strip
// seams), batching by draw state, GS blend / depth / alpha test state, the Z-inversion tracking of the shadow passes and
// the draw itself. Port of the "drawing" section of gs_hw_backend.cpp; the comments on GS behaviour are kept there.
#include "gs_metal_impl.h"

using namespace gsmtl;

void GSMetalBackend::Impl::openSharedSide(const SpriteEdges &prev, const SpriteEdges &cur, HwVertex *prevVertices, float &cu0,
                                          float &cu1, float &cv0, float &cv1)
{
    const auto same = [](const float a[2], const float b[2], float tol)
    {
        return std::fabs(std::min(a[0], a[1]) - std::min(b[0], b[1])) <= tol && std::fabs(std::max(a[0], a[1]) - std::max(b[0], b[1])) <= tol;
    };
    constexpr float kXyTol = 1.0f / 64.0f, kUvTol = 1.0f / 32.0f;
    const auto seam = [&](const float p[2], const float pt[2], const float c[2], const float ct[2], int &ip, int &ic)
    {
        for (ip = 0; ip < 2; ++ip)
            for (ic = 0; ic < 2; ++ic)
                if (std::fabs(p[ip] - c[ic]) <= kXyTol && std::fabs(pt[ip] - ct[ic]) <= kUvTol && (p[1 - ip] - p[ip]) * (c[1 - ic] - c[ic]) < 0.0f)
                    return true;
        return false;
    };
    int ip = 0, ic = 0;
    if (same(prev.ys, cur.ys, kXyTol) && same(prev.vs, cur.vs, kUvTol) && seam(prev.xs, prev.us, cur.xs, cur.us, ip, ic))
    {
        const bool prevHigh = prev.us[ip] > prev.us[1 - ip], curHigh = cur.us[ic] > cur.us[1 - ic];
        for (int i = 0; i < 6; ++i)
            (prevHigh ? prevVertices[i].cu1 : prevVertices[i].cu0) = prevHigh ? 1e30f : -1e30f;
        (curHigh ? cu1 : cu0) = curHigh ? 1e30f : -1e30f;
    }
    else if (same(prev.xs, cur.xs, kXyTol) && same(prev.us, cur.us, kUvTol) && seam(prev.ys, prev.vs, cur.ys, cur.vs, ip, ic))
    {
        const bool prevHigh = prev.vs[ip] > prev.vs[1 - ip], curHigh = cur.vs[ic] > cur.vs[1 - ic];
        for (int i = 0; i < 6; ++i)
            (prevHigh ? prevVertices[i].cv1 : prevVertices[i].cv0) = prevHigh ? 1e30f : -1e30f;
        (curHigh ? cv1 : cv0) = curHigh ? 1e30f : -1e30f;
    }
}

bool GSMetalBackend::Impl::sameState(const GSDrawState &a, const GSDrawState &b)
{
    return GSMetalBackend::SameDrawState(a, b);
}

bool GSMetalBackend::SameDrawState(const GSDrawState &a, const GSDrawState &b)
{
    const GSContext &x = a.context, &y = b.context;
    const GSTex0Reg &t = x.tex0, &u = y.tex0;
    const GSPrimReg &p = a.prim, &q = b.prim;
    return x.frame.fbp == y.frame.fbp && x.frame.fbw == y.frame.fbw && x.frame.psm == y.frame.psm && x.frame.fbmsk == y.frame.fbmsk &&
           x.scissor.x0 == y.scissor.x0 && x.scissor.x1 == y.scissor.x1 && x.scissor.y0 == y.scissor.y0 && x.scissor.y1 == y.scissor.y1 &&
           t.tbp0 == u.tbp0 && t.tbw == u.tbw && t.psm == u.psm && t.tw == u.tw && t.th == u.th && t.tcc == u.tcc && t.tfx == u.tfx &&
           t.cbp == u.cbp && t.cpsm == u.cpsm && t.csm == u.csm && t.csa == u.csa && t.cld == u.cld &&
           x.xyoffset.ofx == y.xyoffset.ofx && x.xyoffset.ofy == y.xyoffset.ofy &&
           x.zbuf.zbp == y.zbuf.zbp && x.zbuf.psm == y.zbuf.psm && x.zbuf.zmask == y.zbuf.zmask &&
           x.tex1 == y.tex1 && x.miptbp1 == y.miptbp1 && x.miptbp2 == y.miptbp2 && x.clamp == y.clamp && x.alpha == y.alpha &&
           x.test == y.test && x.fba == y.fba &&
           p.type == q.type && p.iip == q.iip && p.tme == q.tme && p.fge == q.fge && p.abe == q.abe && p.aa1 == q.aa1 && p.fst == q.fst &&
           p.ctxt == q.ctxt && p.fix == q.fix &&
           a.texa.ta0 == b.texa.ta0 && a.texa.aem == b.texa.aem && a.texa.ta1 == b.texa.ta1 &&
           a.texclut.cbw == b.texclut.cbw && a.texclut.cou == b.texclut.cou && a.texclut.cov == b.texclut.cov &&
           a.pabe == b.pabe && a.scanmsk == b.scanmsk && a.dimx == b.dimx && a.dthe == b.dthe && a.colclamp == b.colclamp &&
           a.fogR == b.fogR && a.fogG == b.fogG && a.fogB == b.fogB && a.textureWidth == b.textureWidth &&
           a.textureHeight == b.textureHeight && a.linearFilter == b.linearFilter;
}

void GSMetalBackend::Impl::addVertex(const GSDrawState &s, const GSVertex &v, float u, float tv, float zmax)
{
    const GSContext &c = s.context;
    // half a HOST pixel: the PS2 sample point of pixel x lands on the centre of host pixel scale * x
    const float half = 0.5f / scale;
    HwVertex h;
    h.x = v.x - static_cast<float>(c.xyoffset.ofx) / 16.0f + half;
    h.y = v.y - static_cast<float>(c.xyoffset.ofy) / 16.0f + half;
    double zq = std::max(v.z, 0.0);
    if (zmax < 4294967295.0f)
        zq = static_cast<double>(static_cast<uint64_t>(zq) & (zmax < 70000.0f ? 0xFFFFull : 0xFFFFFFull));
    h.z = static_cast<float>(std::min(zq / zmax, 1.0));
    h.r = v.r; h.g = v.g; h.b = v.b; h.a = v.a;
    h.s = v.s; h.t = v.t; h.q = v.q;
    h.u = u; h.v = tv;
    pending.push_back(h);
}

void GSMetalBackend::Impl::submit(const GSPrimitiveBatch &b)
{
    const GSDrawState &s = b.state;
    const GSPrimType type = s.prim.type;
    const bool tri = type == GS_PRIM_TRIANGLE || type == GS_PRIM_TRISTRIP || type == GS_PRIM_TRIFAN;
    const bool sprite = type == GS_PRIM_SPRITE;
    const bool line = type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP;
    if (!tri && !sprite && !line)
    {
        ++stats.skippedPrim;
        return;
    }
    const uint8_t fpsm = s.context.frame.psm;
    if (family16(fpsm) && tryShuffle(s, b))
        return;
    flushShuffle();
    if (fpsm != GS_PSM_CT32 && fpsm != GS_PSM_CT24 && fpsm != GS_PSM_CT16 && fpsm != GS_PSM_CT16S)
    {
        ++stats.skippedPsm;
        return;
    }
    for (const RenderTarget &t : targets)
        if (t.fbp == s.context.frame.fbp && t.usedW != 0u && family16(t.psm) != family16(fpsm))
        {
            ++stats.skippedPsm;
            return;
        }
    if (havePending && !sameState(pendingState, s))
        flushPending();
    if (!havePending)
    {
        pendingState = s;
        havePending = true;
    }
    const float zmax = zMaxFor(s.context.zbuf.psm);
    const float texW = static_cast<float>(std::max<uint16_t>(s.textureWidth, 1)), texH = static_cast<float>(std::max<uint16_t>(s.textureHeight, 1));
    const auto texelUV = [&](const GSVertex &v, float &u, float &t)
    {
        u = s.prim.fst ? static_cast<float>(v.u) / 16.0f : 0.0f;
        t = s.prim.fst ? static_cast<float>(v.v) / 16.0f : 0.0f;
    };
    if (line)
    {
        // a GS line as a quad one PS2 pixel wide around the segment (at least one pixel long)
        const GSVertex &a = b.vertices[0], &c = b.vertices[1];
        float dx = c.x - a.x, dy = c.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f)
            dx = 1.0f, dy = 0.0f;
        else
            dx /= len, dy /= len;
        const float ext = len < 1.0f ? (1.0f - len) * 0.5f : 0.0f;
        const float nx = -dy * 0.5f, ny = dx * 0.5f;
        GSVertex e[2] = {a, c};
        e[0].x -= dx * ext; e[0].y -= dy * ext;
        e[1].x += dx * ext; e[1].y += dy * ext;
        const int order[6][2] = {{0, 1}, {1, 1}, {0, -1}, {1, 1}, {1, -1}, {0, -1}};
        const float centre = 0.5f - 0.5f / scale;
        for (const auto &o : order)
        {
            GSVertex v = e[o[0]];
            v.x += nx * static_cast<float>(o[1]) + centre * std::fabs(dy);
            v.y += ny * static_cast<float>(o[1]) + centre * std::fabs(dx);
            float u, t;
            texelUV(v, u, t);
            addVertex(s, v, u, t, zmax);
        }
    }
    else if (tri)
    {
        for (int i = 0; i < 3; ++i)
        {
            float u, t;
            texelUV(b.vertices[i], u, t);
            addVertex(s, b.vertices[i], u, t, zmax);
        }
    }
    else
    {
        const GSVertex &a = b.vertices[0];
        const GSVertex &c = b.vertices[1];
        float u0, v0, u1, v1;
        if (s.prim.fst)
        {
            u0 = a.u / 16.0f; v0 = a.v / 16.0f; u1 = c.u / 16.0f; v1 = c.v / 16.0f;
        }
        else
        {
            const float q0 = std::fabs(a.q) < 1e-20f ? 1e-20f : a.q, q1 = std::fabs(c.q) < 1e-20f ? 1e-20f : c.q;
            u0 = a.s / q0 * texW; v0 = a.t / q0 * texH; u1 = c.s / q1 * texW; v1 = c.t / q1 * texH;
        }
        const float xs[2] = {a.x, c.x}, ys[2] = {a.y, c.y};
        const float us[2] = {u0, u1}, vs[2] = {v0, v1};
        // texel coordinates of the first and last pixel the GS covers (pixel n is covered when lo <= n < hi)
        const auto sampled = [](float p0, float p1, float t0, float t1, float &lo, float &hi)
        {
            lo = std::min(t0, t1);
            hi = std::max(t0, t1);
            const float first = std::ceil(std::min(p0, p1)), last = std::ceil(std::max(p0, p1)) - 1.0f;
            if (last < first || p1 == p0)
                return;
            const float ta = t0 + (first - p0) * (t1 - t0) / (p1 - p0), tb = t0 + (last - p0) * (t1 - t0) / (p1 - p0);
            lo = std::min(ta, tb);
            hi = std::max(ta, tb);
        };
        float cu0, cu1, cv0, cv1;
        sampled(a.x - s.context.xyoffset.ofx / 16.0f, c.x - s.context.xyoffset.ofx / 16.0f, u0, u1, cu0, cu1);
        sampled(a.y - s.context.xyoffset.ofy / 16.0f, c.y - s.context.xyoffset.ofy / 16.0f, v0, v1, cv0, cv1);
        SpriteEdges cur{{xs[0], xs[1]}, {ys[0], ys[1]}, {us[0], us[1]}, {vs[0], vs[1]}};
        if (lastSprite.batch == batchSerial && lastSprite.end == pending.size() && lastSprite.end >= 6)
            openSharedSide(lastSprite.edges, cur, &pending[lastSprite.end - 6], cu0, cu1, cv0, cv1);
        const int order[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
        for (const auto &o : order)
        {
            GSVertex v = c; // colour, z, fog come from the second vertex
            v.x = xs[o[0]];
            v.y = ys[o[1]];
            addVertex(s, v, us[o[0]], vs[o[1]], zmax);
            HwVertex &h = pending.back();
            h.s = us[o[0]] / texW;
            h.t = vs[o[1]] / texH;
            h.q = 1.0f;
            h.cu0 = cu0;
            h.cv0 = cv0;
            h.cu1 = cu1;
            h.cv1 = cv1;
        }
        lastSprite = {cur, pending.size(), batchSerial};
    }
    ++stats.draws;
}

void GSMetalBackend::Impl::flushPending()
{
    ++batchSerial;
    flushShuffle();
    if (!havePending || pending.empty())
    {
        havePending = false;
        pending.clear();
        return;
    }
    havePending = false;
    if (!ensure())
    {
        pending.clear();
        return;
    }
    draw(pendingState, pending);
    pending.clear();
}

// GS blend -> fixed-function blend when it can express it, else `soft` (done in the shader on the fetched destination)
void GSMetalBackend::Impl::blendState(const GSDrawState &s, BlendDesc &blend, float &constant, bool &soft)
{
    blend = {};
    soft = false;
    constant = 0.0f;
    if (!s.prim.abe)
        return;
    const uint64_t alpha = s.context.alpha;
    const uint32_t a = alpha & 3, b = (alpha >> 2) & 3, c = (alpha >> 4) & 3, d = (alpha >> 6) & 3;
    const uint32_t fix = static_cast<uint32_t>((alpha >> 32) & 0xFF);
    uint8_t cs, cd; // factor C and 1-C
    if (c == 0) { cs = kSrc1Alpha; cd = kOneMinusSrc1Alpha; }
    else if (c == 2) { cs = kConstant; cd = kOneMinusConstant; constant = std::min(fix / 128.0f, 1.0f); }
    else { soft = true; cs = kSrc1Alpha; cd = kOneMinusSrc1Alpha; } // C = Ad
    // COLCLAMP=0: results wrap around instead of saturating (the shadow-volume counting passes)
    if ((s.colclamp & 1u) == 0u && a != b)
        soft = true;
    if (a == b)
    {
        if (d == 0) return;                                       // Cs: plain write
        blend.enabled = true; blend.sf = kZero; blend.df = kOne; return; // Cd: destination unchanged
    }
    blend.enabled = true;
    if (a == 0 && b == 1 && d == 1) { blend.sf = cs; blend.df = cd; }
    else if (a == 0 && b == 2 && d == 1) { blend.sf = cs; blend.df = kOne; }
    else if (a == 2 && b == 1 && d == 1) { blend.sf = kZero; blend.df = cd; }
    else if (a == 0 && b == 2 && d == 2) { blend.sf = cs; blend.df = kZero; }
    else if (a == 2 && b == 0 && d == 2) { blend.sf = kZero; blend.df = kZero; }
    else if (a == 2 && b == 0 && d == 1) { blend.sf = cs; blend.df = kOne; blend.reverse = true; }
    else { soft = true; blend.sf = cs; blend.df = cd; }
    if (soft)
        ++stats.unsupportedBlend;
}

// a full-screen Cs - Cd (Cs = 255) colour draw into the Z memory: the shadow passes invert the Z buffer
bool GSMetalBackend::Impl::isZInversion(const GSDrawState &s, const std::vector<HwVertex> &verts)
{
    const GSContext &c = s.context;
    const uint64_t al = c.alpha;
    if (c.frame.fbp != c.zbuf.zbp || c.frame.psm != GS_PSM_CT24 || s.prim.tme || !s.prim.abe || c.frame.fbmsk != 0u ||
        (al & 0xFFu) != 0xA4u || ((al >> 32) & 0xFFu) != 0x80u || verts.empty())
        return false;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const HwVertex &v : verts)
    {
        if (v.r < 254.5f || v.g < 254.5f || v.b < 254.5f)
            return false;
        x0 = std::min(x0, v.x), y0 = std::min(y0, v.y), x1 = std::max(x1, v.x), y1 = std::max(y1, v.y);
    }
    return x0 <= c.scissor.x0 + 1.0f && y0 <= c.scissor.y0 + 1.0f && x1 >= c.scissor.x1 && y1 >= c.scissor.y1;
}

void GSMetalBackend::Impl::draw(const GSDrawState &s, const std::vector<HwVertex> &verts)
{
    ++frameBatch;
    const GSContext &c = s.context;
    // GOW_GS_HW_SKIPTBP=<hex>: debug, skip draws textured from that block (bisecting post passes)
    static const long skipTbp = std::getenv("GOW_GS_HW_SKIPTBP") ? std::strtol(std::getenv("GOW_GS_HW_SKIPTBP"), nullptr, 16) : -1;
    if (skipTbp >= 0 && s.prim.tme && static_cast<long>(c.tex0.tbp0) == skipTbp)
        return;
    RenderTarget &rt = target(c.frame.fbp, c.frame.fbw);
    attachDepth(rt, c.zbuf.zbp);
    rt.psm = c.frame.psm;
    ++rt.version;
    rt.lastDrawSeq = ++writeSeq;
    {
        float maxX = 0.0f, maxY = 0.0f;
        for (const HwVertex &v : verts)
            maxX = std::max(maxX, v.x), maxY = std::max(maxY, v.y);
        const uint32_t bx = static_cast<uint32_t>(std::min<float>(std::max(maxX, 0.0f), static_cast<float>(c.scissor.x1 + 1u)));
        const uint32_t by = static_cast<uint32_t>(std::min<float>(std::max(maxY, 0.0f), static_cast<float>(c.scissor.y1 + 1u)));
        rt.usedW = std::max<uint32_t>(rt.usedW, std::min<uint32_t>(bx, rt.pxW));
        rt.usedH = std::max<uint32_t>(rt.usedH, std::min<uint32_t>(by, rt.pxH));
    }

    // depth (ZTE = 0: ALWAYS); Z in this target's own colour memory: tested and written in the shader
    const uint32_t ztst = ((c.test >> 16) & 1u) ? static_cast<uint32_t>((c.test >> 17) & 3u) : 1u;
    static const bool noDepth = std::getenv("GOW_GS_HW_NODEPTH") != nullptr;
    const bool zAlias = c.zbuf.zbp == c.frame.fbp && ((c.test >> 16) & 1u) != 0u && (c.frame.psm == GS_PSM_CT32 || c.frame.psm == GS_PSM_CT24);
    const bool depthOn = !noDepth && !zAlias;
    const bool zInv = !zAlias && ((c.test >> 16) & 1u) != 0u && zInverted(c.zbuf.zbp);
    const Compare compare = zInv ? (ztst == 0 ? kNever : ztst == 1 ? kAlways : ztst == 2 ? kLEqual : kLess)
                                 : (ztst == 0 ? kNever : ztst == 1 ? kAlways : ztst == 2 ? kGEqual : kGreater);
    std::vector<HwVertex> flippedZ;
    if (zInv)
    {
        flippedZ = verts;
        for (HwVertex &v : flippedZ)
            v.z = 1.0f - v.z;
    }
    const std::vector<HwVertex> &dverts = zInv ? flippedZ : verts;
    if (isZInversion(s, verts))
    {
        if (zInverted(c.zbuf.zbp))
            invertedZ.erase(std::find(invertedZ.begin(), invertedZ.end(), c.zbuf.zbp));
        else
            invertedZ.push_back(c.zbuf.zbp);
    }
    const bool zwrite = !c.zbuf.zmask && !zAlias;
    if (zwrite && ((c.test >> 16) & 1u) != 0u)
        rt.lastDepthSeq = rt.lastDrawSeq;
    const bool zAliasWrite = zAlias && !c.zbuf.zmask;

    const uint32_t fbmsk = c.frame.fbmsk;
    const uint8_t mask = static_cast<uint8_t>(((fbmsk & 0xFF) != 0xFF ? kMaskR : 0) | ((fbmsk & 0xFF00) != 0xFF00 ? kMaskG : 0) |
                                              ((fbmsk & 0xFF0000) != 0xFF0000 ? kMaskB : 0) | ((fbmsk & 0xFF000000u) != 0xFF000000u ? kMaskA : 0));

    BlendDesc blend;
    float constant = 0.0f;
    bool softBlend = false;
    blendState(s, blend, constant, softBlend);
    // COLCLAMP=0 Cs*FIX + Cd with one flat untextured colour (shadow-volume counting): a constant add or subtract mod 256
    float wrapAmount[3] = {0.0f, 0.0f, 0.0f};
    bool wrapCount = false;
    if (softBlend && !zAliasWrite && (s.colclamp & 1u) == 0u && !s.prim.tme && !verts.empty())
    {
        const uint64_t al = c.alpha;
        const uint32_t a = al & 3u, b = (al >> 2) & 3u, cc = (al >> 4) & 3u, d = (al >> 6) & 3u, fix = static_cast<uint32_t>((al >> 32) & 0xFFu);
        if (a == 0u && b == 2u && cc == 2u && d == 1u)
        {
            const float col[3] = {verts[0].r, verts[0].g, verts[0].b};
            const bool written[3] = {(mask & kMaskR) != 0, (mask & kMaskG) != 0, (mask & kMaskB) != 0};
            bool flat = true;
            for (const HwVertex &v : verts)
                flat = flat && v.r == col[0] && v.g == col[1] && v.b == col[2];
            int direction = 0;
            for (int ch = 0; ch < 3 && flat; ++ch)
            {
                if (!written[ch])
                    continue;
                const uint32_t v = std::min<uint32_t>(255u, (static_cast<uint32_t>(col[ch]) * fix) >> 7);
                const int dir = v >= 128u ? -1 : 1;
                direction = direction == 0 || direction == dir ? dir : 2;
                wrapAmount[ch] = col[ch] > 0.0f ? static_cast<float>(dir < 0 ? 256u - v : v) / col[ch] : 0.0f;
            }
            if (flat && (direction == 1 || direction == -1))
            {
                wrapCount = true;
                softBlend = false;
                blend = {};
                blend.enabled = true;
                blend.sf = kConstant;
                blend.df = kOne;
                blend.reverse = direction < 0;
            }
        }
    }
    if (softBlend || zAliasWrite)
        blend.enabled = false;
    const bool date = ((c.test >> 14) & 1u) != 0u && c.frame.psm != GS_PSM_CT24;
    // the destination is read: framebuffer fetch (raster order groups keep overlapping fragments of one draw in order,
    // which the GL renderer got by drawing the wrapping passes one triangle at a time)
    const bool fetch = date || softBlend || zAlias;

    FsUniforms u{};
    u.date = date ? 1 : 0;
    u.datm = ((c.test >> 15) & 1u) != 0u ? 1 : 0;
    const uint64_t al = c.alpha;
    u.swBlend = softBlend && !zAliasWrite ? 1 : 0;
    u.colClamp = (s.colclamp & 1u) != 0u ? 1 : 0;
    u.abcd[0] = static_cast<int32_t>(al & 3u);
    u.abcd[1] = static_cast<int32_t>((al >> 2) & 3u);
    u.abcd[2] = static_cast<int32_t>((al >> 4) & 3u);
    u.abcd[3] = static_cast<int32_t>((al >> 6) & 3u);
    u.fix = static_cast<float>((al >> 32) & 0xFFu);
    u.zAlias = zAlias ? 1 + static_cast<int32_t>(ztst) : 0;
    u.zAliasWrite = zAliasWrite ? 1 : 0;
    u.zAliasMax = zMaxFor(c.zbuf.psm);
    u.tme = s.prim.tme ? 1 : 0;
    u.fst = s.prim.fst ? 1 : 0;
    u.iip = s.prim.iip ? 1 : 0;
    u.tfx = c.tex0.tfx;
    u.tcc = c.tex0.tcc;
    u.blendScale = 1.0f / 128.0f;
    u.blendColor[0] = wrapCount ? wrapAmount[0] : constant;
    u.blendColor[1] = wrapCount ? wrapAmount[1] : constant;
    u.blendColor[2] = wrapCount ? wrapAmount[2] : constant;
    u.blendColor[3] = wrapCount ? 0.0f : constant;

    id<MTLTexture> texture = dummyColor, ztexture = dummyDepth, rtColor = dummyColor;
    id<MTLBuffer> ipal = zeroBuffer;
    uint32_t rpal[256];
    bool hasRpal = false;
    bool linear = false, clampU = true, clampV = true;
    if (s.prim.tme)
    {
        // a texture that lives in a render target of the same format is sampled straight from it
        RenderTarget *src = nullptr;
        const auto family = [](uint8_t psm) { return (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S) ? 16 : (psm == GS_PSM_CT32 || psm == GS_PSM_CT24) ? 32 : 0; };
        if (family(c.tex0.psm) != 0 && (c.tex0.tbp0 % 32u) == 0u)
            if (RenderTarget *candidate = findTarget(c.tex0.tbp0 / 32u))
                if (family(candidate->psm) == family(c.tex0.psm) && !uploadedSinceDraw(*candidate, c.tex0.tbp0) && !overwrittenByDepth(*candidate))
                    src = candidate;
        RenderTarget *zsrc = nullptr;
        const bool zFormat = c.tex0.psm == 0x30 || c.tex0.psm == 0x31 || c.tex0.psm == 0x32 || c.tex0.psm == 0x3A;
        if (zFormat && (c.tex0.tbp0 % 32u) == 0u)
            for (RenderTarget &t : targets)
                if (t.depth && t.zbp == c.tex0.tbp0 / 32u && t.fbp != t.zbp)
                    zsrc = &t;
        if (zsrc)
            src = nullptr;
        RenderTarget *rsrc = nullptr;
        if (!src && !zsrc)
            rsrc = findReinterpretSource(c.tex0.tbp0, c.tex0.psm);
        const bool paletted = !(src || rsrc || zsrc) && isIndexed(c.tex0.psm) && vram != nullptr;
        Texture *tex = (src || rsrc || zsrc) ? nullptr : paletted ? &indexTextureFor(s) : &textureFor(s);
        // converted to RGBA with the CLUT (depalTextureFor): sampled like a direct-colour texture
        bool indexed = paletted;
        if (paletted && depalEnabled && tex->indices.size() == static_cast<size_t>(tex->width) * tex->height)
        {
            tex = &depalTextureFor(s, *tex);
            indexed = false;
        }
        u.indexed = indexed ? 1 : 0;
        u.linear = s.linearFilter ? 1 : 0;
        if (indexed)
        {
            ipal = paletteFor(s);
            u.wrapX = (c.clamp & 3u) == 0u ? 0 : 1;
            u.wrapY = ((c.clamp >> 2) & 3u) == 0u ? 0 : 1;
        }
        if (zsrc)
            ztexture = sampleable(zsrc->depth, rt);
        else if (src)
        {
            // what the draw can sample, in host pixels (vertex texel coordinates: perspective interpolation stays inside
            // their range), so the feedback copy covers only that (the screen-space distortion fans are small). A REPEAT axis
            // needs the footprint a texel inside the edges (a bilinear tap at the edge wraps to the far side); else everything.
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            const float tw = static_cast<float>(std::max<uint16_t>(s.textureWidth, 1)), th = static_cast<float>(std::max<uint16_t>(s.textureHeight, 1));
            for (const HwVertex &v : verts)
            {
                const float q = std::fabs(v.q) < 1e-20f ? 1e-20f : v.q;
                const float tu = s.prim.fst ? v.u : v.s / q * tw, tv = s.prim.fst ? v.v : v.t / q * th;
                lo[0] = std::min(lo[0], tu), lo[1] = std::min(lo[1], tv), hi[0] = std::max(hi[0], tu), hi[1] = std::max(hi[1], tv);
            }
            const float limit[2] = {static_cast<float>(src->pxW), static_cast<float>(src->pxH)};
            const bool clampAxis[2] = {(c.clamp & 3u) != 0u, ((c.clamp >> 2) & 3u) != 0u};
            bool usable = true;
            for (int axis = 0; axis < 2; ++axis)
            {
                const float margin = clampAxis[axis] ? -1.0f : 1.0f;
                usable = usable && std::isfinite(lo[axis]) && std::isfinite(hi[axis]) && lo[axis] >= margin && hi[axis] <= limit[axis] - margin;
            }
            MTLRegion area = MTLRegionMake2D(0, 0, 0, 0);
            if (usable)
            {
                const long x0 = std::max(0L, static_cast<long>(std::floor(lo[0] * scale)) - 2);
                const long y0 = std::max(0L, static_cast<long>(std::floor(lo[1] * scale)) - 2);
                const long x1 = std::min(static_cast<long>(src->glW), static_cast<long>(std::ceil(hi[0] * scale)) + 2);
                const long y1 = std::min(static_cast<long>(src->glH), static_cast<long>(std::ceil(hi[1] * scale)) + 2);
                if (x1 > x0 && y1 > y0)
                    area = MTLRegionMake2D(static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0), static_cast<NSUInteger>(x1 - x0),
                                           static_cast<NSUInteger>(y1 - y0));
            }
            texture = sampleable(src->color, rt, area.size.width != 0u ? &area : nullptr);
        }
        else if (rsrc)
            rtColor = sampleable(rsrc->color, rt);
        else
            texture = tex->tex;
        u.zTex = !zsrc ? 0 : c.tex0.psm == 0x32 || c.tex0.psm == 0x3A ? 16 : c.tex0.psm == 0x31 ? 24 : 32;
        u.zMax = zsrc ? zMaxFor(static_cast<uint8_t>(c.tex0.psm)) : 1.0f;
        u.rtTex24 = !(src || rsrc) ? 0 : c.tex0.psm == GS_PSM_CT24 ? 1 : family16(c.tex0.psm) ? 2 : 0;
        u.reinterp = rsrc ? 1 : 0;
        if (rsrc)
            setupReinterpret(s, *rsrc, u, rpal, hasRpal);
        u.ta0 = static_cast<float>(s.texa.ta0);
        u.ta1 = static_cast<float>(s.texa.ta1);
        u.aem = s.texa.aem ? 1 : 0;
        linear = s.linearFilter && !indexed && !zsrc;
        clampU = (c.clamp & 3u) != 0u;
        clampV = ((c.clamp >> 2) & 3u) != 0u;
        u.texDimX = static_cast<float>(std::max<uint16_t>(s.textureWidth, 1));
        u.texDimY = static_cast<float>(std::max<uint16_t>(s.textureHeight, 1));
        const float sizeW = zsrc ? zsrc->pxW : src ? src->pxW : tex ? tex->width : 1u;
        const float sizeH = zsrc ? zsrc->pxH : src ? src->pxH : tex ? tex->height : 1u;
        u.texSizeX = sizeW;
        u.texSizeY = sizeH;
    }

    const bool ate = (c.test & 1u) != 0u;
    const uint32_t afail = static_cast<uint32_t>((c.test >> 12) & 3u);
    static const bool forceNoTest = std::getenv("GOW_GS_HW_NOATEST") != nullptr;
    u.ate = ate ? 1 : 0;
    u.atst = static_cast<int32_t>((c.test >> 1) & 7u);
    u.aref = static_cast<int32_t>((c.test >> 4) & 0xFFu);
    u.pass = (ate && !forceNoTest) ? 1 : 0;
    if (fetch)
    {
        u.hwBlend = blend.enabled ? 1 : 0;
        u.hwSf = blend.sf;
        u.hwDf = blend.df;
        u.hwEq = blend.reverse ? 1 : 0;
    }

    NSUInteger offset = 0;
    id<MTLBuffer> vb = upload(dverts.data(), dverts.size() * sizeof(HwVertex), offset);
    beginPass(rt);
    bool empty = false;
    scissor(enc, rt, c.scissor.x0, c.scissor.y0, c.scissor.x1, c.scissor.y1, empty);
    if (empty)
        return;
    // the draw's shader path for the specialised pipeline (kTexPath / kFlags in gs_metal_shaders.inc)
    const int texPath = !s.prim.tme ? 0 : u.reinterp ? 3 : u.indexed ? 2 : u.zTex ? 4 : 1;
    const int flags = (u.linear ? 1 : 0) | (u.swBlend ? 2 : 0) | (u.zAlias ? 4 : 0) | (u.date ? 8 : 0) | (u.pass ? 16 : 0);
    const auto psoFor = [&](uint8_t m)
    { return fetch ? pipelineFor(Pso::Fetch, m, true, {}, texPath, flags) : pipelineFor(Pso::Main, m, true, blend, texPath, flags); };
    setPso(psoFor(mask));
    setDss(depthOn ? depthState(compare, zwrite) : depthState(kAlways, false));
    if (blend.enabled && !fetch)
        [enc setBlendColorRed:u.blendColor[0] green:u.blendColor[1] blue:u.blendColor[2] alpha:u.blendColor[3]];
    const VsUniforms vu{1.0f / rt.pxW, 1.0f / rt.pxH};
    [enc setVertexBuffer:vb offset:offset atIndex:0];
    [enc setVertexBytes:&vu length:sizeof(vu) atIndex:1];
    [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
    [enc setFragmentBuffer:tables offset:0 atIndex:1];
    if (hasRpal)
        [enc setFragmentBytes:rpal length:sizeof(rpal) atIndex:2];
    else
        [enc setFragmentBuffer:zeroBuffer offset:0 atIndex:2];
    [enc setFragmentBuffer:ipal offset:0 atIndex:3];
    [enc setFragmentTexture:texture atIndex:0];
    [enc setFragmentTexture:ztexture atIndex:1];
    [enc setFragmentTexture:rtColor atIndex:2];
    [enc setFragmentSamplerState:samplerFor(linear, clampU, clampV) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:dverts.size()];
    static const bool noFailPass = std::getenv("GOW_GS_HW_NOFAIL") != nullptr;
    if (ate && afail != 0u && !noFailPass)
    {
        // failing fragments: 1 FB_ONLY (colour+alpha, no Z), 2 ZB_ONLY (Z only), 3 RGB_ONLY (colour, no alpha, no Z)
        u.pass = 2;
        static const bool failTint = std::getenv("GOW_GS_HW_FAILTINT") != nullptr;
        u.failTint = (failTint && afail == 3u && s.prim.tme && c.frame.fbp == 0u) ? 1 : 0;
        uint8_t failMask = mask;
        bool failZ = zwrite;
        if (afail == 1u)
            failZ = false;
        else if (afail == 2u)
            failMask = 0;
        else
            failZ = false, failMask = static_cast<uint8_t>(mask & ~kMaskA);
        setPso(psoFor(failMask));
        setDss(depthOn ? depthState(compare, failZ) : depthState(kAlways, false));
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:dverts.size()];
    }
    ++stats.batches;
    ++encDraws;
    encFetch += fetch ? 1u : 0u;
    encReint += u.reinterp ? 1u : 0u;
    encIdx += u.indexed ? 1u : 0u;
    encRt += u.rtTex24 != 0 || u.zTex != 0 ? 1u : 0u;
    encLin += u.linear ? 1u : 0u;
    encVerts += static_cast<uint32_t>(dverts.size());
    // GOW_GS_MTL_TRACE=<present>[/<present>...][:x,y]: every batch of those frames with its state and the pixel (PS2 x, y;
    // default the centre of the scene) it leaves in its target (one readback per batch: debug only)
    static const char *trace = std::getenv("GOW_GS_MTL_TRACE");
    static const std::vector<int> tracePresents = []
    {
        std::vector<int> list;
        for (const char *p = trace; p && *p && *p != ':'; ++p)
            if (p == trace || p[-1] == '/')
                list.push_back(std::atoi(p));
        return list;
    }();
    if (trace && std::find(tracePresents.begin(), tracePresents.end(), presentCount) != tracePresents.end())
    {
        uint32_t probeX = 256u, probeY = 208u;
        if (const char *at = std::strchr(trace, ':'))
            std::sscanf(at + 1, "%u,%u", &probeX, &probeY);
        uint32_t px = 0u;
        float depth = -1.0f;
        const uint32_t hx = std::min(rt.glW - 1u, static_cast<uint32_t>(probeX * scale)), hy = std::min(rt.glH - 1u, static_cast<uint32_t>(probeY * scale));
        readback(rt.color, hx, hy, 1u, 1u, &px);
        if (rt.depth)
            readback(rt.depth, hx, hy, 1u, 1u, &depth);
        std::fprintf(stderr, "[gs-mtl-z] #%d vz=%.9f depth=%.9f compare=%d zwrite=%d depthOn=%d zInv=%d\n", frameBatch, dverts[0].z, depth,
                     static_cast<int>(compare), zwrite ? 1 : 0, depthOn ? 1 : 0, zInv ? 1 : 0);
        std::fprintf(stderr, "[gs-mtl-b] #%d fbp=%u psm=%u n=%zu prim=%u tme=%d tbp=%x tpsm=%x tfx=%u tcc=%u abe=%d alpha=%llx test=%llx fbmsk=%x zbp=%x soft=%d fetch=%d blend=%d:%u,%u%s ate=%d src/rsrc/idx=%d/%d/%d v0col=%.0f,%.0f,%.0f,%.0f -> %08x\n",
                     frameBatch, c.frame.fbp, c.frame.psm, verts.size(), static_cast<unsigned>(s.prim.type), s.prim.tme ? 1 : 0, c.tex0.tbp0, c.tex0.psm,
                     c.tex0.tfx, c.tex0.tcc, s.prim.abe ? 1 : 0, (unsigned long long)c.alpha, (unsigned long long)c.test, c.frame.fbmsk, c.zbuf.zbp,
                     softBlend ? 1 : 0, fetch ? 1 : 0, blend.enabled ? 1 : 0, blend.sf, blend.df, blend.reverse ? "r" : "", ate ? 1 : 0,
                     u.rtTex24 != 0 ? 1 : 0, u.reinterp, u.indexed, verts[0].r, verts[0].g, verts[0].b, verts[0].a, px);
    }
}

void GSMetalBackend::Impl::reportStats()
{
    if (!statsEnabled)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now - stats.last < std::chrono::seconds(5))
        return;
    stats.last = now;
    std::fprintf(stderr, "[gs-hw] shuffles=%llu draws=%llu batches=%llu texDecodes=%llu texHits=%llu depals=%llu skippedPsm=%llu skippedPrim=%llu unsupportedBlend=%llu textures=%zu feedbackCopies=%llu feedbackMpx=%.1f commits=%llu pipelines=%zu gpu_ms=%.0f\n",
                 (unsigned long long)stats.shuffles, (unsigned long long)stats.draws, (unsigned long long)stats.batches, (unsigned long long)stats.texDecodes,
                 (unsigned long long)stats.texHits, (unsigned long long)stats.depals, (unsigned long long)stats.skippedPsm, (unsigned long long)stats.skippedPrim,
                 (unsigned long long)stats.unsupportedBlend, textures.size(), (unsigned long long)stats.feedbackCopies, stats.feedbackPixels * 1e-6,
                 (unsigned long long)stats.commits, pipelines.size(), gpuNanos->exchange(0u) * 1e-6);
    stats = {};
    stats.last = now;
}
