// GOW-Port: adaptado de Taylor N. Albarnaz / LightVelox (GPL-3.0).
// Origen: PS2Recomp sotc-port, ac9efa070638ad3b3accd284de6f898d5ab271d1.
#include "gs_gl.h"
#include "runtime/gs/gs_shared_present.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{
    constexpr int kContextMajorVersion = 0x2091;
    constexpr int kContextMinorVersion = 0x2092;
    constexpr int kContextFlags = 0x2094;
    constexpr int kContextProfileMask = 0x9126;
    constexpr int kContextCoreProfileBit = 0x0001;

    using CreateContextAttribs = HGLRC(WINAPI *)(HDC, HGLRC, const int *);

    HGLRC s_hostContext = nullptr;
    HDC s_hostDc = nullptr;
    PIXELFORMATDESCRIPTOR s_hostFormat{};
    int s_hostFormatIndex = 0;

    void forgetHostContext()
    {
        s_hostContext = nullptr;
        s_hostDc = nullptr;
    }

    HMODULE openGlModule()
    {
        static HMODULE module = LoadLibraryA("opengl32.dll");
        return module;
    }

    void *loadProc(const char *name)
    {
        void *proc = reinterpret_cast<void *>(wglGetProcAddress(name));
        const auto value = reinterpret_cast<intptr_t>(proc);
        if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1)
            proc = reinterpret_cast<void *>(GetProcAddress(openGlModule(), name));
        return proc;
    }

    template <typename T>
    bool load(T &target, const char *name, std::string &missing)
    {
        target = reinterpret_cast<T>(loadProc(name));
        if (!target)
        {
            missing += missing.empty() ? name : std::string(", ") + name;
            return false;
        }
        return true;
    }

    LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        return DefWindowProcA(window, message, wparam, lparam);
    }
}
#elif defined(__SWITCH__)
#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace
{
    // GOW-Port: the host window's EGL context (raylib/SDL, main thread), captured so the GS renderer's context shares
    // its objects (the presented frame's texture).
    EGLDisplay s_hostEglDisplay = EGL_NO_DISPLAY;
    EGLContext s_hostEglContext = EGL_NO_CONTEXT;

    void forgetHostContext()
    {
        s_hostEglDisplay = EGL_NO_DISPLAY;
        s_hostEglContext = EGL_NO_CONTEXT;
    }

    void *loadProc(const char *name)
    {
        return reinterpret_cast<void *>(eglGetProcAddress(name));
    }

    template <typename T>
    bool load(T &target, const char *name, std::string &missing)
    {
        target = reinterpret_cast<T>(loadProc(name));
        if (!target)
        {
            missing += missing.empty() ? name : std::string(", ") + name;
            return false;
        }
        return true;
    }
}
#endif

