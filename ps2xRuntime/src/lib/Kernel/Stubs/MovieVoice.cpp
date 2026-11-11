#include "MovieVoice.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ps2_stubs
{
    namespace
    {
        // PSS audio stream: SShd {u32 size, type, rate, channels, interleave, loop start, loop end} + SSbd {u32 size}
        constexpr size_t kStreamHeader = 0x28u;
        constexpr size_t kKeyBytes = kStreamHeader + 2048u;
        constexpr uint32_t kPcmBlock = 512u; // the PCM the game plays: 512 bytes left, 512 bytes right
        constexpr uint32_t kKindAdpcm = 0u, kKindPcm = 1u;

        uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
        uint64_t le64(const uint8_t *p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

        uint64_t fnv1a64(const uint8_t *p, size_t n)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (size_t i = 0; i < n; ++i)
                h = (h ^ p[i]) * 0x100000001B3ull;
            return h;
        }

        // One part of a dubbed track: SPU ADPCM or PCM16, stereo, <interleave> bytes per channel, starting at <start>
        // (a sample of the original stream; the European ADPCM tracks start one silent block early: -28).
        struct Segment
        {
            int64_t start = 0;
            uint32_t kind = 0u, interleave = 0u;
            std::vector<uint8_t> data;
            // ADPCM decoding, forward only: samples [base, base + window[c].size()) of each channel
            size_t nextChunk = 0u;
            int32_t history[2][2] = {};
            int64_t base = 0;
            std::vector<int16_t> window[2];

            int64_t samples() const
            {
                const uint64_t chunks = interleave ? data.size() / (2u * interleave) : 0u;
                return static_cast<int64_t>(chunks * (kind == kKindAdpcm ? interleave / 16u * 28u : interleave / 2u));
            }

            void decodeAdpcmBlock(const uint8_t *block, int c)
            {
                static const int32_t f0[5] = {0, 60, 115, 98, 122}, f1[5] = {0, 0, -52, -55, -60};
                const int shift = (block[0] & 15) > 12 ? 9 : (block[0] & 15);
                const int filter = (block[0] >> 4) > 4 ? 0 : (block[0] >> 4);
                int32_t s1 = history[c][0], s2 = history[c][1];
                for (int i = 0; i < 28; ++i)
                {
                    const int nibble = (block[2 + i / 2] >> ((i & 1) * 4)) & 15;
                    int32_t v = static_cast<int16_t>(nibble << 12) >> shift;
                    v += (s1 * f0[filter] + s2 * f1[filter] + 32) >> 6;
                    v = v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
                    window[c].push_back(static_cast<int16_t>(v));
                    s2 = s1;
                    s1 = v;
                }
                history[c][0] = s1;
                history[c][1] = s2;
            }

            int16_t sample(int c, int64_t t)
            {
                if (kind == kKindPcm)
                {
                    const uint64_t half = interleave / 2u;
                    const uint64_t at = (t / half) * 2u * interleave + c * interleave + (t % half) * 2u;
                    return at + 2u <= data.size() ? static_cast<int16_t>(data[at] | (data[at + 1] << 8)) : 0;
                }
                if (t < base) // a seek back: decode again from the start
                {
                    nextChunk = 0u;
                    base = 0;
                    std::memset(history, 0, sizeof(history));
                    window[0].clear();
                    window[1].clear();
                }
                if (window[0].size() > 65536u && t - base > 8192)
                {
                    const size_t drop = static_cast<size_t>(t - base - 4096);
                    for (std::vector<int16_t> &w : window)
                        w.erase(w.begin(), w.begin() + drop);
                    base += static_cast<int64_t>(drop);
                }
                while (t >= base + static_cast<int64_t>(window[0].size()))
                {
                    if ((nextChunk + 1u) * 2u * interleave > data.size())
                        return 0;
                    const uint8_t *chunk = data.data() + nextChunk * 2u * interleave;
                    for (int ch = 0; ch < 2; ++ch)
                        for (uint32_t b = 0; b < interleave; b += 16u)
                            decodeAdpcmBlock(chunk + ch * interleave + b, ch);
                    ++nextChunk;
                }
                return window[c][static_cast<size_t>(t - base)];
            }
        };

        struct Track
        {
            std::vector<Segment> segments;

            int16_t sample(int c, int64_t s)
            {
                for (Segment &g : segments)
                    if (s >= g.start && s < g.start + g.samples())
                        return g.sample(c, s - g.start);
                return 0;
            }
        };

        // lang/movies_<code>.bin (tools/lang_pack.py pack_movies): 'PSMV', u32 version 1, u32 movies, u32 0; per movie
        // {u64 key, u32 first segment, u32 segments}; per segment {i32 start, u32 kind, u32 interleave, u32 0,
        // u64 data offset, u64 size}; data.
        class Library
        {
        public:
            Library()
            {
                const char *value = std::getenv("PS2X_MOVIE_VOICES");
                if (!value || !*value)
                    return;
                m_path = value;
                m_file = std::fopen(m_path.c_str(), "rb");
                uint8_t head[16] = {};
                if (!m_file || std::fread(head, 1, 16, m_file) != 16 || std::memcmp(head, "PSMV", 4) != 0 || le32(head + 4) != 1u)
                {
                    std::fprintf(stderr, "[movie-voice] cannot read %s: movies keep their sound\n", m_path.c_str());
                    return;
                }
                const uint32_t movies = le32(head + 8);
                std::vector<uint8_t> index(static_cast<size_t>(movies) * 16u);
                if (std::fread(index.data(), 1, index.size(), m_file) != index.size())
                    return;
                for (uint32_t i = 0; i < movies; ++i)
                {
                    const uint8_t *e = index.data() + i * 16u;
                    m_movies[le64(e)] = {le32(e + 8), le32(e + 12)};
                }
                std::fprintf(stderr, "[movie-voice] %s: dubbed sound for %u movies\n", m_path.c_str(), movies);
            }

            bool active() const { return !m_movies.empty(); }

            std::shared_ptr<Track> find(uint64_t key)
            {
                const auto it = m_movies.find(key);
                if (it == m_movies.end())
                    return nullptr;
                auto track = std::make_shared<Track>();
                const long tableAt = 16L + static_cast<long>(m_movies.size()) * 16L;
                for (uint32_t i = 0; i < it->second.second; ++i)
                {
                    uint8_t s[32] = {};
                    Segment g;
                    if (std::fseek(m_file, tableAt + static_cast<long>(it->second.first + i) * 32L, SEEK_SET) != 0 ||
                        std::fread(s, 1, 32, m_file) != 32)
                        return nullptr;
                    g.start = static_cast<int32_t>(le32(s));
                    g.kind = le32(s + 4);
                    g.interleave = le32(s + 8);
                    g.data.resize(static_cast<size_t>(le64(s + 24)));
                    if ((g.kind != kKindAdpcm && g.kind != kKindPcm) || g.interleave == 0u || g.interleave % 16u != 0u ||
                        std::fseek(m_file, static_cast<long>(le64(s + 16)), SEEK_SET) != 0 ||
                        std::fread(g.data.data(), 1, g.data.size(), m_file) != g.data.size())
                        return nullptr;
                    track->segments.push_back(std::move(g));
                }
                return track;
            }

        private:
            std::string m_path;
            std::FILE *m_file = nullptr;
            std::map<uint64_t, std::pair<uint32_t, uint32_t>> m_movies;
        };

        struct StreamState
        {
            uint64_t offset = 0u; // stream bytes seen (after the sub-stream headers), 0 = the SShd
            bool decided = false;
            std::shared_ptr<Track> track;
            uint64_t replaced = 0u;
        };

        std::mutex g_mutex;
        std::map<uint32_t, StreamState> g_streams;

        Library &library()
        {
            static Library instance;
            return instance;
        }
    }

    void movieVoiceFilterAudio(uint32_t mpegAddr, std::vector<uint8_t> &payload)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Library &lib = library();
        if (!lib.active() || payload.size() <= 4u)
            return;
        uint8_t *data = payload.data() + 4;
        const size_t size = payload.size() - 4u;
        StreamState &st = g_streams[mpegAddr];
        if (size >= 4u && std::memcmp(data, "SShd", 4) == 0) // a new stream
        {
            if (st.track)
                std::fprintf(stderr, "[movie-voice] mpeg=0x%x: %llu PCM bytes dubbed\n", mpegAddr,
                             static_cast<unsigned long long>(st.replaced));
            st = StreamState{};
        }
        if (!st.decided)
        {
            st.decided = true;
            // only the PCM layout the game plays (type 1, 2 channels, interleave 512), recognised from the first packet
            if (st.offset == 0u && size >= kKeyBytes && le32(data + 8) == 1u && le32(data + 16) == 2u && le32(data + 20) == kPcmBlock)
            {
                const uint64_t key = fnv1a64(data, kKeyBytes);
                st.track = lib.find(key);
                if (st.track)
                {
                    int64_t end = 0;
                    for (const Segment &g : st.track->segments)
                        end = std::max(end, g.start + g.samples());
                    std::fprintf(stderr, "[movie-voice] mpeg=0x%x movie %016llx: dubbed track, %zu part(s), %.1f s\n", mpegAddr,
                                 static_cast<unsigned long long>(key), st.track->segments.size(), end / 48000.0);
                }
                else
                    std::fprintf(stderr, "[movie-voice] mpeg=0x%x movie %016llx: no dubbed track, original sound\n", mpegAddr,
                                 static_cast<unsigned long long>(key));
            }
        }
        if (st.track)
        {
            for (size_t i = 0; i < size; ++i)
            {
                const uint64_t pos = st.offset + i;
                if (pos < kStreamHeader)
                    continue;
                const uint64_t p = pos - kStreamHeader, w = p % (2u * kPcmBlock);
                const int c = static_cast<int>(w / kPcmBlock);
                const int64_t s = static_cast<int64_t>((p / (2u * kPcmBlock)) * (kPcmBlock / 2u) + (w % kPcmBlock) / 2u);
                const uint16_t v = static_cast<uint16_t>(st.track->sample(c, s));
                data[i] = static_cast<uint8_t>((w & 1u) ? (v >> 8) : (v & 0xFFu));
                ++st.replaced;
            }
        }
        st.offset += size;
    }
}
