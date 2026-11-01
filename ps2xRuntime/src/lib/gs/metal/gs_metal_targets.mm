// GOW-Port: render targets, the texture / palette caches and the reinterpreting fetch of GSMetalBackend. Port of the
// "render targets", "textures" and "CLUT from a render target" sections of gs_hw_backend.cpp (same rules; comments on
// the GS behaviour are kept there).
#include "gs_metal_impl.h"

#include <sys/sysctl.h>

using namespace gsmtl;

namespace
{
    // entries of a texture cache not looked up during the last `age` lookups; `bytes` is the cache's byte count
    template <class Map>
    void dropIdle(Map &map, uint64_t now, uint64_t age, uint64_t &bytes)
    {
        for (auto it = map.begin(); it != map.end();)
        {
            if (it->second.lastUse + age < now)
            {
                bytes -= std::min(bytes, it->second.bytes);
                it = map.erase(it);
            }
            else
                ++it;
        }
    }
}

GSMetalBackend::Impl::RenderTarget &GSMetalBackend::Impl::target(uint32_t fbp, uint32_t fbw)
{
    for (RenderTarget &t : targets)
        if (t.fbp == fbp && t.fbw >= fbw)
            return t;
    static const bool rtLog = std::getenv("GOW_GS_HW_RTLOG") != nullptr;
    if (rtLog)
        std::fprintf(stderr, "[gs-hw-rt] new/grow fbp=%u fbw=%u\n", fbp, fbw);
    for (RenderTarget &t : targets) // grow an existing one
        if (t.fbp == fbp)
        {
            destroy(t);
            createTarget(t, fbp, fbw);
            return t;
        }
    targets.emplace_back();
    createTarget(targets.back(), fbp, fbw);
    return targets.back();
}

void GSMetalBackend::Impl::destroy(RenderTarget &t)
{
    if (enc && (encColor == t.color || (t.depth && encDepth == t.depth)))
        endEncoder();
    t = {}; // in-flight command buffers keep the textures alive
}

void GSMetalBackend::Impl::createTarget(RenderTarget &t, uint32_t fbp, uint32_t fbw)
{
    t.fbp = fbp;
    t.psm = 0;
    t.fbw = std::max<uint32_t>(fbw, 1u);
    t.pxW = t.fbw * 64u;
    t.pxH = 512u;
    t.glW = static_cast<uint32_t>(std::ceil(t.pxW * scaleX));
    t.glH = static_cast<uint32_t>(std::ceil(t.pxH * scaleY));
    t.color = newTexture(MTLPixelFormatRGBA8Unorm, t.glW, t.glH, true);
    clearPass(t.color, nil, MTLClearColorMake(0, 0, 0, 0));
    std::fprintf(stderr, "[gs-hw] render target fbp=%u fbw=%u -> %ux%u\n", fbp, t.fbw, t.glW, t.glH);
}

void GSMetalBackend::Impl::attachDepth(RenderTarget &t, uint32_t zbp)
{
    if (!t.depth)
    {
        if (enc && encColor == t.color)
            endEncoder(); // the pass gets the depth attachment when it is begun again
        t.depth = newTexture(MTLPixelFormatDepth32Float, t.glW, t.glH, true);
        clearPass(nil, t.depth, MTLClearColorMake(0, 0, 0, 0));
    }
    t.zbp = zbp;
}

GSMetalBackend::Impl::RenderTarget *GSMetalBackend::Impl::findTarget(uint32_t fbp)
{
    for (RenderTarget &t : targets)
        if (t.fbp == fbp)
            return &t;
    return nullptr;
}

