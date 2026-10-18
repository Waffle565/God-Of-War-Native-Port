// Standalone VU1 replay harness (aarch64 Linux, under qemu or native): runs the GOW_VU1N_REPLAY records through the
// native blocks and the exact-engine fallback exactly as the game does, with no GS (silent verify mode never touches it).
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: vu1h <records> [reps]\n");
        return 1;
    }
    setenv("GOW_VU1N_REPLAY", argv[1], 1);
    setenv("GOW_VU1N_REPLAY_REPS", argc > 2 ? argv[2] : "3", 1);
    auto vu = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU1);
    alignas(64) static uint8_t code[PS2_VU1_CODE_SIZE];
    alignas(64) static uint8_t data[16384];
    alignas(64) static uint8_t gsStorage[1 << 16];
    // E bit on the first pair: the program the replay is triggered from ends at once
    const uint32_t lower = 0x8000033Cu, upper = 0x400002FFu; // NOP lower, NOP upper with E
    std::memcpy(code, &lower, 4);
    std::memcpy(code + 4, &upper, 4);
    GS &gs = *reinterpret_cast<GS *>(gsStorage);
    vu->execute(code, PS2_VU1_CODE_SIZE, data, sizeof(data), gs, nullptr, 0, 0, 0, 64);
    return 0;
}
