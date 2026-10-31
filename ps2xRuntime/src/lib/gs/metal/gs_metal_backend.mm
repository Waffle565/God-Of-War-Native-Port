// GOW-Port: GSRasterBackend entry points of GSMetalBackend (same behaviour as the GSHwBackend ones in gs_hw_backend.cpp).
// They run on the GS worker thread, a plain std::thread without an autorelease pool: every entry drains its own pool, or
// the autoreleased Metal objects (command buffers, encoders, pass descriptors) would pile up until the thread exits.
#include "gs_metal_impl.h"

using namespace gsmtl;

GSMetalBackend::GSMetalBackend() : m_impl(std::make_unique<Impl>()) {}

GSMetalBackend::~GSMetalBackend()
{
    @autoreleasepool
    {
        if (m_impl->initialized)
            m_impl->commit(true);
        m_impl.reset();
    }
}

void GSMetalBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    @autoreleasepool
    {
        m_impl->vram = vram;
        m_impl->vramSize = vramSize;
        m_impl->store.Initialize(vram, vramSize);
        m_impl->ensure();
    }
}

void GSMetalBackend::Reset()
{
    @autoreleasepool
    {
        Impl &d = *m_impl;
        d.flushPending();
        d.store.Reset();
        d.invertedZ.clear();
        for (auto &kv : d.textures)
            kv.second.valid = false;
    }
}

void GSMetalBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (batch.vertexCount == 0u)
        return;
    @autoreleasepool
    {
        Impl &d = *m_impl;
        if (!d.ensure())
            return;
        d.submit(batch);
    }
}

void GSMetalBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    @autoreleasepool
    {
        Impl &d = *m_impl;
        d.flushPending();
        if (d.initialized)
            d.store.LoadClut(tex0, texclut, [&d](const GSTex0Reg &t, uint32_t width, const uint16_t *xy, uint32_t count, uint32_t *words)
                             { return d.clutFromTarget(t, width, xy, count, words); });
        else
            d.store.LoadClut(tex0, texclut);
        d.clutHash = d.store.ClutHash();
    }
}

void GSMetalBackend::BeginTransfer(const GSTransferCommand &command)
{
    @autoreleasepool
    {
        Impl &d = *m_impl;
        d.flushPending();
        d.store.BeginTransfer(command);
        if (command.direction == 0u || command.direction == 2u)
        {
            const uint32_t rows = static_cast<uint32_t>(command.trxpos.dsay) + command.trxreg.rrh;
            const uint32_t lo = command.bitbltbuf.dbp, hi = lo + spanBlocks(command.bitbltbuf.dpsm, command.bitbltbuf.dbw, rows);
            d.invalidateBlocks(lo, hi);
            if (d.uploadLog.size() > 4096u)
                d.uploadLog.erase(d.uploadLog.begin(), d.uploadLog.begin() + 2048);
            d.uploadLog.push_back({lo, hi, ++d.writeSeq});
        }
    }
}

void GSMetalBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    m_impl->store.Upload(data, sizeBytes);
}

void GSMetalBackend::Flush()
{
    @autoreleasepool
    {
        m_impl->flushPending();
    }
}

void GSMetalBackend::TextureFlush()
{
    @autoreleasepool
    {
        m_impl->flushPending(); // textures are re-read from the store when an upload overlaps them, not on TEXFLUSH
    }
}

void GSMetalBackend::Sync(GSSyncReason)
{
    @autoreleasepool
    {
        m_impl->flushPending();
    }
}

PresentationFrame GSMetalBackend::Present(const GSPresentationRequest &request)
{
    @autoreleasepool
    {
        return m_impl->present(request);
    }
}

bool GSMetalBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    @autoreleasepool
    {
        return m_impl->clearFramebuffer(context, rgba);
    }
}

// GOW-Port: the game never reads GS memory back (LOCAL->HOST), so there is nothing to hand out.
uint32_t GSMetalBackend::ConsumeLocalToHostBytes(uint8_t *, uint32_t)
{
    return 0u;
}

uint32_t GSMetalBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    return m_impl->store.Read(psm, base, bw, x, y);
}

void GSMetalBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    m_impl->store.Write(psm, base, bw, x, y, value);
}

void GSMetalBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    m_impl->store.Snapshot(out);
}

GSTransferSnapshot GSMetalBackend::GetTransferSnapshot() const
{
    return m_impl->store.TransferState();
}