// ---- textures -------------------------------------------------------------------------------------------------------
// A texture decoded again (an upload overlapped it) gets a new MTLTexture: the old one may still be read by draws the GPU
// has not executed yet (GL re-specified the same texture name and synchronised implicitly).
GSMetalBackend::Impl::Texture &GSMetalBackend::Impl::textureFor(const GSDrawState &s)
{
    const GSTex0Reg &t = s.context.tex0;
    uint64_t key = t.tbp0 | (static_cast<uint64_t>(t.tbw) << 14) | (static_cast<uint64_t>(t.psm & 63) << 20) |
                   (static_cast<uint64_t>(s.textureWidth) << 26) | (static_cast<uint64_t>(s.textureHeight) << 38);
    key = key * 1099511628211ull ^ (static_cast<uint64_t>(s.texa.ta0) | (static_cast<uint64_t>(s.texa.ta1) << 8) | (static_cast<uint64_t>(s.texa.aem) << 16));
    if (isIndexed(t.psm))
    {
        key = key * 1099511628211ull ^ clutHash;
        key = key * 1099511628211ull ^ (t.cpsm | (t.csm << 8) | (t.csa << 16));
    }
    Texture &tex = textures[key];
    tex.lastUse = ++useCounter;
    if (tex.valid)
    {
        ++stats.texHits;
        return tex;
    }
    ++stats.texDecodes;
    std::vector<uint32_t> &pixels = pixelScratch;
    decodeDirect(s, pixels);
    tex.width = std::max<uint32_t>(1u, s.textureWidth);
    tex.height = std::max<uint32_t>(1u, s.textureHeight);
    tex.blockLo = t.tbp0;
    tex.blockHi = t.tbp0 + spanBlocks(t.psm, t.tbw, tex.height);
    setTexture(tex, MTLPixelFormatRGBA8Unorm, 4u, textureBytes);
    [tex.tex replaceRegion:MTLRegionMake2D(0, 0, tex.width, tex.height) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:tex.width * 4u];
    tex.valid = true;
    tex.repl = {}; // new data: the replacement names are computed again
    if (textures.size() > 6000u || textureBytes > kTextureBudget)
        evictOld();
    return textures[key];
}

// Texels of a direct-colour texture read from GS memory with the swizzle tables, alpha expanded by TEXA like the GS
// texture unit (CT24: TA0 / AEM, CT16: TA0/TA1 by the alpha bit).
void GSMetalBackend::Impl::decodeDirect(const GSDrawState &s, std::vector<uint32_t> &out)
{
    const GSTex0Reg &t = s.context.tex0;
    const GSSwizzle::Format &f = GSSwizzle::GetFormat(t.psm);
    const uint32_t w = std::max<uint32_t>(1u, s.textureWidth), h = std::max<uint32_t>(1u, s.textureHeight);
    const bool sixteen = f.kind == GSSwizzle::Kind::Bits16;
    if (!vram || !f.table || !(sixteen || f.kind == GSSwizzle::Kind::Bits32 || f.kind == GSSwizzle::Kind::Bits24))
    {
        store.DecodeTextureRgba(s, out);
        out.resize(static_cast<size_t>(w) * h, 0u);
        return;
    }
    out.resize(static_cast<size_t>(w) * h);
    for (uint32_t y = 0; y < h; ++y)
    {
        uint32_t *line = out.data() + static_cast<size_t>(y) * w;
        if (!GSMem::ReadColorRun(vram, t.psm, t.tbp0, t.tbw, 0u, y, w, line))
        {
            const GSSwizzle::AxisPart row = GSSwizzle::RowPart(f, t.tbp0, t.tbw, y);
            for (uint32_t x = 0; x < w; ++x)
            {
                const GSSwizzle::Location at = GSSwizzle::Combine(f, row, GSSwizzle::ColumnPart(f, x));
                line[x] = GSSwizzle::LoadAt(f, vram + at.byte, at.shift);
            }
        }
        for (uint32_t x = 0; x < w; ++x)
            line[x] = GSCpuInternal::applyTexa(s.texa, t.psm, sixteen ? GSCpuInternal::Rgba5551ToRgba8888(static_cast<uint16_t>(line[x])) : line[x]);
    }
}

