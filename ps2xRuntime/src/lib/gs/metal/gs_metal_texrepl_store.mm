// GOW-Port: index, asynchronous loading, memory budget and dump mode of the replacement texture packs
// (gs_metal_texrepl_store.h). Called from the GS worker thread only, except load(), which runs on the loader queue.
#include "gs_metal_texrepl_store.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

gsrepl::Store::~Store()
{
    // queued loads and dumps hold raw pointers into this store
    if (m_loadQueue)
        dispatch_sync(m_loadQueue, ^{});
    if (m_dumpQueue)
        dispatch_sync(m_dumpQueue, ^{});
}

bool gsrepl::Store::init(id<MTLDevice> device)
{
    m_device = device;
    m_log = std::getenv("GOW_GS_MTL_TEXREPL_LOG") != nullptr;
    // 1/16 of the Mac's memory: the GS, the game and the rest of the system share the same RAM (2048 MB on a 16 GB Mac
    // left too little under pressure)
    m_budget = std::clamp<size_t>(static_cast<size_t>([NSProcessInfo processInfo].physicalMemory / 16u), 256ull << 20, 2048ull << 20);
    if (const char *mb = std::getenv("GOW_GS_MTL_TEXREPL_MB"); mb && std::atoi(mb) > 0)
        m_budget = static_cast<size_t>(std::atoi(mb)) << 20;
    if (const char *dump = std::getenv("GOW_GS_MTL_TEXDUMP"); dump && *dump)
    {
        std::error_code ec;
        fs::create_directories(dump, ec);
        if (fs::is_directory(dump, ec))
            m_dumpDir = dump;
        else
            std::fprintf(stderr, "[gs-texrepl] dump folder %s cannot be created\n", dump);
    }
    const char *off = std::getenv("GOW_GS_MTL_TEXREPL");
    std::string dir;
    if (const char *env = std::getenv("GOW_TEXTURES_DIR"); env && *env)
        dir = env;
    else if (const char *home = std::getenv("HOME"))
        dir = std::string(home) + "/Library/Application Support/God of War/textures/SCUS-97399/replacements";
    std::error_code ec;
    if (!(off && *off == '0') && !dir.empty() && fs::is_directory(dir, ec))
    {
        // file names only (PCSX2 ReloadReplacementMap: the folder and its subfolders, .dds / .png)
        size_t skipped = 0;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec))
                continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".dds" && ext != ".png")
                continue;
            Name name;
            const std::string file = it->path().filename().string();
            if (!parseFileName(file.c_str(), name))
            {
                ++skipped; // mip levels (-mipN) and foreign files: the DDS carries its own chain, PNG chains are generated
                continue;
            }
            auto slot = std::make_unique<Slot>();
            slot->path = it->path().string();
            slot->name = name;
            m_index.emplace(name, std::move(slot));
        }
        std::fprintf(stderr, "[gs-texrepl] %zu replacement textures in %s (%zu other files), budget %zu MB\n", m_index.size(), dir.c_str(),
                     skipped, m_budget >> 20);
    }
    else if (!(off && *off == '0'))
        std::fprintf(stderr, "[gs-texrepl] no replacement folder at %s\n", dir.c_str());
    if (!replacing() && !dumping())
        return false;
    m_queue = [device newCommandQueue];
    m_loadQueue = dispatch_queue_create("gow.gs.texrepl.load", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    m_dumpQueue = dispatch_queue_create("gow.gs.texrepl.dump", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_BACKGROUND, 0));
    if (dumping())
        std::fprintf(stderr, "[gs-texrepl] dumping new textures to %s\n", m_dumpDir.c_str());
    return true;
}

gsrepl::Slot *gsrepl::Store::find(const Name &name, bool count)
{
    const auto it = m_index.find(name);
    Slot *slot = it == m_index.end() ? nullptr : it->second.get();
    if (count && m_seen.insert(name).second)
    {
        if (slot)
            m_matched.insert(name);
        if (m_log)
            std::fprintf(stderr, "[gs-texrepl] %s %s\n", slot ? "match" : "seen ", fileStem(name).c_str());
    }
    return slot;
}

