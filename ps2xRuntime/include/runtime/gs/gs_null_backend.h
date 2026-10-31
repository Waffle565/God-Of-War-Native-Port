#pragma once

#include "runtime/gs/gs_backend.h"

// GOW-Port: GS renderer that accepts every call and draws nothing. GS::setRasterBackend(nullptr) installs it at host
// shutdown (it releases the renderer and its GL context and ignores the GS packets the EE may still send while the
// process exits), and GSThreadedBackend::MakeDefault() picks it for headless runs (PS2X_GS_NULL; default on macOS,
// whose OpenGL 4.1 cannot run the GL 4.3 renderer). Present returns an empty frame, VRAM reads return zero.
class GSNullBackend final : public GSRasterBackend
{
public:
    void Initialize(uint8_t *, uint32_t) override {}
    void Reset() override {}
    void Submit(const GSPrimitiveBatch &) override {}
    void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
    void BeginTransfer(const GSTransferCommand &) override {}
    void UploadImage(const uint8_t *, uint32_t) override {}
    void Flush() override {}
    void TextureFlush() override {}
    void Sync(GSSyncReason) override {}
    PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
    bool ClearFramebuffer(const GSContext &, uint32_t) override { return false; }
    uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override { return 0u; }
    uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0u; }
    void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void SnapshotVram(std::vector<uint8_t> &out) const override { out.clear(); }
    GSTransferSnapshot GetTransferSnapshot() const override { return {}; }
};
