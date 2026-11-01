// GOW-Port: Metal layer of GSMetalBackend: device, queue, shader library, pipeline / depth / sampler caches, the vertex arena,
// command buffer and render pass handling, feedback copies and readbacks. The GL renderer's counterparts are gs_gl.cpp and
// the GL calls inside gs_hw_backend.cpp. Pipeline caching by (kind, write mask, depth, blend) follows PCSX2's Metal
// renderer (GSDeviceMTL.mm, GPL-3.0); destination reads use framebuffer fetch like its HAS_FBFETCH path.
#include "gs_metal_impl.h"

#import <Foundation/Foundation.h>

using namespace gsmtl;

namespace
{
    const char *kShaderSource =
#include "gs_metal_shaders.inc"
        ;

    constexpr size_t kChunkBytes = 4u << 20;

    MTLBlendFactor factor(uint8_t f)
    {
        switch (f)
        {
        case kZero: return MTLBlendFactorZero;
        case kOne: return MTLBlendFactorOne;
        case kSrc1Alpha: return MTLBlendFactorSource1Alpha;
        case kOneMinusSrc1Alpha: return MTLBlendFactorOneMinusSource1Alpha;
        case kConstant: return MTLBlendFactorBlendColor;
        case kOneMinusConstant: return MTLBlendFactorOneMinusBlendColor;
        case kSrcAlpha: return MTLBlendFactorSourceAlpha;
        default: return MTLBlendFactorOneMinusSourceAlpha;
        }
    }

    MTLCompareFunction compareFunction(Compare c)
    {
        switch (c)
        {
        case kNever: return MTLCompareFunctionNever;
        case kAlways: return MTLCompareFunctionAlways;
        case kGEqual: return MTLCompareFunctionGreaterEqual;
        case kGreater: return MTLCompareFunctionGreater;
        case kLEqual: return MTLCompareFunctionLessEqual;
        default: return MTLCompareFunctionLess;
        }
    }
}

bool GSMetalBackend::Impl::ensure()
{
    if (initialized)
        return true;
    if (failed)
        return false;
    device = MTLCreateSystemDefaultDevice();
    if (!device)
    {
        std::fprintf(stderr, "[gs-mtl] no Metal device\n");
        failed = true;
        return false;
    }
    queue = [device newCommandQueue];
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    library = [device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource] options:options error:&error];
    if (!library)
    {
        std::fprintf(stderr, "[gs-mtl] shader error: %s\n", error ? error.localizedDescription.UTF8String : "?");
        failed = true;
        return false;
    }
    buildTables();
    zeroBuffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    std::memset(zeroBuffer.contents, 0, 4096);
    dummyColor = newTexture(MTLPixelFormatRGBA8Unorm, 1, 1, false);
    const uint32_t zero = 0u;
    [dummyColor replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];
    dummyDepth = newTexture(MTLPixelFormatDepth32Float, 1, 1, true);
    clearPass(nil, dummyDepth, MTLClearColorMake(0, 0, 0, 0));
    chooseScale();
    std::fprintf(stderr, "[gs-hw] render-target renderer ready, scale %.3fx%.3f (Metal: %s)\n", scaleX, scaleY, device.name.UTF8String);
    readGfxOptions();
    repl = std::make_unique<gsrepl::Store>();
    if (!repl->init(device))
        repl.reset();
    initialized = true;
    return true;
}

// forward page tables of every texture format + inverse tables (in-page pixel index -> in-page x/y) of the render-target
// formats, in one uint16 buffer (same offsets as the GL renderer's R16UI texture)
void GSMetalBackend::Impl::buildTables()
{
    std::vector<uint16_t> data;
    const uint32_t psms[] = {0x00, 0x01, 0x02, 0x0A, 0x13, 0x14, 0x1B, 0x24, 0x2C};
    for (const uint32_t psm : psms)
    {
        const GSSwizzle::Format &f = GSSwizzle::GetFormat(psm);
        if (!f.table || tableOffset.count(f.table))
            continue;
        const uint32_t size = 32u << (f.pageShiftX + f.pageShiftY);
        tableOffset[f.table] = static_cast<uint32_t>(data.size());
        data.insert(data.end(), f.table, f.table + size);
    }
    for (const uint32_t psm : {0x00u, 0x01u, 0x02u, 0x0Au})
    {
        const GSSwizzle::Format &f = GSSwizzle::GetFormat(psm);
        const uint32_t pageSize = 1u << (f.pageShiftX + f.pageShiftY);
        inverseOffset[psm] = static_cast<uint32_t>(data.size());
        data.resize(data.size() + pageSize, 0);
        uint16_t *inv = &data[inverseOffset[psm]];
        for (uint32_t e = 0; e < pageSize; ++e)
            inv[f.table[e]] = static_cast<uint16_t>(e);
    }
    data.resize(data.size() + 2048u, 0);
    tables = [device newBufferWithBytes:data.data() length:data.size() * sizeof(uint16_t) options:MTLResourceStorageModeShared];
}