id<MTLTexture> gsrepl::Store::use(Slot *slot)
{
    if (!slot)
        return nil;
    slot->lastUse = m_frame;
    const int state = slot->state.load(std::memory_order_acquire);
    if (state == 2)
        return slot->tex;
    if (state == 0)
    {
        slot->state.store(1, std::memory_order_relaxed);
        m_pending.fetch_add(1u, std::memory_order_relaxed);
        dispatch_async(m_loadQueue, ^{ load(slot); });
    }
    return nil;
}

void gsrepl::Store::load(Slot *slot)
{
    size_t bytes = 0;
    id<MTLTexture> tex = loadReplacement(m_device, m_queue, slot->path, bytes);
    m_pending.fetch_sub(1u, std::memory_order_relaxed);
    if (!tex)
    {
        m_failures.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr, "[gs-texrepl] cannot load %s\n", slot->path.c_str());
        slot->state.store(3, std::memory_order_release);
        return;
    }
    slot->tex = tex;
    slot->bytes = bytes;
    m_bytes.fetch_add(bytes, std::memory_order_relaxed);
    m_loads.fetch_add(1u, std::memory_order_relaxed);
    // ready before it becomes visible to frame()/evict(): an eviction in between would leave a nil texture marked ready
    slot->state.store(2, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_loadedMutex);
        m_loaded.push_back(slot);
    }
}

void gsrepl::Store::dump(const Name &name, const uint32_t *rgba, uint32_t width, uint32_t height)
{
    if (!dumping() || width == 0u || height == 0u || !m_dumped.insert(name).second)
        return;
    const std::string path = m_dumpDir + "/" + fileStem(name) + ".png";
    std::error_code ec;
    if (fs::exists(path, ec))
        return;
    auto pixels = std::make_shared<std::vector<uint32_t>>(rgba, rgba + static_cast<size_t>(width) * height);
    dispatch_async(m_dumpQueue, ^{
        if (!writePng(path, pixels->data(), width, height))
            std::fprintf(stderr, "[gs-texrepl] cannot write %s\n", path.c_str());
    });
}

// least recently used first, never what the last two frames drew (in-flight command buffers retain their textures anyway)
size_t gsrepl::Store::evict(size_t target)
{
    std::sort(m_resident.begin(), m_resident.end(), [](const Slot *a, const Slot *b) { return a->lastUse < b->lastUse; });
    size_t dropped = 0, released = 0;
    for (Slot *slot : m_resident)
    {
        if (m_bytes.load(std::memory_order_relaxed) <= target || slot->lastUse + 2u >= m_frame)
            break;
        if (slot->state.load(std::memory_order_acquire) != 2)
        {
            ++dropped; // no longer ready (already evicted): just leave the resident list
            continue;
        }
        slot->tex = nil;
        m_bytes.fetch_sub(slot->bytes, std::memory_order_relaxed);
        released += slot->bytes;
        slot->state.store(0, std::memory_order_release);
        ++dropped;
    }
    m_resident.erase(m_resident.begin(), m_resident.begin() + static_cast<ptrdiff_t>(dropped));
    return released;
}

size_t gsrepl::Store::trim()
{
    {
        std::lock_guard<std::mutex> lock(m_loadedMutex);
        m_resident.insert(m_resident.end(), m_loaded.begin(), m_loaded.end());
        m_loaded.clear();
    }
    return evict(0u);
}

void gsrepl::Store::frame()
{
    ++m_frame;
    {
        std::lock_guard<std::mutex> lock(m_loadedMutex);
        m_resident.insert(m_resident.end(), m_loaded.begin(), m_loaded.end());
        m_loaded.clear();
    }
    if (m_bytes.load(std::memory_order_relaxed) > m_budget)
        evict(m_budget - m_budget / 8u);
    if (m_frame - m_lastReport >= 300u)
    {
        m_lastReport = m_frame;
        std::fprintf(stderr, "[gs-texrepl] seen=%zu matched=%zu loaded=%u failed=%u pending=%u resident=%zu (%.1f MB) dumped=%zu\n", m_seen.size(),
                     m_matched.size(), m_loads.load(), m_failures.load(), m_pending.load(), m_resident.size(),
                     static_cast<double>(m_bytes.load()) / (1024.0 * 1024.0), m_dumped.size());
    }
}
