#pragma once

#include <cstdint>
#include <vector>

namespace ps2_stubs
{
    // GOW-Port: dubbed movie sound. PS2X_MOVIE_VOICES=<file> (lang/movies_<code>.bin from tools/lang_pack.py; relative
    // to the working directory) holds the sound of another language for PSS movies whose audio is PCM. Each movie is
    // recognised by its audio stream's header and first 2048 PCM bytes; while it is demuxed, its PCM is replaced, byte
    // position by byte position, by the dubbed track decoded to the same layout (16-bit, 512 bytes left / 512 right), so
    // the game's own player and A/V timing are unchanged. Streams that are not in the file keep their sound.
    // payload: one private stream 1 audio packet (4-byte sub-stream header + stream bytes), every packet of the stream
    // in demux order, whether or not the game takes it.
    void movieVoiceFilterAudio(uint32_t mpegAddr, std::vector<uint8_t> &payload);
}