id<MTLTexture> GSMetalBackend::Impl::newTexture(MTLPixelFormat format, uint32_t width, uint32_t height, bool renderTarget)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:std::max<uint32_t>(1u, width)
                                                                                height:std::max<uint32_t>(1u, height) mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | (renderTarget ? MTLTextureUsageRenderTarget : 0);
    // render targets live in GPU memory; uploaded textures are written by the CPU (unified memory)
    d.storageMode = renderTarget ? MTLStorageModePrivate : MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d];
    if (!texture && !allocationFailed)
    {
        // out of GPU memory: draws that would use it sample nothing / write nowhere until a later allocation succeeds
        std::fprintf(stderr, "[gs-mtl] texture allocation failed (%ux%u, device %.0f MB); drawing without it\n", width, height,
                     device.currentAllocatedSize / 1048576.0);
        allocationFailed = true;
    }
    return texture;
}

id<MTLCommandBuffer> GSMetalBackend::Impl::commands()
{
    if (!cmd)
    {
        cmd = [queue commandBuffer];
        const uint64_t serial = ++cmdSerial;
        std::shared_ptr<std::atomic<uint64_t>> done = completedSerial, gpu = gpuNanos;
        const bool timed = statsEnabled;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> b) {
            if (timed && b.GPUEndTime > b.GPUStartTime)
                gpu->fetch_add(static_cast<uint64_t>((b.GPUEndTime - b.GPUStartTime) * 1e9), std::memory_order_relaxed);
            done->store(serial, std::memory_order_release);
        }];
    }
    return cmd;
}

void GSMetalBackend::Impl::endEncoder()
{
    if (!enc)
        return;
    if (labels)
        enc.label = [NSString stringWithFormat:@"rt fbp=%u %lux%lu draws=%u verts=%u fetch=%u reint=%u idx=%u rtsrc=%u lin=%u", encFbp,
                                               (unsigned long)encColor.width, (unsigned long)encColor.height, encDraws, encVerts, encFetch,
                                               encReint, encIdx, encRt, encLin];
    [enc endEncoding];
    enc = nil;
    encColor = encDepth = nil;
    encPso = nil;
    encDss = nil;
}

id<MTLRenderCommandEncoder> GSMetalBackend::Impl::beginPass(RenderTarget &rt)
{
    if (enc && encColor == rt.color && encDepth == rt.depth)
        return enc;
    endEncoder();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = rt.color;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (rt.depth)
    {
        pass.depthAttachment.texture = rt.depth;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
    }
    enc = [commands() renderCommandEncoderWithDescriptor:pass];
    encColor = rt.color;
    encFbp = rt.fbp;
    encDraws = encVerts = encFetch = encReint = encIdx = encRt = encLin = 0;
    encDepth = rt.depth;
    [enc setViewport:MTLViewport{0.0, 0.0, static_cast<double>(rt.glW), static_cast<double>(rt.glH), 0.0, 1.0}];
    [enc setCullMode:MTLCullModeNone];
    return enc;
}

