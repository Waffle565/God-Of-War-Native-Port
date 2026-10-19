// GOW-Port: adaptado de Taylor N. Albarnaz / LightVelox (GPL-3.0).
// Origen: PS2Recomp sotc-port, ac9efa070638ad3b3accd284de6f898d5ab271d1.
#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/gs/gs_hw_backend.h"
#include "runtime/gs/gs_shared_present.h"
#include "gs_inventory.h"
#include "ThreadPriority.h"
#include "ThreadNaming.h"
#include "runtime/ps2_perf.h"
#include "runtime/gs/gs_stage.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#if defined(USE_SSE2NEON)
#include "sse2neon.h" // GOW-Port: ARM (Switch)
#else
#include <xmmintrin.h>
#endif

namespace
{
    constexpr size_t kPublishBytes = 64u * 1024u;

    // GOW-Port: the MXCSR of the producing thread, sent ahead of the ops it queues. The GS stage thread runs no guest
    // or VU code and never changes it, so there it is read once (on ARM, sse2neon's _mm_getcsr is an FPCR read plus
    // fegetround, and it ran for every queued op); other producers (the EE or the worker without the stage) read it
    // every time.
    uint32_t producerCsr()
    {
        thread_local const bool constantCsr = ps2_gs_stage::onStageThread();
        thread_local const uint32_t firstCsr = _mm_getcsr();
        return constantCsr ? firstCsr : _mm_getcsr();
    }
    constexpr size_t kMaxPendingChunks = 256u;

    // GOW-Port: GL-thread time per queued op kind (GOW_PERF_DIAG=1), printed as [gs-gl] every 5 s: the GS frontend's
    // gsq_wait_ms says it waits on this thread, this line says on what (draws, CLUT readbacks, uploads, the present
    // readback, or the Switch driver lock). Only the GL thread touches it.
    struct GlOpTiming
    {
        static constexpr size_t kOps = 16u;
        uint64_t nanos[kOps] = {}, count[kOps] = {}, lockWait = 0u, busy = 0u, chunks = 0u;
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        void report()
        {
            static const char *const names[kOps] = {"mxcsr", "submit", "clut", "begintx", "upload", "texflush", "wvram", "sync",
                                                    "present", "state", "verts", "vrun", "call", "pad", "?", "?"};
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(5))
                return;
            char line[768];
            int at = std::snprintf(line, sizeof(line), "[gs-gl] window=%.2f busy_ms=%.1f lock_wait_ms=%.1f chunks=%llu",
                                   std::chrono::duration<double>(now - last).count(), busy * 1e-6, lockWait * 1e-6,
                                   static_cast<unsigned long long>(chunks));
            for (size_t i = 0; i < kOps && at > 0 && at < static_cast<int>(sizeof(line)); ++i)
                if (count[i])
                    at += std::snprintf(line + at, sizeof(line) - at, " %s=%.1fms/%llu", names[i], nanos[i] * 1e-6,
                                        static_cast<unsigned long long>(count[i]));
            std::fprintf(stderr, "%s\n", line);
            *this = GlOpTiming{};
        }
    };
    GlOpTiming s_glTiming;
    std::atomic<bool> s_enabledByDefault{false};

    bool discardDraws()
    {
        static const bool discard = []
        {
            const char *value = std::getenv("PS2X_GS_DISCARD_DRAWS");
            return value && *value && *value != '0';
        }();
        return discard;
    }

    struct LoadClutArgs
    {
        GSTex0Reg tex0;
        GSTexClutReg texclut;
    };

    struct WriteVramArgs
    {
        uint32_t psm;
        uint32_t base;
        uint32_t bw;
        uint32_t x;
        uint32_t y;
        uint32_t value;
    };

    template <typename T>
    T readAt(const uint8_t *data)
    {
        T value;
        std::memcpy(&value, data, sizeof(T));
        return value;
    }
}