// Index texture of a paletted format: the raw indices, independent of the CLUT.
GSMetalBackend::Impl::Texture &GSMetalBackend::Impl::indexTextureFor(const GSDrawState &s)
{
    const GSTex0Reg &t = s.context.tex0;
    const uint64_t key = (1ull << 63) | t.tbp0 | (static_cast<uint64_t>(t.tbw) << 14) | (static_cast<uint64_t>(t.psm & 63) << 20) |
                         (static_cast<uint64_t>(s.textureWidth) << 26) | (static_cast<uint64_t>(s.textureHeight) << 38);
    Texture &tex = textures[key];
    tex.lastUse = ++useCounter;
    if (tex.valid)
    {
        ++stats.texHits;
        return tex;
    }
    ++stats.texDecodes;
    tex.width = std::max<uint32_t>(1u, s.textureWidth);
    tex.height = std::max<uint32_t>(1u, s.textureHeight);
    std::vector<uint8_t> indices(static_cast<size_t>(tex.width) * tex.height);
    for (uint32_t y = 0; y < tex.height; ++y)
        GSMem::ReadIndexRun(vram, t.psm, t.tbp0, t.tbw, 0u, y, tex.width, indices.data() + static_cast<size_t>(y) * tex.width);
    tex.blockLo = t.tbp0;
    tex.blockHi = t.tbp0 + spanBlocks(t.psm, t.tbw, tex.height);
    setTexture(tex, MTLPixelFormatR8Unorm, 1u, textureBytes);
    [tex.tex replaceRegion:MTLRegionMake2D(0, 0, tex.width, tex.height) mipmapLevel:0 withBytes:indices.data() bytesPerRow:tex.width];
    tex.generation = ++textureGeneration;
    tex.indices = std::move(indices);
    tex.valid = true;
    if (textures.size() > 6000u || textureBytes > kTextureBudget)
        evictOld();
    return textures[key];
}

// the CLUT as the draw reads it: loaded colours, format, offset, TEXA alpha
uint64_t GSMetalBackend::Impl::paletteKey(const GSDrawState &s) const
{
    const GSTex0Reg &t = s.context.tex0;
    uint64_t key = clutHash;
    key = key * 1099511628211ull ^ (t.cpsm | (t.csm << 8) | (t.csa << 16) | (static_cast<uint64_t>(t.psm & 63) << 24));
    key = key * 1099511628211ull ^ (static_cast<uint64_t>(s.texa.ta0) | (static_cast<uint64_t>(s.texa.ta1) << 8) | (static_cast<uint64_t>(s.texa.aem) << 16));
    return key;
}

GSMetalBackend::Impl::Texture &GSMetalBackend::Impl::depalTextureFor(const GSDrawState &s, const Texture &index)
{
    const uint64_t key = index.generation * 0x9E3779B97F4A7C15ull ^ paletteKey(s);
    {
        Texture &tex = depalCache[key];
        tex.lastUse = ++useCounter;
        if (tex.valid)
            return tex;
        ++stats.depals;
        uint32_t colors[256];
        store.BuildPalette(s, colors);
        tex.width = index.width;
        tex.height = index.height;
        std::vector<uint32_t> &pixels = pixelScratch;
        pixels.resize(index.indices.size());
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = colors[index.indices[i]];
        setTexture(tex, MTLPixelFormatRGBA8Unorm, 4u, depalBytes);
        [tex.tex replaceRegion:MTLRegionMake2D(0, 0, tex.width, tex.height) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:tex.width * 4u];
        tex.valid = true;
    }
    // animated CLUTs (the title's fire) make new copies every frame: keep the set small, drop what was not used lately
    if (depalCache.size() > 1024u || depalBytes > kDepalBudget)
        dropIdle(depalCache, useCounter, 3000u, depalBytes);
    return depalCache[key];
}

// 256 RGBA entries of the current CLUT as the draw reads it, cached per CLUT
id<MTLBuffer> GSMetalBackend::Impl::paletteFor(const GSDrawState &s)
{
    const uint64_t key = paletteKey(s);
    if (const auto it = palettes.find(key); it != palettes.end())
        return it->second;
    if (palettes.size() > 4096u)
        palettes.clear();
    uint32_t colors[256];
    store.BuildPalette(s, colors);
    id<MTLBuffer> buffer = [device newBufferWithBytes:colors length:sizeof(colors) options:MTLResourceStorageModeShared];
    palettes[key] = buffer;
    return buffer;
}

