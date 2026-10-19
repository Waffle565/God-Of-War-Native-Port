#pragma once

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <vector>

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    // GOW-Port: primitives that share one draw state, in order (the GS frontend collects the triangles of its strips).
    virtual void SubmitRun(const GSDrawState &state, const GSPrimitiveVerts *prims, uint32_t count)
    {
        GSPrimitiveBatch batch;
        batch.state = state;
        for (uint32_t i = 0; i < count; ++i)
        {
            static_cast<GSPrimitiveVerts &>(batch) = prims[i];
            Submit(batch);
        }
    }
    // GOW-Port: a triangle strip or fan as the GS received it: every vertex kick's vertex in order (the run starts with
    // the vertices the GS's window still held from before it), GSVertex::kick = 1 where the kick drew a triangle. The
    // triangles follow the GS's own rules: a strip draws the last three vertices, a fan the first and the last two.
    // Strip vertices are sent once instead of in three triangles each. `serial` is the state's GS::m_stateSerial.
    virtual void SubmitKicks(const GSDrawState &state, uint64_t serial, const GSVertex *verts, uint32_t count)
    {
        std::vector<GSPrimitiveVerts> prims;
        ExpandKicks(state.prim.type, serial, verts, count, prims);
        if (!prims.empty())
            SubmitRun(state, prims.data(), static_cast<uint32_t>(prims.size()));
    }
    static void ExpandKicks(GSPrimType type, uint64_t serial, const GSVertex *verts, uint32_t count, std::vector<GSPrimitiveVerts> &out)
    {
        const bool fan = type == GS_PRIM_TRIFAN;
        uint32_t window[2] = {}, held = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (held < 2u)
            {
                window[held++] = i;
                continue;
            }
            if (verts[i].kick)
            {
                GSPrimitiveVerts p;
                p.vertexCount = 3;
                p.vertices[0] = verts[window[0]];
                p.vertices[1] = verts[window[1]];
                p.vertices[2] = verts[i];
                for (GSVertex &v : p.vertices)
                    v.kick = 0;
                p.stateSerial = serial;
                out.push_back(p);
            }
            if (!fan)
                window[0] = window[1];
            window[1] = i;
        }
    }
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    // GOW-Port: capacidad efectiva por instancia; consultar no necesita un contexto GL.
    virtual bool UsesSharedPresentation() const { return false; }
    // GOW-Port: Present only queues the frame behind the earlier commands (shared textures, or the threaded backend's
    // asynchronous RAM frames), so the frontend adds no Flush/Sync barrier in front of it.
    virtual bool QueuesPresentation() const { return UsesSharedPresentation(); }

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;
};