GSThreadedBackend::GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner)
    : m_inner(std::move(inner))
{
    const char *compact = std::getenv("PS2X_GS_COMPACT_QUEUE");
    m_compactQueue = !compact || *compact != '0';
    const char *asyncReadback = std::getenv("GOW_GS_ASYNC_READBACK");
    m_asyncReadback = !asyncReadback || *asyncReadback != '0';
    if (const char *limit = std::getenv("PS2X_GS_QUEUE_CHUNKS"))
        m_queueChunkLimit = static_cast<size_t>(std::clamp(std::atoi(limit), 1, static_cast<int>(kMaxPendingChunks)));
    m_open.reserve(kPublishBytes + 4096u);
    m_worker = std::thread([this]
                           { WorkerLoop(); });
}

GSThreadedBackend::~GSThreadedBackend()
{
    // GOW-Port: completar incluso el último bloque antes de cerrar el contexto GL.
    Drain();
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stop = true;
        m_pending.clear();
    }
    m_workCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

std::unique_ptr<GSRasterBackend> GSThreadedBackend::MakeDefault()
{
    // GOW-Port: the native render-target renderer is the only GS renderer (no VRAM-emulating rasteriser).
    return std::make_unique<GSThreadedBackend>(std::make_unique<GSHwBackend>());
}

void GSThreadedBackend::SetEnabledByDefault(bool enabled)
{
    s_enabledByDefault.store(enabled);
}


// Appends one op to the open chunk (m_producerMutex held), preceded by the MXCSR when it changed. align > 1: the
// payload starts at a multiple of it within the chunk (a Pad op in front), so the GL thread can read it in place
// (chunk storage comes from operator new, aligned for any type).
void GSThreadedBackend::AppendLocked(Op op, const void *payload, uint32_t size, uint32_t align)
{
    const uint32_t csr = producerCsr();
    if (csr != m_lastMxcsr)
    {
        m_lastMxcsr = csr;
        const uint8_t header = static_cast<uint8_t>(Op::Mxcsr);
        const uint32_t csrSize = sizeof(csr);
        m_open.push_back(header);
        m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&csrSize), reinterpret_cast<const uint8_t *>(&csrSize) + 4);
        m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&csr), reinterpret_cast<const uint8_t *>(&csr) + 4);
    }
    uint8_t header[5];
    if (align > 1u && (m_open.size() + sizeof(header)) % align != 0u)
    {
        const uint32_t padBytes = static_cast<uint32_t>((align - (m_open.size() + 2u * sizeof(header)) % align) % align);
        header[0] = static_cast<uint8_t>(Op::Pad);
        std::memcpy(header + 1u, &padBytes, sizeof(padBytes));
        m_open.insert(m_open.end(), header, header + sizeof(header));
        m_open.insert(m_open.end(), padBytes, uint8_t{0});
    }
    header[0] = static_cast<uint8_t>(op);
    std::memcpy(header + 1u, &size, sizeof(size));
    m_open.insert(m_open.end(), header, header + sizeof(header));
    if (size)
        m_open.insert(m_open.end(), static_cast<const uint8_t *>(payload), static_cast<const uint8_t *>(payload) + size);
}

// Op::Kicks: [serial][GSVertex x count], the vertices 8-aligned in the chunk like AppendLocked's aligned payloads
void GSThreadedBackend::AppendKicksLocked(uint64_t serial, const GSVertex *verts, uint32_t count)
{
    const uint32_t bytes = count * static_cast<uint32_t>(sizeof(GSVertex));
    AppendLocked(Op::Kicks, nullptr, 0u, 8u); // header + alignment pad; the payload follows in place
    const size_t sizeAt = m_open.size() - 4u; // the op's length field
    const uint32_t length = 8u + bytes;
    std::memcpy(m_open.data() + sizeAt, &length, sizeof(length));
    m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&serial), reinterpret_cast<const uint8_t *>(&serial) + 8);
    m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(verts), reinterpret_cast<const uint8_t *>(verts) + bytes);
}

