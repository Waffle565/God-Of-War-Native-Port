// GOW-Port: graphics options of GSMetalBackend and the output chain they need (macOS only, docs/macos.md "Graphics options").
// The composed frame goes through FXAA (GOW_GS_MTL_AA=fxaa, at render resolution), MetalFX spatial upscaling
// (GOW_GS_MTL_METALFX=spatial) or the window scaler (fs_scale: bicubic when magnifying, bilinear otherwise) and the one
// contrast-adaptive sharpen (GOW_SHARPEN) into the window's drawable or, for captures with GOW_GS_MTL_CAPTURE_POST=1,
// into a texture that is read back. With the options off and a magnified frame this is the single fs_scale pass of before.
// Anisotropic filtering (GOW_GS_MTL_ANISO) is a sampler setting (gs_metal_draw.mm).
#include "gs_metal_impl.h"

#include <cmath>

using namespace gsmtl;

namespace
{
    enum PostStage { kStageScale = 0, kStageFxaa = 1, kStageCas = 2 };
}

void GSMetalBackend::Impl::readGfxOptions()
{
    const auto env = [](const char *name) { const char *v = std::getenv(name); return v ? v : ""; };
    const int aniso = std::atoi(env("GOW_GS_MTL_ANISO"));
    gfx.aniso = aniso >= 16 ? 16u : aniso >= 8 ? 8u : aniso >= 4 ? 4u : aniso >= 2 ? 2u : 1u;
    gfx.metalfx = std::strcmp(env("GOW_GS_MTL_METALFX"), "spatial") == 0;
    gfx.fxaa = std::strcmp(env("GOW_GS_MTL_AA"), "fxaa") == 0;
    gfx.capturePost = std::strcmp(env("GOW_GS_MTL_CAPTURE_POST"), "1") == 0;
    // GOW_SCALER=bilinear: one bilinear tap, no bicubic and no sharpen (as the raylib host scaler)
    gfx.bilinearOnly = std::strcmp(env("GOW_SCALER"), "bilinear") == 0;
    const char *sharpen = env("GOW_SHARPEN");
    if (gfx.bilinearOnly || std::strcmp(sharpen, "off") == 0)
        gfx.sharpen = -1.0f;
    else if (*sharpen)
        gfx.sharpen = std::clamp(static_cast<float>(std::atof(sharpen)), 0.0f, 1.0f);
    if (gfx.metalfx && ![MTLFXSpatialScalerDescriptor supportsDevice:device])
    {
        std::fprintf(stderr, "[gs-mtl] MetalFX spatial scaling is not supported on this device; plain scaler\n");
        gfx.metalfx = false;
    }
    if (gfx.aniso > 1u || gfx.post() || gfx.capturePost || *sharpen)
        std::fprintf(stderr, "[gs-mtl] graphics: aniso=%ux metalfx=%s aa=%s sharpen=%.2f%s\n", gfx.aniso, gfx.metalfx ? "spatial" : "off",
                     gfx.fxaa ? "fxaa" : "off", gfx.sharpen, gfx.capturePost ? " captures=post" : "");
}

void GSMetalBackend::Impl::fitRect(uint32_t dw, uint32_t dh, double &rx, double &ry, double &rw, double &rh)
{
    // the PS2 picture is shown at 16:9 (the game renders widescreen, gow_overrides.cpp), centred, black bars
    constexpr double kAspect = 16.0 / 9.0;
    rw = std::min<double>(dw, dh * kAspect);
    rh = rw / kAspect;
    rx = std::floor((dw - rw) * 0.5);
    ry = std::floor((dh - rh) * 0.5);
}

id<MTLRenderPipelineState> GSMetalBackend::Impl::postPipeline(int stage, MTLPixelFormat format)
{
    const uint32_t key = static_cast<uint32_t>(stage) | (static_cast<uint32_t>(format) << 4);
    if (const auto it = postPipelines.find(key); it != postPipelines.end())
        return it->second;
    static const char *const names[] = {"fs_scale", "fs_fxaa", "fs_cas"};
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [library newFunctionWithName:@"vs_full"];
    d.fragmentFunction = [library newFunctionWithName:[NSString stringWithUTF8String:names[stage]]];
    d.colorAttachments[0].pixelFormat = format;
    NSError *error = nil;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!pso)
        std::fprintf(stderr, "[gs-mtl] pipeline %s failed: %s\n", names[stage], error ? error.localizedDescription.UTF8String : "?");
    postPipelines[key] = pso;
    return pso;
}

id<MTLTexture> GSMetalBackend::Impl::postTarget(int slot, uint32_t width, uint32_t height)
{
    __strong id<MTLTexture> &t = postTex[slot];
    if (!t || t.width != width || t.height != height)
    {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width
                                                                                    height:height mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget |
                  (slot == 0 && spatialScaler ? spatialScaler.outputTextureUsage : 0);
        d.storageMode = MTLStorageModePrivate;
        t = [device newTextureWithDescriptor:d];
    }
    return t;
}