#if defined(_WIN32) || defined(__SWITCH__)
namespace
{
    // Every entry point the GS backend uses; graphics/hwRaster/CopyImageSubData are optional groups (flags in Api).
    void loadApi(GSGL::Api &a, std::string &missing)
    {
    load(a.GenBuffers, "glGenBuffers", missing);
    load(a.DeleteBuffers, "glDeleteBuffers", missing);
    load(a.BindBuffer, "glBindBuffer", missing);
    load(a.BufferData, "glBufferData", missing);
    load(a.BufferSubData, "glBufferSubData", missing);
    load(a.GetBufferSubData, "glGetBufferSubData", missing);
    load(a.BindBufferBase, "glBindBufferBase", missing);
    load(a.BindBufferRange, "glBindBufferRange", missing);
    load(a.ClearBufferData, "glClearBufferData", missing);
    load(a.CopyBufferSubData, "glCopyBufferSubData", missing);
    load(a.BufferStorage, "glBufferStorage", missing);
    load(a.MapBufferRange, "glMapBufferRange", missing);
    load(a.UnmapBuffer, "glUnmapBuffer", missing);
    load(a.GenQueries, "glGenQueries", missing);
    load(a.DeleteQueries, "glDeleteQueries", missing);
    load(a.BeginQuery, "glBeginQuery", missing);
    load(a.EndQuery, "glEndQuery", missing);
    load(a.GetQueryObjectui64v, "glGetQueryObjectui64v", missing);
    load(a.QueryCounter, "glQueryCounter", missing);
    load(a.CreateShader, "glCreateShader", missing);
    load(a.ShaderSource, "glShaderSource", missing);
    load(a.CompileShader, "glCompileShader", missing);
    load(a.GetShaderiv, "glGetShaderiv", missing);
    load(a.GetShaderInfoLog, "glGetShaderInfoLog", missing);
    load(a.DeleteShader, "glDeleteShader", missing);
    load(a.CreateProgram, "glCreateProgram", missing);
    load(a.AttachShader, "glAttachShader", missing);
    load(a.LinkProgram, "glLinkProgram", missing);
    load(a.GetProgramiv, "glGetProgramiv", missing);
    load(a.GetProgramInfoLog, "glGetProgramInfoLog", missing);
    load(a.ProgramParameteri, "glProgramParameteri", missing);
    load(a.GetProgramBinary, "glGetProgramBinary", missing);
    load(a.ProgramBinary, "glProgramBinary", missing);
    load(a.DeleteProgram, "glDeleteProgram", missing);
    load(a.UseProgram, "glUseProgram", missing);
    load(a.GetUniformLocation, "glGetUniformLocation", missing);
    load(a.Uniform1ui, "glUniform1ui", missing);
    load(a.Uniform4ui, "glUniform4ui", missing);
    load(a.Uniform1uiv, "glUniform1uiv", missing);
    load(a.DispatchCompute, "glDispatchCompute", missing);
    load(a.MemBarrier, "glMemoryBarrier", missing);
    load(a.FenceSync, "glFenceSync", missing);
    load(a.ClientWaitSync, "glClientWaitSync", missing);
    load(a.DeleteSync, "glDeleteSync", missing);
    load(a.Finish, "glFinish", missing);
    load(a.Flush, "glFlush", missing);
    load(a.GetError, "glGetError", missing);
    load(a.GetString, "glGetString", missing);
    load(a.GenTextures, "glGenTextures", missing);
    load(a.DeleteTextures, "glDeleteTextures", missing);
    load(a.BindTexture, "glBindTexture", missing);
    load(a.TexParameteri, "glTexParameteri", missing);
    load(a.TexImage2D, "glTexImage2D", missing);
    load(a.TexSubImage2D, "glTexSubImage2D", missing);
    {
        std::string graphicsMissing;
        load(a.GenFramebuffers, "glGenFramebuffers", graphicsMissing);
        load(a.DeleteFramebuffers, "glDeleteFramebuffers", graphicsMissing);
        load(a.BindFramebuffer, "glBindFramebuffer", graphicsMissing);
        load(a.FramebufferParameteri, "glFramebufferParameteri", graphicsMissing);
        load(a.Viewport, "glViewport", graphicsMissing);
        load(a.DrawArrays, "glDrawArrays", graphicsMissing);
        load(a.GenVertexArrays, "glGenVertexArrays", graphicsMissing);
        load(a.DeleteVertexArrays, "glDeleteVertexArrays", graphicsMissing);
        load(a.BindVertexArray, "glBindVertexArray", graphicsMissing);
        load(a.Enable, "glEnable", graphicsMissing);
        load(a.Disable, "glDisable", graphicsMissing);
        std::string hwMissing;
        load(a.ActiveTexture, "glActiveTexture", hwMissing);
        load(a.FramebufferTexture2D, "glFramebufferTexture2D", hwMissing);
        load(a.CheckFramebufferStatus, "glCheckFramebufferStatus", hwMissing);
        load(a.BlendFuncSeparate, "glBlendFuncSeparate", hwMissing);
        load(a.BlendEquationSeparate, "glBlendEquationSeparate", hwMissing);
        load(a.BlendColor, "glBlendColor", hwMissing);
        load(a.ColorMask, "glColorMask", hwMissing);
        load(a.DepthMask, "glDepthMask", hwMissing);
        load(a.DepthFunc, "glDepthFunc", hwMissing);
        load(a.ClearColor, "glClearColor", hwMissing);
        load(a.ClearDepth, "glClearDepth", hwMissing);
        load(a.Clear, "glClear", hwMissing);
        load(a.Scissor, "glScissor", hwMissing);
        load(a.Uniform1i, "glUniform1i", hwMissing);
        load(a.Uniform1f, "glUniform1f", hwMissing);
        load(a.Uniform2f, "glUniform2f", hwMissing);
        load(a.Uniform4f, "glUniform4f", hwMissing);
        load(a.Uniform4i, "glUniform4i", hwMissing);
        load(a.VertexAttribPointer, "glVertexAttribPointer", hwMissing);
        load(a.VertexAttribIPointer, "glVertexAttribIPointer", hwMissing);
        load(a.EnableVertexAttribArray, "glEnableVertexAttribArray", hwMissing);
        load(a.DrawBuffers, "glDrawBuffers", hwMissing);
        load(a.ReadPixels, "glReadPixels", hwMissing);
        load(a.BlitFramebuffer, "glBlitFramebuffer", hwMissing);
        load(a.PixelStorei, "glPixelStorei", hwMissing);
        std::string barrierMissing; // core since 4.5; GL_NV_texture_barrier on older contexts (switch-mesa)
        if (!load(a.TextureBarrier, "glTextureBarrier", barrierMissing))
            load(a.TextureBarrier, "glTextureBarrierNV", hwMissing);
        std::string copyMissing;
        load(a.CopyImageSubData, "glCopyImageSubData", copyMissing);
        std::string indexedMissing;
        load(a.DrawElementsBaseVertex, "glDrawElementsBaseVertex", indexedMissing);
        a.hwRaster = graphicsMissing.empty() && hwMissing.empty();
        if (!a.hwRaster)
            std::fprintf(stderr, "[gs-gl] classic raster entry points missing: %s (graphics=%d)\n", hwMissing.c_str(), graphicsMissing.empty() ? 1 : 0);
        std::string parallelMissing;
        load(a.MaxShaderCompilerThreads, "glMaxShaderCompilerThreadsARB", parallelMissing);
        if (!a.MaxShaderCompilerThreads)
            load(a.MaxShaderCompilerThreads, "glMaxShaderCompilerThreadsKHR", parallelMissing);
        a.graphics = graphicsMissing.empty();
    }
    }
}
#endif

