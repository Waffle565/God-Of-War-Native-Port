#include "MiniTest.h"
#include "runtime/ps2_pad_keyboard.h"

#include <cstdint>
#include <cstring>

namespace
{
    constexpr uint16_t kUp = 0x0010u, kRight = 0x0020u, kDown = 0x0040u, kLeft = 0x0080u;
    constexpr uint16_t kStart = 0x0008u, kSelect = 0x0001u;
    constexpr uint16_t kL2 = 0x0100u, kR2 = 0x0200u, kL1 = 0x0400u, kR1 = 0x0800u;
    constexpr uint16_t kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;

    // Idle analog packet: no button pressed, both sticks centred.
    void idlePacket(uint8_t *data)
    {
        std::memset(data, 0, 32);
        data[0] = 0x01;
        data[1] = 0x73;
        data[2] = data[3] = 0xFF;
        data[4] = data[5] = data[6] = data[7] = 0x80;
    }

    uint16_t pressed(const uint8_t *data)
    {
        return static_cast<uint16_t>(~(data[2] | (data[3] << 8)) & 0xFFFFu);
    }
}

void register_pad_keyboard_tests()
{
    MiniTest::Case("PadKeyboard", [](TestCase &tc)
                   {
        tc.Run("no keys leave the packet unchanged", [](TestCase &t)
               {
            uint8_t data[32], before[32];
            idlePacket(data);
            std::memcpy(before, data, 32);
            ps2PadApplyKeyboard(PadKeyboardInput{}, data);
            t.IsTrue(std::memcmp(data, before, 32) == 0, "packet must not change");
        });

        tc.Run("each key maps to its button", [](TestCase &t)
               {
            struct Case { bool PadKeyboardInput::*key; uint16_t button; const char *name; };
            const Case cases[] = {
                {&PadKeyboardInput::mouseLeft, kSquare, "left click = Square"},
                {&PadKeyboardInput::mouseRight, kTriangle, "right click = Triangle"},
                {&PadKeyboardInput::space, kCross, "Space = Cross"},
                {&PadKeyboardInput::e, kCircle, "E = Circle"},
                {&PadKeyboardInput::q, kL1, "Q = L1"},
                {&PadKeyboardInput::r, kR1, "R = R1"},
                {&PadKeyboardInput::f, kL2, "F = L2"},
                {&PadKeyboardInput::g, kR2, "G = R2"},
                {&PadKeyboardInput::one, kUp, "1 = D-pad up"},
                {&PadKeyboardInput::two, kRight, "2 = D-pad right"},
                {&PadKeyboardInput::three, kDown, "3 = D-pad down"},
                {&PadKeyboardInput::four, kLeft, "4 = D-pad left"},
                {&PadKeyboardInput::enter, kStart, "Enter = Start"},
                {&PadKeyboardInput::backspace, kSelect, "Backspace = Select"},
            };
            for (const Case &c : cases)
            {
                uint8_t data[32];
                idlePacket(data);
                PadKeyboardInput in;
                in.*c.key = true;
                ps2PadApplyKeyboard(in, data);
                t.Equals(pressed(data), c.button, c.name);
                t.IsTrue(data[4] == 0x80 && data[5] == 0x80 && data[6] == 0x80 && data[7] == 0x80,
                         std::string(c.name) + ": sticks stay centred");
            }
        });

        tc.Run("WASD drive the left stick and press no D-pad", [](TestCase &t)
               {
            uint8_t data[32];
            idlePacket(data);
            PadKeyboardInput in;
            in.w = true;
            ps2PadApplyKeyboard(in, data);
            t.Equals(static_cast<int>(data[6]), 0x80, "W: left X centred");
            t.Equals(static_cast<int>(data[7]), 1, "W: left Y full up");
            t.Equals(pressed(data), uint16_t{0}, "W: no button");

            idlePacket(data);
            in = {};
            in.d = true;
            ps2PadApplyKeyboard(in, data);
            t.Equals(static_cast<int>(data[6]), 255, "D: left X full right");
            t.Equals(static_cast<int>(data[7]), 0x80, "D: left Y centred");

            idlePacket(data);
            in = {};
            in.a = true;
            in.s = true;
            ps2PadApplyKeyboard(in, data);
            t.Equals(static_cast<int>(data[6]), 38, "A+S diagonal X normalised (128 - 127/sqrt2)");
            t.Equals(static_cast<int>(data[7]), 218, "A+S diagonal Y normalised (128 + 127/sqrt2)");
            t.Equals(static_cast<int>(data[4]), 0x80, "A+S: right stick untouched");

            idlePacket(data);
            in = {};
            in.a = true;
            in.d = true;
            ps2PadApplyKeyboard(in, data);
            t.Equals(static_cast<int>(data[6]), 0x80, "A+D cancel out");
        });

        tc.Run("arrows drive the right stick", [](TestCase &t)
               {
            uint8_t data[32];
            idlePacket(data);
            PadKeyboardInput in;
            in.left = true;
            in.down = true;
            ps2PadApplyKeyboard(in, data);
            t.Equals(static_cast<int>(data[4]), 38, "left+down: right X");
            t.Equals(static_cast<int>(data[5]), 218, "left+down: right Y");
            t.Equals(static_cast<int>(data[6]), 0x80, "left stick untouched");
            t.Equals(pressed(data), uint16_t{0}, "arrows press no D-pad");
        });

        tc.Run("gamepad and keyboard merge", [](TestCase &t)
               {
            uint8_t data[32];
            idlePacket(data);
            data[3] = static_cast<uint8_t>(~(kCross >> 8)); // gamepad holds Cross
            data[6] = 0xC0;                                  // gamepad left stick half right
            data[4] = 0xFF;                                  // gamepad right stick full right
            PadKeyboardInput in;
            in.e = true;  // Circle
            in.a = true;  // full left: further from centre than the gamepad's half right
            in.up = true; // full up on the right stick: same deflection as the gamepad's full right
            ps2PadApplyKeyboard(in, data);
            t.Equals(pressed(data), static_cast<uint16_t>(kCross | kCircle), "buttons add up");
            t.Equals(static_cast<int>(data[6]), 1, "left stick: keyboard deflects further");
            t.Equals(static_cast<int>(data[7]), 0x80, "left stick Y from keyboard");
            t.Equals(static_cast<int>(data[4]), 0xFF, "right stick: tie keeps the gamepad");
            t.Equals(static_cast<int>(data[5]), 0x80, "right stick Y kept from gamepad");
        });
    });
}
