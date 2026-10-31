// GOW-Port: private state of GSMetalBackend (include/runtime/gs/gs_metal_backend.h), shared by the gs_metal_*.mm files.
// Objective-C++ (ARC) only. The members and functions mirror GSHwBackend::Impl in gs_hw_backend.cpp one to one; where the
// GL renderer called the GL wrapper (gs_gl.h), this one calls the small Metal layer declared in the "Metal layer" block
// (gs_metal_device.mm).
#pragma once

#if !defined(__OBJC__)
#error "gs_metal_impl.h is Objective-C++ only"
#endif

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include "runtime/gs/gs_metal_backend.h"
#include "runtime/gs/gs_swizzle.h"
#include "../gpu/gs_texture_store.h"
#include "../gs_gpu_helpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gsmtl
{
    // GS vertex as sent to the GPU (HwVertex in gs_hw_backend.cpp and gs_metal_shaders.inc).
    struct HwVertex
    {
        float x, y, z;
        float r, g, b, a;
        float s, t, q;
        float u, v;
        float cu0 = -1e30f, cv0 = -1e30f, cu1 = 1e30f, cv1 = 1e30f; // texel range the GS samples (sprites)
    };
    static_assert(sizeof(HwVertex) == 64, "HwVertex layout is shared with the shader");

    struct ShuffleVertex
    {
        float x, y, ox, oy;
    };

    // FsUniforms / ShUniforms / PresentUniforms of gs_metal_shaders.inc (same field order).
    struct FsUniforms
    {
        int32_t tme, fst, tfx, tcc;
        int32_t iip, indexed, linear, reinterp;
        int32_t wrapX, wrapY, rtTex24, aem;
        int32_t ate, atst, aref, pass;
        int32_t date, datm, swBlend, colClamp;
        int32_t abcd[4];
        int32_t zAlias, zAliasWrite, zTex, failTint;
        int32_t hwBlend, hwSf, hwDf, hwEq;
        float fix, zAliasMax, zMax, blendScale;
        float ta0, ta1, texDimX, texDimY;
        float texSizeX, texSizeY, rtPxX, rtPxY;
        float blendColor[4];
        int32_t texFmt[4], texFmt2[4], rtFmt[4], rtFmt2[4], tex0[4];
    };
    static_assert(sizeof(FsUniforms) == 272, "FsUniforms layout is shared with the shader");

    struct ShUniforms
    {
        int32_t srcZ, hs, hd, pad0;
        float zMax, scale, pad1, pad2;
        int32_t texa[4];
    };

    struct PresentUniforms
    {
        int32_t offX, offY, ct24, alphaX2;
        int32_t edge, pad0, pad1, pad2;
    };

    struct VsUniforms
    {
        float invW, invH;
    };

    struct ScaleUniforms
    {
        float originX, originY, sizeX, sizeY;
        float srcW, srcH, sharpen, plain;
    };

    struct Texture
    {
        id<MTLTexture> tex = nil;
        uint32_t width = 0, height = 0;
        uint32_t blockLo = 0, blockHi = 0;
        bool valid = false;
        uint64_t lastUse = 0;
        uint64_t generation = 0;        // decode number (paletted textures: keys the converted RGBA copies)
        std::vector<uint8_t> indices;   // paletted textures: the indices, kept to convert them with a CLUT on the CPU
    };

    struct RenderTarget
    {
        id<MTLTexture> color = nil, depth = nil;
        uint32_t fbp = 0, fbw = 1; // PS2 page base and width in 64 px units
        uint8_t psm = 0;
        uint32_t zbp = ~0u;
        uint32_t pxW = 0, pxH = 0; // size in PS2 pixels
        uint32_t glW = 0, glH = 0; // size in host pixels
        uint64_t version = 1, lastDrawSeq = 0;
        uint64_t lastDepthSeq = 0;
        uint32_t usedW = 0, usedH = 0;
    };

    // pipeline kinds (vertex + fragment function pairs)
    enum class Pso : uint8_t { Main, Fetch, Shuffle, Present, Color, Clut }; // window output: postPipeline
    // blend factors (0-5 are also the codes fs_fetch understands)
    enum Factor : uint8_t { kZero = 0, kOne, kSrc1Alpha, kOneMinusSrc1Alpha, kConstant, kOneMinusConstant, kSrcAlpha, kOneMinusSrcAlpha };
    // depth compare functions
    enum Compare : uint8_t { kNever = 0, kAlways, kGEqual, kGreater, kLEqual, kLess };
    // colour write mask bits
    enum : uint8_t { kMaskR = 1, kMaskG = 2, kMaskB = 4, kMaskA = 8, kMaskAll = 15 };

    struct BlendDesc
    {
        bool enabled = false;
        uint8_t sf = kOne, df = kZero, asf = kOne, adf = kZero;
        bool reverse = false; // RGB: dst * df - src * sf
    };

    inline uint32_t pageRowsFor(uint32_t psm)
    {
        switch (psm & 0x3F)
        {
        case 0x13: case 0x1B: case 0x24: case 0x2C: return 64u;  // T8 / T8H
        case 0x14: case 0x2C + 1: return 128u;                    // T4
        case 0x02: case 0x0A: case 0x32: case 0x3A: return 64u;  // 16 bit
        default: return 32u;
        }
    }

    // conservative block (256 B) span of `rows` rows of a buffer `bw` (64 px units) wide
    inline uint32_t spanBlocks(uint32_t psm, uint32_t bw, uint32_t rows)
    {
        const uint32_t pagesDown = (rows + pageRowsFor(psm) - 1u) / pageRowsFor(psm);
        return std::max<uint32_t>(1u, pagesDown) * std::max<uint32_t>(1u, bw) * 32u;
    }

    inline bool family16(uint8_t psm) { return psm == GS_PSM_CT16 || psm == GS_PSM_CT16S; }

    inline float zMaxFor(uint8_t zpsm)
    {
        switch (zpsm)
        {
        case 0x30: return 4294967295.0f;
        case 0x32: case 0x3A: return 65535.0f;
        default: return 16777215.0f;
        }
    }

    inline bool isIndexed(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H || psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }
}