#if defined(_WIN32) || defined(__SWITCH__)
// GOW-Port: shared-texture presentation, the same on both platforms: the GS renderer's context shares objects with the
// host window's context; Present copies the composed frame into one of these slots (a texture of the shared namespace)
// and fences it, the host takes the newest finished slot and draws it, then fences its use before the slot is reused.
// The contexts' platform part (WGL / EGL) is below.
namespace
{
    struct SharedSlot
    {
        GSSharedPresent::Frame frame;
        GSGL::GLsync produced = nullptr;
        GSGL::GLsync consumed = nullptr;
        GSGL::GLuint hashBuffer = 0;
        GSGL::GLuint hashDeviceBuffer = 0;
        const uint32_t *hash = nullptr;
        bool reading = false;
    };

    std::mutex s_sharedMutex;
    std::array<SharedSlot, 8> s_sharedSlots;
    GSGL::Api s_hostApi; // the host window context's fence entry points
    int s_readingSlot = -1;
    uint64_t s_sharedSequence = 0;
    std::atomic<bool> s_sharedActive{false};
    std::atomic<uint64_t> s_publishedSequence{0}; // s_sharedSequence, readable without the lock (LatestSequence)

    bool completed(const GSGL::Api &gl, GSGL::GLsync fence)
    {
        if (!fence)
            return true;
        const auto status = gl.ClientWaitSync(fence, 0u, 0u);
        return status == GSGL::kAlreadySignaled || status == GSGL::kConditionSatisfied;
    }

    // GOW-Port: sharing self-check. On the first console run with shared textures the window showed only the magenta
    // placeholder for the whole run: the host never got a frame it could show, though both contexts reported success.
    // So the GS context makes a 1x1 probe texture by the same GPU copy the frames use (CopyImageSubData) and the host
    // context reads it back once before trusting any slot; a wrong value, or no slot ever completing within 3 s of the
    // first publish (fences not visible across the contexts), turns sharing off and both sides go back to RAM frames.
    constexpr uint8_t kProbe[4] = {0x5A, 0xC3, 0x3C, 0xA5};
    GSGL::GLuint s_probeTexture = 0;
    bool s_probeChecked = false;
    bool s_everAcquired = false;
    std::chrono::steady_clock::time_point s_firstPublish{};
    GSGL::Api s_hostProbeApi; // the host context's framebuffer/readback entry points (empty: no probe)

    void createProbe(const GSGL::Api &gl)
    {
        using namespace GSGL;
        if (!gl.CopyImageSubData)
            return;
        GLuint textures[2] = {0u, 0u};
        gl.GenTextures(2, textures);
        for (int i = 0; i < 2; ++i)
        {
            gl.BindTexture(kTexture2D, textures[i]);
            gl.TexImage2D(kTexture2D, 0, kRgba8, 1, 1, 0, kRgba, kUnsignedByte, i == 0 ? kProbe : nullptr);
            gl.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
            gl.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
        }
        gl.BindTexture(kTexture2D, 0u);
        gl.CopyImageSubData(textures[0], kTexture2D, 0, 0, 0, 0, textures[1], kTexture2D, 0, 0, 0, 0, 1, 1, 1);
        gl.DeleteTextures(1, &textures[0]);
        gl.Finish();
        s_probeTexture = textures[1];
    }

    void disableShared(const char *why)
    {
        std::fprintf(stderr, "[gs-present] shared textures do not work here (%s); RAM presentation\n", why);
        std::fflush(stderr);
        s_sharedActive.store(false, std::memory_order_release);
    }

    // host thread, s_sharedMutex held: false when the probe read back wrong (sharing then disabled)
    bool checkProbe()
    {
        using namespace GSGL;
        if (s_probeChecked)
            return true;
        s_probeChecked = true;
        const Api &h = s_hostProbeApi;
        if (!s_probeTexture || !h.GenFramebuffers || !h.ReadPixels)
        {
            std::fprintf(stderr, "[gs-present] shared probe skipped\n");
            return true;
        }
        GLint previous = 0;
        h.GetIntegerv(kFramebufferBinding, &previous);
        GLuint fbo = 0u;
        h.GenFramebuffers(1, &fbo);
        h.BindFramebuffer(kFramebuffer, fbo);
        h.FramebufferTexture2D(kFramebuffer, 0x8CE0 /* COLOR_ATTACHMENT0 */, kTexture2D, s_probeTexture, 0);
        uint8_t pixel[4] = {0u, 0u, 0u, 0u};
        h.ReadPixels(0, 0, 1, 1, kRgba, kUnsignedByte, pixel);
        h.BindFramebuffer(kFramebuffer, static_cast<GLuint>(previous));
        h.DeleteFramebuffers(1, &fbo);
        const bool ok = std::memcmp(pixel, kProbe, 4) == 0;
        std::fprintf(stderr, "[gs-present] shared probe %s (read %02x%02x%02x%02x)\n", ok ? "ok" : "FAILED", pixel[0], pixel[1],
                     pixel[2], pixel[3]);
        if (!ok)
            disableShared("probe texture read back wrong");
        return ok;
    }