void GSMetalBackend::Impl::clearPass(id<MTLTexture> color, id<MTLTexture> depth, MTLClearColor clearColor)
{
    endEncoder();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (color)
    {
        pass.colorAttachments[0].texture = color;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = clearColor;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    if (depth)
    {
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.clearDepth = 0.0;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
    }
    [[commands() renderCommandEncoderWithDescriptor:pass] endEncoding];
}

void GSMetalBackend::Impl::commit(bool wait)
{
    endEncoder();
    if (!cmd)
        return;
    [cmd commit];
    if (wait)
        [cmd waitUntilCompleted];
    cmd = nil;
    activeChunk = -1;
    ++stats.commits;
    stats.inFlightMax = std::max<uint64_t>(stats.inFlightMax, cmdSerial - completedSerial->load(std::memory_order_acquire));
    stats.cmdBytesMax = std::max<uint64_t>(stats.cmdBytesMax, cmdBytes);
    cmdBytes = 0;
}

id<MTLRenderPipelineState> GSMetalBackend::Impl::pipelineFor(Pso kind, uint8_t writeMask, bool hasDepth, const BlendDesc &b, int texPath,
                                                         int flags)
{
    const bool shaded = kind == Pso::Main || kind == Pso::Fetch;
    if (!shaded || !specialize)
        texPath = flags = -1;
    uint64_t key = static_cast<uint64_t>(kind) | (static_cast<uint64_t>(writeMask & 15u) << 3) | (hasDepth ? 1ull << 7 : 0ull);
    if (b.enabled)
        key |= 1ull << 8 | static_cast<uint64_t>(b.sf) << 9 | static_cast<uint64_t>(b.df) << 12 | static_cast<uint64_t>(b.asf) << 15 |
               static_cast<uint64_t>(b.adf) << 18 | (b.reverse ? 1ull << 21 : 0ull);
    key |= static_cast<uint64_t>(texPath + 1) << 24 | static_cast<uint64_t>(flags + 1) << 32;
    if (const auto it = pipelines.find(key); it != pipelines.end())
        return it->second;
    static const char *const names[][2] = {{"vs_main", "fs_main"}, {"vs_main", "fs_fetch"}, {"vs_shuffle", "fs_shuffle"},
                                           {"vs_full", "fs_present"}, {"vs_full", "fs_color"}, {"vs_full", "fs_clut"}};
    const auto *pair = names[static_cast<int>(kind)];
    std::shared_ptr<SpecSlot> slot;
    if (texPath >= 0)
    {
        std::shared_ptr<SpecSlot> &pending = specPending[key];
        if (pending)
        {
            const int state = pending->state.load(std::memory_order_acquire);
            if (state == 1)
                return pipelineFor(kind, writeMask, hasDepth, b);
            id<MTLRenderPipelineState> ready = state == 2 ? pending->pso : pipelineFor(kind, writeMask, hasDepth, b);
            pipelines[key] = ready;
            specPending.erase(key);
            return ready;
        }
        pending = std::make_shared<SpecSlot>();
        pending->state.store(1);
        slot = pending;
    }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [library newFunctionWithName:[NSString stringWithUTF8String:pair[0]]];
    MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
    c.pixelFormat = MTLPixelFormatRGBA8Unorm;
    c.writeMask = ((writeMask & kMaskR) ? MTLColorWriteMaskRed : 0) | ((writeMask & kMaskG) ? MTLColorWriteMaskGreen : 0) |
                  ((writeMask & kMaskB) ? MTLColorWriteMaskBlue : 0) | ((writeMask & kMaskA) ? MTLColorWriteMaskAlpha : 0);
    c.blendingEnabled = b.enabled ? YES : NO;
    if (b.enabled)
    {
        c.sourceRGBBlendFactor = factor(b.sf);
        c.destinationRGBBlendFactor = factor(b.df);
        c.rgbBlendOperation = b.reverse ? MTLBlendOperationReverseSubtract : MTLBlendOperationAdd;
        c.sourceAlphaBlendFactor = factor(b.asf);
        c.destinationAlphaBlendFactor = factor(b.adf);
        c.alphaBlendOperation = MTLBlendOperationAdd;
    }
    d.depthAttachmentPixelFormat = hasDepth ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;
    NSString *fragmentName = [NSString stringWithUTF8String:pair[1]];
    MTLFunctionConstantValues *constants = [MTLFunctionConstantValues new];
    const int32_t tp = texPath, fl = flags;
    [constants setConstantValue:&tp type:MTLDataTypeInt atIndex:0];
    [constants setConstantValue:&fl type:MTLDataTypeInt atIndex:1];
    if (slot)
    {
        id<MTLDevice> dev = device;
        [library newFunctionWithName:fragmentName constantValues:constants
                   completionHandler:^(id<MTLFunction> function, NSError *) {
                       if (!function)
                       {
                           slot->state.store(3, std::memory_order_release);
                           return;
                       }
                       d.fragmentFunction = function;
                       [dev newRenderPipelineStateWithDescriptor:d
                                               completionHandler:^(id<MTLRenderPipelineState> pso, NSError *) {
                                                   slot->pso = pso;
                                                   slot->state.store(pso ? 2 : 3, std::memory_order_release);
                                               }];
                   }];
        return pipelineFor(kind, writeMask, hasDepth, b);
    }
    NSError *error = nil;
    d.fragmentFunction = shaded ? [library newFunctionWithName:fragmentName constantValues:constants error:&error]
                                : [library newFunctionWithName:fragmentName];
    id<MTLRenderPipelineState> pso = d.fragmentFunction ? [device newRenderPipelineStateWithDescriptor:d error:&error] : nil;
    if (!pso)
        std::fprintf(stderr, "[gs-mtl] pipeline %s/%s failed: %s\n", pair[0], pair[1], error ? error.localizedDescription.UTF8String : "?");
    pipelines[key] = pso;
    return pso;
}

id<MTLDepthStencilState> GSMetalBackend::Impl::depthState(Compare compare, bool write)
{
    const uint32_t key = static_cast<uint32_t>(compare) | (write ? 8u : 0u);
    if (const auto it = depthStates.find(key); it != depthStates.end())
        return it->second;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = compareFunction(compare);
    d.depthWriteEnabled = write ? YES : NO;
    id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:d];
    depthStates[key] = state;
    return state;
}