void GSMetalBackend::Impl::evictOld()
{
    dropIdle(textures, useCounter, 3000u, textureBytes);
}

void GSMetalBackend::Impl::setTexture(Texture &entry, MTLPixelFormat format, uint32_t bytesPerPixel, uint64_t &total)
{
    total -= std::min(total, entry.bytes);
    entry.tex = newTexture(format, entry.width, entry.height, false);
    entry.bytes = entry.tex ? static_cast<uint64_t>(entry.width) * entry.height * bytesPerPixel : 0u;
    total += entry.bytes;
}

// Vertex chunks: the arena grows to what the frames in flight need at their busiest and would keep that forever (324 MB
// after the first fight on the ship), so chunks idle for 30 s are released down to 8. Under memory pressure (warn or
// critical, again every 5 s while it lasts) every cache drops what the last frames did not use: GS textures and their
// RGBA conversions, palettes, feedback copies, idle vertex chunks and the pack's replacements; they are rebuilt on demand.
// The level is the system's (kern.memorystatus_vm_pressure_level: 1 normal, 2 warn, 4 critical), polled: a
// DISPATCH_SOURCE_TYPE_MEMORYPRESSURE source got no event during minutes at warn (the kernel notifies a few chosen processes).
void GSMetalBackend::Impl::trimCaches()
{
    if (presentCount % 60 == 0)
    {
        int value = 0;
        size_t length = sizeof(value);
        if (sysctlbyname("kern.memorystatus_vm_pressure_level", &value, &length, nullptr, 0) == 0)
            memoryPressure = value >= 4 ? 2 : value >= 2 ? 1 : 0;
    }
    const int level = memoryPressure;
    const bool pressure = level != 0 && (level > pressureHandled || presentCount % 300 == 0);
    if (level != pressureHandled && level == 0)
        std::fprintf(stderr, "[gs-mtl] memory pressure over (device %.0f MB)\n", device.currentAllocatedSize / 1048576.0);
    pressureHandled = level;
    const uint64_t done = completedSerial->load(std::memory_order_acquire);
    id<MTLBuffer> active = activeChunk >= 0 ? chunks[activeChunk].buffer : nil;
    const double before = device.currentAllocatedSize / 1048576.0;
    for (auto it = chunks.begin(); it != chunks.end();)
    {
        const bool idle = it->buffer != active && it->serial <= done && (pressure || presentCount - it->lastPresent > 1800);
        if (idle && (pressure || chunks.size() > 8u))
            it = chunks.erase(it);
        else
            ++it;
    }
    activeChunk = -1;
    for (size_t i = 0; active && i < chunks.size(); ++i)
        if (chunks[i].buffer == active)
            activeChunk = static_cast<int>(i);
    if (!pressure)
        return;
    dropIdle(textures, useCounter, 300u, textureBytes);
    dropIdle(depalCache, useCounter, 300u, depalBytes);
    palettes.clear();
    clutCache.clear();
    scratch.clear();
    const size_t replMb = repl ? repl->trim() >> 20 : 0u;
    std::fprintf(stderr, "[gs-mtl] memory pressure %s: caches trimmed, device %.0f -> %.0f MB (replacements -%zu MB)\n",
                 level == 2 ? "critical" : "warn", before, device.currentAllocatedSize / 1048576.0, replMb);
}

void GSMetalBackend::Impl::invalidateBlocks(uint32_t lo, uint32_t hi)
{
    for (auto &kv : textures)
    {
        Texture &t = kv.second;
        if (t.valid && t.blockLo < hi && lo < t.blockHi)
            t.valid = false;
    }
}