    void loadHostProbeApi()
    {
        std::string missing;
        GSGL::Api api;
        load(api.GetIntegerv, "glGetIntegerv", missing);
        load(api.GenFramebuffers, "glGenFramebuffers", missing);
        load(api.DeleteFramebuffers, "glDeleteFramebuffers", missing);
        load(api.BindFramebuffer, "glBindFramebuffer", missing);
        load(api.FramebufferTexture2D, "glFramebufferTexture2D", missing);
        load(api.ReadPixels, "glReadPixels", missing);
        s_hostProbeApi = missing.empty() ? api : GSGL::Api{};
    }

    void releaseReading()
    {
        if (s_readingSlot < 0)
            return;
        SharedSlot &slot = s_sharedSlots[s_readingSlot];
        slot.consumed = s_hostApi.FenceSync(GSGL::kSyncGpuCommandsComplete, 0u);
        s_hostApi.Flush();
        slot.reading = false;
        s_readingSlot = -1;
    }
}

namespace GSSharedPresent
{
    bool Active()
    {
        return s_sharedActive.load(std::memory_order_acquire);
    }

    uint64_t LatestSequence()
    {
        return s_publishedSequence.load(std::memory_order_acquire);
    }

    bool Acquire(Frame &frame)
    {
        if (!Active())
            return false;
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        if (!checkProbe())
            return false;
        int newest = -1;
        uint64_t sequence = s_readingSlot >= 0 ? s_sharedSlots[s_readingSlot].frame.sequence : 0u;
        for (int i = 0; i < static_cast<int>(s_sharedSlots.size()); ++i)
        {
            const SharedSlot &slot = s_sharedSlots[i];
            if (!slot.reading && slot.frame.sequence > sequence && slot.produced && completed(s_hostApi, slot.produced))
            {
                newest = i;
                sequence = slot.frame.sequence;
            }
        }
        if (newest >= 0)
        {
            releaseReading();
            SharedSlot &slot = s_sharedSlots[newest];
            s_hostApi.DeleteSync(slot.produced);
            slot.produced = nullptr;
            if (slot.frame.hashValid)
                slot.frame.contentHash = static_cast<uint64_t>(slot.hash[0]) | (static_cast<uint64_t>(slot.hash[1]) << 32u);
            slot.reading = true;
            s_readingSlot = newest;
            s_everAcquired = true;
        }
        else if (!s_everAcquired && s_sharedSequence != 0u &&
                 std::chrono::steady_clock::now() - s_firstPublish > std::chrono::seconds(3))
        {
            disableShared("no published frame completed for the host in 3 s");
            return false;
        }
        if (s_readingSlot < 0)
            return false;
        frame = s_sharedSlots[s_readingSlot].frame;
        return true;
    }

    void ShutdownHost()
    {
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        releaseReading();
        s_sharedActive.store(false, std::memory_order_release);
        forgetHostContext();
    }
}

namespace GSGL
{
    bool SharedAvailable(const Api &gl)
    {
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        for (const SharedSlot &slot : s_sharedSlots)
            if (!slot.reading && completed(gl, slot.produced) && completed(gl, slot.consumed))
                return true;
        return false;
    }

