#ifndef PS2RECOMP_CODEGEN_HELPERS_H
#define PS2RECOMP_CODEGEN_HELPERS_H

#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <fmt/format.h>

namespace ps2recomp::codegen
{
    inline std::string formatFloatLiteral(float value)
    {
        if (!std::isfinite(value))
        {
            return (value < 0.0f) ? "-INFINITY" : "INFINITY";
        }

        std::string literal = fmt::format("{:.9g}", value);
        if (literal.find_first_of(".eE") == std::string::npos)
        {
            literal += ".0";
        }
        literal += 'f';
        return literal;
    }

    // GOW-Port: which translated instructions need their `ctx->pc = <address>` store. The PC is read only by the
    // runtime (calls, exceptions, dispatch, checkpoints), and every branch, call and checkpoint the emitter writes
    // stores its own PC first. So:
    //  - NeedsPc: the instruction calls the runtime itself or touches ctx->pc (syscalls, overflow traps, COP0,
    //    MMIO services, multi-line constructs): unchanged.
    //  - GuestMemory: its only way into the runtime is a guest memory access macro (READn/WRITEn), whose
    //    special-address path publishes ps2_gen_pc (ps2_runtime_macros.h): emitted in a block declaring it.
    //  - Pure: register/FPU/VU0 arithmetic and plain RAM accesses: no PC store at all.
    // Before, a PC store stayed in front of every load/store (the slow path could read it) and kept the previous
    // instruction's store alive as well: on AArch64 a mov/movk/str per guest instruction around every access.
    enum class PcUse { NeedsPc, GuestMemory, Pure };

    inline PcUse classifyPcUse(const std::string &body)
    {
        if (body.find("runtime") != std::string::npos || body.find("ctx->pc") != std::string::npos ||
            body.find("throw") != std::string::npos || body.find('\n') != std::string::npos)
        {
            return PcUse::NeedsPc;
        }
        for (const char *macro : {"READ8(", "READ16(", "READ32(", "READ64(", "READ128(",
                                  "WRITE8(", "WRITE16(", "WRITE32(", "WRITE64(", "WRITE128("})
        {
            for (size_t at = body.find(macro); at != std::string::npos; at = body.find(macro, at + 1))
            {
                // whole macro name only (FAST_READ32 is a plain RAM access, it does not count)
                if (at == 0 || !(std::isalnum(static_cast<unsigned char>(body[at - 1])) || body[at - 1] == '_'))
                {
                    return PcUse::GuestMemory;
                }
            }
        }
        return PcUse::Pure;
    }

    inline std::string vuMaskExpr(uint8_t dest_mask)
    {
        return fmt::format("_mm_castsi128_ps(_mm_set_epi32({}, {}, {}, {}))",
                           (dest_mask & 0x1) ? -1 : 0,
                           (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0,
                           (dest_mask & 0x8) ? -1 : 0);
    }
}

#endif // PS2RECOMP_CODEGEN_HELPERS_H
