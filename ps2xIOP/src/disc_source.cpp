#include "ps2x/iop/disc_source.h"

#include "ps2x/iop/ps2_path.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace ps2x::iop
{
    namespace
    {
        constexpr uint32_t kSector = DiscSource::kSectorSize;
        constexpr uint32_t kPrimaryVolumeDescriptorLsn = 16u;

        uint32_t readLe32(const uint8_t *source)
        {
            return uint32_t{source[0]} | (uint32_t{source[1]} << 8u) | (uint32_t{source[2]} << 16u) |
                   (uint32_t{source[3]} << 24u);
        }

        // ISO9660 identifier comparison: case-insensitive, version suffix (";1") ignored.
        std::string normalizedComponent(std::string_view value)
        {
            std::string result(value);
            const size_t semicolon = result.rfind(';');
            if (semicolon != std::string::npos)
                result.resize(semicolon);
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char ch)
                           { return static_cast<char>(std::toupper(ch)); });
            return result;
        }

        // "cdrom0:\DIR\NAME;1" → "DIR/NAME"; empty if not a cdrom path.
        std::string normalizedDiscPath(std::string_view guestPath)
        {
            const ParsedPs2Path parsed = parsePs2Path(guestPath);
            if (!parsed || parsed.device != Ps2PathDevice::Cdrom)
                return {};
            std::string result;
            size_t begin = 0u;
            while (begin <= parsed.path.size())
            {
                const size_t end = parsed.path.find('/', begin);
                const size_t length = (end == std::string::npos) ? parsed.path.size() - begin : end - begin;
                const std::string_view component(parsed.path.data() + begin, length);
                begin = (end == std::string::npos) ? parsed.path.size() + 1u : end + 1u;
                if (component.empty() || component == ".")
                    continue;
                if (!result.empty())
                    result += '/';
                result += normalizedComponent(component);
            }
            return result;
        }

        class ImageDisc final : public DiscSource
        {
        public:
            explicit ImageDisc(const std::filesystem::path &path)
                : m_file(path, std::ios::binary)
            {
                std::array<uint8_t, kSector> descriptor{};
                if (!readRaw(uint64_t{kPrimaryVolumeDescriptorLsn} * kSector, descriptor.data(), kSector) ||
                    std::memcmp(descriptor.data() + 1, "CD001", 5) != 0)
                    return;
                m_volumes.push_back({0u, descriptor});
                const uint32_t layer0Sectors = readLe32(descriptor.data() + 80);
                if (readRaw(uint64_t{layer0Sectors} * kSector, descriptor.data(), kSector) &&
                    std::memcmp(descriptor.data() + 1, "CD001", 5) == 0)
                {
                    m_layer1Start = layer0Sectors - kPrimaryVolumeDescriptorLsn;
                    m_volumes.push_back({m_layer1Start, descriptor});
                }
            }

            bool valid() const { return !m_volumes.empty(); }

            bool readSectors(uint32_t lsn, uint32_t count, void *destination) override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                return readRaw(uint64_t{lsn} * kSector, destination, size_t{count} * kSector);
            }

            std::optional<File> findFile(std::string_view guestPath) override
            {
                const std::string wanted = normalizedDiscPath(guestPath);
                if (wanted.empty())
                    return std::nullopt;
                std::lock_guard<std::mutex> lock(m_mutex);
                // Each layer carries its own tree; extents are relative to the layer origin.
                for (const Volume &volume : m_volumes)
                    if (auto file = findIn(volume, wanted))
                        return file;
                return std::nullopt;
            }

            uint32_t layer1Start() const override { return m_layer1Start; }

        private:
            struct Volume
            {
                uint32_t base = 0;
                std::array<uint8_t, kSector> descriptor{};
            };

            bool readRaw(uint64_t offset, void *destination, size_t size)
            {
                std::memset(destination, 0, size);
                m_file.clear();
                m_file.seekg(static_cast<std::streamoff>(offset));
                m_file.read(static_cast<char *>(destination), static_cast<std::streamsize>(size));
                return static_cast<size_t>(m_file.gcount()) == size;
            }

            std::optional<File> findIn(const Volume &volume, const std::string &wanted)
            {
                const uint8_t *root = volume.descriptor.data() + 156;
                File current{"", volume.base + readLe32(root + 2), readLe32(root + 10), true};
                size_t begin = 0u;
                while (begin < wanted.size())
                {
                    const size_t end = wanted.find('/', begin);
                    const std::string component = wanted.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                    begin = end == std::string::npos ? wanted.size() : end + 1u;
                    if (!current.directory)
                        return std::nullopt;

                    std::vector<uint8_t> records(current.size);
                    if (!readRaw(uint64_t{current.lsn} * kSector, records.data(), records.size()))
                        return std::nullopt;
                    bool found = false;
                    for (size_t offset = 0u; offset < records.size();)
                    {
                        const uint8_t length = records[offset];
                        if (length == 0u)
                        {
                            offset = (offset / kSector + 1u) * kSector; // records never span sectors
                            continue;
                        }
                        if (length < 34u || offset + length > records.size())
                            break;
                        const uint8_t *record = records.data() + offset;
                        const std::string_view name(reinterpret_cast<const char *>(record + 33),
                                                    std::min<size_t>(record[32], length - 33u));
                        if (record[32] > 1u && normalizedComponent(name) == component)
                        {
                            current = {normalizedComponent(name), volume.base + readLe32(record + 2), readLe32(record + 10),
                                       (record[25] & 2u) != 0u};
                            found = true;
                            break;
                        }
                        offset += length;
                    }
                    if (!found)
                        return std::nullopt;
                }
                return current;
            }

            std::mutex m_mutex;
            std::ifstream m_file;
            std::vector<Volume> m_volumes;
            uint32_t m_layer1Start = 0u;
        };

        class ExtractedDisc final : public DiscSource
        {
        public:
            explicit ExtractedDisc(const std::filesystem::path &root)
                : m_root(root)
            {
                readLayout(root / ".disc_layout", {});
                if (const char *overlay = std::getenv("PS2X_DISC_OVERLAY"); overlay != nullptr && overlay[0] != '\0')
                {
                    const std::filesystem::path file = root / std::filesystem::u8path(overlay);
                    if (readLayout(file, file.parent_path()))
                        std::fprintf(stderr, "[disc] overlay %s\n", reinterpret_cast<const char *>(file.u8string().c_str()));
                    else
                        std::fprintf(stderr, "[disc] overlay %s not found; plain disc\n",
                                     reinterpret_cast<const char *>(file.u8string().c_str()));
                }
                // stable: at equal LSNs the overlay file (added later) sorts after the disc file it lies in, so
                // fileAt finds it first
                std::stable_sort(m_files.begin(), m_files.end(), [](const Entry &a, const Entry &b)
                                 { return a.lsn < b.lsn; });
                // Open every overlay file now, at boot, reading its first bytes: the first open of a freshly written
                // file can take long (antivirus scan), and when that happened at a movie's first read the game
                // crashed in its movie start (JALR to 0 at 0x161c68, 2026-10-19, docs/settings.md).
                for (const Entry &entry : m_files)
                    if (entry.overlay)
                    {
                        std::array<char, kSector> first{};
                        std::ifstream &stream = handle(entry);
                        stream.read(first.data(), static_cast<std::streamsize>(first.size()));
                        stream.clear();
                    }
                for (Dub &dub : m_dubs)
                    loadDub(dub);
                std::sort(m_dubs.begin(), m_dubs.end(), [](const Dub &a, const Dub &b) { return a.lsn < b.lsn; });

                std::ifstream meta(root / ".disc_meta", std::ios::binary);
                std::array<uint8_t, 4u + kSector> record{};
                while (meta.read(reinterpret_cast<char *>(record.data()), static_cast<std::streamsize>(record.size())))
                {
                    std::array<uint8_t, kSector> &sector = m_meta[readLe32(record.data())];
                    std::memcpy(sector.data(), record.data() + 4u, kSector);
                }
            }

            bool valid() const { return m_sectors != 0u && !m_files.empty(); }

            bool readSectors(uint32_t lsn, uint32_t count, void *destination) override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto *bytes = static_cast<uint8_t *>(destination);
                bool ok = true;
                for (uint32_t i = 0u; i < count;)
                {
                    const uint32_t sector = lsn + i;
                    uint8_t *out = bytes + size_t{i} * kSector;
                    if (sector >= m_sectors)
                    {
                        std::memset(out, 0, size_t{count - i} * kSector);
                        return false;
                    }
                    if (const auto meta = m_meta.find(sector); meta != m_meta.end())
                    {
                        std::memcpy(out, meta->second.data(), kSector);
                        ++i;
                        continue;
                    }
                    const Entry *file = fileAt(sector);
                    if (!file)
                    {
                        std::memset(out, 0, kSector);
                        ++i;
                        continue;
                    }
                    if (file->overlay && !file->reported)
                    {
                        file->reported = true;
                        std::fprintf(stderr, "[disc] overlay read: %s\n", file->path.c_str());
                    }
                    // Read the run of sectors that lies inside this file in one go.
                    const uint32_t fileSectors = (file->size + kSector - 1u) / kSector;
                    const uint32_t run = std::min(count - i, file->lsn + fileSectors - sector);
                    std::memset(out, 0, size_t{run} * kSector);
                    const uint64_t offset = uint64_t{sector - file->lsn} * kSector;
                    const size_t wanted = static_cast<size_t>(std::min<uint64_t>(uint64_t{run} * kSector, file->size - offset));
                    std::ifstream &stream = handle(*file);
                    stream.clear();
                    stream.seekg(static_cast<std::streamoff>(offset));
                    stream.read(reinterpret_cast<char *>(out), static_cast<std::streamsize>(wanted));
                    ok = ok && static_cast<size_t>(stream.gcount()) == wanted;
                    i += run;
                }
                if (!m_dubs.empty())
                    applyDubs(lsn, count, bytes);
                return ok;
            }

            std::optional<File> findFile(std::string_view guestPath) override
            {
                const std::string wanted = normalizedDiscPath(guestPath);
                for (const Entry &entry : m_files)
                {
                    std::string name = entry.path;
                    std::replace(name.begin(), name.end(), '\\', '/');
                    if (normalizedComponent(name) == wanted)
                        return File{wanted.substr(wanted.rfind('/') + 1u), entry.lsn, entry.size, false};
                }
                return std::nullopt;
            }

            uint32_t layer1Start() const override { return m_layer1Start; }

        private:
            struct Entry
            {
                uint32_t lsn = 0;
                uint32_t size = 0;
                std::string path;
                bool overlay = false;           // from PS2X_DISC_OVERLAY
                mutable bool reported = false;  // its first read is logged (shows the overlay is in use)
            };

            // Adds a layout's entries; with a base folder (overlay) its paths are made absolute and an entry at an
            // LSN already listed replaces that entry.
            bool readLayout(const std::filesystem::path &file, const std::filesystem::path &base)
            {
                std::ifstream layout(file);
                if (!layout)
                    return false;
                std::string line;
                while (std::getline(layout, line))
                {
                    std::istringstream fields(line);
                    std::string kind;
                    fields >> kind;
                    if (kind == "sectors")
                        fields >> m_sectors;
                    else if (kind == "layer1")
                        fields >> m_layer1Start;
                    else if (kind == "dub" && !base.empty())
                    {
                        // GOW-Port: dub <lsn> <size> <path>: the movie file at <lsn> plays the dub file's audio
                        // (tools/gow_movie.py dub_file; applyDubs)
                        Dub dub;
                        fields >> dub.lsn >> dub.size;
                        std::getline(fields >> std::ws, dub.path);
                        if (dub.path.empty())
                            continue;
                        const auto absolute = (base / std::filesystem::u8path(dub.path)).u8string();
                        dub.path.assign(absolute.begin(), absolute.end());
                        m_dubs.push_back(std::move(dub));
                    }
                    else if (kind == "file")
                    {
                        Entry entry;
                        fields >> entry.lsn >> entry.size;
                        std::getline(fields >> std::ws, entry.path);
                        if (entry.path.empty())
                            continue;
                        if (!base.empty())
                        {
                            const auto absolute = (base / std::filesystem::u8path(entry.path)).u8string();
                            entry.path.assign(absolute.begin(), absolute.end());
                            entry.overlay = true;
                        }
                        // an overlay file replaces a disc file at the same LSN only when it covers it (the patched
                        // GODOFWAR.TOC); a smaller one at a PAK's first sector (R_PERM.WAD at PART1's start) lies
                        // inside the PAK and keeps it (fileAt)
                        auto same = std::find_if(m_files.begin(), m_files.end(), [&](const Entry &e)
                                                 { return e.lsn == entry.lsn && entry.size >= e.size; });
                        if (same != m_files.end())
                            *same = std::move(entry);
                        else
                            m_files.push_back(std::move(entry));
                    }
                }
                return true;
            }

            const Entry *fileAt(uint32_t sector) const
            {
                // GOW-Port: overlay files may lie inside a disc file (PART*.PAK, tools/lang_pack.py puts replacements
                // in the space of the files they replace: the game addresses a PAK with 32-bit byte offsets, so
                // nothing past 4 GiB from a PAK's start is reachable). The nearest entry starting at or before the
                // sector is then such an inner file; when the sector is past its end, the enclosing file further
                // back holds it.
                auto it = std::upper_bound(m_files.begin(), m_files.end(), sector, [](uint32_t value, const Entry &entry)
                                           { return value < entry.lsn; });
                while (it != m_files.begin())
                {
                    --it;
                    const uint32_t sectors = (it->size + kSector - 1u) / kSector;
                    if (sector < it->lsn + sectors)
                        return &*it;
                    if (!it->overlay)
                        return nullptr; // disc files do not nest: an earlier one ends before this one starts
                }
                return nullptr;
            }

            std::ifstream &handle(const Entry &entry)
            {
                auto it = m_handles.find(entry.path);
                if (it == m_handles.end())
                    it = m_handles.emplace(entry.path, std::ifstream(m_root / std::filesystem::u8path(entry.path), std::ios::binary)).first;
                return it->second;
            }

            // GOW-Port: a dubbed movie (docs/settings.md "Movies"). The overlay keeps the USA movie in place and
            // names a dub file holding another language's track as PS2 ADPCM (tools/gow_movie.py dub_file: header,
            // the movie's audio packet ranges, the ADPCM). Reads of the movie's sectors get the PCM bytes of its audio
            // packets replaced by that track, decoded on the fly: the movie, its packets and its own SShd/SSbd
            // headers stay the USA ones, so nothing else in the player changes. Whole dubbed copies of the ~40
            // speaking movies would need ~3.4 GB per language; the dub files are ~55 KB per second of movie.
            struct Dub
            {
                static constexpr uint32_t kWindowFrames = 8192u; // decoded ADPCM frames kept per channel (~4.8 s)
                uint32_t lsn = 0, size = 0;
                std::string path;
                bool valid = false, reported = false;
                uint32_t pcmStart = 0, pcmBytes = 0, pcmInterleave = 0, channels = 0, adpcmInterleave = 0, adpcmBytes = 0;
                int32_t lag = 0;
                uint64_t adpcmAt = 0;
                std::vector<uint32_t> rangeOffset, rangeLength, rangeTrack; // file offset, length, track offset
                std::ifstream file;
                std::vector<uint8_t> chunk;         // cached ADPCM bytes [chunkAt, chunkAt + chunk.size())
                uint64_t chunkAt = ~0ull;
                struct Channel
                {
                    int64_t nextFrame = 0;          // next frame to decode
                    int32_t h1 = 0, h2 = 0;
                    std::vector<int16_t> window;    // frame f's 28 samples at (f % kWindowFrames) * 28
                } channel[2];
            };

            void loadDub(Dub &dub)
            {
                const std::filesystem::path full = dub.path.find(":/") != std::string::npos
                                                       ? std::filesystem::u8path(dub.path)
                                                       : m_root / std::filesystem::u8path(dub.path);
                dub.file.open(full, std::ios::binary);
                std::array<uint8_t, 64> head{};
                dub.file.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
                const uint32_t ranges = readLe32(head.data() + 12);
                if (!dub.file || std::memcmp(head.data(), "GOWDUB1", 8) != 0 || readLe32(head.data() + 8) != dub.size)
                {
                    std::fprintf(stderr, "[disc] dub %s: not a dub file for this movie; English\n", dub.path.c_str());
                    return;
                }
                dub.pcmStart = readLe32(head.data() + 16);
                dub.pcmBytes = readLe32(head.data() + 20);
                dub.pcmInterleave = readLe32(head.data() + 24);
                dub.channels = readLe32(head.data() + 28);
                dub.lag = static_cast<int32_t>(readLe32(head.data() + 32));
                dub.adpcmInterleave = readLe32(head.data() + 36);
                dub.adpcmBytes = readLe32(head.data() + 40);
                if (dub.channels == 0u || dub.channels > 2u || dub.pcmInterleave < 2u || dub.adpcmInterleave < 16u ||
                    dub.adpcmInterleave % 16u != 0u)
                {
                    std::fprintf(stderr, "[disc] dub %s: unsupported layout; English\n", dub.path.c_str());
                    return;
                }
                std::vector<uint8_t> table(size_t{ranges} * 8u);
                dub.file.read(reinterpret_cast<char *>(table.data()), static_cast<std::streamsize>(table.size()));
                if (!dub.file)
                    return;
                uint32_t track = 0;
                for (uint32_t r = 0; r < ranges; ++r)
                {
                    dub.rangeOffset.push_back(readLe32(table.data() + r * 8u));
                    dub.rangeLength.push_back(readLe32(table.data() + r * 8u + 4u));
                    dub.rangeTrack.push_back(track);
                    track += dub.rangeLength.back();
                }
                dub.adpcmAt = 64u + table.size();
                dub.valid = true;
            }

            // sample n of a channel of the dub's track (0 outside it)
            int16_t dubSample(Dub &dub, uint32_t c, int64_t n)
            {
                static constexpr int32_t kF0[5] = {0, 60, 115, 98, 122};
                static constexpr int32_t kF1[5] = {0, 0, -52, -55, -60};
                const int64_t frames = dub.adpcmBytes / dub.channels / 16u;
                if (n < 0 || n / 28 >= frames)
                    return 0;
                const int64_t frame = n / 28;
                Dub::Channel &ch = dub.channel[c];
                if (ch.window.empty())
                {
                    for (Dub &other : m_dubs) // one movie plays at a time: keep only this one's windows
                        if (&other != &dub)
                            for (Dub::Channel &o : other.channel)
                                o = Dub::Channel{};
                    ch.window.assign(size_t{Dub::kWindowFrames} * 28u, 0);
                }
                if (frame < ch.nextFrame - static_cast<int64_t>(Dub::kWindowFrames))
                    ch.nextFrame = 0, ch.h1 = ch.h2 = 0; // behind the window: decode again from the start
                const uint32_t chunkFrames = dub.adpcmInterleave / 16u;
                while (ch.nextFrame <= frame)
                {
                    const int64_t f = ch.nextFrame;
                    const uint64_t at = uint64_t(f / chunkFrames) * dub.adpcmInterleave * dub.channels +
                                        uint64_t{c} * dub.adpcmInterleave + uint64_t(f % chunkFrames) * 16u;
                    if (dub.chunkAt == ~0ull || at < dub.chunkAt || at + 16u > dub.chunkAt + dub.chunk.size())
                    {
                        dub.chunkAt = at & ~uint64_t{0xFFFF} ;
                        dub.chunk.resize(0x10000u + 16u);
                        dub.file.clear();
                        dub.file.seekg(static_cast<std::streamoff>(dub.adpcmAt + dub.chunkAt));
                        dub.file.read(reinterpret_cast<char *>(dub.chunk.data()), static_cast<std::streamsize>(dub.chunk.size()));
                        dub.chunk.resize(static_cast<size_t>(dub.file.gcount()));
                        if (at + 16u > dub.chunkAt + dub.chunk.size())
                            return 0;
                    }
                    const uint8_t *data = dub.chunk.data() + (at - dub.chunkAt);
                    int32_t shift = data[0] & 15;
                    if (shift > 12)
                        shift = 9;
                    const int32_t filter = std::min<int32_t>((data[0] >> 4) & 7, 4);
                    int16_t *out = ch.window.data() + size_t(f % Dub::kWindowFrames) * 28u;
                    for (int k = 0; k < 28; ++k)
                    {
                        const int32_t nibble = (data[2 + k / 2] >> ((k & 1) * 4)) & 15;
                        const int32_t sample = static_cast<int16_t>(nibble << 12) >> shift;
                        int32_t value = sample + ((ch.h1 * kF0[filter] + ch.h2 * kF1[filter] + 32) >> 6);
                        value = std::clamp(value, -32768, 32767);
                        ch.h2 = ch.h1;
                        ch.h1 = value;
                        out[k] = static_cast<int16_t>(value);
                    }
                    ++ch.nextFrame;
                }
                return dub.channel[c].window[size_t(frame % Dub::kWindowFrames) * 28u + size_t(n % 28)];
            }

            // the PCM bytes of dubbed movies inside sectors [lsn, lsn + count) just read into bytes
            void applyDubs(uint32_t lsn, uint32_t count, uint8_t *bytes)
            {
                const uint64_t readBegin = uint64_t{lsn} * kSector, readEnd = readBegin + uint64_t{count} * kSector;
                for (Dub &dub : m_dubs)
                {
                    const uint64_t fileBegin = uint64_t{dub.lsn} * kSector;
                    if (!dub.valid || fileBegin >= readEnd || fileBegin + dub.size <= readBegin)
                        continue;
                    if (!dub.reported)
                    {
                        dub.reported = true;
                        std::fprintf(stderr, "[disc] dub read: %s\n", dub.path.c_str());
                    }
                    const uint64_t lo = std::max(readBegin, fileBegin) - fileBegin;
                    const uint64_t hi = std::min(readEnd, fileBegin + dub.size) - fileBegin;
                    // the first range ending after lo
                    size_t r = static_cast<size_t>(std::upper_bound(dub.rangeOffset.begin(), dub.rangeOffset.end(), lo) -
                                                   dub.rangeOffset.begin());
                    r = r > 0u ? r - 1u : 0u;
                    const uint32_t block = dub.pcmInterleave * dub.channels;
                    for (; r < dub.rangeOffset.size() && dub.rangeOffset[r] < hi; ++r)
                    {
                        const uint64_t from = std::max<uint64_t>(lo, dub.rangeOffset[r]);
                        const uint64_t to = std::min<uint64_t>(hi, uint64_t{dub.rangeOffset[r]} + dub.rangeLength[r]);
                        for (uint64_t at = from; at < to; ++at)
                        {
                            const uint64_t track = dub.rangeTrack[r] + (at - dub.rangeOffset[r]);
                            if (track < dub.pcmStart || track >= uint64_t{dub.pcmStart} + dub.pcmBytes)
                                continue;
                            const uint64_t p = track - dub.pcmStart;
                            const uint32_t inBlock = static_cast<uint32_t>(p % block);
                            const int64_t n = int64_t(p / block) * (dub.pcmInterleave / 2u) +
                                              (inBlock % dub.pcmInterleave) / 2u;
                            const int16_t value = dubSample(dub, inBlock / dub.pcmInterleave, n - dub.lag);
                            bytes[fileBegin + at - readBegin] = static_cast<uint8_t>((p & 1u) ? uint16_t(value) >> 8 : uint16_t(value) & 0xFFu);
                        }
                    }
                }
            }

            std::mutex m_mutex;
            std::filesystem::path m_root;
            uint32_t m_sectors = 0u;
            uint32_t m_layer1Start = 0u;
            std::vector<Entry> m_files;
            std::vector<Dub> m_dubs;
            std::unordered_map<uint32_t, std::array<uint8_t, kSector>> m_meta;
            std::map<std::string, std::ifstream> m_handles; // by path: an overlay file can share its LSN with the PAK it lies in
        };
    }

    std::unique_ptr<DiscSource> DiscSource::openImage(const std::filesystem::path &image)
    {
        auto disc = std::make_unique<ImageDisc>(image);
        if (!disc->valid())
            return nullptr;
        return disc;
    }

    std::unique_ptr<DiscSource> DiscSource::openExtracted(const std::filesystem::path &root)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(root / ".disc_layout", error))
            return nullptr;
        auto disc = std::make_unique<ExtractedDisc>(root);
        if (!disc->valid())
            return nullptr;
        return disc;
    }
}