    bool PublishShared(const Api &gl, GLuint buffer, uint32_t width, uint32_t height, uint64_t renderSequence,
                       uint32_t displayFbp, uint32_t sourceFbp, bool usedPreferred, bool blank,
                       uint64_t vsyncTick, GLuint hashProgram, GLint hashLocation)
    {
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        if (!s_sharedActive.load(std::memory_order_relaxed))
            return false;
        SharedSlot *available = nullptr;
        for (SharedSlot &slot : s_sharedSlots)
        {
            if (slot.reading || !completed(gl, slot.produced) || !completed(gl, slot.consumed))
                continue;
            if (!available || slot.frame.sequence < available->frame.sequence)
                available = &slot;
        }
        if (!available)
            return false;
        SharedSlot &slot = *available;
        if (slot.produced)
            gl.DeleteSync(slot.produced);
        if (slot.consumed)
            gl.DeleteSync(slot.consumed);
        slot.produced = slot.consumed = nullptr;
        slot.frame.hashValid = false;
        if (hashProgram)
        {
            if (!slot.hashBuffer)
            {
                gl.GenBuffers(1, &slot.hashBuffer);
                gl.BindBuffer(kShaderStorageBuffer, slot.hashBuffer);
                const GLbitfield flags = kMapReadBit | kMapPersistentBit | kMapCoherentBit;
                gl.BufferStorage(kShaderStorageBuffer, 8, nullptr, flags);
                slot.hash = static_cast<const uint32_t *>(gl.MapBufferRange(kShaderStorageBuffer, 0, 8, flags));
            }
            if (slot.hash)
            {
                static const bool deviceHash = [] {
                    const char *setting = std::getenv("PS2X_FRAME_HASH_DEVICE");
                    return !setting || std::string_view(setting) != "0";
                }();
                if (deviceHash && !slot.hashDeviceBuffer)
                {
                    gl.GenBuffers(1, &slot.hashDeviceBuffer);
                    gl.BindBuffer(kShaderStorageBuffer, slot.hashDeviceBuffer);
                    gl.BufferData(kShaderStorageBuffer, 8, nullptr, kDynamicDraw);
                }
                const GLuint output = deviceHash ? slot.hashDeviceBuffer : slot.hashBuffer;
                const uint32_t zero = 0u;
                gl.BindBuffer(kShaderStorageBuffer, output);
                gl.ClearBufferData(kShaderStorageBuffer, kR32UI, kRedInteger, kUnsignedInt, &zero);
                gl.BindBufferBase(kShaderStorageBuffer, 7u, buffer);
                gl.BindBufferBase(kShaderStorageBuffer, 10u, output);
                gl.MemBarrier(kAllBarrierBits);
                gl.UseProgram(hashProgram);
                gl.Uniform1ui(hashLocation, width * height);
                gl.DispatchCompute((width * height + 255u) / 256u, 1u, 1u);
                gl.MemBarrier(kAllBarrierBits);
                if (deviceHash)
                {
                    gl.BindBuffer(kCopyReadBuffer, output);
                    gl.BindBuffer(kCopyWriteBuffer, slot.hashBuffer);
                    gl.CopyBufferSubData(kCopyReadBuffer, kCopyWriteBuffer, 0, 0, 8);
                    gl.MemBarrier(kAllBarrierBits);
                }
                slot.frame.hashValid = true;
            }
        }
        gl.BindBuffer(kPixelUnpackBuffer, 0u);
        if (!slot.frame.texture)
            gl.GenTextures(1, &slot.frame.texture);
        gl.BindTexture(kTexture2D, slot.frame.texture);
        if (slot.frame.width != width || slot.frame.height != height)
        {
            gl.TexImage2D(kTexture2D, 0, kRgba8, width, height, 0, kRgba, kUnsignedByte, nullptr);
            // GOW-Port: igualar el filtrado por defecto de la textura CPU de raylib.
            gl.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
            gl.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
            gl.TexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
            gl.TexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
        }
        gl.MemBarrier(kAllBarrierBits);
        gl.BindBuffer(kPixelUnpackBuffer, buffer);
        gl.TexSubImage2D(kTexture2D, 0, 0, 0, width, height, kRgba, kUnsignedByte, nullptr);
        gl.MemBarrier(kAllBarrierBits);
        gl.BindBuffer(kPixelUnpackBuffer, 0u);
        gl.BindTexture(kTexture2D, 0u);
        slot.frame.width = width;
        slot.frame.height = height;
        if (s_sharedSequence == 0u)
            s_firstPublish = std::chrono::steady_clock::now();
        slot.frame.sequence = ++s_sharedSequence;
        s_publishedSequence.store(s_sharedSequence, std::memory_order_release);
        slot.frame.renderSequence = renderSequence;
        slot.frame.displayFbp = displayFbp;
        slot.frame.sourceFbp = sourceFbp;
        slot.frame.usedPreferred = usedPreferred;
        slot.frame.blank = blank;
        slot.frame.vsyncTick = vsyncTick;
        slot.produced = gl.FenceSync(kSyncGpuCommandsComplete, 0u);
        gl.Flush();
        return true;
    }

    bool PublishSharedTexture(const Api &gl, GLuint source, uint32_t width, uint32_t height, uint64_t renderSequence,
                              uint32_t displayFbp, uint32_t sourceFbp, uint64_t vsyncTick)
    {
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        if (!s_sharedActive.load(std::memory_order_relaxed) || !gl.CopyImageSubData)
            return false;
        SharedSlot *available = nullptr;
        for (SharedSlot &slot : s_sharedSlots)
        {
            if (slot.reading || !completed(gl, slot.produced) || !completed(gl, slot.consumed))
                continue;
            if (!available || slot.frame.sequence < available->frame.sequence)
                available = &slot;
        }
        if (!available)
            return false;
        SharedSlot &slot = *available;
        if (slot.produced)
            gl.DeleteSync(slot.produced);
        if (slot.consumed)
            gl.DeleteSync(slot.consumed);
        slot.produced = slot.consumed = nullptr;
        slot.frame.hashValid = false;
        const bool blank = source == 0u || width == 0u || height == 0u;
        if (!blank)
        {
            if (!slot.frame.texture)
                gl.GenTextures(1, &slot.frame.texture);
            if (slot.frame.width != width || slot.frame.height != height)
            {
                gl.BindTexture(kTexture2D, slot.frame.texture);
                gl.TexImage2D(kTexture2D, 0, kRgba8, width, height, 0, kRgba, kUnsignedByte, nullptr);
                gl.TexParameteri(kTexture2D, kTextureMinFilter, kLinear);
                gl.TexParameteri(kTexture2D, kTextureMagFilter, kLinear);
                gl.TexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
                gl.TexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
                gl.BindTexture(kTexture2D, 0u);
                slot.frame.width = width;
                slot.frame.height = height;
            }
            gl.CopyImageSubData(source, kTexture2D, 0, 0, 0, 0, slot.frame.texture, kTexture2D, 0, 0, 0, 0,
                                static_cast<GLsizei>(width), static_cast<GLsizei>(height), 1);
        }
        if (s_sharedSequence == 0u)
            s_firstPublish = std::chrono::steady_clock::now();
        slot.frame.sequence = ++s_sharedSequence;
        s_publishedSequence.store(s_sharedSequence, std::memory_order_release);
        slot.frame.renderSequence = renderSequence;
        slot.frame.displayFbp = displayFbp;
        slot.frame.sourceFbp = sourceFbp;
        slot.frame.usedPreferred = false;
        slot.frame.blank = blank;
        slot.frame.vsyncTick = vsyncTick;
        slot.produced = gl.FenceSync(kSyncGpuCommandsComplete, 0u);
        gl.Flush();
        return true;
    }