struct GSMetalBackend::Impl
{
    using HwVertex = gsmtl::HwVertex;
    using ShuffleVertex = gsmtl::ShuffleVertex;
    using RenderTarget = gsmtl::RenderTarget;
    using Texture = gsmtl::Texture;

    GSTextureStore store;
    bool initialized = false, failed = false;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0;
    float scale = 2.0f;

    // ---- Metal layer (gs_metal_device.mm) ------------------------------------------------------------------------
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> library = nil;
    id<MTLBuffer> tables = nil;       // forward + inverse page tables (uint16), see buildTables
    id<MTLBuffer> zeroBuffer = nil;   // bound where a shader buffer is not used
    id<MTLTexture> dummyColor = nil, dummyDepth = nil;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipelines;
    std::unordered_map<uint32_t, id<MTLDepthStencilState>> depthStates;
    std::unordered_map<uint32_t, id<MTLSamplerState>> samplers;
    std::unordered_map<uint64_t, id<MTLTexture>> scratch; // copies of render targets sampled while drawn into

    id<MTLCommandBuffer> cmd = nil;
    id<MTLRenderCommandEncoder> enc = nil;
    id<MTLTexture> encColor = nil, encDepth = nil;
    id<MTLRenderPipelineState> encPso = nil;
    id<MTLDepthStencilState> encDss = nil;
    uint64_t cmdSerial = 0;
    // GOW_GS_MTL_LABELS=1: render passes are labelled with their target and draw count (GPU traces)
    bool labels = std::getenv("GOW_GS_MTL_LABELS") != nullptr;
    uint32_t encFbp = 0, encDraws = 0, encVerts = 0, encFetch = 0, encReint = 0, encIdx = 0, encRt = 0, encLin = 0;
    std::shared_ptr<std::atomic<uint64_t>> completedSerial = std::make_shared<std::atomic<uint64_t>>(0u);
    std::shared_ptr<std::atomic<uint64_t>> gpuNanos = std::make_shared<std::atomic<uint64_t>>(0u); // GPU busy time (stats)
    struct Chunk
    {
        id<MTLBuffer> buffer = nil;
        size_t used = 0;
        uint64_t serial = 0; // command buffer that last used it
    };
    std::vector<Chunk> chunks;
    int activeChunk = -1;

