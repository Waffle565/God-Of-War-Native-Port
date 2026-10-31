// GOW-Port (macOS): the CAMetalLayer of the game window that GSMetalBackend presents into directly (zero-copy: no readback
// to RAM, no raylib texture upload). A layer-hosting view sits over raylib's OpenGL content view; the host thread keeps
// running raylib (input, the debug panel) and the GS worker composes and presents each frame into the layer with vsync.
// PS2X_GS_DIRECT_PRESENT=0 keeps the RAM presentation (readback + upload), as on the other platforms.
#pragma once

#include <cstdint>

namespace GSMetalLayer
{
    // host (main) thread, after InitWindow: attaches the layer to the NSWindow (raylib GetWindowHandle). false = RAM frames.
    bool Attach(void *nsWindow);
    void Detach();
    // a layer is attached: the GS backend presents into it (fixed for the whole run)
    bool Active();
    // the debug panel (ImGui, drawn by raylib in OpenGL) is open: the layer is hidden and the host shows RAM frames
    void SetOverlayVisible(bool visible);
    // Active() and not hidden by the overlay: the host draws no game frame
    bool Showing();
    // the window is on screen (not minimised or fully covered); updated by UpdateSize
    bool WindowVisible();
    // host thread, every host frame: window size and backing scale -> drawable size (pixels)
    void UpdateSize();
    // GS worker: drawable size in pixels (0 x 0 until the host reported it)
    void DrawableSize(uint32_t &width, uint32_t &height);
    // GS worker: pixel size of the screen the window is on (0 x 0 until the host reported it), what F11 fullscreen fills
    void ScreenSize(uint32_t &width, uint32_t &height);
}

#if defined(__OBJC__)
#import <QuartzCore/CAMetalLayer.h>
namespace GSMetalLayer
{
    // GS worker: the layer, nil when not Active()
    CAMetalLayer *Layer();
}
#endif