    void DestroyShared(const Api &gl)
    {
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        s_sharedActive.store(false, std::memory_order_release);
        for (SharedSlot &slot : s_sharedSlots)
        {
            if (slot.produced)
                gl.DeleteSync(slot.produced);
            if (slot.consumed)
                gl.DeleteSync(slot.consumed);
            if (slot.frame.texture)
                gl.DeleteTextures(1, &slot.frame.texture);
            if (slot.hashBuffer)
                gl.DeleteBuffers(1, &slot.hashBuffer);
            if (slot.hashDeviceBuffer)
                gl.DeleteBuffers(1, &slot.hashDeviceBuffer);
            slot = {};
        }
        s_readingSlot = -1;
    }
}
#endif

#if defined(_WIN32)

namespace GSSharedPresent
{
    void CaptureHostContext()
    {
        // GOW-Port: on by default; PS2X_GS_DIRECT_PRESENT=0 keeps the RAM (readback + upload) presentation.
        const char *value = std::getenv("PS2X_GS_DIRECT_PRESENT");
        if (value && *value == '0')
            return;
        const HDC dc = wglGetCurrentDC();
        const HGLRC context = wglGetCurrentContext();
        if (!dc || !context)
            return;
        std::string missing;
        GSGL::Api api;
        load(api.FenceSync, "glFenceSync", missing);
        load(api.ClientWaitSync, "glClientWaitSync", missing);
        load(api.DeleteSync, "glDeleteSync", missing);
        load(api.Flush, "glFlush", missing);
        if (!missing.empty())
        {
            std::fprintf(stderr, "[gs-present] shared presentation unavailable: %s\n", missing.c_str());
            return;
        }
        loadHostProbeApi();
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        s_hostFormatIndex = GetPixelFormat(dc);
        if (!DescribePixelFormat(dc, s_hostFormatIndex, sizeof(s_hostFormat), &s_hostFormat))
            return;
        s_hostApi = api;
        s_hostContext = context;
        s_hostDc = dc;
        wglMakeCurrent(nullptr, nullptr);
    }

    void RestoreHostContext()
    {
        if (s_hostDc && s_hostContext)
            wglMakeCurrent(s_hostDc, s_hostContext);
    }
}

namespace GSGL
{
    Context::~Context()
    {
        Destroy();
    }

    bool Context::Create(std::string &error)
    {
        Destroy();
        WNDCLASSA windowClass{};
        windowClass.style = CS_OWNDC;
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = GetModuleHandleA(nullptr);
        windowClass.lpszClassName = "PS2XGSGpuContext";
        RegisterClassA(&windowClass);
        HWND window = CreateWindowExA(0, windowClass.lpszClassName, "gs", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr,
                                      windowClass.hInstance, nullptr);
        if (!window)
        {
            error = "CreateWindowEx failed";
            return false;
        }
        HDC dc = GetDC(window);
        PIXELFORMATDESCRIPTOR descriptor{};
        descriptor.nSize = sizeof(descriptor);
        descriptor.nVersion = 1;
        descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        descriptor.iPixelType = PFD_TYPE_RGBA;
        descriptor.cColorBits = 32;
        const int format = s_hostContext ? s_hostFormatIndex : ChoosePixelFormat(dc, &descriptor);
        if (s_hostContext)
            descriptor = s_hostFormat;
        if (format == 0 || !SetPixelFormat(dc, format, &descriptor))
        {
            ReleaseDC(window, dc);
            DestroyWindow(window);
            error = "no pixel format";
            return false;
        }
        HGLRC legacy = wglCreateContext(dc);
        if (!legacy || !wglMakeCurrent(dc, legacy))
        {
            if (legacy)
                wglDeleteContext(legacy);
            ReleaseDC(window, dc);
            DestroyWindow(window);
            error = "wglCreateContext failed";
            return false;
        }
        const auto createAttribs = reinterpret_cast<CreateContextAttribs>(wglGetProcAddress("wglCreateContextAttribsARB"));
        HGLRC context = nullptr;
        if (createAttribs)
        {
            const int attributes[] = {kContextMajorVersion, 4, kContextMinorVersion, 6, kContextProfileMask, kContextCoreProfileBit,
                                      kContextFlags, 0, 0};
            context = createAttribs(dc, s_hostContext, attributes);
            m_shared = context && s_hostContext;
            if (!context && s_hostContext)
            {
                std::fprintf(stderr, "[gs-present] WGL sharing failed (%lu); using RAM presentation\n", GetLastError());
                context = createAttribs(dc, nullptr, attributes);
            }
        }
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(legacy);
        if (!context || !wglMakeCurrent(dc, context))
        {
            if (context)
                wglDeleteContext(context);
            ReleaseDC(window, dc);
            DestroyWindow(window);
            error = "OpenGL 4.6 core context unavailable";
            return false;
        }
        m_window = window;
        m_dc = dc;
        m_context = context;

        std::string missing;
        loadApi(m_api, missing);
        if (!missing.empty())
        {
            error = "missing GL functions: " + missing;
            Destroy();
            return false;
        }
        if (m_shared)
        {
            createProbe(m_api);
            s_sharedActive.store(true, std::memory_order_release);
            std::fprintf(stderr, "[gs-present] shared GPU textures active\n");
        }
        return true;
    }