// A DrawState op only when the state differs from the last one sent (equal serial = equal state).
void GSThreadedBackend::AppendDrawStateLocked(const GSDrawState &state, uint64_t serial)
{
    const bool sameSerial = m_drawStateValid && serial != 0u && serial == m_lastDrawSerial;
    if (!sameSerial && (!m_drawStateValid || !(m_lastDrawState == state)))
    {
        std::memcpy(&m_lastDrawState, &state, sizeof(state));
        m_drawStateValid = true;
        AppendLocked(Op::DrawState, &state, sizeof(state));
    }
    m_lastDrawSerial = serial;
}

void GSThreadedBackend::Enqueue(Op op, const void *payload, uint32_t size)
{
    std::lock_guard<std::mutex> lock(m_producerMutex);
    if (op == Op::Submit && m_compactQueue)
    {
        const auto &draw = *static_cast<const GSPrimitiveBatch *>(payload);
        AppendDrawStateLocked(draw.state, draw.stateSerial);
        AppendLocked(Op::Vertices, &draw, sizeof(GSPrimitiveVerts));
    }
    else
        AppendLocked(op, payload, size);
    if (m_open.size() >= kPublishBytes)
        PublishLocked();
}

void GSThreadedBackend::PublishLocked()
{
    if (m_open.empty())
        return;
    std::vector<uint8_t> next;
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        ps2_perf::Scope perf(ps2_perf::Bucket::GsQueueWait);
        m_doneCv.wait(lock, [&]
                      { return m_stop || m_pending.size() < m_queueChunkLimit; });
        m_pending.push_back(std::move(m_open));
        ++m_published;
        if (!m_spare.empty())
        {
            next = std::move(m_spare.back());
            m_spare.pop_back();
        }
    }
    m_workCv.notify_one();
    next.clear();
    m_open = std::move(next);
    if (m_open.capacity() < kPublishBytes)
        m_open.reserve(kPublishBytes + 4096u);
}

void GSThreadedBackend::Drain() const
{
    auto *self = const_cast<GSThreadedBackend *>(this);
    uint64_t target = 0u;
    {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        self->PublishLocked();
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        target = m_published;
    }
    std::unique_lock<std::mutex> lock(m_queueMutex);
    ps2_perf::Scope perf(ps2_perf::Bucket::GsQueueWait);
    m_doneCv.wait(lock, [&]
                  { return m_completed >= target || m_stop; });
}

void GSThreadedBackend::WorkerLoop()
{
    ThreadNaming::SetCurrentThreadName("GSThread");
    ThreadPriority::RaiseCurrentThread(ThreadPriority::Level::AboveNormal);
    GSPrimitiveBatch draw;
    for (;;)
    {
        std::vector<uint8_t> chunk;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_workCv.wait(lock, [&]
                          { return m_stop || !m_pending.empty(); });
            if (m_stop)
            {
                m_inner.reset();
                return;
            }
            chunk = std::move(m_pending.front());
            m_pending.pop_front();
        }
        {
            static const bool timed = ps2_perf::countFrames();
            const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            std::lock_guard<std::mutex> exec(m_execMutex);
#if defined(__SWITCH__)
            std::lock_guard<std::mutex> driver(GSSharedPresent::DriverMutex());
#endif
            const auto t1 = timed ? std::chrono::steady_clock::now() : t0;
            Execute(chunk, draw);
            if (timed)
            {
                s_glTiming.lockWait += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                s_glTiming.busy += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t1).count();
                ++s_glTiming.chunks;
                s_glTiming.report();
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            ++m_completed;
            if (m_spare.size() < 64u)
                m_spare.push_back(std::move(chunk));
        }
        m_doneCv.notify_all();
    }
}

