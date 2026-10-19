// GOW-Port: adaptado de Taylor N. Albarnaz / LightVelox (GPL-3.0).
// Origen: PS2Recomp sotc-port, ac9efa070638ad3b3accd284de6f898d5ab271d1.
#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_backend_state.h"

#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class GSThreadedBackend final : public GSRasterBackend, public GSBackendStateAccess
{
public:
    explicit GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner);
    ~GSThreadedBackend() override;

    static std::unique_ptr<GSRasterBackend> MakeDefault();
    static void SetEnabledByDefault(bool enabled);

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void SubmitRun(const GSDrawState &state, const GSPrimitiveVerts *prims, uint32_t count) override;
    void SubmitKicks(const GSDrawState &state, uint64_t serial, const GSVertex *verts, uint32_t count) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;

    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool UsesSharedPresentation() const override;
    bool QueuesPresentation() const override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    bool ExportState(GSBackendState &out) override;
    bool ImportState(const GSBackendState &state) override;

    GSRasterBackend &Inner() { return *m_inner; }

private:
    enum class Op : uint8_t
    {
        Mxcsr,
        Submit,
        LoadClut,
        BeginTransfer,
        Upload,
        TextureFlush,
        WriteVram,
        Sync,
        Present,
        DrawState,
        Vertices,
        VerticesRun, // GSPrimitiveVerts[n] drawn with the current DrawState
        Call,
        Pad, // filler bytes in front of a payload that is read in place (AppendLocked's align)
        Kicks, // GSVertex[n] of a strip or fan (SubmitKicks) drawn with the current DrawState
    };

    void Enqueue(Op op, const void *payload, uint32_t size);
    void AppendLocked(Op op, const void *payload, uint32_t size, uint32_t align = 1u);
    void AppendDrawStateLocked(const GSDrawState &state, uint64_t serial);
    void AppendKicksLocked(uint64_t serial, const GSVertex *verts, uint32_t count);
    void PublishLocked();
    void Drain() const;
    void RunOnWorker(const std::function<void()> &function) const;
    void WorkerLoop();
    void Execute(const std::vector<uint8_t> &chunk, GSPrimitiveBatch &draw);

    std::unique_ptr<GSRasterBackend> m_inner;
    // GOW-Port: publicado por el worker tras Initialize, sin consultar GL desde el host.
    std::atomic<bool> m_sharedPresentation{false};
    // GOW-Port: RAM frames without a barrier (GOW_GS_ASYNC_READBACK, default on): Present queues the frame and returns
    // the newest one the GL thread has finished reading back (m_lastFrame), one or two presents old.
    bool m_asyncReadback = true;
    std::mutex m_frameMutex;
    PresentationFrame m_lastFrame;

    mutable std::mutex m_producerMutex;
    std::vector<uint8_t> m_open;
    uint32_t m_lastMxcsr = UINT32_MAX;
    GSDrawState m_lastDrawState{};
    bool m_drawStateValid = false;
    uint64_t m_lastDrawSerial = 0; // GSPrimitiveBatch::stateSerial of m_lastDrawState
    bool m_compactQueue = true;
    size_t m_queueChunkLimit = 8u;

    mutable std::mutex m_queueMutex;
    mutable std::condition_variable m_workCv;
    mutable std::condition_variable m_doneCv;
    std::deque<std::vector<uint8_t>> m_pending;
    std::vector<std::vector<uint8_t>> m_spare;
    uint64_t m_published = 0u;
    uint64_t m_completed = 0u;
    bool m_stop = false;

    mutable std::mutex m_execMutex;
    std::thread m_worker;
};