// The render target whose memory range holds the texture's first block (and was not overwritten by an upload since).
// Formats that live only in bits 24-31 (T8H, T4HL, T4HH) are never written by a 24 bit target.
GSMetalBackend::Impl::RenderTarget *GSMetalBackend::Impl::findReinterpretSource(uint32_t tbp, uint8_t tpsm)
{
    const bool highByte = tpsm == GS_PSM_T8H || tpsm == GS_PSM_T4HL || tpsm == GS_PSM_T4HH;
    for (RenderTarget &t : targets)
    {
        if (t.usedW == 0u || (highByte && t.psm == GS_PSM_CT24) || overwrittenByDepth(t))
            continue;
        const uint32_t tlo = t.fbp * 32u, thi = tlo + spanBlocks(t.psm, t.fbw, t.usedH);
        if (tbp < tlo || tbp >= thi)
            continue;
        if (!uploadedSinceDraw(t, tbp))
            return &t;
    }
    return nullptr;
}

// another target's depth buffer lives in this target's memory and was written after this target's last colour draw
bool GSMetalBackend::Impl::overwrittenByDepth(const RenderTarget &t) const
{
    for (const RenderTarget &o : targets)
        if (&o != &t && o.depth && o.zbp == t.fbp && o.lastDepthSeq > t.lastDrawSeq)
            return true;
    return false;
}

bool GSMetalBackend::Impl::uploadedSinceDraw(const RenderTarget &t, uint32_t tbp) const
{
    for (const UploadRecord &u : uploadLog)
        if (u.seq > t.lastDrawSeq && u.lo <= tbp && tbp < u.hi)
            return true;
    return false;
}

void GSMetalBackend::Impl::setupReinterpret(const GSDrawState &s, const RenderTarget &rt, FsUniforms &u, uint32_t palette[256], bool &hasPalette)
{
    const GSContext &c = s.context;
    const GSSwizzle::Format &tf = GSSwizzle::GetFormat(c.tex0.psm);
    const GSSwizzle::Format &rf = GSSwizzle::GetFormat(rt.psm);
    const auto offsetOf = [&](const uint16_t *table) { const auto it = tableOffset.find(table); return it == tableOffset.end() ? 0u : it->second; };
    int kind = 3, texelBits = 8;
    switch (tf.kind)
    {
    case GSSwizzle::Kind::Bits32: kind = 0; texelBits = 32; break;
    case GSSwizzle::Kind::Bits24: kind = 1; texelBits = 24; break;
    case GSSwizzle::Kind::Bits16: kind = 2; texelBits = 16; break;
    case GSSwizzle::Kind::Bits8: kind = 3; texelBits = 8; break;
    default: kind = 3; texelBits = 4; break;
    }
    hasPalette = kind == 3;
    if (hasPalette)
        store.BuildPalette(s, palette);
    const auto inv = inverseOffset.find(rt.psm);
    const int32_t texFmt[4] = {tf.pageShiftX, tf.pageShiftY, tf.pixelsPerPageShift, tf.unpackedShift};
    const int32_t texFmt2[4] = {tf.bitOffset, texelBits, static_cast<int32_t>(offsetOf(tf.table)), kind};
    const int32_t rtFmt[4] = {rf.pageShiftX, rf.pageShiftY, rf.pixelsPerPageShift, 1 << rf.unpackedShift};
    const int32_t rtFmt2[4] = {inv == inverseOffset.end() ? 0 : static_cast<int32_t>(inv->second), static_cast<int32_t>(rt.fbp),
                               static_cast<int32_t>(rt.fbw), 0};
    const int32_t tex0[4] = {static_cast<int32_t>(c.tex0.tbp0), c.tex0.tbw, s.textureWidth, s.textureHeight};
    std::memcpy(u.texFmt, texFmt, sizeof(texFmt));
    std::memcpy(u.texFmt2, texFmt2, sizeof(texFmt2));
    std::memcpy(u.rtFmt, rtFmt, sizeof(rtFmt));
    std::memcpy(u.rtFmt2, rtFmt2, sizeof(rtFmt2));
    std::memcpy(u.tex0, tex0, sizeof(tex0));
    u.rtPxX = static_cast<float>(rt.pxW);
    u.rtPxY = static_cast<float>(rt.pxH);
}

