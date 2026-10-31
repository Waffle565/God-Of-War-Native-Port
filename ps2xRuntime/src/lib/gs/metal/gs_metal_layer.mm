// GOW-Port (macOS): the window's CAMetalLayer (include/runtime/gs/gs_metal_layer.h). Host side: a layer-hosting NSView over
// GLFW's content view, resized with the window, transparent to mouse clicks (GLFW's view keeps the input). The GS worker
// reads the layer and the drawable size; the host sets them.
#include "runtime/gs/gs_metal_layer.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

// clicks and mouse moves go through to GLFW's view below
@interface GowMetalView : NSView
@end

@implementation GowMetalView
- (NSView *)hitTest:(NSPoint)point
{
    (void)point;
    return nil;
}
- (BOOL)acceptsFirstResponder
{
    return NO;
}
@end

namespace
{
    std::mutex s_mutex;
    CAMetalLayer *s_layer = nil;
    GowMetalView *s_view = nil;
    __weak NSWindow *s_window = nil;
    std::atomic<bool> s_active{false}, s_overlay{false}, s_visible{true};
    std::atomic<uint32_t> s_width{0}, s_height{0};
    std::atomic<uint32_t> s_screenWidth{0}, s_screenHeight{0};
}

namespace GSMetalLayer
{
    bool Attach(void *nsWindow)
    {
        const char *setting = std::getenv("PS2X_GS_DIRECT_PRESENT");
        if ((setting && *setting == '0') || !nsWindow || s_active.load())
            return s_active.load();
        @autoreleasepool
        {
            NSWindow *window = (__bridge NSWindow *)nsWindow;
            NSView *content = window.contentView;
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!content || !device)
                return false;
            CAMetalLayer *layer = [CAMetalLayer layer];
            layer.device = device;
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.framebufferOnly = std::getenv("GOW_HOST_SHOT") == nullptr; // the shot reads the drawable back
            layer.displaySyncEnabled = YES;
            layer.maximumDrawableCount = 3;
            layer.opaque = YES;
            layer.contentsScale = window.backingScaleFactor;
            GowMetalView *view = [[GowMetalView alloc] initWithFrame:content.bounds];
            view.layer = layer; // layer-hosting: the view never draws into it
            view.wantsLayer = YES;
            view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            [content addSubview:view];
            std::lock_guard<std::mutex> lock(s_mutex);
            s_layer = layer;
            s_view = view;
            s_window = window;
        }
        s_active.store(true);
        UpdateSize();
        uint32_t w = 0, h = 0;
        DrawableSize(w, h);
        std::fprintf(stderr, "[gs-present] Metal layer attached: drawable %ux%u, vsync on\n", w, h);
        return true;
    }

    void Detach()
    {
        if (!s_active.exchange(false))
            return;
        @autoreleasepool
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            [s_view removeFromSuperview];
            s_view = nil;
            s_layer = nil;
            s_window = nil;
        }
    }

    bool Active() { return s_active.load(std::memory_order_acquire); }

    void SetOverlayVisible(bool visible)
    {
        if (!Active() || s_overlay.exchange(visible) == visible)
            return;
        @autoreleasepool
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_view.hidden = visible ? YES : NO;
        }
    }

    bool Showing() { return Active() && !s_overlay.load(std::memory_order_acquire); }

    bool WindowVisible() { return s_visible.load(std::memory_order_acquire); }

    void UpdateSize()
    {
        if (!Active())
            return;
        @autoreleasepool
        {
            NSView *view = nil;
            NSWindow *window = nil;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                view = s_view;
                window = s_window;
            }
            if (!view || !window)
                return;
            // a minimised or covered window gets no drawables (nextDrawable would stall the GS worker)
            s_visible.store((window.occlusionState & NSWindowOcclusionStateVisible) != 0);
            if (NSScreen *screen = window.screen)
            {
                const CGFloat screenScale = screen.backingScaleFactor;
                s_screenWidth.store(static_cast<uint32_t>(screen.frame.size.width * screenScale + 0.5));
                s_screenHeight.store(static_cast<uint32_t>(screen.frame.size.height * screenScale + 0.5));
            }
            const CGFloat scale = window.backingScaleFactor;
            const NSSize size = view.bounds.size;
            const uint32_t w = static_cast<uint32_t>(size.width * scale + 0.5), h = static_cast<uint32_t>(size.height * scale + 0.5);
            if (w != s_width.load() || h != s_height.load())
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_layer.contentsScale = scale;
                s_width.store(w);
                s_height.store(h);
            }
        }
    }

    void DrawableSize(uint32_t &width, uint32_t &height)
    {
        width = s_width.load(std::memory_order_acquire);
        height = s_height.load(std::memory_order_acquire);
    }

    void ScreenSize(uint32_t &width, uint32_t &height)
    {
        width = s_screenWidth.load(std::memory_order_acquire);
        height = s_screenHeight.load(std::memory_order_acquire);
    }

    void SetupWindow(void *nsWindow)
    {
        if (!nsWindow)
            return;
        @autoreleasepool
        {
            NSWindow *window = (__bridge NSWindow *)nsWindow;
            window.styleMask |= NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                NSWindowStyleMaskResizable;
            window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
            NSScreen *screen = window.screen ?: NSScreen.mainScreen;
            if (!screen)
                return;
            // the visible area leaves out the menu bar and the Dock: a 1920x1080 window on a 1080p screen would otherwise
            // put its title bar (and its buttons) under the menu bar
            const NSRect visible = screen.visibleFrame;
            NSRect frame = window.frame;
            if (frame.size.width > visible.size.width || frame.size.height > visible.size.height)
            {
                const NSRect content = [window contentRectForFrameRect:frame];
                const NSRect room = [window contentRectForFrameRect:visible];
                const CGFloat fit = std::min(room.size.width / content.size.width, room.size.height / content.size.height);
                frame = [window frameRectForContentRect:NSMakeRect(0, 0, std::floor(content.size.width * fit),
                                                                   std::floor(content.size.height * fit))];
            }
            frame.origin.x = visible.origin.x + std::floor((visible.size.width - frame.size.width) / 2);
            frame.origin.y = visible.origin.y + std::floor((visible.size.height - frame.size.height) / 2);
            [window setFrame:frame display:YES];
            const NSRect content = [window contentRectForFrameRect:frame];
            std::fprintf(stderr, "[window] %.0fx%.0f content at (%.0f,%.0f), screen visible area %.0fx%.0f\n",
                         content.size.width, content.size.height, frame.origin.x, frame.origin.y, visible.size.width,
                         visible.size.height);
        }
    }

    void ToggleFullscreen(void *nsWindow)
    {
        if (!nsWindow)
            return;
        @autoreleasepool
        {
            NSWindow *window = (__bridge NSWindow *)nsWindow;
            const bool entering = (window.styleMask & NSWindowStyleMaskFullScreen) == 0;
            [window toggleFullScreen:nil];
            std::fprintf(stderr, "[window] %s macOS fullscreen\n", entering ? "entering" : "leaving");
        }
    }

    CAMetalLayer *Layer()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_active.load() ? s_layer : nil;
    }
}