// MetalFX spatial upscaling of the composed frame into postTex[0] (outW x outH); false when it cannot run (frame larger
// than the output on an axis, or no scaler), then the plain scaler takes over
bool GSMetalBackend::Impl::spatialScale(id<MTLTexture> src, uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH)
{
    if (spatialFailed || srcW > outW || srcH > outH || src.pixelFormat != MTLPixelFormatRGBA8Unorm)
    {
        static bool logged = false;
        if (!logged && !spatialFailed)
            std::fprintf(stderr, "[gs-mtl] MetalFX: frame %ux%u is larger than the output %ux%u, plain scaler\n", srcW, srcH, outW, outH);
        logged = true;
        return false;
    }
    if (!spatialScaler || spatialScaler.inputWidth != srcW || spatialScaler.inputHeight != srcH || spatialScaler.outputWidth != outW ||
        spatialScaler.outputHeight != outH)
    {
        MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
        d.inputWidth = srcW;
        d.inputHeight = srcH;
        d.outputWidth = outW;
        d.outputHeight = outH;
        d.colorTextureFormat = MTLPixelFormatRGBA8Unorm;
        d.outputTextureFormat = MTLPixelFormatRGBA8Unorm;
        d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual; // gamma-encoded 8-bit colour
        spatialScaler = [d newSpatialScalerWithDevice:device];
        postTex[0] = nil;
        if (!spatialScaler)
        {
            std::fprintf(stderr, "[gs-mtl] MetalFX: no spatial scaler for %ux%u -> %ux%u, plain scaler\n", srcW, srcH, outW, outH);
            spatialFailed = true;
            return false;
        }
        std::fprintf(stderr, "[gs-mtl] MetalFX spatial scaler %ux%u -> %ux%u\n", srcW, srcH, outW, outH);
    }
    spatialScaler.colorTexture = src;
    spatialScaler.outputTexture = postTarget(0, outW, outH);
    spatialScaler.inputContentWidth = srcW;
    spatialScaler.inputContentHeight = srcH;
    [spatialScaler encodeToCommandBuffer:commands()];
    return true;
}

void GSMetalBackend::Impl::runPost(id<MTLTexture> src, uint32_t srcW, uint32_t srcH, id<MTLTexture> dst, double rx, double ry, double rw,
                                   double rh)
{
    endEncoder();
    id<MTLCommandBuffer> c = commands();
    // one full-screen pass of `stage` reading `in` (inW x inH) into the rectangle (x, y, w, h) of `out`
    const auto pass = [&](int stage, id<MTLTexture> in, uint32_t inW, uint32_t inH, id<MTLTexture> out, double x, double y, double w,
                          double h, float sharpen, bool plain)
    {
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = out;
        d.colorAttachments[0].loadAction = out == dst ? MTLLoadActionClear : MTLLoadActionDontCare;
        d.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [c renderCommandEncoderWithDescriptor:d];
        ScaleUniforms u{};
        u.originX = static_cast<float>(x);
        u.originY = static_cast<float>(y);
        u.sizeX = static_cast<float>(w);
        u.sizeY = static_cast<float>(h);
        u.srcW = static_cast<float>(inW);
        u.srcH = static_cast<float>(inH);
        u.sharpen = sharpen;
        u.plain = plain ? 1.0f : 0.0f;
        [e setViewport:MTLViewport{x, y, w, h, 0.0, 1.0}];
        [e setRenderPipelineState:postPipeline(stage, out.pixelFormat)];
        [e setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [e setFragmentTexture:in atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    };
    const uint32_t ow = static_cast<uint32_t>(std::max(1L, std::lround(rw))), oh = static_cast<uint32_t>(std::max(1L, std::lround(rh)));
    id<MTLTexture> cur = src;
    uint32_t cw = srcW, ch = srcH;
    // FXAA on the composed frame at its render resolution, before any scaling: after a magnifying scale the stair steps are
    // more than a pixel wide and FXAA misses them, and MetalFX's spatial scaler expects an anti-aliased input
    if (gfx.fxaa)
    {
        id<MTLTexture> out = postTarget(1, cw, ch);
        pass(kStageFxaa, cur, cw, ch, out, 0.0, 0.0, cw, ch, 0.0f, false);
        cur = out;
    }
    const bool upscaled = gfx.metalfx && spatialScale(cur, cw, ch, ow, oh);
    if (upscaled)
        cur = postTex[0], cw = ow, ch = oh;
    // the one sharpen (CAS), last: folded into the bicubic pass when that pass magnifies the frame (the window scaler as
    // before), its own pass on the output picture after a minifying or 1:1 scale, none after MetalFX (it sharpens itself)
    const bool sharpen = gfx.sharpen >= 0.0f && !upscaled;
    const bool magnify = !upscaled && (cw < rw || ch < rh);
    const bool fused = sharpen && magnify;
    if (sharpen && !fused)
    {
        id<MTLTexture> mid = (cw == ow && ch == oh) ? cur : postTarget(2, ow, oh);
        if (mid != cur)
            pass(kStageScale, cur, cw, ch, mid, 0.0, 0.0, ow, oh, -1.0f, true);
        pass(kStageCas, mid, ow, oh, dst, rx, ry, rw, rh, gfx.sharpen, false);
    }
    else
        pass(kStageScale, cur, cw, ch, dst, rx, ry, rw, rh, fused ? gfx.sharpen : -1.0f, gfx.bilinearOnly || !magnify);
}