void GSThreadedBackend::Execute(const std::vector<uint8_t> &chunk, GSPrimitiveBatch &draw)
{
    const uint8_t *data = chunk.data();
    const size_t size = chunk.size();
    size_t offset = 0u;
    while (offset + 5u <= size)
    {
        const Op op = static_cast<Op>(data[offset]);
        const uint32_t length = readAt<uint32_t>(data + offset + 1u);
        const uint8_t *payload = data + offset + 5u;
        offset += 5u + length;
        static const bool timed = ps2_perf::countFrames();
        struct OpTimer
        {
            bool on;
            size_t index;
            std::chrono::steady_clock::time_point start;
            ~OpTimer()
            {
                if (!on)
                    return;
                s_glTiming.nanos[index] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                ++s_glTiming.count[index];
            }
        } opTimer{timed, std::min<size_t>(static_cast<size_t>(op), GlOpTiming::kOps - 1u),
                  timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
        switch (op)
        {
        case Op::Mxcsr:
            _mm_setcsr(readAt<uint32_t>(payload));
            break;
        case Op::Submit:
            if (!discardDraws())
                m_inner->Submit(readAt<GSPrimitiveBatch>(payload));
            break;
        case Op::LoadClut:
        {
            const LoadClutArgs args = readAt<LoadClutArgs>(payload);
            m_inner->LoadClut(args.tex0, args.texclut);
            break;
        }
        case Op::BeginTransfer:
            m_inner->BeginTransfer(readAt<GSTransferCommand>(payload));
            break;
        case Op::Upload:
            m_inner->UploadImage(payload, length);
            break;
        case Op::TextureFlush:
            m_inner->TextureFlush();
            break;
        case Op::WriteVram:
        {
            const WriteVramArgs a = readAt<WriteVramArgs>(payload);
            m_inner->WriteVram(a.psm, a.base, a.bw, a.x, a.y, a.value);
            break;
        }
        case Op::Sync:
            m_inner->Sync(readAt<GSSyncReason>(payload));
            break;
        case Op::DrawState:
            std::memcpy(&draw.state, payload, sizeof(draw.state));
            break;
        case Op::Vertices:
            std::memcpy(static_cast<GSPrimitiveVerts *>(&draw), payload, sizeof(GSPrimitiveVerts));
            if (!discardDraws())
                m_inner->Submit(draw);
            break;
        case Op::VerticesRun:
        {
            // one SubmitRun with the current state, read in place (SubmitRun aligns the payload; copied if it is not)
            const uint32_t count = length / static_cast<uint32_t>(sizeof(GSPrimitiveVerts));
            const GSPrimitiveVerts *run = reinterpret_cast<const GSPrimitiveVerts *>(payload);
            thread_local std::vector<GSPrimitiveVerts> copy;
            if (reinterpret_cast<uintptr_t>(payload) % alignof(GSPrimitiveVerts) != 0u)
            {
                copy.resize(count);
                std::memcpy(copy.data(), payload, count * sizeof(GSPrimitiveVerts));
                run = copy.data();
            }
            if (!discardDraws() && count)
            {
                m_inner->SubmitRun(draw.state, run, count);
                static_cast<GSPrimitiveVerts &>(draw) = run[count - 1u];
            }
            break;
        }
        case Op::Kicks:
        {
            // the state serial, then the vertices (read in place when aligned)
            const uint64_t serial = readAt<uint64_t>(payload);
            const uint32_t count = (length - 8u) / static_cast<uint32_t>(sizeof(GSVertex));
            const GSVertex *verts = reinterpret_cast<const GSVertex *>(payload + 8u);
            thread_local std::vector<GSVertex> copy;
            if (reinterpret_cast<uintptr_t>(verts) % alignof(GSVertex) != 0u)
            {
                copy.resize(count);
                std::memcpy(copy.data(), payload + 8u, count * sizeof(GSVertex));
                verts = copy.data();
            }
            if (!discardDraws() && count)
                m_inner->SubmitKicks(draw.state, serial, verts, count);
            break;
        }
        case Op::Pad:
            break;
        case Op::Present:
        {
            // shared mode returns no pixels; an asynchronous RAM frame lands here once its readback has completed
            PresentationFrame frame = m_inner->Present(readAt<GSPresentationRequest>(payload));
            if (frame)
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_lastFrame = std::move(frame);
            }
            break;
        }
        case Op::Call:
            (*readAt<const std::function<void()> *>(payload))();
            break;
        }
    }
}

