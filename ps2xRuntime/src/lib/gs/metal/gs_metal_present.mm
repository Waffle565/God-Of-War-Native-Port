// GOW-Port: display output of GSMetalBackend: the two CRTC read circuits composed on the GPU (circuit 1 over circuit 2 or
// BGCOLOR, by ALP or its own alpha x2), then either presented into the window's CAMetalLayer (presentToLayer, zero-copy)
// or read back once and returned as pixels (RAM presentation, as on the Switch; captures and PS2X_GS_DIRECT_PRESENT=0).
// Port of GSHwBackend::Present and ClearFramebuffer (gs_hw_backend.cpp).
#include "gs_metal_impl.h"
#include "runtime/gs/gs_metal_layer.h"

using namespace gsmtl;

PresentationFrame GSMetalBackend::Impl::present(const GSPresentationRequest &request)
{
    PresentationFrame frame;
    if (!ensure())
        return frame;
    flushPending();
    ++presentCount;
    frameBatch = 0;
    reportStats();
    if (repl)
        repl->frame();
    const auto decode = [&](uint64_t dispfb, uint64_t display, uint32_t &fbp, uint32_t &dbx, uint32_t &dby, uint32_t &w, uint32_t &h)
    {
        fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
        dbx = static_cast<uint32_t>((dispfb >> 32) & 0x7FFu);
        dby = static_cast<uint32_t>((dispfb >> 43) & 0x7FFu);
        const uint32_t hdiv = static_cast<uint32_t>((display >> 23) & 0xFu) + 1u;
        const uint32_t vdiv = static_cast<uint32_t>((display >> 27) & 0x3u) + 1u;
        w = (static_cast<uint32_t>((display >> 32) & 0xFFFu) + 1u) / hdiv;
        h = (static_cast<uint32_t>((display >> 44) & 0x7FFu) + 1u) / vdiv;
        return display != 0u && w != 0u && h != 0u;
    };
    // GOW_GS_HW_DISPLOG=1: the display registers each time they change
    static const bool dispLog = std::getenv("GOW_GS_HW_DISPLOG") != nullptr;
    static uint64_t lastDisp[6] = {};
    if (dispLog && (request.pmode != lastDisp[0] || request.dispfb1 != lastDisp[1] || request.display1 != lastDisp[2] ||
                    request.dispfb2 != lastDisp[3] || request.display2 != lastDisp[4] || request.smode2 != lastDisp[5]))
    {
        lastDisp[0] = request.pmode; lastDisp[1] = request.dispfb1; lastDisp[2] = request.display1;
        lastDisp[3] = request.dispfb2; lastDisp[4] = request.display2; lastDisp[5] = request.smode2;
        std::fprintf(stderr, "[gs-hw-disp] present=%d pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx\n",
                     presentCount, (unsigned long long)request.pmode, (unsigned long long)request.smode2, (unsigned long long)request.dispfb1,
                     (unsigned long long)request.display1, (unsigned long long)request.dispfb2, (unsigned long long)request.display2);
    }
    // GOW_GS_MTL_DUMP=<n>: at present n, every render target to gs_mtl_rt<fbp>_p<n>.ppm (+ _a.ppm: alpha as grey), host size
    static const int rtDumpAt = std::getenv("GOW_GS_MTL_DUMP") ? std::atoi(std::getenv("GOW_GS_MTL_DUMP")) : -1;
    if (presentCount == rtDumpAt)
        for (RenderTarget &t : targets)
        {
            std::vector<uint8_t> px(static_cast<size_t>(t.glW) * t.glH * 4u);
            readback(t.color, 0, 0, t.glW, t.glH, px.data());
            char name[64];
            std::snprintf(name, sizeof(name), "gs_mtl_rt%u_p%d.ppm", t.fbp, presentCount);
            if (FILE *f = std::fopen(name, "wb"))
            {
                std::fprintf(f, "P6\n%u %u\n255\n", t.glW, t.glH);
                for (size_t i = 0; i + 3 < px.size(); i += 4)
                    std::fwrite(&px[i], 1, 3, f);
                std::fclose(f);
            }
            std::snprintf(name, sizeof(name), "gs_mtl_rt%u_p%d_a.ppm", t.fbp, presentCount);
            if (FILE *f = std::fopen(name, "wb"))
            {
                std::fprintf(f, "P6\n%u %u\n255\n", t.glW, t.glH);
                for (size_t i = 3; i < px.size(); i += 4)
                    std::fwrite(&px[i], 1, 1, f), std::fwrite(&px[i], 1, 1, f), std::fwrite(&px[i], 1, 1, f);
                std::fclose(f);
            }
            std::fprintf(stderr, "[gs-mtl] dumped %s psm=%u used=%ux%u\n", name, t.psm, t.usedW, t.usedH);
        }
    uint32_t fbp1 = 0, dbx1 = 0, dby1 = 0, w1 = 0, h1 = 0, fbp2 = 0, dbx2 = 0, dby2 = 0, w2 = 0, h2 = 0;
    const bool ok1 = (request.pmode & 1ull) != 0ull && decode(request.dispfb1, request.display1, fbp1, dbx1, dby1, w1, h1);
    const bool ok2 = (request.pmode & 2ull) != 0ull && decode(request.dispfb2, request.display2, fbp2, dbx2, dby2, w2, h2);
    const bool direct = !request.forceReadback && GSMetalLayer::Active();
    if (!ok1 && !ok2)
    {
        if (direct)
            presentToLayer(nil, 0u, 0u);
        else
            commit(false); // keep the GPU busy with the frame's draws even when nothing is shown
        return frame;
    }
    // the TV area: 448 lines (movies set 480 lines with 32 black ones below the picture)
    constexpr uint32_t kTvLines = 448u;
    const uint32_t fbp = ok1 ? fbp1 : fbp2, w = ok1 ? w1 : w2, h = std::min(ok1 ? h1 : h2, kTvLines);
    frame.displayFbp = fbp;
    frame.sourceFbp = fbp;
    // the host pixels that lie wholly inside the display (at 2.5x, 447 lines are 1117.5 rows: a last half row would show what
    // the game leaves below its picture, a dark line)
    const uint32_t outW = std::max(1u, static_cast<uint32_t>(std::floor(w * scaleX))), outH = std::max(1u, static_cast<uint32_t>(std::floor(h * scaleY)));
    frame.width = outW;
    frame.height = outH;
    if (!presentTex || presentW != outW || presentH != outH)
    {
        presentTex = newTexture(MTLPixelFormatRGBA8Unorm, outW, outH, true);
        presentW = outW;
        presentH = outH;
    }
    const bool mmod = ((request.pmode >> 5) & 1u) != 0u, slbg = ((request.pmode >> 7) & 1u) != 0u;
    const uint32_t alp = static_cast<uint32_t>((request.pmode >> 8) & 0xFFu);
    MTLClearColor clearColor = MTLClearColorMake(0, 0, 0, 1);
    if (ok1 && ok2 && slbg)
        clearColor = MTLClearColorMake((request.bgcolor & 0xFFu) / 255.0, ((request.bgcolor >> 8) & 0xFFu) / 255.0,
                                       ((request.bgcolor >> 16) & 0xFFu) / 255.0, 1.0);
    endEncoder();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = presentTex;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = clearColor;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [commands() renderCommandEncoderWithDescriptor:pass];
    [e setViewport:MTLViewport{0.0, 0.0, static_cast<double>(outW), static_cast<double>(outH), 0.0, 1.0}];
    // the first two PS2 rows and columns repeat the next one (the game's black line on row 0, hidden by TV overscan): host
    // pixels below the first one that samples PS2 position 2 (draw.mm addVertex: host pixel k samples k / scale)
    const int32_t edgeX = static_cast<int32_t>(std::ceil(2.0f * scaleX)), edgeY = static_cast<int32_t>(std::ceil(2.0f * scaleY));
    const auto drawCircuit = [&](uint32_t cfbp, uint64_t dispfb, uint32_t cdbx, uint32_t cdby, bool alphaFactor, const BlendDesc &blend)
    {
        RenderTarget *rt = findTarget(cfbp);
        if (!rt)
            return;
        PresentUniforms u{};
        u.offX = static_cast<int32_t>(std::lround(cdbx * scaleX));
        u.offY = static_cast<int32_t>(std::lround(cdby * scaleY));
        u.ct24 = ((dispfb >> 15) & 0x1Fu) == GS_PSM_CT24 ? 1 : 0;
        u.alphaX2 = alphaFactor ? 1 : 0;
        u.edgeX = edgeX;
        u.edgeY = edgeY;
        [e setRenderPipelineState:pipelineFor(Pso::Present, kMaskAll, false, blend)];
        [e setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [e setFragmentTexture:rt->color atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    };
    if (ok1 && ok2)
    {
        if (!slbg)
            drawCircuit(fbp2, request.dispfb2, dbx2, dby2, false, BlendDesc{});
        BlendDesc over;
        over.enabled = true;
        over.asf = kZero;
        over.adf = kOne;
        if (mmod)
        {
            const float f = static_cast<float>(alp) / 255.f;
            [e setBlendColorRed:f green:f blue:f alpha:f];
            over.sf = kConstant;
            over.df = kOneMinusConstant;
        }
        else
        {
            over.sf = kSrcAlpha;
            over.df = kOneMinusSrcAlpha;
        }
        drawCircuit(fbp1, request.dispfb1, dbx1, dby1, true, over);
    }
    else
        drawCircuit(fbp, ok1 ? request.dispfb1 : request.dispfb2, ok1 ? dbx1 : dbx2, ok1 ? dby1 : dby2, false, BlendDesc{});
    [e endEncoding];
    if (direct)
    {
        presentToLayer(presentTex, outW, outH);
        return frame;
    }
    id<MTLTexture> shown = presentTex;
    uint32_t dw = 0, dh = 0;
    GSMetalLayer::DrawableSize(dw, dh);
    if (gfx.capturePost && request.forceReadback && GSMetalLayer::Active() && dw != 0u && dh != 0u)
    {
        // GOW_GS_MTL_CAPTURE_POST=1: a capture shows the window's picture (the output chain at the drawable's 16:9 size)
        double rx = 0.0, ry = 0.0, rw = 0.0, rh = 0.0;
        fitRect(dw, dh, rx, ry, rw, rh);
        frame.width = static_cast<uint32_t>(std::lround(rw));
        frame.height = static_cast<uint32_t>(std::lround(rh));
        shown = postTarget(3, frame.width, frame.height);
        runPost(presentTex, outW, outH, shown, 0.0, 0.0, frame.width, frame.height);
    }
    frame.pixels.resize(static_cast<size_t>(frame.width) * frame.height * 4u);
    readback(shown, 0, 0, frame.width, frame.height, frame.pixels.data());
    // the displayed image is opaque (the host draws it with blending)
    for (size_t i = 3; i < frame.pixels.size(); i += 4)
        frame.pixels[i] = 0xFFu;
    // the frontend packs presentation rows with a fixed 640 pixel stride
    if (frame.width < 640u)
    {
        std::vector<uint8_t> rows(static_cast<size_t>(640u) * frame.height * 4u, 0u);
        for (uint32_t y = 0; y < frame.height; ++y)
            std::memcpy(rows.data() + static_cast<size_t>(y) * 640u * 4u, frame.pixels.data() + static_cast<size_t>(y) * frame.width * 4u,
                        static_cast<size_t>(frame.width) * 4u);
        frame.pixels = std::move(rows);
    }
    return frame;
}

void GSMetalBackend::Impl::presentToLayer(id<MTLTexture> src, uint32_t width, uint32_t height)
{
    constexpr long kFramesInFlight = 3;
    CAMetalLayer *layer = GSMetalLayer::Layer();
    uint32_t dw = 0, dh = 0;
    GSMetalLayer::DrawableSize(dw, dh);
    if (!layer || !GSMetalLayer::Showing() || !GSMetalLayer::WindowVisible() || dw == 0u || dh == 0u)
    {
        commit(false);
        return;
    }
    if (!framesInFlight)
        framesInFlight = dispatch_semaphore_create(kFramesInFlight);
    // at most kFramesInFlight frames queued on the GPU: the GS worker runs ahead of the GPU by that much, no more
    dispatch_semaphore_wait(framesInFlight, DISPATCH_TIME_FOREVER);
    if (layer.drawableSize.width != dw || layer.drawableSize.height != dh)
        layer.drawableSize = CGSizeMake(dw, dh);
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    id<MTLCommandBuffer> c = commands();
    dispatch_semaphore_t inFlight = framesInFlight;
    [c addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(inFlight); }];
    if (!drawable)
    {
        commit(false);
        return;
    }
    if (src && width != 0u && height != 0u)
    {
        double rx = 0.0, ry = 0.0, rw = 0.0, rh = 0.0;
        fitRect(dw, dh, rx, ry, rw, rh);
        runPost(src, width, height, drawable.texture, rx, ry, rw, rh);
    }
    else
        clearPass(drawable.texture, nil, MTLClearColorMake(0, 0, 0, 1));
    // GOW_HOST_SHOT=<n>: the n-th window frame as it is presented -> host_shot.ppm (working directory)
    static const long shotAt = [] { const char *v = std::getenv("GOW_HOST_SHOT"); return v ? std::atol(v) : -1L; }();
    if (static_cast<long>(layerPresents++) == shotAt && !layer.framebufferOnly)
    {
        std::vector<uint8_t> px(static_cast<size_t>(dw) * dh * 4u);
        [c presentDrawable:drawable];
        readback(drawable.texture, 0, 0, dw, dh, px.data());
        if (FILE *f = std::fopen("host_shot.ppm", "wb"))
        {
            std::fprintf(f, "P6\n%u %u\n255\n", dw, dh);
            for (size_t i = 0; i + 3 < px.size(); i += 4)
            {
                const uint8_t rgb[3] = {px[i + 2], px[i + 1], px[i]};
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
        }
        std::fprintf(stderr, "[gs-present] host_shot frame=%ux%u drawable=%ux%u (Metal layer)\n", width, height, dw, dh);
        return;
    }
    [c presentDrawable:drawable];
    commit(false);
}

bool GSMetalBackend::Impl::clearFramebuffer(const GSContext &context, uint32_t rgba)
{
    if (!ensure() || context.frame.fbw == 0u)
        return false;
    flushPending();
    RenderTarget &rt = target(context.frame.fbp, context.frame.fbw);
    beginPass(rt);
    bool empty = false;
    scissor(enc, rt, context.scissor.x0, context.scissor.y0, context.scissor.x1, context.scissor.y1, empty);
    if (empty)
        return true;
    const float color[4] = {(rgba & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f};
    setPso(pipelineFor(Pso::Color, kMaskAll, rt.depth != nil));
    if (rt.depth)
        setDss(depthState(kAlways, false));
    [enc setFragmentBytes:color length:sizeof(color) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    return true;
}