    void *Context::CreateWorker() const
    {
        const auto createAttribs = reinterpret_cast<CreateContextAttribs>(wglGetProcAddress("wglCreateContextAttribsARB"));
        if (!createAttribs || !m_context || !m_dc)
            return nullptr;
        const int attributes[] = {kContextMajorVersion, 4, kContextMinorVersion, 6, kContextProfileMask, kContextCoreProfileBit,
                                  kContextFlags, 0, 0};
        return createAttribs(static_cast<HDC>(m_dc), static_cast<HGLRC>(m_context), attributes);
    }

    bool Context::MakeWorkerCurrent(void *worker) const
    {
        return worker && m_dc && wglMakeCurrent(static_cast<HDC>(m_dc), static_cast<HGLRC>(worker));
    }

    void Context::DestroyWorker(void *worker)
    {
        if (!worker)
            return;
        if (wglGetCurrentContext() == static_cast<HGLRC>(worker))
            wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(static_cast<HGLRC>(worker));
    }

    void Context::Destroy()
    {
        if (m_context)
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(static_cast<HGLRC>(m_context));
            m_context = nullptr;
        }
        if (m_dc && m_window)
            ReleaseDC(static_cast<HWND>(m_window), static_cast<HDC>(m_dc));
        m_dc = nullptr;
        if (m_window)
            DestroyWindow(static_cast<HWND>(m_window));
        m_window = nullptr;
        m_api = {};
        m_shared = false;
    }
}
#elif defined(__SWITCH__)
// GOW-Port: Switch (switch-mesa): the GS renderer's own GL core context through EGL, surfaceless when the driver allows
// it (the renderer only draws into its own framebuffers), else on a small pbuffer. It shares objects with the host
// window's context (captured by CaptureHostContext) for the shared-texture presentation above; without sharing, frames
// go through the RAM path (readback + upload to raylib's texture). m_dc = EGLDisplay, m_window = EGLSurface (or none),
// m_context = EGLContext; the config is s_eglConfig.
namespace
{
    EGLConfig s_eglConfig = nullptr;
    EGLDisplay s_eglDisplay = EGL_NO_DISPLAY;
    bool s_surfaceless = false;

    EGLContext createCore(EGLDisplay display, EGLContext share, int &major, int &minor)
    {
        static const int versions[][2] = {{4, 6}, {4, 5}, {4, 3}};
        for (const auto &version : versions)
        {
            const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, version[0], EGL_CONTEXT_MINOR_VERSION, version[1],
                                         EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
            const EGLContext context = eglCreateContext(display, s_eglConfig, share, attributes);
            if (context != EGL_NO_CONTEXT)
            {
                major = version[0];
                minor = version[1];
                return context;
            }
        }
        return EGL_NO_CONTEXT;
    }
}

namespace GSSharedPresent
{
    void CaptureHostContext()
    {
        // GOW-Port: on by default; PS2X_GS_DIRECT_PRESENT=0 keeps the RAM (readback + upload) presentation. EGL lets
        // a context share with one that is current on another thread, so the host context stays current here.
        const char *value = std::getenv("PS2X_GS_DIRECT_PRESENT");
        if (value && *value == '0')
            return;
        const EGLDisplay display = eglGetCurrentDisplay();
        const EGLContext context = eglGetCurrentContext();
        if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT)
        {
            std::fprintf(stderr, "[gs-present] no current EGL context on the host thread; RAM presentation\n");
            return;
        }
        std::string missing;
        GSGL::Api api;
        load(api.FenceSync, "glFenceSync", missing);
        load(api.ClientWaitSync, "glClientWaitSync", missing);
        load(api.DeleteSync, "glDeleteSync", missing);
        load(api.Flush, "glFlush", missing);
        if (!missing.empty())
        {
            std::fprintf(stderr, "[gs-present] shared presentation unavailable: %s\n", missing.c_str());
            return;
        }
        loadHostProbeApi();
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        s_hostApi = api;
        s_hostEglDisplay = display;
        s_hostEglContext = context;
    }

    void RestoreHostContext() {}
}

namespace GSGL
{
    Context::~Context()
    {
        Destroy();
    }