void GSThreadedBackend::RunOnWorker(const std::function<void()> &function) const
{
    auto *self = const_cast<GSThreadedBackend *>(this);
    const std::function<void()> *pointer = &function;
    self->Enqueue(Op::Call, &pointer, sizeof(pointer));
    Drain();
}

// live: the host's sharing self-check can turn sharing off during the run (gs_gl.cpp checkProbe)
bool GSThreadedBackend::UsesSharedPresentation() const
{
    return m_sharedPresentation.load(std::memory_order_acquire) && GSSharedPresent::Active();
}

bool GSThreadedBackend::QueuesPresentation() const
{
    return m_asyncReadback || UsesSharedPresentation();
}

void GSThreadedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    RunOnWorker([&] {
        m_inner->Initialize(vram, vramSize);
        m_sharedPresentation.store(m_inner->UsesSharedPresentation(), std::memory_order_release);
    });
}

void GSThreadedBackend::Reset()
{
    RunOnWorker([&] { m_inner->Reset(); });
}

void GSThreadedBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (GsInventory::get().enabled())
        GsInventory::get().draw(batch);
    Enqueue(Op::Submit, &batch, sizeof(batch));
}

void GSThreadedBackend::SubmitRun(const GSDrawState &state, const GSPrimitiveVerts *prims, uint32_t count)
{
    if (!count)
        return;
    if (!m_compactQueue || GsInventory::get().enabled())
    {
        GSRasterBackend::SubmitRun(state, prims, count); // one Submit per primitive
        return;
    }
    std::lock_guard<std::mutex> lock(m_producerMutex);
    AppendDrawStateLocked(state, prims[0].stateSerial);
    AppendLocked(Op::VerticesRun, prims, static_cast<uint32_t>(count * sizeof(GSPrimitiveVerts)), alignof(GSPrimitiveVerts));
    if (m_open.size() >= kPublishBytes)
        PublishLocked();
}

void GSThreadedBackend::SubmitKicks(const GSDrawState &state, uint64_t serial, const GSVertex *verts, uint32_t count)
{
    if (!count)
        return;
    if (!m_compactQueue || GsInventory::get().enabled())
    {
        GSRasterBackend::SubmitKicks(state, serial, verts, count); // expanded into triangles
        return;
    }
    std::lock_guard<std::mutex> lock(m_producerMutex);
    AppendDrawStateLocked(state, serial);
    AppendKicksLocked(serial, verts, count);
    if (m_open.size() >= kPublishBytes)
        PublishLocked();
}

void GSThreadedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    if (GsInventory::get().enabled())
        GsInventory::get().clut(tex0);
    const LoadClutArgs args{tex0, texclut};
    Enqueue(Op::LoadClut, &args, sizeof(args));
}

void GSThreadedBackend::BeginTransfer(const GSTransferCommand &command)
{
    if (GsInventory::get().enabled())
        GsInventory::get().transfer(command);
    Enqueue(Op::BeginTransfer, &command, sizeof(command));
}

void GSThreadedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Enqueue(Op::Upload, data, data ? sizeBytes : 0u);
}

void GSThreadedBackend::Flush()
{
    // GOW-Port: barrera para lectura/presentación y llamadas del frontend existente.
    RunOnWorker([&] { m_inner->Flush(); });
}

