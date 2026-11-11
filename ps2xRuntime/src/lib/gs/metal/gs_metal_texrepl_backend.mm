// GOW-Port: replacement texture packs in GSMetalBackend (draw side). The PCSX2 name of a texture is computed once per
// decode (direct textures) or per palette conversion (paletted ones), and again only when the draw's CLAMP region or mip
// registers change, since PCSX2 hashes those into the name as well.
#include "gs_metal_impl.h"

using namespace gsmtl;

id<MTLTexture> GSMetalBackend::Impl::replacementFor(const GSDrawState &s, Texture &entry, const Texture *index)
{
    const GSContext &c = s.context;
    const uint32_t mips = gsrepl::mipLevels(c);
    uint64_t key = gsrepl::regionFromClamp(c.tex0, c.clamp) * 1099511628211ull ^ mips;
    if (mips)
        key = ((key * 1099511628211ull ^ c.miptbp1) * 1099511628211ull ^ c.miptbp2) * 1099511628211ull ^ ((c.tex1 >> 9) & 1u);
    for (const Texture::Repl &r : entry.repl)
        if (r.key == key)
            return r.slot ? repl->use(r.slot) : nil;
    gsrepl::Slot *found = nullptr;
    gsrepl::Name name;
    // with mipmapping active PCSX2 hashes the whole chain, else the base level: a pack can hold either name
    bool ok = false;
    if (mips)
    {
        gsrepl::Name chain;
        if (gsrepl::nameFor(vram, s, store.Clut(), mips, replScratch, chain) && repl->find(chain, false))
            found = repl->find(chain), name = chain, ok = true;
    }
    if (!ok && gsrepl::nameFor(vram, s, store.Clut(), 0u, replScratch, name))
    {
        ok = true;
        found = repl->find(name);
    }
    // a region name covers only the CLAMP rectangle; the decoded texture here is the whole TW x TH
    if (found && (name.regionW != 0u || name.regionH != 0u))
        found = nullptr;
    if (ok && !found && repl->dumping() && !repl->dumped(name))
    {
        std::vector<uint32_t> pixels;
        if (index)
        {
            uint32_t colors[256];
            store.BuildPalette(s, colors);
            pixels.resize(index->indices.size());
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = colors[index->indices[i]];
        }
        else
            decodeDirect(s, pixels);
        const uint32_t w = entry.width, h = entry.height;
        if (pixels.size() == static_cast<size_t>(w) * h && w > 0u && h > 0u)
        {
            // a region name holds the CLAMP rectangle (PCSX2 DumpTexture), cropped from the whole texture when inside it
            const uint64_t region = gsrepl::regionFromClamp(c.tex0, c.clamp);
            const uint32_t x0 = (name.regionW != 0u) ? static_cast<uint16_t>(region) : 0u;
            const uint32_t y0 = (name.regionH != 0u) ? static_cast<uint16_t>(region >> 32) : 0u;
            const uint32_t rw = name.regionW ? name.regionW : w, rh = name.regionH ? name.regionH : h;
            if (x0 + rw <= w && y0 + rh <= h)
            {
                std::vector<uint32_t> crop(static_cast<size_t>(rw) * rh);
                for (uint32_t y = 0; y < rh; ++y)
                    std::memcpy(crop.data() + static_cast<size_t>(y) * rw, pixels.data() + static_cast<size_t>(y0 + y) * w + x0, rw * 4u);
                repl->dump(name, crop.data(), rw, rh);
            }
        }
    }
    entry.repl[entry.replNext] = {key, found};
    entry.replNext = static_cast<uint8_t>((entry.replNext + 1u) % entry.repl.size());
    return found ? repl->use(found) : nil;
}
