// GOW-Port: keyboard + mouse layout for playing without a gamepad, kept free of raylib so it can be tested.
// WASD = left stick, arrows = right stick, left/right click = Square/Triangle, Space = Cross, E = Circle,
// Q/R = L1/R1, F/G = L2/R2, 1-4 = D-pad up/right/down/left, Enter = Start, Backspace = Select.
#pragma once

#include <cmath>
#include <cstdint>

struct PadKeyboardInput
{
    bool w = false, a = false, s = false, d = false;              // left stick
    bool up = false, down = false, left = false, right = false;  // right stick
    bool mouseLeft = false, mouseRight = false;                  // Square, Triangle
    bool space = false, e = false;                               // Cross, Circle
    bool q = false, r = false, f = false, g = false;             // L1, R1, L2, R2
    bool one = false, two = false, three = false, four = false;  // D-pad up, right, down, left
    bool enter = false, backspace = false;                       // Start, Select
};

// Merges the keyboard/mouse into a DualShock 2 analog packet that already holds the gamepad's state:
// data[2..3] = buttons (active low), data[4..5] = right stick X/Y, data[6..7] = left stick X/Y (0x80 = centre).
// Buttons add to the gamepad's; each stick takes whichever source is deflected further from the centre.
inline void ps2PadApplyKeyboard(const PadKeyboardInput &in, uint8_t *data)
{
    uint16_t pressed = 0;
    auto press = [&pressed](bool key, uint16_t mask)
    {
        if (key)
            pressed |= mask;
    };
    press(in.backspace, 0x0001u); // Select
    press(in.enter, 0x0008u);     // Start
    press(in.one, 0x0010u);       // D-pad up
    press(in.two, 0x0020u);       // D-pad right
    press(in.three, 0x0040u);     // D-pad down
    press(in.four, 0x0080u);      // D-pad left
    press(in.f, 0x0100u);         // L2
    press(in.g, 0x0200u);         // R2
    press(in.q, 0x0400u);         // L1
    press(in.r, 0x0800u);         // R1
    press(in.mouseRight, 0x1000u); // Triangle
    press(in.e, 0x2000u);         // Circle
    press(in.space, 0x4000u);     // Cross
    press(in.mouseLeft, 0x8000u); // Square
    data[2] = static_cast<uint8_t>(data[2] & ~(pressed & 0xFFu));
    data[3] = static_cast<uint8_t>(data[3] & ~(pressed >> 8));

    // Same scale as the gamepad (128 + axis * 127); a diagonal is normalised so it is not faster than a straight line.
    auto mergeStick = [data](bool neg_x, bool pos_x, bool neg_y, bool pos_y, int ix, int iy)
    {
        const int x = (pos_x ? 1 : 0) - (neg_x ? 1 : 0);
        const int y = (pos_y ? 1 : 0) - (neg_y ? 1 : 0);
        if (x == 0 && y == 0)
            return;
        const double scale = (x != 0 && y != 0) ? 127.0 / std::sqrt(2.0) : 127.0;
        const int kx = 128 + static_cast<int>(std::lround(x * scale));
        const int ky = 128 + static_cast<int>(std::lround(y * scale));
        const int gx = data[ix] - 128, gy = data[iy] - 128;
        if ((kx - 128) * (kx - 128) + (ky - 128) * (ky - 128) <= gx * gx + gy * gy)
            return; // the gamepad is deflected at least as far: keep it
        data[ix] = static_cast<uint8_t>(kx);
        data[iy] = static_cast<uint8_t>(ky);
    };
    mergeStick(in.a, in.d, in.w, in.s, 6, 7);
    mergeStick(in.left, in.right, in.up, in.down, 4, 5);
}