id<MTLSamplerState> GSMetalBackend::Impl::samplerFor(bool linear, bool clampU, bool clampV, uint32_t anisotropy, bool mipmapped)
{
    anisotropy = linear ? std::clamp<uint32_t>(anisotropy, 1u, 16u) : 1u;
    mipmapped = mipmapped && linear;
    const uint32_t key = (linear ? 1u : 0u) | (clampU ? 2u : 0u) | (clampV ? 4u : 0u) | (anisotropy << 3) | (mipmapped ? 0x100u : 0u);
    if (const auto it = samplers.find(key); it != samplers.end())
        return it->second;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = d.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.sAddressMode = clampU ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
    d.tAddressMode = clampV ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
    d.maxAnisotropy = anisotropy;
    if (mipmapped) // replacement textures carry a mip chain (the GS textures are sampled at level 0 only)
        d.mipFilter = MTLSamplerMipFilterLinear;
    id<MTLSamplerState> state = [device newSamplerStateWithDescriptor:d];
    samplers[key] = state;
    return state;
}

void GSMetalBackend::Impl::setPso(id<MTLRenderPipelineState> pso)
{
    if (pso != encPso)
        [enc setRenderPipelineState:pso], encPso = pso;
}

void GSMetalBackend::Impl::setDss(id<MTLDepthStencilState> dss)
{
    if (dss != encDss)
        [enc setDepthStencilState:dss], encDss = dss;
}

id<MTLBuffer> GSMetalBackend::Impl::upload(const void *data, size_t bytes, NSUInteger &offset)
{
    commands(); // the chunk is tagged with the command buffer that reads it
    bytes = (bytes + 255u) & ~size_t(255u);
    if (activeChunk < 0 || chunks[activeChunk].used + bytes > chunks[activeChunk].buffer.length)
    {
        activeChunk = -1;
        const uint64_t done = completedSerial->load(std::memory_order_acquire);
        for (size_t i = 0; i < chunks.size(); ++i)
            if (chunks[i].serial <= done && chunks[i].serial != cmdSerial && chunks[i].buffer.length >= bytes)
            {
                activeChunk = static_cast<int>(i);
                chunks[i].used = 0;
                break;
            }
        if (activeChunk < 0)
        {
            Chunk chunk;
            chunk.buffer = [device newBufferWithLength:std::max(kChunkBytes, bytes) options:MTLResourceStorageModeShared];
            if (!chunk.buffer)
            {
                if (!allocationFailed)
                    std::fprintf(stderr, "[gs-mtl] vertex buffer allocation failed (device %.0f MB); draws skipped\n",
                                 device.currentAllocatedSize / 1048576.0);
                allocationFailed = true;
                offset = 0;
                return nil; // the caller skips the draw
            }
            chunks.push_back(chunk);
            activeChunk = static_cast<int>(chunks.size() - 1u);
        }
    }
    Chunk &chunk = chunks[activeChunk];
    chunk.serial = cmdSerial;
    chunk.lastPresent = presentCount;
    offset = chunk.used;
    std::memcpy(static_cast<uint8_t *>(chunk.buffer.contents) + chunk.used, data, bytes);
    chunk.used += bytes;
    cmdBytes += bytes;
    return chunk.buffer;
}

