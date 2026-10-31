// GOW-Port: Metal GS renderer (macOS). The same render-target renderer as GSHwBackend (gs_hw_backend.cpp): every draw is
// rasterised by the GPU into scaled RGBA render targets (one per frame buffer base, PS2X_GS_HW_SCALE), paletted textures are
// index + palette, the game's reinterpretation passes (channel shuffles, Z in colour memory, CRTC merge) are done natively.
// The logic is a function-by-function port of GSHwBackend::Impl; only the GPU layer differs (Metal instead of OpenGL 4.3,
// which macOS does not have). Reads of the destination (DATE, GS blends fixed-function blending cannot express, Z in the
// target's colour) use Apple GPU framebuffer fetch instead of glTextureBarrier. Built only on APPLE (gs_metal_backend.mm).
#pragma once

#include "runtime/gs/gs_backend.h"

#include <memory>

class GSMetalBackend final : public GSRasterBackend
{
public:
    GSMetalBackend();
    ~GSMetalBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    // RAM presentation (stage 1): the composed frame is read back and returned as pixels for the host window.
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
