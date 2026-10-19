// GOW-Port: the GS renderer on the Switch's GPU through deko3d (docs/switch.md "deko3d renderer").
//
// The GL backend (gs_hw_backend.cpp) reaches the GPU only through the GSGL::Api table. On the Switch this file fills
// that table with a thin GL emulation over deko3d (devkitPro's low-level NVN-style API) instead of switch-mesa: the
// backend keeps its GL code, and each call costs a few state writes or one command instead of Mesa/nouveau's ~130 us
// per draw. Programs are the uam builds of the backend's own GLSL, found by a hash of the source text
// (gs_dk_shaders.h, tools/dk_shaders.py). The finished frame never leaves the GPU: PublishSharedTexture (the backend's
// shared-presentation path) scales it onto the 1280x720 swapchain image and presents it from this (the GS) thread, so
// the host thread draws nothing (gs_host_present.cpp) and raylib opens no window (ps2_runtime.cpp).
//
// GL conventions are kept by the device flags (origin lower left, depth -1..1): row 0 of a texture is GL's bottom row,
// as uploads, render targets and readbacks expect. Only what the backend uses is implemented; the rest stays null.
#include "gs_gl.h"
#include "gs_dk_shaders.h"
#include "runtime/gs/gs_shared_present.h"

#include <deko3d.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace GSGL;

    // ---- GL enums the backend passes -------------------------------------------------------------------------------
    constexpr GLenum kArrayBuffer = 0x8892, kPixelPack = 0x88EB, kPixelUnpack = 0x88EC, kSsbo = 0x90D2,
                     kCopyRead = 0x8F36, kCopyWrite = 0x8F37, kUniformBufferTarget = 0x8A11, kElementArray = 0x8893;
    constexpr GLenum kBlend = 0x0BE2, kDepthTest = 0x0B71, kScissorTest = 0x0C11, kCullFace = 0x0B44,
                     kConservative = 0x9346;
    constexpr GLenum kTexture0 = 0x84C0;
    constexpr GLenum kFloatType = 0x1406, kUByte = 0x1401, kUShort = 0x1403, kUInt = 0x1405, kIntType = 0x1404;
    constexpr GLenum kColorAttachment0 = 0x8CE0, kDepthAttachmentPoint = 0x8D00;
    constexpr GLbitfield kColorBit = 0x4000, kDepthBit = 0x100;
    constexpr GLenum kFramebufferComplete = 0x8CD5;

    // ---- device ------------------------------------------------------------------------------------------------------
    constexpr uint32_t kCmdChunk = 1u << 20;         // command memory per chunk
    constexpr uint32_t kCmdChunks = 16u;             // ring of chunks, each fenced by the submit that used it last
    constexpr uint32_t kImageSlots = 16384u;         // image descriptors (a ring: a slot comes back after 16384 images)
    constexpr uint32_t kSamplerSlots = 64u;
    constexpr uint32_t kSwapImages = 3u;
    constexpr uint32_t kScreenW = 1280u, kScreenH = 720u;

    uint32_t align(uint32_t v, uint32_t a) { return (v + a - 1u) / a * a; }

    struct Submit
    {
        DkFence fence{};
        uint64_t serial = 0;
    };

    struct Texture
    {
        DkMemBlock mem = nullptr;
        DkImage image{};
        DkImageView view{};
        DkImageFormat format = DkImageFormat_None;
        uint32_t width = 0, height = 0, bpp = 0;
        uint32_t slot = 0;     // image descriptor
        bool valid = false, depth = false, integer = false;
        GLint minFilter = 0x2601, magFilter = 0x2601, wrapS = 0x2901, wrapT = 0x2901; // LINEAR, REPEAT (GL defaults
        // are NEAREST_MIPMAP_LINEAR / LINEAR; every backend texture sets its filters, linear is the safe guess)
    };

    struct Buffer
    {
        DkMemBlock mem = nullptr;
        uint8_t *cpu = nullptr;
        DkGpuAddr gpu = 0;
        uint32_t size = 0;
    };

    struct Shader
    {
        GLenum type = 0;
        std::string source;
        const GSDK::ShaderBinary *binary = nullptr;
    };

    struct Program
    {
        std::vector<const GSDK::ShaderBinary *> stages;
        DkShader shaders[2]{};
        uint32_t stageMask = 0;
        uint32_t numShaders = 0;
        bool compute = false, linked = false;
        const GSDK::Family *family = nullptr;
        std::vector<uint8_t> ubo;
        bool uboDirty = true;
        uint32_t dirtyLo = 0, dirtyHi = 0; // the bytes of ubo not yet pushed (when uboDirty)
        DkGpuAddr uboAddr = 0;
        uint32_t uboSize = 0;
        uint32_t samplerUnit[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        uint32_t numSamplers = 0;
    };

    struct Attrib
    {
        bool enabled = false;
        GLuint buffer = 0;
        uint32_t offset = 0, stride = 0;
        DkVtxAttribSize size = DkVtxAttribSize_1x32;
        DkVtxAttribType type = DkVtxAttribType_Float;
    };
    struct Vao
    {
        Attrib attribs[8];
        GLuint element = 0; // GL_ELEMENT_ARRAY_BUFFER is vertex-array state
    };

    struct Fbo
    {
        GLuint color = 0, depth = 0;
    };

    struct Fence
    {
        DkFence fence{};
        bool recorded = false;
        uint64_t serial = 0; // the submit that carries it (0 = still in the open command list)
    };

    struct Garbage
    {
        uint64_t serial;
        DkMemBlock mem;
    };

    struct State
    {
        DkDevice device = nullptr;
        DkQueue queue = nullptr;
        DkCmdBuf cmd = nullptr;
        DkMemBlock cmdMem = nullptr;
        uint64_t chunkSerial[kCmdChunks] = {};
        uint32_t chunk = 0, chunksThisList = 0;
        size_t bytesThisList = 0;
        std::deque<Submit> submits; // in flight
        uint64_t nextSerial = 1, doneSerial = 0;

        DkMemBlock descMem = nullptr; // image descriptors, then sampler descriptors
        DkImageDescriptor *imageDescs = nullptr;
        DkSamplerDescriptor *samplerDescs = nullptr;
        DkGpuAddr imageDescAddr = 0, samplerDescAddr = 0;
        uint32_t nextSlot = 1; // 0 = the dummy texture
        bool descriptorsDirty = true;
        bool setsBound = false;

        DkMemBlock codeMem = nullptr;
        uint32_t codeUsed = 0, codeSize = 0;
        std::unordered_map<const GSDK::ShaderBinary *, uint32_t> codeOffset;
        std::unordered_map<uint64_t, const GSDK::ShaderBinary *> binaries;

        DkMemBlock uboMem = nullptr;
        uint32_t uboUsed = 0, uboSize = 0;

        // staging for uploads (ring, fenced per submit like the command memory)
        DkMemBlock stageMem = nullptr;
        uint8_t *stageCpu = nullptr;
        uint32_t stageSize = 0, stageHead = 0;

        DkMemBlock readMem = nullptr; // synchronous readbacks
        uint32_t readSize = 0;

        std::deque<Texture> textures{1}; // a deque: a DkImageView points at its DkImage, so textures must not move
        std::vector<GLuint> freeTextures;
        std::vector<Buffer> buffers{1};
        std::vector<Shader> shaders{1};
        std::deque<Program> programs{1}; // DkShader objects are referenced while bound
        std::vector<Vao> vaos{1};
        std::vector<Fbo> fbos{1};
        std::deque<Garbage> garbage;

        // GL state
        GLuint units[16] = {};
        uint32_t activeUnit = 0;
        GLuint arrayBuffer = 0, packBuffer = 0, unpackBuffer = 0, copyRead = 0, copyWrite = 0, ssboGeneric = 0;
        GLuint ssbo[4] = {};
        GLuint vao = 0, fbo = 0, program = 0, noVaoElement = 0;
        DkGpuAddr boundIdx = 0; // index buffer of the open list (0: none)
        DkIdxFormat boundIdxFormat = DkIdxFormat_Uint32;
        bool blend = false, depthTest = false, scissor = false;
        GLenum blendSrc = 1, blendDst = 0, blendSrcA = 1, blendDstA = 0, eqColor = 0x8006, eqAlpha = 0x8006;
        float blendConst[4] = {};
        bool mask[4] = {true, true, true, true};
        bool depthWrite = true;
        GLenum depthFunc = 0x0201;
        float clearColor[4] = {}, clearDepth = 1.f;
        GLint viewport[4] = {}, scissorBox[4] = {};
        bool stateDirty = true, rtDirty = true, viewportDirty = true, programDirty = true, blendConstDirty = true;
        // what the open command list has bound (GOW-Port: re-bound only when it changes; reset at every submit)
        struct StageBinding
        {
            DkGpuAddr ubo = 0;
            uint32_t uboSize = 0, numTextures = 0;
            DkResHandle textures[8] = {};
        } stageBound[2];
        bool resourcesValid = false;
        struct VertexBinding
        {
            uint32_t numAttribs = 0, numBuffers = 0;
            DkVtxAttribState attribs[8] = {};
            DkVtxBufferState states[8] = {};
            DkGpuAddr gpu[8] = {};
            uint32_t size[8] = {};
        } vtxBound;
        bool vertexValid = false;
        bool imageInvalidate = false, descriptorInvalidate = true;
        GLuint boundColor = 0, boundDepth = 0; // what the bound render targets are
        uint32_t rtWidth = 1, rtHeight = 1;

        // presentation
        DkMemBlock swapMem = nullptr;
        DkImage swapImages[kSwapImages]{};
        DkSwapchain swapchain = nullptr;
        GLuint scaleProgram = 0;
        GLint scaleRect = -1, scaleSrcSize = -1, scaleSharpen = -1, scalePlain = -1;
        std::atomic<uint64_t> presented{0};
        bool flipPresent = true;
        uint32_t samplerLinearClamp = 0;

        // counters ([gs-dk] every 5 s with GOW_PERF_DIAG)
        uint64_t draws = 0, submitsCount = 0, waits = 0, waitNanos = 0, uploads = 0, uploadBytes = 0, presents = 0;
        uint64_t stateBinds = 0, resourceBinds = 0, textureBinds = 0, programBinds = 0, uboPushes = 0, uboPushBytes = 0,
                 vtxBinds = 0, vtxBufferBinds = 0, rtBinds = 0, barrierCount = 0, drawNanos = 0;
        uint64_t acquireNanos = 0, presentSubmitNanos = 0, submitNanos = 0;
        std::chrono::steady_clock::time_point statsAt = std::chrono::steady_clock::now();
    };

    State *s = nullptr;

    void fail(const char *what)
    {
        std::fprintf(stderr, "[gs-dk] %s\n", what);
        std::fflush(stderr);
    }

    void debugCallback(void *, const char *context, DkResult result, const char *message)
    {
        std::fprintf(stderr, "[gs-dk] deko3d %s: result %d %s\n", context ? context : "", static_cast<int>(result),
                     message ? message : "");
        std::fflush(stderr);
    }

    DkMemBlock makeMem(uint32_t size, uint32_t flags)
    {
        DkMemBlockMaker maker;
        dkMemBlockMakerDefaults(&maker, s->device, align(size, DK_MEMBLOCK_ALIGNMENT));
        maker.flags = flags;
        return dkMemBlockCreate(&maker);
    }

    // ---- submission and completion -------------------------------------------------------------------------------
    void retire()
    {
        while (!s->submits.empty() && dkFenceWait(&s->submits.front().fence, 0) == DkResult_Success)
        {
            s->doneSerial = s->submits.front().serial;
            s->submits.pop_front();
        }
        while (!s->garbage.empty() && s->garbage.front().serial <= s->doneSerial)
        {
            dkMemBlockDestroy(s->garbage.front().mem);
            s->garbage.pop_front();
        }
    }

    void waitSerial(uint64_t serial)
    {
        if (serial <= s->doneSerial)
            return;
        const auto start = std::chrono::steady_clock::now();
        while (!s->submits.empty() && s->submits.front().serial <= serial)
        {
            dkFenceWait(&s->submits.front().fence, -1);
            s->doneSerial = s->submits.front().serial;
            s->submits.pop_front();
        }
        ++s->waits;
        s->waitNanos += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        retire();
    }

    void bindSets();

    // ends the open command list, submits it (signalling its fence) and opens the next
    uint64_t submit()
    {
        const uint64_t serial = s->nextSerial++;
        s->submits.emplace_back();
        Submit &sub = s->submits.back();
        sub.serial = serial;
        dkCmdBufSignalFence(s->cmd, &sub.fence, true);
        const auto start = std::chrono::steady_clock::now();
        const DkCmdList list = dkCmdBufFinishList(s->cmd);
        dkQueueSubmitCommands(s->queue, list);
        dkQueueFlush(s->queue);
        s->submitNanos += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        // the chunks this list used are free once its fence signals
        for (uint32_t i = 0; i < s->chunksThisList; ++i)
            s->chunkSerial[(s->chunk + kCmdChunks - i) % kCmdChunks] = serial;
        s->chunksThisList = 1;
        s->chunkSerial[s->chunk] = serial; // the current chunk continues in the next list
        s->bytesThisList = 0;
        ++s->submitsCount;
        s->setsBound = false; // re-bound at the start of every list
        s->stateDirty = s->rtDirty = s->viewportDirty = s->programDirty = s->blendConstDirty = true;
        s->resourcesValid = s->vertexValid = false;
        s->boundIdx = 0;
        retire();
        return serial;
    }

    void addMemory(void *, DkCmdBuf cmd, size_t)
    {
        // the next chunk of the ring, once the list that used it last has finished
        s->chunk = (s->chunk + 1u) % kCmdChunks;
        waitSerial(s->chunkSerial[s->chunk]);
        s->chunkSerial[s->chunk] = UINT64_MAX; // in the open list
        ++s->chunksThisList;
        dkCmdBufAddMemory(cmd, s->cmdMem, s->chunk * kCmdChunk, kCmdChunk);
    }

    // a long open list holds command chunks and staging; draw calls submit it from time to time
    void maybeSubmit()
    {
        if (s->chunksThisList >= kCmdChunks / 4u)
            submit();
    }

    // ---- staging ring (uploads) ------------------------------------------------------------------------------------
    DkGpuAddr stage(const void *data, uint32_t bytes, uint32_t alignment = 256u)
    {
        if (bytes > s->stageSize / 2u)
        {
            fail("upload larger than the staging ring");
            return 0;
        }
        uint32_t at = align(s->stageHead, alignment);
        if (at + bytes > s->stageSize)
        {
            // wrap: everything staged in the previous lap must be consumed (submitted and finished), so nothing within
            // one lap overlaps older data
            waitSerial(submit());
            at = 0;
        }
        std::memcpy(s->stageCpu + at, data, bytes);
        s->stageHead = at + bytes;
        return dkMemBlockGetGpuAddr(s->stageMem) + at;
    }

    // ---- shaders ------------------------------------------------------------------------------------------------------
    uint64_t fnv64(const std::string &text)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned char c : text)
            h = (h ^ c) * 0x100000001b3ull;
        return h;
    }

    struct DkshHeader
    {
        uint32_t magic, headerSize, controlSize, codeSize, programsOffset, numPrograms;
    };

    bool loadCode(const GSDK::ShaderBinary *binary, DkShader &shader)
    {
        DkshHeader header;
        std::memcpy(&header, binary->code, sizeof(header));
        uint32_t offset;
        if (const auto it = s->codeOffset.find(binary); it != s->codeOffset.end())
            offset = it->second;
        else
        {
            offset = align(s->codeUsed, DK_SHADER_CODE_ALIGNMENT);
            if (offset + header.codeSize > s->codeSize - DK_SHADER_CODE_UNUSABLE_SIZE)
            {
                fail("shader code memory full");
                return false;
            }
            std::memcpy(static_cast<uint8_t *>(dkMemBlockGetCpuAddr(s->codeMem)) + offset,
                        binary->code + header.controlSize, header.codeSize);
            s->codeUsed = offset + header.codeSize;
            s->codeOffset[binary] = offset;
            dkMemBlockFlushCpuCache(s->codeMem, offset, header.codeSize);
        }
        DkShaderMaker maker;
        dkShaderMakerDefaults(&maker, s->codeMem, offset);
        maker.control = binary->code;
        dkShaderInitialize(&shader, &maker);
        return dkShaderIsValid(&shader);
    }

    const GSDK::ShaderBinary *findBinary(uint64_t hash)
    {
        const auto it = s->binaries.find(hash);
        return it == s->binaries.end() ? nullptr : it->second;
    }

    // ---- textures -----------------------------------------------------------------------------------------------------
    struct Format
    {
        DkImageFormat dk;
        uint32_t bpp;
        bool depth, integer;
    };
    bool formatOf(GLint internal, Format &f)
    {
        switch (internal)
        {
        case 0x8058: f = {DkImageFormat_RGBA8_Unorm, 4, false, false}; return true;     // RGBA8
        case 0x8229: f = {DkImageFormat_R8_Unorm, 1, false, false}; return true;        // R8
        case 0x8234: f = {DkImageFormat_R16_Uint, 2, false, true}; return true;         // R16UI
        case 0x823A: f = {DkImageFormat_RG16_Uint, 4, false, true}; return true;        // RG16UI
        case 0x8236: f = {DkImageFormat_R32_Uint, 4, false, true}; return true;         // R32UI
        case 0x8CAC: f = {DkImageFormat_ZF32, 4, true, false}; return true;             // DEPTH_COMPONENT32F
        default: return false;
        }
    }

    Texture *boundTexture()
    {
        const GLuint id = s->units[s->activeUnit];
        return id && id < s->textures.size() ? &s->textures[id] : nullptr;
    }

    void writeDescriptor(Texture &t)
    {
        t.slot = s->nextSlot;
        s->nextSlot = s->nextSlot + 1u >= kImageSlots ? 1u : s->nextSlot + 1u;
        dkImageDescriptorInitialize(&s->imageDescs[t.slot], &t.view, false, false);
        s->descriptorInvalidate = true;
    }

    void releaseImage(Texture &t)
    {
        if (t.mem)
            s->garbage.push_back({s->nextSerial, t.mem}); // freed after the open list (which may use it) finishes
        t.mem = nullptr;
        t.valid = false;
    }

    void uploadRect(Texture &t, int x, int y, uint32_t w, uint32_t h, const void *pixels, const Format &f)
    {
        if (!pixels || !w || !h)
            return;
        const uint32_t bytes = w * h * f.bpp;
        const DkGpuAddr src = stage(pixels, bytes);
        if (!src)
            return;
        DkCopyBuf copy{src, 0u, 0u};
        DkImageRect rect{static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0u, w, h, 1u};
        dkCmdBufCopyBufferToImage(s->cmd, &copy, &t.view, &rect, 0u);
        s->imageInvalidate = true;
        ++s->uploads;
        s->uploadBytes += bytes;
    }

    void allocImage(Texture &t, uint32_t w, uint32_t h, const Format &f)
    {
        releaseImage(t);
        DkImageLayoutMaker maker;
        dkImageLayoutMakerDefaults(&maker, s->device);
        maker.type = DkImageType_2D;
        maker.format = f.dk;
        maker.flags = DkImageFlags_Usage2DEngine | (f.integer ? 0u : DkImageFlags_UsageRender);
        if (f.depth)
            maker.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
        maker.dimensions[0] = std::max(w, 1u);
        maker.dimensions[1] = std::max(h, 1u);
        DkImageLayout layout;
        dkImageLayoutInitialize(&layout, &maker);
        t.mem = makeMem(static_cast<uint32_t>(dkImageLayoutGetSize(&layout)), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
        if (!t.mem)
        {
            fail("image memory allocation failed");
            return;
        }
        dkImageInitialize(&t.image, &layout, t.mem, 0);
        dkImageViewDefaults(&t.view, &t.image);
        t.format = f.dk;
        t.width = std::max(w, 1u);
        t.height = std::max(h, 1u);
        t.bpp = f.bpp;
        t.depth = f.depth;
        t.integer = f.integer;
        t.valid = true;
        writeDescriptor(t);
        if (s->boundColor && &s->textures[s->boundColor] == &t)
            s->rtDirty = true;
        if (s->boundDepth && &s->textures[s->boundDepth] == &t)
            s->rtDirty = true;
    }

    uint32_t samplerIndex(const Texture &t)
    {
        // 16 samplers: min, mag (nearest / linear) x wrap S, T (repeat / clamp)
        const uint32_t i = (t.minFilter == 0x2601 || t.minFilter == 0x2703 || t.minFilter == 0x2701 ? 1u : 0u) |
                           (t.magFilter == 0x2601 ? 2u : 0u) | (t.wrapS == 0x2901 ? 0u : 4u) | (t.wrapT == 0x2901 ? 0u : 8u);
        return i;
    }

    // ---- state flush before a draw / clear / dispatch -----------------------------------------------------------------
    void bindSets()
    {
        if (s->setsBound)
            return;
        dkCmdBufBindImageDescriptorSet(s->cmd, s->imageDescAddr, kImageSlots);
        dkCmdBufBindSamplerDescriptorSet(s->cmd, s->samplerDescAddr, kSamplerSlots);
        s->setsBound = true;
    }

    void barriers()
    {
        if (s->descriptorInvalidate || s->imageInvalidate)
        {
            dkCmdBufBarrier(s->cmd, s->imageInvalidate ? DkBarrier_Primitives : DkBarrier_None,
                            (s->imageInvalidate ? DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache : 0u) |
                                (s->descriptorInvalidate ? DkInvalidateFlags_Descriptors : 0u));
            s->descriptorInvalidate = s->imageInvalidate = false;
        }
    }

    bool bindTargets()
    {
        if (!s->rtDirty)
            return true;
        const Fbo &fb = s->fbos[s->fbo < s->fbos.size() ? s->fbo : 0];
        Texture *color = fb.color ? &s->textures[fb.color] : nullptr;
        Texture *depth = fb.depth ? &s->textures[fb.depth] : nullptr;
        if (!color || !color->valid)
            return false;
        // render target changes order the previous target's fragments before anything samples it
        dkCmdBufBarrier(s->cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
        const DkImageView *views[] = {&color->view};
        dkCmdBufBindRenderTargets(s->cmd, views, 1, depth && depth->valid ? &depth->view : nullptr);
        ++s->rtBinds;
        s->boundColor = fb.color;
        s->boundDepth = depth && depth->valid ? fb.depth : 0u;
        s->rtWidth = color->width;
        s->rtHeight = color->height;
        s->rtDirty = false;
        s->viewportDirty = true;
        return true;
    }

    DkBlendFactor blendFactor(GLenum f)
    {
        switch (f)
        {
        case 0: return DkBlendFactor_Zero;
        case 1: return DkBlendFactor_One;
        case 0x0300: return DkBlendFactor_SrcColor;
        case 0x0301: return DkBlendFactor_InvSrcColor;
        case 0x0302: return DkBlendFactor_SrcAlpha;
        case 0x0303: return DkBlendFactor_InvSrcAlpha;
        case 0x0304: return DkBlendFactor_DstAlpha;
        case 0x0305: return DkBlendFactor_InvDstAlpha;
        case 0x0306: return DkBlendFactor_DstColor;
        case 0x0307: return DkBlendFactor_InvDstColor;
        case 0x8001: return DkBlendFactor_ConstColor;
        case 0x8002: return DkBlendFactor_InvConstColor;
        case 0x8003: return DkBlendFactor_ConstAlpha;
        case 0x8004: return DkBlendFactor_InvConstAlpha;
        case 0x8589: return DkBlendFactor_Src1Alpha;
        case 0x88FB: return DkBlendFactor_InvSrc1Alpha;
        case 0x88F9: return DkBlendFactor_Src1Color;
        case 0x88FA: return DkBlendFactor_InvSrc1Color;
        default: return DkBlendFactor_One;
        }
    }

    DkBlendOp blendOp(GLenum e)
    {
        switch (e)
        {
        case 0x800A: return DkBlendOp_Sub;
        case 0x800B: return DkBlendOp_RevSub;
        case 0x8007: return DkBlendOp_Min;
        case 0x8008: return DkBlendOp_Max;
        default: return DkBlendOp_Add;
        }
    }

    DkCompareOp compareOp(GLenum f)
    {
        return static_cast<DkCompareOp>(std::clamp<int>(static_cast<int>(f) - 0x0200 + 1, 1, 8));
    }

    void flushViewport()
    {
        if (!s->viewportDirty)
            return;
        DkViewport vp{static_cast<float>(s->viewport[0]), static_cast<float>(s->viewport[1]),
                      static_cast<float>(s->viewport[2]), static_cast<float>(s->viewport[3]), 0.f, 1.f};
        dkCmdBufSetViewports(s->cmd, 0, &vp, 1);
        DkScissor sc;
        if (s->scissor)
        {
            const int x0 = std::max(0, s->scissorBox[0]), y0 = std::max(0, s->scissorBox[1]);
            const int x1 = std::max(x0, s->scissorBox[0] + s->scissorBox[2]), y1 = std::max(y0, s->scissorBox[1] + s->scissorBox[3]);
            sc = {static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
        }
        else
            sc = {0u, 0u, 16384u, 16384u};
        dkCmdBufSetScissors(s->cmd, 0, &sc, 1);
        s->viewportDirty = false;
    }

    void flushFixedState()
    {
        if (s->blendConstDirty)
        {
            dkCmdBufSetBlendConst(s->cmd, s->blendConst[0], s->blendConst[1], s->blendConst[2], s->blendConst[3]);
            s->blendConstDirty = false;
            ++s->stateBinds;
        }
        if (!s->stateDirty)
            return;
        ++s->stateBinds;
        DkRasterizerState raster;
        dkRasterizerStateDefaults(&raster);
        raster.cullMode = DkFace_None;
        dkCmdBufBindRasterizerState(s->cmd, &raster);
        DkColorState color;
        dkColorStateDefaults(&color);
        dkColorStateSetBlendEnable(&color, 0, s->blend);
        dkCmdBufBindColorState(s->cmd, &color);
        DkColorWriteState write;
        dkColorWriteStateDefaults(&write);
        dkColorWriteStateSetMask(&write, 0, (s->mask[0] ? DkColorMask_R : 0u) | (s->mask[1] ? DkColorMask_G : 0u) |
                                                (s->mask[2] ? DkColorMask_B : 0u) | (s->mask[3] ? DkColorMask_A : 0u));
        dkCmdBufBindColorWriteState(s->cmd, &write);
        DkBlendState blend;
        dkBlendStateDefaults(&blend);
        dkBlendStateSetOps(&blend, blendOp(s->eqColor), blendOp(s->eqAlpha));
        dkBlendStateSetFactors(&blend, blendFactor(s->blendSrc), blendFactor(s->blendDst), blendFactor(s->blendSrcA),
                               blendFactor(s->blendDstA));
        dkCmdBufBindBlendState(s->cmd, 0, &blend);
        DkDepthStencilState depth;
        dkDepthStencilStateDefaults(&depth);
        depth.depthTestEnable = s->depthTest;
        depth.depthWriteEnable = s->depthTest && s->depthWrite; // GL: no depth write without the test
        depth.depthCompareOp = compareOp(s->depthFunc);
        dkCmdBufBindDepthStencilState(s->cmd, &depth);
        s->stateDirty = false;
    }

    void bindProgramResources(Program &p, DkStage stage)
    {
        DkResHandle handles[8];
        for (uint32_t b = 0; b < p.numSamplers; ++b)
        {
            const GLuint id = s->units[p.samplerUnit[b] & 15u];
            const Texture *t = id && id < s->textures.size() && s->textures[id].valid ? &s->textures[id] : &s->textures[0];
            handles[b] = dkMakeTextureHandle(t->slot, samplerIndex(*t));
        }
        if (stage == DkStage_Compute)
        {
            dkCmdBufBindUniformBuffer(s->cmd, stage, 0, p.uboAddr, p.uboSize);
            if (p.numSamplers)
                dkCmdBufBindTextures(s->cmd, stage, 0, handles, p.numSamplers);
            return;
        }
        State::StageBinding &bound = s->stageBound[stage == DkStage_Vertex ? 0 : 1];
        if (!s->resourcesValid || bound.ubo != p.uboAddr || bound.uboSize != p.uboSize)
        {
            dkCmdBufBindUniformBuffer(s->cmd, stage, 0, p.uboAddr, p.uboSize);
            bound.ubo = p.uboAddr;
            bound.uboSize = p.uboSize;
            ++s->resourceBinds;
        }
        if (p.numSamplers && (!s->resourcesValid || bound.numTextures != p.numSamplers ||
                              std::memcmp(bound.textures, handles, p.numSamplers * sizeof(DkResHandle)) != 0))
        {
            dkCmdBufBindTextures(s->cmd, stage, 0, handles, p.numSamplers);
            bound.numTextures = p.numSamplers;
            std::memcpy(bound.textures, handles, p.numSamplers * sizeof(DkResHandle));
            ++s->textureBinds;
        }
    }

    bool flushProgram(bool compute)
    {
        if (!s->program || s->program >= s->programs.size())
            return false;
        Program &p = s->programs[s->program];
        if (!p.linked || p.compute != compute)
            return false;
        if (s->programDirty)
        {
            const DkShader *list[2] = {&p.shaders[0], &p.shaders[1]};
            dkCmdBufBindShaders(s->cmd, p.stageMask, list, p.numShaders);
            s->programDirty = false;
            ++s->programBinds;
        }
        // GOW-Port: only the changed bytes are pushed (an in-order update of the program's own uniform memory, which
        // keeps its contents across program switches and submits), not the whole block after every change or bind
        if (p.uboDirty)
        {
            const uint32_t lo = p.dirtyLo & ~3u, hi = std::min(p.uboSize, (p.dirtyHi + 3u) & ~3u);
            if (hi > lo)
                dkCmdBufPushConstants(s->cmd, p.uboAddr, p.uboSize, lo, hi - lo, p.ubo.data() + lo);
            p.uboDirty = false;
            ++s->uboPushes;
            s->uboPushBytes += hi > lo ? hi - lo : 0u;
        }
        if (compute)
            bindProgramResources(p, DkStage_Compute);
        else
        {
            bindProgramResources(p, DkStage_Vertex);
            bindProgramResources(p, DkStage_Fragment);
            s->resourcesValid = true;
        }
        return true;
    }

    bool flushVertices()
    {
        if (!s->vao || s->vao >= s->vaos.size())
            return false;
        Vao &v = s->vaos[s->vao];
        DkVtxAttribState attribs[8];
        DkVtxBufferState states[8];
        uint32_t n = 0, nb = 0;
        GLuint bufferOf[8] = {};
        uint32_t strideOf[8] = {};
        for (uint32_t i = 0; i < 8; ++i)
        {
            const Attrib &a = v.attribs[i];
            if (!a.enabled)
                break;
            uint32_t b = 0;
            while (b < nb && !(bufferOf[b] == a.buffer && strideOf[b] == a.stride))
                ++b;
            if (b == nb)
            {
                bufferOf[nb] = a.buffer;
                strideOf[nb] = a.stride;
                states[nb] = {a.stride, 0u};
                ++nb;
            }
            attribs[n] = DkVtxAttribState{};
            attribs[n].bufferId = b;
            attribs[n].isFixed = 0;
            attribs[n].offset = a.offset;
            attribs[n].size = a.size;
            attribs[n].type = a.type;
            ++n;
        }
        if (!n)
            return true;
        State::VertexBinding &bound = s->vtxBound;
        const bool layoutSame = s->vertexValid && bound.numAttribs == n && bound.numBuffers == nb &&
                                std::memcmp(bound.attribs, attribs, n * sizeof(DkVtxAttribState)) == 0 &&
                                std::memcmp(bound.states, states, nb * sizeof(DkVtxBufferState)) == 0;
        if (!layoutSame)
        {
            dkCmdBufBindVtxAttribState(s->cmd, attribs, n);
            dkCmdBufBindVtxBufferState(s->cmd, states, nb);
            bound.numAttribs = n;
            bound.numBuffers = nb;
            std::memcpy(bound.attribs, attribs, n * sizeof(DkVtxAttribState));
            std::memcpy(bound.states, states, nb * sizeof(DkVtxBufferState));
            ++s->vtxBinds;
        }
        for (uint32_t b = 0; b < nb; ++b)
        {
            const Buffer &buf = s->buffers[bufferOf[b] < s->buffers.size() ? bufferOf[b] : 0];
            if (!buf.mem)
                return s->vertexValid = false, false;
            if (!layoutSame || bound.gpu[b] != buf.gpu || bound.size[b] != buf.size)
            {
                dkCmdBufBindVtxBuffer(s->cmd, b, buf.gpu, buf.size);
                bound.gpu[b] = buf.gpu;
                bound.size[b] = buf.size;
                ++s->vtxBufferBinds;
            }
        }
        s->vertexValid = true;
        return true;
    }

    void stats()
    {
        static const bool on = [] { const char *v = std::getenv("GOW_PERF_DIAG"); return v && *v && *v != '0'; }();
        if (!on)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now - s->statsAt < std::chrono::seconds(5))
            return;
        std::fprintf(stderr, "[gs-dk] draws=%llu submits=%llu presents=%llu waits=%llu wait_ms=%.1f uploads=%llu upload_kb=%llu textures=%zu\n",
                     (unsigned long long)s->draws, (unsigned long long)s->submitsCount, (unsigned long long)s->presents,
                     (unsigned long long)s->waits, s->waitNanos * 1e-6, (unsigned long long)s->uploads,
                     (unsigned long long)(s->uploadBytes >> 10), s->textures.size() - s->freeTextures.size());
        std::fprintf(stderr, "[gs-dk-binds] draw_ms=%.1f state=%llu rt=%llu program=%llu ubo_push=%llu ubo_kb=%llu ubo_bind=%llu tex_bind=%llu vtx_state=%llu vtx_buf=%llu barriers=%llu\n",
                     s->drawNanos * 1e-6, (unsigned long long)s->stateBinds, (unsigned long long)s->rtBinds,
                     (unsigned long long)s->programBinds, (unsigned long long)s->uboPushes,
                     (unsigned long long)(s->uboPushBytes >> 10), (unsigned long long)s->resourceBinds,
                     (unsigned long long)s->textureBinds, (unsigned long long)s->vtxBinds,
                     (unsigned long long)s->vtxBufferBinds, (unsigned long long)s->barrierCount);
        s->draws = s->submitsCount = s->presents = s->waits = s->waitNanos = s->uploads = s->uploadBytes = 0;
        s->stateBinds = s->resourceBinds = s->textureBinds = s->programBinds = s->uboPushes = s->uboPushBytes = 0;
        std::fprintf(stderr, "[gs-dk-time] acquire_ms=%.1f present_submit_ms=%.1f submit_ms=%.1f\n", s->acquireNanos * 1e-6,
                     s->presentSubmitNanos * 1e-6, s->submitNanos * 1e-6);
        s->vtxBinds = s->vtxBufferBinds = s->rtBinds = s->barrierCount = s->drawNanos = 0;
        s->acquireNanos = s->presentSubmitNanos = s->submitNanos = 0;
        s->statsAt = now;
    }

    // ---- synchronous readback of an image rectangle into CPU memory (or a pack buffer) --------------------------------
    void readRect(Texture &t, int x, int y, int w, int h, void *out, uint32_t outBpp, GLenum format, GLenum type)
    {
        if (!t.valid || w <= 0 || h <= 0)
            return;
        x = std::clamp(x, 0, static_cast<int>(t.width));
        y = std::clamp(y, 0, static_cast<int>(t.height));
        w = std::min(w, static_cast<int>(t.width) - x);
        h = std::min(h, static_cast<int>(t.height) - y);
        if (w <= 0 || h <= 0)
            return;
        const uint32_t bytes = static_cast<uint32_t>(w * h) * t.bpp;
        DkImageRect rect{static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0u, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
        bindTargets();
        dkCmdBufBarrier(s->cmd, DkBarrier_Fragments, 0);
        if (s->packBuffer && s->packBuffer < s->buffers.size() && s->buffers[s->packBuffer].mem)
        {
            // into a pixel pack buffer: no wait (the caller fences); out is the offset in it
            const Buffer &b = s->buffers[s->packBuffer];
            DkCopyBuf dst{b.gpu + reinterpret_cast<uintptr_t>(out), 0u, 0u};
            dkCmdBufCopyImageToBuffer(s->cmd, &t.view, &rect, &dst, 0u);
            return;
        }
        if (bytes > s->readSize)
        {
            if (s->readMem)
                s->garbage.push_back({s->nextSerial, s->readMem});
            s->readSize = align(bytes, DK_MEMBLOCK_ALIGNMENT);
            s->readMem = makeMem(s->readSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        }
        DkCopyBuf dst{dkMemBlockGetGpuAddr(s->readMem), 0u, 0u};
        dkCmdBufCopyImageToBuffer(s->cmd, &t.view, &rect, &dst, 0u);
        waitSerial(submit());
        const uint8_t *src = static_cast<const uint8_t *>(dkMemBlockGetCpuAddr(s->readMem));
        if (outBpp == t.bpp)
            std::memcpy(out, src, bytes);
        else
        {
            // RGBA8 out of a 1/2 byte format etc.: only what the backend asks for (RGBA8 / depth float)
            for (int i = 0; i < w * h; ++i)
                std::memcpy(static_cast<uint8_t *>(out) + static_cast<size_t>(i) * outBpp, src + static_cast<size_t>(i) * t.bpp,
                            std::min(outBpp, t.bpp));
        }
        (void)format;
        (void)type;
    }

    // ======== the GL entry points ======================================================================================
    template <typename List>
    GLuint allocate(List &list)
    {
        list.emplace_back();
        return static_cast<GLuint>(list.size() - 1u);
    }

    void dkGenBuffers(GLsizei n, GLuint *ids)
    {
        for (GLsizei i = 0; i < n; ++i)
            ids[i] = allocate(s->buffers);
    }
    void dkDeleteBuffers(GLsizei n, const GLuint *ids)
    {
        for (GLsizei i = 0; i < n; ++i)
            if (ids[i] && ids[i] < s->buffers.size() && s->buffers[ids[i]].mem)
            {
                s->garbage.push_back({s->nextSerial, s->buffers[ids[i]].mem});
                s->buffers[ids[i]] = Buffer{};
            }
    }
    GLuint &bufferBinding(GLenum target)
    {
        switch (target)
        {
        case kPixelPack: return s->packBuffer;
        case kPixelUnpack: return s->unpackBuffer;
        case kCopyRead: return s->copyRead;
        case kCopyWrite: return s->copyWrite;
        case kSsbo: return s->ssboGeneric;
        case kElementArray: return s->vao && s->vao < s->vaos.size() ? s->vaos[s->vao].element : s->noVaoElement;
        default: return s->arrayBuffer;
        }
    }
    void dkBindBuffer(GLenum target, GLuint id) { bufferBinding(target) = id; }
    Buffer *bound(GLenum target)
    {
        const GLuint id = bufferBinding(target);
        return id && id < s->buffers.size() ? &s->buffers[id] : nullptr;
    }
    // new storage (GL orphaning): the old one is freed once the GPU is done with it
    void storage(Buffer &b, GLsizeiptr size, const void *data)
    {
        if (b.mem)
            s->garbage.push_back({s->nextSerial, b.mem});
        b.size = align(static_cast<uint32_t>(std::max<GLsizeiptr>(size, 1)), 256u);
        b.mem = makeMem(b.size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        b.cpu = static_cast<uint8_t *>(dkMemBlockGetCpuAddr(b.mem));
        b.gpu = dkMemBlockGetGpuAddr(b.mem);
        if (data)
            std::memcpy(b.cpu, data, static_cast<size_t>(size));
    }
    void dkBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum)
    {
        if (Buffer *b = bound(target))
            storage(*b, size, data);
    }
    void dkBufferStorage(GLenum target, GLsizeiptr size, const void *data, GLbitfield) { dkBufferData(target, size, data, 0); }
    void dkBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
    {
        // the backend appends into unused ranges and orphans before reusing one (streamVertices): a plain write
        if (Buffer *b = bound(target); b && b->mem && offset + size <= static_cast<GLintptr>(b->size))
            std::memcpy(b->cpu + offset, data, static_cast<size_t>(size));
    }
    void dkGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void *data)
    {
        if (Buffer *b = bound(target); b && b->mem)
        {
            waitSerial(submit());
            std::memcpy(data, b->cpu + offset, static_cast<size_t>(size));
        }
    }
    void dkBindBufferBase(GLenum target, GLuint index, GLuint id)
    {
        if (target == kSsbo && index < 4)
            s->ssbo[index] = id;
        bufferBinding(target) = id;
    }
    void dkCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size)
    {
        Buffer *r = bound(readTarget), *w = bound(writeTarget);
        if (r && w && r->mem && w->mem)
            dkCmdBufCopyBuffer(s->cmd, r->gpu + readOffset, w->gpu + writeOffset, static_cast<uint32_t>(size));
    }
    void *dkMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr, GLbitfield access)
    {
        Buffer *b = bound(target);
        if (!b || !b->mem)
            return nullptr;
        if (access & 0x0001) // read: the GPU's writes must be done
            waitSerial(submit());
        return b->cpu + offset;
    }
    GLboolean dkUnmapBuffer(GLenum) { return 1; }

    void dkGenQueries(GLsizei n, GLuint *ids)
    {
        for (GLsizei i = 0; i < n; ++i)
            ids[i] = 1u;
    }
    void dkDeleteQueries(GLsizei, const GLuint *) {}
    void dkBeginQuery(GLenum, GLuint) {}
    void dkEndQuery(GLenum) {}
    void dkGetQueryObjectui64v(GLuint, GLenum pname, GLuint64 *v) { *v = pname == kQueryResultAvailable ? 1u : 0u; }
    void dkQueryCounter(GLuint, GLenum) {}

    GLuint dkCreateShader(GLenum type)
    {
        const GLuint id = allocate(s->shaders);
        s->shaders[id].type = type;
        return id;
    }
    void dkShaderSource(GLuint id, GLsizei count, const GLchar *const *parts, const GLint *lengths)
    {
        std::string &text = s->shaders[id].source;
        text.clear();
        for (GLsizei i = 0; i < count; ++i)
            text.append(parts[i], lengths && lengths[i] >= 0 ? static_cast<size_t>(lengths[i]) : std::strlen(parts[i]));
    }
    void dkCompileShader(GLuint id)
    {
        Shader &sh = s->shaders[id];
        sh.binary = findBinary(fnv64(sh.source));
        if (!sh.binary)
        {
            // a draw variant this build has no DKSH for: its key (gs_hw_backend.cpp drawVariantKey) rebuilt from the
            // defines drawVariantFor wrote, for switch/dk_variant_keys.txt (tools/dk_shaders.py)
            const char *text = sh.source.c_str();
            const auto define = [text](const char *name) -> int
            {
                const std::string needle = std::string("#define ") + name + " ";
                const char *at = std::strstr(text, needle.c_str());
                return at ? std::atoi(at + needle.size()) : 0;
            };
            if (std::strstr(text, "#define GS_SPECIALIZED"))
            {
                const int ztex = define("uZTex");
                const uint32_t key = static_cast<uint32_t>(define("uTme")) | static_cast<uint32_t>(define("uReinterp")) << 1 |
                                     static_cast<uint32_t>(define("uIndexed")) << 2 |
                                     static_cast<uint32_t>(ztex == 0 ? 0 : ztex == 16 ? 1 : 2) << 3 |
                                     static_cast<uint32_t>(define("uLinear")) << 5 | static_cast<uint32_t>(define("uRtTex24")) << 6 |
                                     static_cast<uint32_t>(define("uTfx")) << 8 | static_cast<uint32_t>(define("uTcc")) << 10 |
                                     static_cast<uint32_t>(define("uFst")) << 11 | static_cast<uint32_t>(define("uIip")) << 12 |
                                     static_cast<uint32_t>(define("uPass")) << 13 | static_cast<uint32_t>(define("uSwBlend")) << 15 |
                                     static_cast<uint32_t>(define("uZAlias")) << 16 | static_cast<uint32_t>(define("uZAliasWrite")) << 19 |
                                     static_cast<uint32_t>(define("uDate")) << 20 |
                                     (std::strstr(text, "#define GS_DUAL_SOURCE") ? 1u << 21 : 0u);
                std::fprintf(stderr, "[gs-dk] no deko3d build of draw variant key=%06x (uber used)\n", key);
            }
            else
                std::fprintf(stderr, "[gs-dk] no deko3d build of a shader (hash %016llx)\n", static_cast<unsigned long long>(fnv64(sh.source)));
        }
    }
    void dkGetShaderiv(GLuint id, GLenum pname, GLint *v)
    {
        *v = pname == kCompileStatus ? (s->shaders[id].binary ? 1 : 0) : 0;
    }
    void dkGetShaderInfoLog(GLuint, GLsizei size, GLsizei *length, GLchar *log)
    {
        const char *msg = "no deko3d build of this shader (tools/dk_shaders.py)";
        const GLsizei n = std::min<GLsizei>(size - 1, static_cast<GLsizei>(std::strlen(msg)));
        std::memcpy(log, msg, static_cast<size_t>(std::max(n, 0)));
        if (size > 0)
            log[std::max(n, 0)] = 0;
        if (length)
            *length = n;
    }
    void dkDeleteShader(GLuint) {}
    GLuint dkCreateProgram() { return allocate(s->programs); }
    void dkAttachShader(GLuint program, GLuint shader)
    {
        if (s->shaders[shader].binary)
            s->programs[program].stages.push_back(s->shaders[shader].binary);
        else
            s->programs[program].stages.push_back(nullptr);
    }
    void dkLinkProgram(GLuint id)
    {
        Program &p = s->programs[id];
        p.linked = false;
        if (p.stages.empty() || std::find(p.stages.begin(), p.stages.end(), nullptr) != p.stages.end())
            return;
        p.numShaders = 0;
        p.stageMask = 0;
        for (const GSDK::ShaderBinary *b : p.stages)
        {
            if (p.numShaders == 2 || !loadCode(b, p.shaders[p.numShaders]))
                return;
            p.stageMask |= b->stage == 0 ? DkStageFlag_Vertex : b->stage == 1 ? DkStageFlag_Fragment : DkStageFlag_Compute;
            p.compute = b->stage == 2;
            ++p.numShaders;
        }
        p.family = &GSDK::kFamilies[p.stages.back()->family];
        p.uboSize = p.family->uboSize;
        p.ubo.assign(p.uboSize, 0u);
        const uint32_t at = align(s->uboUsed, 256u);
        if (at + p.uboSize > s->uboSize)
        {
            fail("uniform memory full");
            return;
        }
        p.uboAddr = dkMemBlockGetGpuAddr(s->uboMem) + at;
        s->uboUsed = at + p.uboSize;
        p.numSamplers = 0;
        while (p.family->samplers[p.numSamplers])
            ++p.numSamplers;
        p.uboDirty = true;
        p.dirtyLo = 0;
        p.dirtyHi = p.uboSize;
        p.linked = true;
    }
    void dkGetProgramiv(GLuint id, GLenum pname, GLint *v) { *v = pname == kLinkStatus ? (s->programs[id].linked ? 1 : 0) : 0; }
    void dkGetProgramInfoLog(GLuint id, GLsizei size, GLsizei *length, GLchar *log) { dkGetShaderInfoLog(id, size, length, log); }
    void dkDeleteProgram(GLuint) {}
    void dkUseProgram(GLuint id)
    {
        if (id != s->program)
        {
            s->program = id;
            s->programDirty = true;
        }
    }
    // locations: the index of the GSU member, or 0x10000 + sampler binding
    GLint dkGetUniformLocation(GLuint id, const GLchar *name)
    {
        const Program &p = s->programs[id];
        if (!p.family)
            return -1;
        for (uint32_t i = 0; p.family->uniforms[i].name; ++i)
            if (std::strcmp(p.family->uniforms[i].name, name) == 0)
                return static_cast<GLint>(i);
        for (uint32_t b = 0; p.family->samplers[b]; ++b)
            if (std::strcmp(p.family->samplers[b], name) == 0)
                return static_cast<GLint>(0x10000 + b);
        return -1;
    }
    Program *current() { return s->program && s->program < s->programs.size() ? &s->programs[s->program] : nullptr; }
    void setUniform(GLint loc, const void *data, uint32_t bytes)
    {
        Program *p = current();
        if (!p || !p->family || loc < 0)
            return;
        if (loc >= 0x10000)
        {
            uint32_t unit;
            std::memcpy(&unit, data, 4);
            if (loc - 0x10000 < 8)
                p->samplerUnit[loc - 0x10000] = unit;
            return;
        }
        const GSDK::UniformInfo &u = p->family->uniforms[loc];
        if (u.offset + bytes > p->uboSize || std::memcmp(p->ubo.data() + u.offset, data, bytes) == 0)
            return;
        std::memcpy(p->ubo.data() + u.offset, data, bytes);
        if (!p->uboDirty)
            p->dirtyLo = u.offset, p->dirtyHi = u.offset + bytes;
        else
            p->dirtyLo = std::min(p->dirtyLo, u.offset), p->dirtyHi = std::max(p->dirtyHi, u.offset + bytes);
        p->uboDirty = true;
    }
    void dkUniform1ui(GLint loc, GLuint v) { setUniform(loc, &v, 4); }
    void dkUniform4ui(GLint loc, GLuint a, GLuint b, GLuint c, GLuint d)
    {
        const GLuint v[4] = {a, b, c, d};
        setUniform(loc, v, 16);
    }
    void dkUniform1uiv(GLint loc, GLsizei n, const GLuint *v) { setUniform(loc, v, static_cast<uint32_t>(n) * 4u); }
    void dkUniform1i(GLint loc, GLint v) { setUniform(loc, &v, 4); }
    void dkUniform1f(GLint loc, float v) { setUniform(loc, &v, 4); }
    void dkUniform2f(GLint loc, float a, float b)
    {
        const float v[2] = {a, b};
        setUniform(loc, v, 8);
    }
    void dkUniform4f(GLint loc, float a, float b, float c, float d)
    {
        const float v[4] = {a, b, c, d};
        setUniform(loc, v, 16);
    }
    void dkUniform4i(GLint loc, GLint a, GLint b, GLint c, GLint d)
    {
        const GLint v[4] = {a, b, c, d};
        setUniform(loc, v, 16);
    }

    void dkDispatchCompute(GLuint x, GLuint y, GLuint z)
    {
        bindSets();
        barriers();
        if (!flushProgram(true))
            return;
        for (uint32_t i = 0; i < 4; ++i)
            if (s->ssbo[i] && s->ssbo[i] < s->buffers.size() && s->buffers[s->ssbo[i]].mem)
                dkCmdBufBindStorageBuffer(s->cmd, DkStage_Compute, i, s->buffers[s->ssbo[i]].gpu, s->buffers[s->ssbo[i]].size);
        dkCmdBufDispatchCompute(s->cmd, x, y, z);
    }
    void dkMemBarrier(GLbitfield)
    {
        dkCmdBufBarrier(s->cmd, DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    }

    GLsync dkFenceSync(GLenum, GLbitfield)
    {
        Fence *f = new Fence();
        dkCmdBufSignalFence(s->cmd, &f->fence, true);
        f->recorded = true;
        f->serial = 0;
        return reinterpret_cast<GLsync>(f);
    }
    GLenum dkClientWaitSync(GLsync sync, GLbitfield, GLuint64 timeout)
    {
        Fence *f = reinterpret_cast<Fence *>(sync);
        if (!f)
            return kAlreadySignaled;
        if (!f->serial)
            f->serial = submit();
        const DkResult r = dkFenceWait(&f->fence, timeout > INT64_MAX ? -1 : static_cast<int64_t>(timeout));
        return r == DkResult_Success ? kConditionSatisfied : kTimeoutExpired;
    }
    void dkDeleteSync(GLsync sync)
    {
        Fence *f = reinterpret_cast<Fence *>(sync);
        if (!f)
            return;
        // the fence object must outlive its signal: wait for the submit carrying it
        if (!f->serial)
            f->serial = submit();
        waitSerial(f->serial);
        delete f;
    }
    void dkFinish() { waitSerial(submit()); }
    void dkFlush()
    {
        submit();
        stats();
    }
    GLenum dkGetError() { return kNoError; }
    void dkGetIntegerv(GLenum pname, GLint *v)
    {
        *v = pname == kFramebufferBinding ? static_cast<GLint>(s->fbo) : 0;
    }
    const unsigned char *dkGetString(GLenum name)
    {
        static const unsigned char vendor[] = "devkitPro", renderer[] = "deko3d (Tegra X1)", version[] = "4.3 deko3d GSGL emulation";
        return name == kVendor ? vendor : name == kRenderer ? renderer : version;
    }

    void dkGenTextures(GLsizei n, GLuint *ids)
    {
        for (GLsizei i = 0; i < n; ++i)
        {
            if (!s->freeTextures.empty())
            {
                ids[i] = s->freeTextures.back();
                s->freeTextures.pop_back();
                s->textures[ids[i]] = Texture{};
            }
            else
                ids[i] = allocate(s->textures);
        }
    }
    void dkDeleteTextures(GLsizei n, const GLuint *ids)
    {
        for (GLsizei i = 0; i < n; ++i)
        {
            const GLuint id = ids[i];
            if (!id || id >= s->textures.size())
                continue;
            releaseImage(s->textures[id]);
            for (GLuint &u : s->units)
                if (u == id)
                    u = 0;
            if (s->boundColor == id || s->boundDepth == id)
                s->rtDirty = true;
            s->freeTextures.push_back(id);
        }
    }
    void dkBindTexture(GLenum, GLuint id) { s->units[s->activeUnit] = id; }
    void dkActiveTexture(GLenum unit) { s->activeUnit = (unit - kTexture0) & 15u; }
    void dkTexParameteri(GLenum, GLenum pname, GLint value)
    {
        Texture *t = boundTexture();
        if (!t)
            return;
        switch (pname)
        {
        case kTextureMinFilter: t->minFilter = value; break;
        case kTextureMagFilter: t->magFilter = value; break;
        case kTextureWrapS: t->wrapS = value; break;
        case kTextureWrapT: t->wrapT = value; break;
        default: break;
        }
    }
    void dkTexImage2D(GLenum, GLint level, GLint internal, GLsizei w, GLsizei h, GLint, GLenum, GLenum, const void *pixels)
    {
        Texture *t = boundTexture();
        Format f;
        if (!t || level != 0 || !formatOf(internal, f))
        {
            if (t && level == 0)
                std::fprintf(stderr, "[gs-dk] unsupported texture format 0x%x\n", internal);
            return;
        }
        if (!t->valid || t->width != static_cast<uint32_t>(w) || t->height != static_cast<uint32_t>(h) || t->format != f.dk)
            allocImage(*t, static_cast<uint32_t>(w), static_cast<uint32_t>(h), f);
        uploadRect(*t, 0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h), pixels, f);
    }
    void dkTexSubImage2D(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum, GLenum, const void *pixels)
    {
        Texture *t = boundTexture();
        if (!t || !t->valid || level != 0)
            return;
        Format f{t->format, t->bpp, t->depth, t->integer};
        uploadRect(*t, x, y, static_cast<uint32_t>(w), static_cast<uint32_t>(h), pixels, f);
    }
    void dkPixelStorei(GLenum, GLint) {} // the backend's rows are tightly packed for every format it uses

    void dkGenFramebuffers(GLsizei n, GLuint *ids)
    {
        for (GLsizei i = 0; i < n; ++i)
            ids[i] = allocate(s->fbos);
    }
    void dkDeleteFramebuffers(GLsizei n, const GLuint *ids)
    {
        for (GLsizei i = 0; i < n; ++i)
            if (ids[i] && ids[i] < s->fbos.size())
            {
                s->fbos[ids[i]] = Fbo{};
                if (s->fbo == ids[i])
                    s->rtDirty = true;
            }
    }
    void dkBindFramebuffer(GLenum, GLuint id)
    {
        if (id != s->fbo)
        {
            s->fbo = id;
            s->rtDirty = true;
        }
    }
    void dkFramebufferTexture2D(GLenum, GLenum attachment, GLenum, GLuint texture, GLint)
    {
        if (!s->fbo || s->fbo >= s->fbos.size())
            return;
        Fbo &fb = s->fbos[s->fbo];
        if (attachment == kDepthAttachmentPoint)
            fb.depth = texture;
        else if (attachment == kColorAttachment0)
            fb.color = texture;
        s->rtDirty = true;
    }
    GLenum dkCheckFramebufferStatus(GLenum) { return kFramebufferComplete; }
    void dkFramebufferParameteri(GLenum, GLenum, GLint) {}
    void dkDrawBuffers(GLsizei, const GLenum *) {}

    // GOW-Port: every setter marks its state dirty only when the value changes (the backend sets the whole state
    // before every draw; re-sending unchanged state cost command-buffer writes and GPU state rolls per draw)
    void dkViewport(GLint x, GLint y, GLsizei w, GLsizei h)
    {
        if (s->viewport[0] == x && s->viewport[1] == y && s->viewport[2] == w && s->viewport[3] == h)
            return;
        s->viewport[0] = x, s->viewport[1] = y, s->viewport[2] = w, s->viewport[3] = h;
        s->viewportDirty = true;
    }
    void dkScissor(GLint x, GLint y, GLsizei w, GLsizei h)
    {
        if (s->scissorBox[0] == x && s->scissorBox[1] == y && s->scissorBox[2] == w && s->scissorBox[3] == h)
            return;
        s->scissorBox[0] = x, s->scissorBox[1] = y, s->scissorBox[2] = w, s->scissorBox[3] = h;
        if (s->scissor)
            s->viewportDirty = true;
    }
    void setEnable(GLenum cap, bool on)
    {
        switch (cap)
        {
        case kBlend: if (s->blend != on) s->blend = on, s->stateDirty = true; break;
        case kDepthTest: if (s->depthTest != on) s->depthTest = on, s->stateDirty = true; break;
        case kScissorTest: if (s->scissor != on) s->scissor = on, s->viewportDirty = true; break;
        case kConservative: dkCmdBufSetConservativeRasterEnable(s->cmd, on); break;
        default: break;
        }
    }
    void dkEnable(GLenum cap) { setEnable(cap, true); }
    void dkDisable(GLenum cap) { setEnable(cap, false); }
    void dkBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d)
    {
        if (s->blendSrc == a && s->blendDst == b && s->blendSrcA == c && s->blendDstA == d)
            return;
        s->blendSrc = a, s->blendDst = b, s->blendSrcA = c, s->blendDstA = d;
        s->stateDirty = true;
    }
    void dkBlendEquationSeparate(GLenum a, GLenum b)
    {
        if (s->eqColor == a && s->eqAlpha == b)
            return;
        s->eqColor = a, s->eqAlpha = b;
        s->stateDirty = true;
    }
    void dkBlendColor(float r, float g, float b, float a)
    {
        if (s->blendConst[0] == r && s->blendConst[1] == g && s->blendConst[2] == b && s->blendConst[3] == a)
            return;
        s->blendConst[0] = r, s->blendConst[1] = g, s->blendConst[2] = b, s->blendConst[3] = a;
        s->blendConstDirty = true;
    }
    void dkColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
    {
        const bool m[4] = {r != 0, g != 0, b != 0, a != 0};
        if (s->mask[0] == m[0] && s->mask[1] == m[1] && s->mask[2] == m[2] && s->mask[3] == m[3])
            return;
        s->mask[0] = m[0], s->mask[1] = m[1], s->mask[2] = m[2], s->mask[3] = m[3];
        s->stateDirty = true;
    }
    void dkDepthMask(GLboolean on)
    {
        if (s->depthWrite != (on != 0))
            s->depthWrite = on != 0, s->stateDirty = true;
    }
    void dkDepthFunc(GLenum f)
    {
        if (s->depthFunc != f)
            s->depthFunc = f, s->stateDirty = true;
    }
    void dkClearColor(float r, float g, float b, float a) { s->clearColor[0] = r, s->clearColor[1] = g, s->clearColor[2] = b, s->clearColor[3] = a; }
    void dkClearDepth(double d) { s->clearDepth = static_cast<float>(d); }
    void dkClear(GLbitfield mask)
    {
        bindSets();
        if (!bindTargets())
            return;
        flushViewport();
        // a clear honours the scissor and the colour mask, as in GL
        if (mask & kColorBit)
        {
            const uint32_t m = (s->mask[0] ? DkColorMask_R : 0u) | (s->mask[1] ? DkColorMask_G : 0u) |
                               (s->mask[2] ? DkColorMask_B : 0u) | (s->mask[3] ? DkColorMask_A : 0u);
            if (m)
                dkCmdBufClearColorFloat(s->cmd, 0, m, s->clearColor[0], s->clearColor[1], s->clearColor[2], s->clearColor[3]);
        }
        if ((mask & kDepthBit) && s->boundDepth && s->depthWrite)
            dkCmdBufClearDepthStencil(s->cmd, true, s->clearDepth, 0, 0);
    }

    void dkGenVertexArrays(GLsizei n, GLuint *ids)
    {
        for (GLsizei i = 0; i < n; ++i)
            ids[i] = allocate(s->vaos);
    }
    void dkDeleteVertexArrays(GLsizei, const GLuint *) {}
    void dkBindVertexArray(GLuint id) { s->vao = id; }
    void attribPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer, bool integer)
    {
        if (!s->vao || index >= 8)
            return;
        Attrib &a = s->vaos[s->vao].attribs[index];
        a.buffer = s->arrayBuffer;
        a.offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
        a.stride = static_cast<uint32_t>(stride);
        if (type == kFloatType)
        {
            static const DkVtxAttribSize sizes[] = {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32};
            a.size = sizes[std::clamp(size, 1, 4) - 1];
            a.type = DkVtxAttribType_Float;
        }
        else if (type == kUByte && size == 4)
        {
            a.size = DkVtxAttribSize_4x8;
            a.type = integer ? DkVtxAttribType_Uint : DkVtxAttribType_Uscaled; // non-normalized bytes -> 0..255 floats
        }
        else
        {
            static const DkVtxAttribSize sizes[] = {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32};
            a.size = sizes[std::clamp(size, 1, 4) - 1];
            a.type = type == kIntType ? DkVtxAttribType_Sint : DkVtxAttribType_Uint;
        }
    }
    void dkVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean, GLsizei stride, const void *p)
    {
        attribPointer(i, size, type, stride, p, false);
    }
    void dkVertexAttribIPointer(GLuint i, GLint size, GLenum type, GLsizei stride, const void *p)
    {
        attribPointer(i, size, type, stride, p, true);
    }
    void dkEnableVertexAttribArray(GLuint i)
    {
        if (s->vao && i < 8)
            s->vaos[s->vao].attribs[i].enabled = true;
    }

    DkPrimitive primitiveOf(GLenum mode)
    {
        return mode == 0x0005 ? DkPrimitive_TriangleStrip : mode == 0x0006 ? DkPrimitive_TriangleFan
               : mode == 0x0001 ? DkPrimitive_Lines : mode == 0x0000 ? DkPrimitive_Points : DkPrimitive_Triangles;
    }
    bool prepareDraw()
    {
        bindSets();
        if (!bindTargets())
            return false;
        barriers();
        flushViewport();
        flushFixedState();
        return flushProgram(false) && flushVertices();
    }
    struct DrawTimer
    {
        bool on;
        std::chrono::steady_clock::time_point start;
        DrawTimer()
        {
            static const bool timed = [] { const char *v = std::getenv("GOW_PERF_DIAG"); return v && *v && *v != '0'; }();
            on = timed;
            if (on)
                start = std::chrono::steady_clock::now();
        }
        ~DrawTimer()
        {
            if (on)
                s->drawNanos += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        }
    };
    // indices from the vertex array's element buffer; `offset` is a byte offset into it
    void dkDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *offset, GLint baseVertex)
    {
        if (count <= 0)
            return;
        DrawTimer timer;
        const Buffer *ib = bound(kElementArray);
        if (!ib || !ib->mem || !prepareDraw())
            return;
        const DkIdxFormat format = type == 0x1403 ? DkIdxFormat_Uint16 : type == 0x1401 ? DkIdxFormat_Uint8 : DkIdxFormat_Uint32;
        const uint32_t bytes = format == DkIdxFormat_Uint16 ? 2u : format == DkIdxFormat_Uint8 ? 1u : 4u;
        if (s->boundIdx != ib->gpu || s->boundIdxFormat != format)
        {
            dkCmdBufBindIdxBuffer(s->cmd, format, ib->gpu);
            s->boundIdx = ib->gpu;
            s->boundIdxFormat = format;
        }
        dkCmdBufDrawIndexed(s->cmd, primitiveOf(mode), static_cast<uint32_t>(count), 1,
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(offset) / bytes), baseVertex, 0);
        ++s->draws;
        maybeSubmit();
    }
    void dkDrawArrays(GLenum mode, GLint first, GLsizei count)
    {
        if (count <= 0)
            return;
        DrawTimer timer;
        if (!prepareDraw())
            return;
        dkCmdBufDraw(s->cmd, primitiveOf(mode), static_cast<uint32_t>(count), 1, static_cast<uint32_t>(first), 0);
        ++s->draws;
        maybeSubmit();
    }

    void dkReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *out)
    {
        const Fbo &fb = s->fbos[s->fbo < s->fbos.size() ? s->fbo : 0];
        const bool depth = format == 0x1902; // DEPTH_COMPONENT
        const GLuint id = depth ? fb.depth : fb.color;
        if (!id || id >= s->textures.size())
            return;
        readRect(s->textures[id], x, y, w, h, out, depth ? 4u : 4u, format, type);
    }
    void dkTextureBarrier()
    {
        // what the next draw reads of the bound target (destination alpha, blend in the shader, Z alias)
        dkCmdBufBarrier(s->cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
        ++s->barrierCount;
    }
    void dkCopyImageSubData(GLuint src, GLenum, GLint, GLint sx, GLint sy, GLint, GLuint dst, GLenum, GLint, GLint dx, GLint dy, GLint,
                            GLsizei w, GLsizei h, GLsizei)
    {
        if (!src || !dst || src >= s->textures.size() || dst >= s->textures.size())
            return;
        Texture &a = s->textures[src], &b = s->textures[dst];
        if (!a.valid || !b.valid)
            return;
        dkCmdBufBarrier(s->cmd, DkBarrier_Fragments, 0);
        DkImageRect ra{static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), 0u, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
        DkImageRect rb{static_cast<uint32_t>(dx), static_cast<uint32_t>(dy), 0u, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
        dkCmdBufCopyImage(s->cmd, &a.view, &ra, &b.view, &rb, 0u);
        s->imageInvalidate = true;
    }

    // ---- presentation -------------------------------------------------------------------------------------------------
    bool initSwapchain()
    {
        DkImageLayoutMaker maker;
        dkImageLayoutMakerDefaults(&maker, s->device);
        maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
        maker.format = DkImageFormat_RGBA8_Unorm;
        maker.dimensions[0] = kScreenW;
        maker.dimensions[1] = kScreenH;
        DkImageLayout layout;
        dkImageLayoutInitialize(&layout, &maker);
        const uint32_t size = align(static_cast<uint32_t>(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
        s->swapMem = makeMem(size * kSwapImages, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
        const DkImage *images[kSwapImages];
        for (uint32_t i = 0; i < kSwapImages; ++i)
        {
            dkImageInitialize(&s->swapImages[i], &layout, s->swapMem, i * size);
            images[i] = &s->swapImages[i];
        }
        DkSwapchainMaker sm;
        dkSwapchainMakerDefaults(&sm, s->device, nwindowGetDefault(), images, kSwapImages);
        s->swapchain = dkSwapchainCreate(&sm);
        if (!s->swapchain)
            return false;
        dkSwapchainSetSwapInterval(s->swapchain, 1);
        return true;
    }

    void ensureScaleProgram()
    {
        if (s->scaleProgram)
            return;
        const GSDK::ShaderBinary *vs = findBinary(GSDK::kScaleVertexId), *fs = findBinary(GSDK::kScaleFragmentId);
        if (!vs || !fs)
        {
            fail("presentation scaler missing from the shader table");
            return;
        }
        const GLuint p = dkCreateProgram();
        s->programs[p].stages = {vs, fs};
        dkLinkProgram(p);
        s->scaleProgram = p;
        s->scaleRect = dkGetUniformLocation(p, "uRect");
        s->scaleSrcSize = dkGetUniformLocation(p, "uSrcSize");
        s->scaleSharpen = dkGetUniformLocation(p, "uSharpen");
        s->scalePlain = dkGetUniformLocation(p, "uPlain");
    }

    // the composed frame (texture id, or 0 = blank) onto the next swapchain image, 16:9 over the whole 720p screen
    void present(GLuint texture, uint32_t width, uint32_t height)
    {
        ensureScaleProgram();
        bindSets();
        barriers();
        // the GPU, not this thread, waits for the image to come back from the display
        int slot = 0;
        DkFence acquired{};
        const auto acquireStart = std::chrono::steady_clock::now();
        dkSwapchainAcquireImage(s->swapchain, &slot, &acquired);
        s->acquireNanos += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - acquireStart).count());
        dkCmdBufWaitFence(s->cmd, &acquired);
        dkCmdBufBarrier(s->cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
        DkImageView view;
        dkImageViewDefaults(&view, &s->swapImages[slot]);
        dkCmdBufBindRenderTarget(s->cmd, &view, nullptr);
        DkViewport vp{0.f, 0.f, static_cast<float>(kScreenW), static_cast<float>(kScreenH), 0.f, 1.f};
        dkCmdBufSetViewports(s->cmd, 0, &vp, 1);
        DkScissor sc{0u, 0u, kScreenW, kScreenH};
        dkCmdBufSetScissors(s->cmd, 0, &sc, 1);
        dkCmdBufClearColorFloat(s->cmd, 0, DkColorMask_RGBA, 0.f, 0.f, 0.f, 1.f);
        const Texture *t = texture && texture < s->textures.size() && s->textures[texture].valid ? &s->textures[texture] : nullptr;
        if (t && s->scaleProgram && s->programs[s->scaleProgram].linked)
        {
            // fixed function: opaque, no depth, all channels
            DkRasterizerState raster;
            dkRasterizerStateDefaults(&raster);
            raster.cullMode = DkFace_None;
            dkCmdBufBindRasterizerState(s->cmd, &raster);
            DkColorState color;
            dkColorStateDefaults(&color);
            dkCmdBufBindColorState(s->cmd, &color);
            DkColorWriteState write;
            dkColorWriteStateDefaults(&write);
            dkCmdBufBindColorWriteState(s->cmd, &write);
            DkDepthStencilState depth;
            dkDepthStencilStateDefaults(&depth);
            depth.depthTestEnable = false;
            depth.depthWriteEnable = false;
            dkCmdBufBindDepthStencilState(s->cmd, &depth);
            Program &p = s->programs[s->scaleProgram];
            const float w = static_cast<float>(std::max(width, 1u)), h = static_cast<float>(std::max(height, 1u));
            // the frame's used part (width x height) of its texture; GL row 0 is the picture's bottom
            const float u = w / static_cast<float>(t->width), v = h / static_cast<float>(t->height);
            const float rect[4] = {u, s->flipPresent ? -v : v, 0.f, s->flipPresent ? v : 0.f};
            const float size[2] = {static_cast<float>(t->width), static_cast<float>(t->height)};
            static const float sharpen = [] { const char *e = std::getenv("GOW_SHARPEN"); return e ? static_cast<float>(std::atof(e)) : 0.5f; }();
            static const bool bilinear = [] { const char *e = std::getenv("GOW_SCALER"); return e && std::strcmp(e, "bilinear") == 0; }();
            const float plain = bilinear || (w >= kScreenW && h >= kScreenH) ? 1.f : 0.f;
            const GLuint previous = s->program;
            s->program = s->scaleProgram;
            setUniform(s->scaleRect, rect, 16);
            setUniform(s->scaleSrcSize, size, 8);
            setUniform(s->scaleSharpen, &sharpen, 4);
            setUniform(s->scalePlain, &plain, 4);
            Texture &frame = s->textures[texture];
            const GLint savedMin = frame.minFilter, savedMag = frame.magFilter, savedS = frame.wrapS, savedT = frame.wrapT;
            frame.minFilter = frame.magFilter = 0x2601;
            frame.wrapS = frame.wrapT = 0x812F;
            const GLuint savedUnit0 = s->units[0];
            s->units[0] = texture;
            p.samplerUnit[0] = 0;
            s->programDirty = true;
            flushProgram(false);
            dkCmdBufDraw(s->cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
            s->units[0] = savedUnit0;
            frame.minFilter = savedMin, frame.magFilter = savedMag, frame.wrapS = savedS, frame.wrapT = savedT;
            s->program = previous;
        }
        dkCmdBufBarrier(s->cmd, DkBarrier_Fragments, 0);
        const auto submitStart = std::chrono::steady_clock::now();
        submit();
        dkQueuePresentImage(s->queue, s->swapchain, slot);
        s->presentSubmitNanos += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submitStart).count());
        s->rtDirty = s->stateDirty = s->viewportDirty = s->programDirty = true;
        ++s->presents;
        s->presented.fetch_add(1, std::memory_order_release);
        stats();
    }

    void fillApi(Api &a)
    {
        a.GenBuffers = dkGenBuffers;
        a.DeleteBuffers = dkDeleteBuffers;
        a.BindBuffer = dkBindBuffer;
        a.BufferData = dkBufferData;
        a.BufferSubData = dkBufferSubData;
        a.GetBufferSubData = dkGetBufferSubData;
        a.BindBufferBase = dkBindBufferBase;
        a.CopyBufferSubData = dkCopyBufferSubData;
        a.BufferStorage = dkBufferStorage;
        a.MapBufferRange = dkMapBufferRange;
        a.UnmapBuffer = dkUnmapBuffer;
        a.GenQueries = dkGenQueries;
        a.DeleteQueries = dkDeleteQueries;
        a.BeginQuery = dkBeginQuery;
        a.EndQuery = dkEndQuery;
        a.GetQueryObjectui64v = dkGetQueryObjectui64v;
        a.QueryCounter = dkQueryCounter;
        a.CreateShader = dkCreateShader;
        a.ShaderSource = dkShaderSource;
        a.CompileShader = dkCompileShader;
        a.GetShaderiv = dkGetShaderiv;
        a.GetShaderInfoLog = dkGetShaderInfoLog;
        a.DeleteShader = dkDeleteShader;
        a.CreateProgram = dkCreateProgram;
        a.AttachShader = dkAttachShader;
        a.LinkProgram = dkLinkProgram;
        a.GetProgramiv = dkGetProgramiv;
        a.GetProgramInfoLog = dkGetProgramInfoLog;
        a.DeleteProgram = dkDeleteProgram;
        a.UseProgram = dkUseProgram;
        a.GetUniformLocation = dkGetUniformLocation;
        a.Uniform1ui = dkUniform1ui;
        a.Uniform4ui = dkUniform4ui;
        a.Uniform1uiv = dkUniform1uiv;
        a.DispatchCompute = dkDispatchCompute;
        a.MemBarrier = dkMemBarrier;
        a.FenceSync = dkFenceSync;
        a.ClientWaitSync = dkClientWaitSync;
        a.DeleteSync = dkDeleteSync;
        a.Finish = dkFinish;
        a.Flush = dkFlush;
        a.GetError = dkGetError;
        a.GetIntegerv = dkGetIntegerv;
        a.GetString = dkGetString;
        a.GenTextures = dkGenTextures;
        a.DeleteTextures = dkDeleteTextures;
        a.BindTexture = dkBindTexture;
        a.TexParameteri = dkTexParameteri;
        a.TexImage2D = dkTexImage2D;
        a.TexSubImage2D = dkTexSubImage2D;
        a.GenFramebuffers = dkGenFramebuffers;
        a.DeleteFramebuffers = dkDeleteFramebuffers;
        a.BindFramebuffer = dkBindFramebuffer;
        a.FramebufferParameteri = dkFramebufferParameteri;
        a.Viewport = dkViewport;
        a.DrawArrays = dkDrawArrays;
        a.DrawElementsBaseVertex = dkDrawElementsBaseVertex;
        a.GenVertexArrays = dkGenVertexArrays;
        a.DeleteVertexArrays = dkDeleteVertexArrays;
        a.BindVertexArray = dkBindVertexArray;
        a.Enable = dkEnable;
        a.Disable = dkDisable;
        a.ActiveTexture = dkActiveTexture;
        a.FramebufferTexture2D = dkFramebufferTexture2D;
        a.CheckFramebufferStatus = dkCheckFramebufferStatus;
        a.BlendFuncSeparate = dkBlendFuncSeparate;
        a.BlendEquationSeparate = dkBlendEquationSeparate;
        a.BlendColor = dkBlendColor;
        a.ColorMask = dkColorMask;
        a.DepthMask = dkDepthMask;
        a.DepthFunc = dkDepthFunc;
        a.ClearColor = dkClearColor;
        a.ClearDepth = dkClearDepth;
        a.Clear = dkClear;
        a.Scissor = dkScissor;
        a.Uniform1i = dkUniform1i;
        a.Uniform1f = dkUniform1f;
        a.Uniform2f = dkUniform2f;
        a.Uniform4f = dkUniform4f;
        a.Uniform4i = dkUniform4i;
        a.VertexAttribPointer = dkVertexAttribPointer;
        a.VertexAttribIPointer = dkVertexAttribIPointer;
        a.EnableVertexAttribArray = dkEnableVertexAttribArray;
        a.DrawBuffers = dkDrawBuffers;
        a.ReadPixels = dkReadPixels;
        a.TextureBarrier = dkTextureBarrier;
        a.CopyImageSubData = dkCopyImageSubData;
        a.PixelStorei = dkPixelStorei;
        a.hwRaster = true;
        a.graphics = true;
    }

    // start-up checkpoints (a fault in a guest-less thread is silent in Citron)
    void step(const char *what)
    {
        std::fprintf(stderr, "[gs-dk] init: %s\n", what);
        std::fflush(stderr);
    }

    bool initDevice(std::string &error)
    {
        step("device");
        s = new State();
        DkDeviceMaker dm;
        dkDeviceMakerDefaults(&dm);
        dm.cbDebug = debugCallback;
        dm.flags = DkDeviceFlags_OriginLowerLeft | DkDeviceFlags_DepthMinusOneToOne; // GL conventions
        s->device = dkDeviceCreate(&dm);
        if (!s->device)
        {
            error = "dkDeviceCreate failed";
            return false;
        }
        step("queue");
        DkQueueMaker qm;
        dkQueueMakerDefaults(&qm, s->device);
        qm.flags = DkQueueFlags_Graphics | (std::getenv("GOW_DK_COMPUTE") ? DkQueueFlags_Compute : 0u); // compute: only the RAM-frame pack, unused here
        s->queue = dkQueueCreate(&qm);
        step("queue created");
        s->cmdMem = makeMem(kCmdChunk * kCmdChunks, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        step("command memory");
        DkCmdBufMaker cm;
        dkCmdBufMakerDefaults(&cm, s->device);
        cm.cbAddMem = addMemory;
        s->cmd = dkCmdBufCreate(&cm);
        if (!s->queue || !s->cmdMem || !s->cmd)
        {
            error = "deko3d queue / command buffer creation failed";
            return false;
        }
        s->chunk = 0;
        s->chunkSerial[0] = UINT64_MAX;
        s->chunksThisList = 1;
        dkCmdBufAddMemory(s->cmd, s->cmdMem, 0, kCmdChunk);

        step("descriptors");
        const uint32_t imageBytes = kImageSlots * sizeof(DkImageDescriptor), samplerBytes = kSamplerSlots * sizeof(DkSamplerDescriptor);
        s->descMem = makeMem(imageBytes + samplerBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        s->imageDescs = static_cast<DkImageDescriptor *>(dkMemBlockGetCpuAddr(s->descMem));
        s->samplerDescs = reinterpret_cast<DkSamplerDescriptor *>(static_cast<uint8_t *>(dkMemBlockGetCpuAddr(s->descMem)) + imageBytes);
        s->imageDescAddr = dkMemBlockGetGpuAddr(s->descMem);
        s->samplerDescAddr = s->imageDescAddr + imageBytes;
        for (uint32_t i = 0; i < 16u; ++i)
        {
            DkSampler sampler;
            dkSamplerDefaults(&sampler);
            sampler.minFilter = (i & 1u) ? DkFilter_Linear : DkFilter_Nearest;
            sampler.magFilter = (i & 2u) ? DkFilter_Linear : DkFilter_Nearest;
            sampler.wrapMode[0] = (i & 4u) ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
            sampler.wrapMode[1] = (i & 8u) ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
            sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
            dkSamplerDescriptorInitialize(&s->samplerDescs[i], &sampler);
        }

        step("memory");
        s->codeSize = 8u << 20;
        s->codeMem = makeMem(s->codeSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
        s->uboSize = 1u << 20;
        s->uboMem = makeMem(s->uboSize, DkMemBlockFlags_GpuCached);
        s->stageSize = 48u << 20;
        s->stageMem = makeMem(s->stageSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        s->stageCpu = static_cast<uint8_t *>(dkMemBlockGetCpuAddr(s->stageMem));
        if (!s->descMem || !s->codeMem || !s->uboMem || !s->stageMem)
        {
            error = "deko3d memory allocation failed";
            return false;
        }
        for (unsigned i = 0; i < GSDK::kShaderCount; ++i)
            s->binaries[GSDK::kShaders[i].hash] = &GSDK::kShaders[i];

        step("dummy texture");
        // texture 0: a 1x1 dummy for unbound samplers
        Format f{DkImageFormat_RGBA8_Unorm, 4, false, false};
        allocImage(s->textures[0], 1, 1, f);
        const uint32_t black = 0u;
        uploadRect(s->textures[0], 0, 0, 1, 1, &black, f);

        const char *flip = std::getenv("GOW_DK_PRESENT_FLIP");
        s->flipPresent = !(flip && *flip == '0');
        step("swapchain");
        if (!initSwapchain())
        {
            error = "deko3d swapchain creation failed";
            return false;
        }
        std::fprintf(stderr, "[gs-dk] deko3d renderer: %u shaders, swapchain %ux%u\n", GSDK::kShaderCount, kScreenW, kScreenH);
        return true;
    }
}

// ======== GSGL / GSSharedPresent on deko3d ===========================================================================
namespace GSGL
{
    Context::~Context() { Destroy(); }

    bool Context::Create(std::string &error)
    {
        if (s)
        {
            error = "the deko3d renderer exists already";
            return false;
        }
        if (!initDevice(error))
            return false;
        fillApi(m_api);
        m_context = s;
        m_shared = true; // frames are presented by PublishSharedTexture, never read back
        return true;
    }

    void Context::Destroy()
    {
        if (!m_context || !s)
            return;
        dkQueueWaitIdle(s->queue);
        m_context = nullptr;
        m_api = {};
        m_shared = false;
    }

    void *Context::CreateWorker() const { return nullptr; }
    bool Context::MakeWorkerCurrent(void *) const { return false; }
    void Context::DestroyWorker(void *) {}

    bool PublishShared(const Api &, GLuint, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t, bool, bool, uint64_t, GLuint, GLint)
    {
        return false;
    }

    bool PublishSharedTexture(const Api &, GLuint source, uint32_t width, uint32_t height, uint64_t, uint32_t, uint32_t, uint64_t)
    {
        if (!s)
            return false;
        present(source, width, height);
        return true;
    }

    bool SharedAvailable(const Api &) { return s != nullptr; }
    void DestroyShared(const Api &) {}
}

namespace GSSharedPresent
{
    void CaptureHostContext() {}
    void RestoreHostContext() {}
    bool Active() { return true; } // the GS thread presents; the host thread only paces (gs_host_present.cpp)
    uint64_t LatestSequence() { return s ? s->presented.load(std::memory_order_acquire) : 0u; }
    bool Acquire(Frame &) { return false; }
    void ShutdownHost() {}
}
