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

## Console results (2026-10-19, remote loop: tools/switch_bot.py + switch_mtp.py, `PS2X_GS_HW_SCALE=1`)
- Citron cannot run it: `dkQueueCreate` never returns there (Citron's GPU service), so deko3d builds are tested on the
  console only.
- First console run: the title menu drew correctly (720p, upright, colours right), then the game was closed by an
  error: textures lived in a `std::vector`, and a `DkImageView` points at its `DkImage`, so the vector's growth left
  every view dangling. Textures and programs are in deques now.
- Every vsync's Present composed and presented a frame and waited in `dkQueueAcquireImage` for the display (GS thread
  "present" ~2.6 s per 5 s, menu 18-19 fps). Now an unchanged frame (same display registers and displayed targets,
  the check the GL RAM path already had) is not presented again, and the image is acquired with a fence the GPU waits
  on (`dkSwapchainAcquireImage` + `dkCmdBufWaitFence`), not this thread.

Measured with the env file really applied (an earlier test script uploaded `env_*.txt` under its own name, so the
game kept reading an old `env.txt` with scale 1; the "1.6" figures before 2026-10-19 04:00 were 1x):

| console, guest fps (deko3d + VU1 0732eff + EE d352c1c merged, ext bd65531) | title menu | gameplay (ship, fighting) |
|---|---|---|
| GL v15 (switch-mesa), for comparison | 25-26 | 9-13 |
| deko3d, scale 1.6 (~720 lines internal), bicubic present | 28.0-28.8 | 12.6-15.0 |
| **deko3d, scale 1 (640x448 internal), bicubic GPU upscale to 720p: the default** | **27.8-29.2** | **19.4-28.0** (19.4-20.6 in the longest fight) |

- Rendering at 720p is GPU-bound in gameplay; the PS2's own resolution with the GPU upscale is the default
  (`ps2_switch_runtime.cpp`: `PS2X_GS_HW_SCALE=1`, bicubic + sharpen; env.txt `PS2X_GS_HW_SCALE=1.6` /
  `GOW_SCALER=bilinear` choose otherwise).
- Where VU1 has little to do (the pause screen) the whole pipeline already runs at 59.6-59.8 fps.
- Gameplay per 5 s at scale 1: VU1 ~4.0 s, EE ~2.7 s, GS thread ~1.0 s busy (deko3d): **VU1 is the limit**, then the
  EE. The renderer is no longer what keeps the game under 60.
- Remaining renderer work: the CLUT-from-render-target readback (a GPU round trip, cached per target version) and
  GPU time at 1.6x (fill: the bicubic pass and the 1.6x targets).

## The EE thread's core (agent/sw-ee4, 2026-10-19)
Gameplay on the console (log `port/switch/logs/20261019_s16`, ship fight, ~21 fps), per 5 s:
- core 1 runs the EE thread and the GS frontend (`GSStage`, one priority step above it): EE game code `ee_ms` ~2.9 s +
  IOP and sound `iop_ms` ~1.2 s (both on the EE thread) + GS frontend `gs_ms` ~0.9 s = **core 1 is full**; the EE
  thread still sleeps ~0.9 s (`guest_wait_ms`), waiting at the frame's sync points on the VU1 worker (core 2, ~4.0 s
  busy). VU1 and core 1 limit together: a faster EE alone turns into more waiting.
- The IOP cost is per wall-clock second (the game runs at full speed with fewer frames, `gow-speed` 0.98), so at 60 fps
  it stays ~1.2 s while the EE and the GS frontend triple. Split on PC (`PS2X_IOP_NATIVE_STATS=1`, native IRX code):
  per 5 s the IRX code ~55 ms, sound mixing ~80-170 ms, services ~20 ms and ~50 ms of per-call overhead (~72000
  `runCycles` calls per second, one per 512 IOP cycles). A batch of 2048 IOP cycles cut the call overhead to a third
  but the memory card load then never finished (zone gate slots 1-4): kept at 512.
- `GOW_CORE_<thread>=<0-2>` in env.txt overrides a thread's core (ThreadNaming.h): `GOW_CORE_GSStage=0` moves the GS
  frontend next to the deko3d backend on core 0, which deko3d made light, and leaves core 1 to the EE and the IOP.
  Untested on the console.
- EE generated code (devkitA64 -O2, instructions): 0x162F18 50133 -> 46039 (-8.2 %), 0x111760 52480 -> 50152,
  0x158DA8 4688 -> 4441, 0x1679F8 13655 -> 13226, 0x158020 5271 -> 5203, 0x1658F8 11143 -> 11385: slow-path lambda
  by value (no rdram/ctx reloads from the stack) and no PC/delay-slot stores the runtime cannot read
  (docs/runtime-changes.md). On PC, where those stores are one instruction each, the change is within noise.
- Looked at and left: `__restrict` on rdram/ctx (no gain with the lambda fixed either); direct calls instead of
  `dispatchGuestBranch` for static JAL (the table lookup is already a subtract and a shift; the rest is the debug ring,
  history and two statics); typed (non-memcpy) guest accesses (ctx->r is a vector of 64-bit lanes, so 64/128-bit
  accesses would still alias it). The next big EE lever is keeping guest registers in C++ locals inside a function
  (like the VU1 generator's `--locals`): every guest instruction still loads its operands from and stores its result to
  `ctx`.
