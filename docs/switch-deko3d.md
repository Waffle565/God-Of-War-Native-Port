# Switch: the GS renderer on deko3d

The Switch build draws the game with deko3d (devkitPro's low-level NVN-style API for the Tegra X1, by fincs) instead
of OpenGL on switch-mesa. On Mesa/nouveau the GS GL thread was CPU-bound (~130 us of driver CPU per draw call, a
synchronous swap, the GL driver lock shared with the host thread; docs/switch.md "v14"/"v15") and gameplay stayed at
9-13 fps. Nintendo's own NVN is an NDA SDK; deko3d is its open homebrew counterpart and is installed in the WSL
toolchain (`deko3d` 0.5.0 with the `uam` shader compiler, `/opt/devkitpro/tools/bin/uam`).

## How it is built (ext branch `agent/sw-deko3d`)
- **Same backend, other GL.** `gs_hw_backend.cpp` reaches the GPU only through the `GSGL::Api` function table
  (gs_gl.h). On the Switch, `gs_dk.cpp` fills that table with a thin GL emulation over deko3d and replaces
  `gs_gl.cpp` (CMake option `PS2X_SWITCH_DEKO3D`, default ON for the Switch; OFF keeps switch-mesa). The backend's
  render-target, texture, shuffle, CLUT and present code is unchanged, and the PC build keeps its real GL.
  - GL conventions through the device flags (`DkDeviceFlags_OriginLowerLeft | DepthMinusOneToOne`): row 0 of a
    texture is GL's bottom row for uploads, render targets and readbacks. deko3d clears honour the scissor (it sets
    `ClearBufferFlags::Scissor`), as `glClear` does.
  - A call is a few state writes; a draw flushes only what changed (render targets, fixed-function state, viewport,
    program, uniforms, textures, vertex buffers) into a command buffer. Uniforms go through `dkCmdBufPushConstants`
    (an in-order UBO update in the command stream: no CPU-GPU synchronisation per draw).
  - Memory: buffer objects and uploads in CPU-visible GPU memory (the vertex stream is written in place and orphaned
    with `glBufferData` exactly as the backend does), images in their own blocks, freed when the submit that last
    used them has finished; a 16 MB command-memory ring and a 48 MB staging ring, both fenced per submit.
  - Readbacks (`glReadPixels`) submit and wait; the backend only does that for a CLUT read from a render target
    (cached per target version) and for diagnostics.
- **Shaders are compiled ahead of time.** deko3d has no GLSL compiler at run time. `tools/dk_shaders.py` extracts the
  backend's GLSL (the raw strings of gs_hw_backend.cpp, and the draw-variant sources it builds), rewrites them for uam
  and compiles every one in WSL into `ps2xRuntime/src/lib/gs/gpu/gs_dk_shaders_gen.cpp` (git-ignored, ~300 KB):
  - uam has no loose uniforms: each program family (draw, shuffle, present, clut, pack, scale) gets one std140 block
    with all its uniforms in a fixed order, each `uniform T uX;` becomes `#define uX (gsu_uX)` (a specialised
    variant's own `#define uX 1` still wins through the backend's `#ifndef` guards), samplers get explicit bindings;
  - `glShaderSource` text is matched by its FNV-1a 64 hash; `glGetUniformLocation` returns the member index (or
    0x10000 + sampler binding) and `glUniform*` writes the CPU copy of the block;
  - draw variants: the uber program is always in; the specialised ones come from `switch/dk_variant_keys.txt` (keys
    from `[gs-hw] draw variant ... key=` lines: PC zone tour with `GOW_GS_HW_STATS=1` + console logs, 51 so far). A
    variant without a build logs `[gs-dk] no deko3d build of a shader ... a draw variant (uber used)` and the backend
    falls back to the uber program; add its key and regenerate.
  - Regenerate: `python -I tools/dk_shaders.py ext/PS2Recomp-kl-swdeko ext/PS2Recomp-kl-swdeko/switch/dk_variant_keys.txt
    ext/PS2Recomp-kl-swdeko/ps2xRuntime/src/lib/gs/gpu/gs_dk_shaders_gen.cpp` (Windows Python, calls uam in WSL).
- **Presentation stays on the GPU.** The backend's shared-presentation path calls `GSGL::PublishSharedTexture` with
  the composed frame; on deko3d that draws the frame onto the next 1280x720 swapchain image (Catmull-Rom bicubic +
  sharpen like the PC host scaler, `GOW_SCALER=bilinear` for one tap; `GOW_SHARPEN`) and presents it with swap
  interval 1, from the GS thread. Nothing is read back, uploaded again or drawn by the host thread.
- **No raylib window.** The swapchain needs the default display window, which raylib's SDL would otherwise take, so
  `ps2_runtime.cpp` opens no window under `PS2X_DK`: the host loop only latches each vsync's Present and sleeps
  (`gs_host_present.cpp`), closes on `appletMainLoop()`, and the pad is read through libnx HID (`ps2_pad.cpp`: B =
  Cross, A = Circle, Y = Square, X = Triangle, + = Start, - = Select, ZL/ZR = L2/R2). Sound stays on SDL2 audio.
- **Internal resolution.** `PS2X_GS_HW_SCALE` (env.txt) sets the render targets' scale as before: 1 = the PS2's own
  640x448 (upscaled to 720p by the present pass on the GPU), higher values render more pixels. The present pass is the
  same either way, so the choice is only GPU time against sharpness.
- `GOW_PERF_DIAG=1`: `[gs-dk]` every 5 s (draws, submits, presents, GPU waits, uploads).
- `GOW_DK_PRESENT_FLIP=0` turns the present pass's vertical flip off (if the picture ever shows upside down).