void GSThreadedBackend::TextureFlush()
{
    Enqueue(Op::TextureFlush, nullptr, 0u);
}

// GOW-Port: GOW_GS_FINISH_ASINCRONO=1 (opcional, desactivado por defecto) encola FINISH en el hilo del GS
// sin esperar ni glFinish: el EE ve el bit FINISH enseguida. Las lecturas de VRAM desde el EE ya sincronizan
// por su cuenta (RunOnWorker) y el hilo del GS conserva el orden de los comandos.
static bool gowFinishAsync()
{
    static const bool on = [] { const char *v = std::getenv("GOW_GS_FINISH_ASINCRONO"); return v && v[0] == '1'; }();
    return on;
}

void GSThreadedBackend::Sync(GSSyncReason reason)
{
    if (reason == GSSyncReason::Finish && gowFinishAsync())
    {
        Enqueue(Op::Sync, &reason, sizeof(reason));
        return;
    }
    if (reason == GSSyncReason::Presentation && QueuesPresentation())
        return;
    // GOW-Port: FINISH solo se publica cuando terminaron los comandos anteriores.
    RunOnWorker([&] { m_inner->Sync(reason); });
}

PresentationFrame GSThreadedBackend::Present(const GSPresentationRequest &request)
{
    if (GsInventory::get().enabled())
        GsInventory::get().present();
    if (UsesSharedPresentation() && !request.forceReadback)
    {
        Enqueue(Op::Present, &request, sizeof(request));
        std::lock_guard<std::mutex> lock(m_producerMutex);
        PublishLocked();
        return {};
    }
    if (m_asyncReadback && !request.forceReadback)
    {
        GSPresentationRequest queued = request;
        queued.asyncReadback = true;
        Enqueue(Op::Present, &queued, sizeof(queued));
        {
            std::lock_guard<std::mutex> lock(m_producerMutex);
            PublishLocked();
        }
        std::lock_guard<std::mutex> lock(m_frameMutex);
        return m_lastFrame;
    }
    PresentationFrame frame;
    RunOnWorker([&] { frame = m_inner->Present(request); });
    return frame;
}

bool GSThreadedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    if (GsInventory::get().enabled())
        GsInventory::get().clear();
    bool result = false;
    RunOnWorker([&] { result = m_inner->ClearFramebuffer(context, rgba); });
    return result;
}

uint32_t GSThreadedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    if (GsInventory::get().enabled())
        GsInventory::get().consumeL2H();
    uint32_t result = 0u;
    RunOnWorker([&] { result = m_inner->ConsumeLocalToHostBytes(dst, maxBytes); });
    return result;
}

uint32_t GSThreadedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (GsInventory::get().enabled())
        GsInventory::get().cpuRead();
    uint32_t result = 0u;
    RunOnWorker([&] { result = m_inner->ReadVram(psm, base, bw, x, y); });
    return result;
}

void GSThreadedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (GsInventory::get().enabled())
        GsInventory::get().cpuWrite();
    const WriteVramArgs args{psm, base, bw, x, y, value};
    Enqueue(Op::WriteVram, &args, sizeof(args));
}

void GSThreadedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    if (GsInventory::get().enabled())
        GsInventory::get().snapshot();
    RunOnWorker([&] { m_inner->SnapshotVram(out); });
}

GSTransferSnapshot GSThreadedBackend::GetTransferSnapshot() const
{
    GSTransferSnapshot result;
    RunOnWorker([&] { result = m_inner->GetTransferSnapshot(); });
    return result;
}

bool GSThreadedBackend::ExportState(GSBackendState &out)
{
    bool result = false;
    RunOnWorker([&]
                {
        auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get());
        result = access && access->ExportState(out); });
    return result;
}

bool GSThreadedBackend::ImportState(const GSBackendState &state)
{
    bool result = false;
    RunOnWorker([&]
                {
        auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get());
        result = access && access->ImportState(state); });
    return result;
}