    bool ensure();
    void buildTables();
    id<MTLCommandBuffer> commands();
    void endEncoder();
    // render encoder drawing into rt (colour + its depth buffer if it has one); reused while the attachments stay the same
    id<MTLRenderCommandEncoder> beginPass(RenderTarget &rt);
    // a pass that only clears the given attachments (nil = not touched)
    void clearPass(id<MTLTexture> color, id<MTLTexture> depth, MTLClearColor clearColor);
    void commit(bool wait);
    // texPath / flags (kTexPath / kFlags of gs_metal_shaders.inc, Main and Fetch only): the specialised pipeline once it has
    // compiled in the background, the generic one until then (no compile stall in the middle of a frame)
    id<MTLRenderPipelineState> pipelineFor(gsmtl::Pso kind, uint8_t writeMask, bool hasDepth, const gsmtl::BlendDesc &blend = {},
                                           int texPath = -1, int flags = -1);
    struct SpecSlot
    {
        std::atomic<int> state{0}; // 1 compiling, 2 ready, 3 failed
        id<MTLRenderPipelineState> pso = nil;
    };
    std::unordered_map<uint64_t, std::shared_ptr<SpecSlot>> specPending;
    bool specialize = [] { const char *v = std::getenv("GOW_GS_MTL_SPECIALIZE"); return !(v && *v == '0'); }();
    id<MTLDepthStencilState> depthState(gsmtl::Compare compare, bool write);
    // anisotropy > 1: GOW_GS_MTL_ANISO, linear 3D textures only (draw)
    id<MTLSamplerState> samplerFor(bool linear, bool clampU, bool clampV, uint32_t anisotropy = 1u);
    void setPso(id<MTLRenderPipelineState> pso);
    void setDss(id<MTLDepthStencilState> dss);
    // copies `bytes` into the vertex arena; returns the buffer and the offset
    id<MTLBuffer> upload(const void *data, size_t bytes, NSUInteger &offset);
    // the texture as it is now, safe to sample while `dst` is drawn into (a copy when it is one of dst's attachments, of
    // `area` only when given)
    id<MTLTexture> sampleable(id<MTLTexture> texture, const RenderTarget &dst, const MTLRegion *area = nullptr);
    id<MTLTexture> newTexture(MTLPixelFormat format, uint32_t width, uint32_t height, bool renderTarget);
    // GPU -> CPU copy of a region (RGBA8 or R32 formats, 4 bytes per pixel); commits and waits
    void readback(id<MTLTexture> texture, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void *out);
    void scissor(id<MTLRenderCommandEncoder> e, const RenderTarget &rt, int x0, int y0, int x1, int y1, bool &empty);

    // ---- render targets, textures, palettes (gs_metal_targets.mm) ----------------------------------------------------
    std::unordered_map<const uint16_t *, uint32_t> tableOffset;
    std::unordered_map<uint32_t, uint32_t> inverseOffset; // psm -> offset of the inverse page table
    std::unordered_map<uint64_t, Texture> textures;
    std::unordered_map<uint64_t, id<MTLBuffer>> palettes;
    std::vector<RenderTarget> targets;
    uint64_t useCounter = 0, writeSeq = 0;
    struct UploadRecord { uint32_t lo, hi; uint64_t seq; };
    std::vector<UploadRecord> uploadLog;
    uint64_t clutHash = 0;
    std::unordered_map<uint64_t, std::vector<uint32_t>> clutCache;