// A CLUT whose source memory a render target was drawn into is read from that target on the GPU: one pass writes the raw
// source words (reinterpreting fetch, CPSM view of the target) into a 256x1 target, read back; cached per target version.
bool GSMetalBackend::Impl::clutFromTarget(const GSTex0Reg &tex0, uint32_t width, const uint16_t *xy, uint32_t count, uint32_t *words)
{
    RenderTarget *rt = findReinterpretSource(tex0.cbp, tex0.cpsm);
    if (!rt || count == 0u || count > 256u)
        return false;
    uint64_t key = 1469598103934665603ull;
    const auto mix = [&key](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
    mix(rt->fbp); mix(rt->version); mix(tex0.cbp); mix(tex0.cpsm); mix(width); mix(count);
    for (uint32_t i = 0; i < count * 2u; ++i)
        mix(xy[i]);
    if (const auto it = clutCache.find(key); it != clutCache.end())
    {
        std::memcpy(words, it->second.data(), count * sizeof(uint32_t));
        return true;
    }
    const GSSwizzle::Format &tf = GSSwizzle::GetFormat(tex0.cpsm);
    const GSSwizzle::Format &rf = GSSwizzle::GetFormat(rt->psm);
    const auto table = tableOffset.find(tf.table);
    const auto inv = inverseOffset.find(rt->psm);
    if (table == tableOffset.end() || inv == inverseOffset.end())
        return false;
    const int texelBits = tf.kind == GSSwizzle::Kind::Bits32 ? 32 : tf.kind == GSSwizzle::Kind::Bits24 ? 24 : 16;
    FsUniforms u{};
    const int32_t texFmt[4] = {tf.pageShiftX, tf.pageShiftY, tf.pixelsPerPageShift, tf.unpackedShift};
    const int32_t texFmt2[4] = {tf.bitOffset, texelBits, static_cast<int32_t>(table->second), 0};
    const int32_t rtFmt[4] = {rf.pageShiftX, rf.pageShiftY, rf.pixelsPerPageShift, 1 << rf.unpackedShift};
    const int32_t rtFmt2[4] = {static_cast<int32_t>(inv->second), static_cast<int32_t>(rt->fbp), static_cast<int32_t>(rt->fbw), 0};
    const int32_t t0[4] = {static_cast<int32_t>(tex0.cbp), static_cast<int32_t>(width), 256, 256};
    std::memcpy(u.texFmt, texFmt, sizeof(texFmt));
    std::memcpy(u.texFmt2, texFmt2, sizeof(texFmt2));
    std::memcpy(u.rtFmt, rtFmt, sizeof(rtFmt));
    std::memcpy(u.rtFmt2, rtFmt2, sizeof(rtFmt2));
    std::memcpy(u.tex0, t0, sizeof(t0));
    u.rtPxX = static_cast<float>(rt->pxW);
    u.rtPxY = static_cast<float>(rt->pxH);
    __strong id<MTLTexture> &clutTex = scratch[0xC1u];
    if (!clutTex)
        clutTex = newTexture(MTLPixelFormatRGBA8Unorm, 256, 1, true);
    RenderTarget pass;
    pass.color = clutTex;
    pass.glW = count;
    pass.glH = 1;
    id<MTLTexture> source = rt->color;
    beginPass(pass);
    setPso(pipelineFor(Pso::Clut, kMaskAll, false));
    [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
    [enc setFragmentBuffer:tables offset:0 atIndex:1];
    [enc setFragmentBuffer:zeroBuffer offset:0 atIndex:2];
    [enc setFragmentBytes:xy length:count * 4u atIndex:4];
    [enc setFragmentTexture:source atIndex:2];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    readback(clutTex, 0, 0, count, 1, words);
    if (clutCache.size() > 4096u)
        clutCache.clear();
    clutCache.emplace(key, std::vector<uint32_t>(words, words + count));
    return true;
}