// The GL renderer sampled a target while drawing into it after glTextureBarrier (what was drawn before the draw is
// visible). A Metal attachment cannot be sampled in its own pass, so the pass is ended and the texture copied (only `area`,
// host pixels, when the caller knows what the draw samples; the rest of the copy is stale and never read).
id<MTLTexture> GSMetalBackend::Impl::sampleable(id<MTLTexture> texture, const RenderTarget &dst, const MTLRegion *area)
{
    if (!texture || (texture != dst.color && texture != dst.depth))
        return texture;
    const uint64_t key = static_cast<uint64_t>(texture.width) | (static_cast<uint64_t>(texture.height) << 20) |
                         (static_cast<uint64_t>(texture.pixelFormat) << 40);
    __strong id<MTLTexture> &copy = scratch[key];
    if (!copy)
        copy = newTexture(texture.pixelFormat, static_cast<uint32_t>(texture.width), static_cast<uint32_t>(texture.height), true);
    endEncoder();
    id<MTLBlitCommandEncoder> blit = [commands() blitCommandEncoder];
    if (area)
    {
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:area->origin sourceSize:area->size toTexture:copy
             destinationSlice:0 destinationLevel:0 destinationOrigin:area->origin];
        stats.feedbackPixels += static_cast<uint64_t>(area->size.width) * area->size.height;
    }
    else
    {
        [blit copyFromTexture:texture toTexture:copy];
        stats.feedbackPixels += static_cast<uint64_t>(texture.width) * texture.height;
    }
    [blit endEncoding];
    ++stats.feedbackCopies;
    return copy;
}

void GSMetalBackend::Impl::readback(id<MTLTexture> texture, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void *out)
{
    const size_t bytes = static_cast<size_t>(w) * h * 4u;
    if (!readbackBuffer || readbackBuffer.length < bytes)
        readbackBuffer = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (!texture || !readbackBuffer)
    {
        std::memset(out, 0, bytes);
        return;
    }
    endEncoder();
    id<MTLBlitCommandEncoder> blit = [commands() blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:readbackBuffer destinationOffset:0 destinationBytesPerRow:w * 4u destinationBytesPerImage:bytes];
    [blit endEncoding];
    commit(true);
    std::memcpy(out, readbackBuffer.contents, bytes);
}

// GS scissor (inclusive PS2 pixels) at the internal scale, clamped to the target (Metal rejects rectangles outside it): the
// host pixels whose sample position k / scale lies inside it (the same rows and columns as before at whole scales)
void GSMetalBackend::Impl::scissor(id<MTLRenderCommandEncoder> e, const RenderTarget &rt, int x0, int y0, int x1, int y1, bool &empty)
{
    const long sx = std::min<long>(static_cast<long>(std::ceil(x0 * scaleX)), rt.glW);
    const long sy = std::min<long>(static_cast<long>(std::ceil(y0 * scaleY)), rt.glH);
    const long sw = std::min<long>(static_cast<long>(std::ceil(std::max(x0, x1 + 1) * scaleX)), rt.glW) - sx;
    const long sh = std::min<long>(static_cast<long>(std::ceil(std::max(y0, y1 + 1) * scaleY)), rt.glH) - sy;
    empty = sw <= 0 || sh <= 0;
    if (!empty)
        [e setScissorRect:MTLScissorRect{static_cast<NSUInteger>(sx), static_cast<NSUInteger>(sy), static_cast<NSUInteger>(sw),
                                         static_cast<NSUInteger>(sh)}];
}