    RenderTarget &target(uint32_t fbp, uint32_t fbw);
    void destroy(RenderTarget &t);
    void createTarget(RenderTarget &t, uint32_t fbp, uint32_t fbw);
    void attachDepth(RenderTarget &t, uint32_t zbp);
    RenderTarget *findTarget(uint32_t fbp);
    Texture &textureFor(const GSDrawState &s);
    void decodeDirect(const GSDrawState &s, std::vector<uint32_t> &out);
    Texture &indexTextureFor(const GSDrawState &s);
    uint64_t paletteKey(const GSDrawState &s) const;
    id<MTLBuffer> paletteFor(const GSDrawState &s);
    // a paletted texture converted to RGBA with the current CLUT, cached per (decode, CLUT): sampled with one hardware
    // (bilinear) fetch instead of four index reads + palette lookups per fragment. GOW_GS_MTL_DEPAL=0: shader lookups.
    std::unordered_map<uint64_t, Texture> depalCache;
    uint64_t textureGeneration = 0;
    bool depalEnabled = [] { const char *v = std::getenv("GOW_GS_MTL_DEPAL"); return !(v && *v == '0'); }();
    Texture &depalTextureFor(const GSDrawState &s, const Texture &index);
    void evictOld();
    void invalidateBlocks(uint32_t lo, uint32_t hi);
    RenderTarget *findReinterpretSource(uint32_t tbp, uint8_t tpsm);
    bool overwrittenByDepth(const RenderTarget &t) const;
    bool uploadedSinceDraw(const RenderTarget &t, uint32_t tbp) const;
    // fills the reinterpreting-fetch uniforms; palette (kind 3) goes to `palette`
    void setupReinterpret(const GSDrawState &s, const RenderTarget &rt, gsmtl::FsUniforms &u, uint32_t palette[256], bool &hasPalette);
    bool clutFromTarget(const GSTex0Reg &tex0, uint32_t width, const uint16_t *xy, uint32_t count, uint32_t *words);

    // ---- channel shuffles (gs_metal_shuffle.mm) -----------------------------------------------------------------------
    std::vector<ShuffleVertex> shuffleQuads[4];
    GSDrawState shuffleState{};
    bool haveShuffle = false;
    int shuffleWhy = 0;
    RenderTarget *shuffleDst = nullptr, *shuffleSrc = nullptr;
    bool shuffleSrcZ = false;
    std::unordered_map<uint32_t, std::vector<uint16_t>> ownerInverse;

    const std::vector<uint16_t> &inverseFor(uint32_t ownerPsm);
    bool viewToOwner(uint32_t viewPsm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t ownerPsm, uint32_t fbp,
                     uint32_t fbw, uint32_t &ox, uint32_t &oy, uint32_t &half);
    bool unitToOwner(uint32_t viewPsm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t ownerPsm, uint32_t fbp,
                     uint32_t fbw, uint32_t &ox, uint32_t &oy, uint32_t &half);
    bool tryShuffle(const GSDrawState &s, const GSPrimitiveBatch &b);
    void flushShuffle();

    // ---- drawing (gs_metal_draw.mm) -----------------------------------------------------------------------------------
    struct SpriteEdges
    {
        float xs[2], ys[2], us[2], vs[2];
    };
    struct LastSprite
    {
        SpriteEdges edges{};
        size_t end = 0;
        uint64_t batch = ~0ull;
    } lastSprite;
    uint64_t batchSerial = 0;
    std::vector<HwVertex> pending;
    GSDrawState pendingState{};
    bool havePending = false;
    std::vector<uint32_t> invertedZ;
    int presentCount = 0, frameBatch = 0;

    struct Stats
    {
        uint64_t shuffles = 0, draws = 0, batches = 0, skippedPsm = 0, unsupportedBlend = 0, texDecodes = 0, texHits = 0, depals = 0,
                 skippedPrim = 0, feedbackCopies = 0, feedbackPixels = 0, commits = 0;
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    } stats;
    bool statsEnabled = std::getenv("GOW_GS_HW_STATS") != nullptr;