    bool Context::Create(std::string &error)
    {
        Destroy();
        // the host window's display when its context is shared (EGL shares only within one display)
        const EGLDisplay display = s_hostEglDisplay != EGL_NO_DISPLAY ? s_hostEglDisplay : eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr))
        {
            error = "eglInitialize failed";
            return false;
        }
        if (!eglBindAPI(EGL_OPENGL_API))
        {
            error = "eglBindAPI(OpenGL) failed";
            return false;
        }
        // no surface type: switch-mesa offers window configs only, and the context is surfaceless when it can be
        const EGLint configAttributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                           EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint count = 0;
        if (!eglChooseConfig(display, configAttributes, &s_eglConfig, 1, &count) || count == 0)
        {
            error = "no EGL config for OpenGL";
            return false;
        }
        int major = 0, minor = 0;
        EGLContext context = EGL_NO_CONTEXT;
        if (s_hostEglContext != EGL_NO_CONTEXT)
        {
            context = createCore(display, s_hostEglContext, major, minor);
            m_shared = context != EGL_NO_CONTEXT;
            if (!m_shared)
                std::fprintf(stderr, "[gs-present] EGL sharing failed (0x%x); using RAM presentation\n", eglGetError());
        }
        if (context == EGL_NO_CONTEXT)
            context = createCore(display, EGL_NO_CONTEXT, major, minor);
        if (context == EGL_NO_CONTEXT)
        {
            error = "OpenGL 4.3+ core context unavailable (EGL error " + std::to_string(eglGetError()) + ")";
            return false;
        }
        const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
        s_surfaceless = extensions && std::string_view(extensions).find("EGL_KHR_surfaceless_context") != std::string_view::npos;
        EGLSurface surface = EGL_NO_SURFACE;
        if (!s_surfaceless)
        {
            const EGLint pbufferAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
            surface = eglCreatePbufferSurface(display, s_eglConfig, pbufferAttributes);
        }
        if (!eglMakeCurrent(display, surface, surface, context))
        {
            if (surface != EGL_NO_SURFACE)
                eglDestroySurface(display, surface);
            eglDestroyContext(display, context);
            error = "eglMakeCurrent failed";
            return false;
        }
        s_eglDisplay = display;
        m_dc = display;
        m_window = surface;
        m_context = context;
        std::string missing;
        loadApi(m_api, missing);
        if (!missing.empty())
        {
            error = "missing GL functions: " + missing;
            Destroy();
            return false;
        }
        std::fprintf(stderr, "[gs-gl] EGL OpenGL %d.%d core context (%s)%s%s\n", major, minor,
                     m_api.GetString ? reinterpret_cast<const char *>(m_api.GetString(kRenderer)) : "?",
                     s_surfaceless ? ", surfaceless" : ", pbuffer", m_shared ? ", shared with the host window" : "");
        if (m_shared)
        {
            createProbe(m_api);
            s_sharedActive.store(true, std::memory_order_release);
            std::fprintf(stderr, "[gs-present] shared GPU textures active\n");
        }
        return true;
    }

    // Workers share the main context; without surfaceless contexts there is no surface to give them (they are optional)
    void *Context::CreateWorker() const
    {
        if (!m_context || !s_surfaceless)
            return nullptr;
        int major = 0, minor = 0;
        const EGLContext worker = createCore(static_cast<EGLDisplay>(m_dc), static_cast<EGLContext>(m_context), major, minor);
        return worker == EGL_NO_CONTEXT ? nullptr : worker;
    }

    bool Context::MakeWorkerCurrent(void *worker) const
    {
        return worker && m_dc && eglMakeCurrent(static_cast<EGLDisplay>(m_dc), EGL_NO_SURFACE, EGL_NO_SURFACE, static_cast<EGLContext>(worker));
    }

    void Context::DestroyWorker(void *worker)
    {
        if (!worker || s_eglDisplay == EGL_NO_DISPLAY)
            return;
        if (eglGetCurrentContext() == static_cast<EGLContext>(worker))
            eglMakeCurrent(s_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(s_eglDisplay, static_cast<EGLContext>(worker));
    }

    void Context::Destroy()
    {
        if (m_dc)
        {
            const EGLDisplay display = static_cast<EGLDisplay>(m_dc);
            if (m_context)
            {
                if (eglGetCurrentContext() == static_cast<EGLContext>(m_context))
                    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroyContext(display, static_cast<EGLContext>(m_context));
            }
            if (m_window)
                eglDestroySurface(display, static_cast<EGLSurface>(m_window));
        }
        m_context = m_window = m_dc = nullptr;
        m_api = {};
        m_shared = false;
    }
}
#else
namespace GSGL
{
    bool PublishShared(const Api &, GLuint, uint32_t, uint32_t, uint64_t,
                       uint32_t, uint32_t, bool, bool, uint64_t, GLuint, GLint) { return false; }
    bool SharedAvailable(const Api &) { return false; }
    bool PublishSharedTexture(const Api &, GLuint, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t, uint64_t) { return false; }
    void DestroyShared(const Api &) {}
    Context::~Context() = default;

    bool Context::Create(std::string &error)
    {
        error = "GPU GS backend is only implemented for Windows";
        return false;
    }

    void *Context::CreateWorker() const { return nullptr; }
    bool Context::MakeWorkerCurrent(void *) const { return false; }
    void Context::DestroyWorker(void *) {}

    void Context::Destroy()
    {
    }
}
namespace GSSharedPresent
{
    void CaptureHostContext() {}
    void RestoreHostContext() {}
    bool Active() { return false; }
    uint64_t LatestSequence() { return 0u; }
    bool Acquire(Frame &) { return false; }
    void ShutdownHost() {}
}
#endif
