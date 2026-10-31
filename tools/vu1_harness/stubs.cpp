// Stubs for the two symbols the VU1 sources reference outside the VU: the replay runs in silent mode, which captures the
// XGKICK packets instead of sending them, so neither is ever called.
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include <cstdlib>

void GS::processGIFPacket(const uint8_t *, uint32_t, GifPathId) { std::abort(); }
void PS2Memory::submitGifPacket(GifPathId, const uint8_t *, uint32_t, bool, bool) { std::abort(); }
// sw-dips3 (dips8): the native runs ask for the GS stage buffer; the harness has no PS2Memory (ctx.memory null)
std::vector<uint8_t> *PS2Memory::gifDirectBuffer() { return nullptr; }
void PS2Memory::gifDirectCommit(uint32_t) { std::abort(); }
