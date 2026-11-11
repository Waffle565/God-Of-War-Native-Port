// GOW-Port: replacement texture packs for the Metal GS renderer (PCSX2 format, names from gs_metal_texrepl.h).
// The pack folder is indexed once (file names only); a texture is read and uploaded on a background queue the first time a
// draw needs it, the original is drawn until then (PCSX2's asynchronous loading), and uploaded textures are dropped least
// recently used first above a memory budget. Optional dump mode writes every new texture as a PNG named the PCSX2 way.
// Switches (read once): GOW_GS_MTL_TEXREPL=0 (off), GOW_TEXTURES_DIR (the replacements folder, scanned recursively;
// default ~/Library/Application Support/God of War/textures/SCUS-97399/replacements), GOW_GS_MTL_TEXREPL_MB (GPU budget,
// default 1/16 of the Mac's memory, 256-2048: 1024 MB on 16 GB), GOW_GS_MTL_TEXDUMP=<dir> (dump), GOW_GS_MTL_TEXREPL_LOG=1
// (per-texture log). Replacements live in GPU-only (private) memory, uploaded through a staging buffer by the loader.
#pragma once

#if !defined(__OBJC__)
#error "gs_metal_texrepl_store.h is Objective-C++ only"
#endif

#import <Metal/Metal.h>

#include "gs_metal_texrepl.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gsrepl
{
    struct Slot
    {
        std::atomic<int> state{0}; // 0 not resident, 1 loading, 2 ready, 3 failed
        id<MTLTexture> tex = nil;  // written by the loader before state 2, cleared by the GS thread on eviction
        std::string path;
        size_t bytes = 0;
        uint64_t lastUse = 0;
        Name name;
    };

    // reads a replacement file (DDS: BC1/BC2/BC3/BC7 or 32-bit RGBA with its mip chain; PNG: RGBA8 with generated mips)
    // into a new private texture (copied in with `queue`, waits); nil on failure. `bytes`: GPU size. Runs on the loader queue.
    id<MTLTexture> loadReplacement(id<MTLDevice> device, id<MTLCommandQueue> queue, const std::string &path, size_t &bytes);
    // PNG of `rgba` (straight alpha, PS2 range) at `path`
    bool writePng(const std::string &path, const uint32_t *rgba, uint32_t width, uint32_t height);

    class Store
    {
    public:
        ~Store();
        // reads the switches and indexes the pack; false: neither replacing nor dumping
        bool init(id<MTLDevice> device);
        bool replacing() const { return !m_index.empty(); }
        bool dumping() const { return !m_dumpDir.empty(); }
        // the pack's file for this name (nullptr: none). `count`: the name is counted as seen (and matched) in the statistics.
        Slot *find(const Name &name, bool count = true);
        // the uploaded replacement, or nil while it loads (the load is queued here)
        id<MTLTexture> use(Slot *slot);
        bool dumped(const Name &name) const { return m_dumped.count(name) != 0; }
        // writes `rgba` (w x h, PS2 alpha range) as <dump dir>/<name>.png on a background queue, once per name
        void dump(const Name &name, const uint32_t *rgba, uint32_t width, uint32_t height);
        // once per presented frame: evicts above the budget, prints statistics every few seconds
        void frame();
        // memory pressure: drops every replacement the last two frames did not draw; returns the bytes released
        size_t trim();

    private:
        void load(Slot *slot);
        // least recently used first down to `target` bytes, never what the last two frames drew; returns the bytes released
        size_t evict(size_t target);

        id<MTLDevice> m_device = nil;
        id<MTLCommandQueue> m_queue = nil; // mipmap generation of PNG replacements (loader thread)
        dispatch_queue_t m_loadQueue = nullptr, m_dumpQueue = nullptr;
        std::unordered_map<Name, std::unique_ptr<Slot>, NameHash> m_index;
        std::unordered_set<Name, NameHash> m_seen, m_matched, m_dumped;
        std::vector<Slot *> m_resident;
        std::mutex m_loadedMutex;
        std::vector<Slot *> m_loaded; // finished by the loader, moved to m_resident by frame()
        std::string m_dumpDir;
        std::atomic<size_t> m_bytes{0};
        std::atomic<uint32_t> m_loads{0}, m_failures{0}, m_pending{0};
        size_t m_budget = 1024ull << 20;
        uint64_t m_frame = 0, m_lastReport = 0;
        bool m_log = false;
    };
}