    static void openSharedSide(const SpriteEdges &prev, const SpriteEdges &cur, HwVertex *prevVertices, float &cu0, float &cu1,
                               float &cv0, float &cv1);
    // field by field: GSDrawState has padding bytes that the frontend leaves undefined (a memcmp of the whole struct saw
    // a different state at nearly every primitive and drew each triangle on its own)
    static bool sameState(const GSDrawState &a, const GSDrawState &b);
    void addVertex(const GSDrawState &s, const GSVertex &v, float u, float tv, float zmax);
    void submit(const GSPrimitiveBatch &b);
    void flushPending();
    void blendState(const GSDrawState &s, gsmtl::BlendDesc &blend, float &constant, bool &soft);
    bool zInverted(uint32_t zbp) const { return std::find(invertedZ.begin(), invertedZ.end(), zbp) != invertedZ.end(); }
    static bool isZInversion(const GSDrawState &s, const std::vector<HwVertex> &verts);
    void draw(const GSDrawState &s, const std::vector<HwVertex> &verts);
    void reportStats();

    // ---- display (gs_metal_present.mm) --------------------------------------------------------------------------------
    id<MTLTexture> presentTex = nil;
    id<MTLBuffer> readbackBuffer = nil;
    uint32_t presentW = 0, presentH = 0;
    // frames presented to the window layer and not finished on the GPU (at most kFramesInFlight)
    dispatch_semaphore_t framesInFlight = nullptr;
    uint64_t layerPresents = 0;
    PresentationFrame present(const GSPresentationRequest &request);
    // the composed frame (nil: black) scaled into the window's CAMetalLayer at 16:9 and presented with vsync; commits
    // without waiting
    void presentToLayer(id<MTLTexture> frame, uint32_t width, uint32_t height);
    bool clearFramebuffer(const GSContext &context, uint32_t rgba);

    // ---- graphics options and the output chain (gs_metal_post.mm) ------------------------------------------------------
    // read once (ensure): GOW_GS_MTL_ANISO=0|2|4|8|16, GOW_GS_MTL_METALFX=off|spatial, GOW_GS_MTL_AA=off|fxaa, the window
    // scaler's GOW_SCALER=bilinear and its one sharpen knob GOW_SHARPEN=off|0..1 (CAS sharpness, default 0.5),
    // GOW_GS_MTL_CAPTURE_POST=1 (captures go through the window chain)
    struct GfxOptions
    {
        uint32_t aniso = 1u;
        bool metalfx = false, fxaa = false, bilinearOnly = false, capturePost = false;
        float sharpen = 0.5f; // < 0: off
        bool post() const { return metalfx || fxaa; }
    } gfx;
    void readGfxOptions();
    // the 16:9 picture rectangle inside a dw x dh output, centred (black bars)
    static void fitRect(uint32_t dw, uint32_t dh, double &rx, double &ry, double &rw, double &rh);
    // the composed frame (srcW x srcH) through FXAA, MetalFX or the window scaler and CAS into `dst` (cleared to black) at
    // the rectangle (rx, ry, rw, rh)
    void runPost(id<MTLTexture> src, uint32_t srcW, uint32_t srcH, id<MTLTexture> dst, double rx, double ry, double rw, double rh);
    id<MTLRenderPipelineState> postPipeline(int stage, MTLPixelFormat format);
    id<MTLTexture> postTarget(int slot, uint32_t width, uint32_t height);
    bool spatialScale(id<MTLTexture> src, uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH);
    std::unordered_map<uint32_t, id<MTLRenderPipelineState>> postPipelines;
    id<MTLTexture> postTex[4] = {nil, nil, nil, nil}; // 0 MetalFX output, 1-2 intermediate passes, 3 RAM-path output
    id<MTLFXSpatialScaler> spatialScaler = nil;
    bool spatialFailed = false;
};
