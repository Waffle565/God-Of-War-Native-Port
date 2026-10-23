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
  On the console it helps the title menu a little but not fights: there the deko3d thread fills core 0 (167-211k
  draws per 5 s) and the stage blocks on its queue; same build, same bot fight: 25.4-27.6 fps by default, 20.6-22.0
  with the stage on core 0 (docs/vu1-hle.md "The VU1 thread on the console"). The default stays. `VU1Run` (the
  `PS2X_VU1_ASYNC` thread) defaults to core 2.
- Gate (PC build of ext agent/sw-ee4 2b8d20f, regenerated code): port/swee4/gate/20261019_072605 zones 0-4 60 fps,
  speed 1.00, no capture flagged against port/swvu1/gate/20261019_015639; savetest 129 pages, card directory intact.
- EE generated code (devkitA64 -O2, instructions): 0x162F18 50133 -> 46039 (-8.2 %), 0x111760 52480 -> 50152,
  0x158DA8 4688 -> 4441, 0x1679F8 13655 -> 13226, 0x158020 5271 -> 5203, 0x1658F8 11143 -> 11385: slow-path lambda
  by value (no rdram/ctx reloads from the stack) and no PC/delay-slot stores the runtime cannot read
  (docs/runtime-changes.md). On PC, where those stores are one instruction each, the change is within noise.
- Looked at and left: `__restrict` on rdram/ctx (no gain with the lambda fixed either); direct calls instead of
  `dispatchGuestBranch` for static JAL (the table lookup is already a subtract and a shift; the rest is the debug ring,
  history and two statics); typed (non-memcpy) guest accesses (ctx->r is a vector of 64-bit lanes, so 64/128-bit
  accesses would still alias it).
- EE guest registers in C++ locals (sw-ee5, 2026-10-19, measured with devkitA64 -O2 on the six hot functions, not
  adopted): (a) per function, every scalar-used GPR a local, loaded at entry, written back before calls/dispatch/
  syscalls/returns, reloaded after (callee-saved s0-s7/gp/sp/fp kept, only written registers flushed): 0x162F18 46039
  -> 60451 instructions, stack accesses 375 -> 7671; (b) locals only inside straight-line runs between labels,
  branches and calls: 0x162F18 47448, 0x111760 48686, 0x1658F8 12804 (+12 %), stack accesses 375 -> 1831. GCC
  cannot keep the locals in registers across these functions' few hundred labels and gotos, so they become stack
  slots: no cheaper than `ctx`. Not pursued; the EE side is better served by moving work off core 1.
- Sound mixing off core 1 (sw-ee5): the IOP's sound engine mixes on its own `SoundMix` thread, core 0 on the Switch
  (docs/sound.md "Mixer thread"). Of the IOP's ~1.2 s per 5 s on core 1, the mixing part leaves; the IRX code and
  the services stay on the EE thread. PC: about 64 % of the mixing moves to the thread (IOP time on the EE thread
  -16 % mean over the zones gate, -25 % in slot 2); the rest is mixed at the driver's calls. Not yet measured on the
  console; if the wakes are cheap there, a smaller `GOW_SOUND_BATCH` moves more.
- The IOP off core 1 (sw-iopthr d404373, docs/iop-thread.md): the IOP scheduler, the native IRX code and the
  services run on their own `IopRun` thread, core 0 by default (`GOW_CORE_IopRun=<0-2>`); the EE thread only queues
  the due batches, and every EE call into the IOP drains them first (take-back), so the IOP state the EE sees is
  the inline one. PC zones gate: EE-thread `iop_ms` 168-281 -> 19-32 ms per 5 s (-90 %; what stays is the RPC calls
  themselves), 5/5 ok at 60 fps, savetest and SEMO01 unchanged. Expected on the console: most of the ~1.2 s per 5 s
  of IOP time leaves core 1 for core 0; not yet measured there. `GOW_IOP_THREAD=0` is the A/B switch.

## Draw count and per-draw cost (ext agent/sw-dkdraw, 2026-10-19)
Measured in the bot fight (`PLAY=1`, ship) with `GOW_GS_HW_STATS=1`; logs `port/switch/logs/20261019_dkd_*`. Before
(ext 31a482d, `update_par4`), heavy fight per 5 s: 6.5 M primitives in 14.4 k batches but **142-266 k deko3d draws
with 125-229 k texture barriers**; GS thread (core 0) busy 2.9 s (draws 1.29 s of it, vertex upload 0.66 s for 19.7 M
vertices), GS stage (core 1) 1.4 s.

What the draws were: the shadow-volume passes (COLCLAMP=0, Cs*FIX + Cd counting in G). The VU1 program colours each
volume triangle by its winding (G += 1 or G += 255 = -1), so one batch holds both kinds, is not one flat colour, and
fell back to the shader blend with a texture barrier before every triangle whose box met an earlier one: ~150
barrier+draw pairs per batch, ~1.3 k such batches per 5 s.

Changes (all on PC too, same results):
- **Counting batches as constant-amount groups** (gs_hw_backend.cpp `buildWrapGroups`): every triangle that is flat on
  its own adds one constant modulo 256; the batch is drawn as at most 4 fixed-function draws, one per (direction,
  amount), the adds first, then the subtractions (`GL_FUNC_REVERSE_SUBTRACT` of 256 - v), each group in its original
  order. Modulo-256 additions commute, so the result is the GS's as long as no pixel's partial sum leaves 0..255 on
  the way; only amounts of at most 1 are grouped (the counting passes), which needs fewer than 128 front faces over
  one pixel. Anything else (bigger amounts, a triangle not flat or mixing directions, more than 4 kinds) keeps the
  per-triangle barrier path. `GOW_GS_HW_WRAPGROUPS=0` turns it off; `[gs-hw-wrap]` counts both paths every 5 s.
- **Indexed batches** (`GOW_GS_HW_INDEXED=0`: off): a triangle that shares vertices with the previous one of its run
  reuses their indices; the batch's vertices are converted and uploaded once and drawn with
  `glDrawElementsBaseVertex` (GL 3.2 on PC, `dkCmdBufDrawIndexed` on deko3d). The COLCLAMP=0 paths look at triangles
  in order and get the expanded list.
- **Strips and fans as vertex kicks** (`GOW_GS_KICKS=0`: off): the GS frontend sends a strip/fan as the sequence of
  its vertex kicks (each vertex once, a flag where the kick drew; the run starts with the two vertices the GS window
  holds), `GSRasterBackend::SubmitKicks`, instead of three full vertices per triangle (136 bytes per triangle in the
  GS stage -> GL thread queue, now ~40). The backend rebuilds the triangles with the GS's own window rules (strip: the
  last three, fan: the first and the last two) straight into indices. Other backends expand them into triangles.
- **deko3d per-draw cost** (gs_dk.cpp): state setters mark dirty only on a real change (the backend sets the whole
  state before every draw), the blend constant has its own dirty flag, uniform buffer / texture / vertex / index
  bindings are cached per command list, push constants send only the changed bytes (a program's uniform memory
  keeps its contents across switches and submits). `[gs-dk-binds]` (draw CPU time, binds per kind, barriers) and
  `[gs-dk-time]` (swapchain acquire, present submit, submit) every 5 s.
- `[gs-stage-run]` (GS frontend): runs, triangles, kicks and the time spent handing them to the queue (that time
  includes waiting for the GL thread's queue, `gsq_wait_ms`).

Heavy fight per 5 s after the groups + indices (ext 7a0ac4a, `update_dkd1`): deko3d draws 142 k -> 24 k, barriers
125 k -> 4.8 k, uploaded vertices 19.7 M -> 7.7 M, vertex upload 660 -> 326 ms, deko3d CPU in draw calls 42 ms for
24 k draws (1.7 us each), GS thread busy 2.9 -> 2.2 s. The frame rate did not move with it: the fight is VU1-bound
(VU1 worker 3.7-3.9 s per 5 s) and the GS stage waits on the GL thread's queue only in bursts.

Console runs (bot fight; title = 25-40 s, heavy fight = the worst windows at 115-135 s, later = 140-200 s; guest fps
per 5 s window; the bot fight differs from run to run, Kratos even died in dkd1):

| build (ext, package in port/switch/nsp/out) | title | heavy fight | later |
|---|---|---|---|
| before: 31a482d (`update_par4`) | 35.4-36.2 | 16.2-17.4 | 23-30 |
| + groups, indices, deko3d caches (7a0ac4a, `update_dkd1`) | 33-34 | 15.5-17.6 | 21-24 |
| + VU1 fast float (root 2d99701 VU1_PREGEN; `update_dkd2f`) | 39.4-41.6 | 18.8-24.8 | 29-31 |
| + sw-iopthr merged, IOP thread on (`update_dkd3`) | 36.8-39.2 | 17.6-22.4 | 25-29.6 |
| same, `GOW_IOP_THREAD=0` | 36.8-38.6 | 17.2-24.2 | 27-29.6 |
| + vertex kicks (40bbdaf, `update_dkd4`), IOP inline | 37.4-41.8 | 19.0-23.4 | 26-31 |
| same, IOP thread on | 38.6-40.8 | 17.2-23.4 | 22.6-26 |
| same, IOP inline + `GOW_CORE_GSStage=0` | 38.8-39.8 | 18.4-24.4 | 23-26 |
| **+ sw-vu1cyc merged, IOP inline by default (9996088, `update_dkd5`, installed)** | **39.0-40.2** | **18.6-26.4** | **25.4-28.4** |

Per 5 s in the heavy fight, before -> `update_dkd5`: deko3d draws 142-266 k -> 29-37 k, GS thread (core 0) busy
2.9 s -> 2.1-2.4 s, VU1 runs (`vu_ms`) 4.0 s -> 3.4-3.7 s (VU1 worker job 3.9-4.05 s), EE 3.0 s -> 3.5-3.9 s,
IOP 1.2 s -> 0.8-1.2 s, GS stage 1.4 s -> 1.3-1.57 s, EE waiting (`guest_wait_ms`) 0.8-0.9 s -> 0.2-0.5 s.
**Core 1 is now the limit**: EE + IOP + GS stage ask for 5.9-6.5 s per 5 s, and the EE no longer waits for VU1.

- The IOP thread (sw-iopthr) took the IOP off the EE thread (`iop_ms` 1.0 -> 0.1 s) but the EE then waited for the
  thread's lead 0.8-1.0 s per 5 s (`[IOP:thread] lead_waits`): `IopRun` shares core 0 with the deko3d thread, which
  runs at a higher priority, and falls behind. Inline is the Switch default now (`ps2_switch_runtime.cpp`
  `GOW_IOP_THREAD=0`; env.txt `GOW_IOP_THREAD=1` turns it on). With `GOW_IOP_LEAD=262144` (`update_dkd5`) the EE
  still waited 0.72-0.84 s per 5 s (fewer, longer waits) for 19.2-24.2 / 24.8-33 fps: about even with inline.
  Worth trying: `IopRun` above `GSThread` on core 0.
- The GS stage spends ~180 ns per vertex kick (GIF PACKED decode, register writes, the kick), 1.3-1.6 s per 5 s in
  fights; handing the runs to the GL thread's queue is ~0.15 s of it (`[gs-stage-run] queue_ms` minus
  `gsq_wait_ms`). The next lever on core 1: a PACKED fast path for vertex tags (ST/RGBAQ/XYZ2 loops decoded straight
  into the kick run), or the GS stage on a core with room.
- `PS2X_GS_QUEUE_CHUNKS` (GL-thread queue depth, default 8 x 64 KB) and the GS stage on core 0 were not better.
- Checks: PC zone gate (port/dkdraw/gate, against port/swvu1par/gate/20261019_130004) 5/5 ok at 60 fps, speed
  1.00-1.01, for 7a0ac4a, 40bbdaf and the final 9996088 (`20261019_165610`; only content changes flagged:
  different combat moments); savetest 129 pages, card directory intact. Console screenshots of the fights (shadows
  under Kratos and the enemies, HUD) look right.
- Handoff: worktree ext/PS2Recomp-kl-dkdraw (branch agent/sw-dkdraw: sw-iopthr and sw-vu1cyc merged), PC build
  `out/build-pc`; the Switch build runs from a copy of the worktree in WSL (`/root/src-dkdraw`, synced with
  `tar cf - <paths> | wsl -u root -e tar xf - -C /root/src-dkdraw`, then `touch` the synced files) with
  `/root/dkbuild2.sh` (build dir `/root/gow-switch-dkdraw2`, VU1 blocks `/root/vu1fast/vu1pregen` = sw-vu1fast's):
  compiling from /mnt/c (9p) kept losing the WSL session. Keep exactly one wsl.exe session attached while it builds
  (the distro stops when none is) and poll it rarely. The console's address changes (DHCP): when switch_bot.py times
  out, `switch_bot.py find`. Installed: `update_dkd5`, no env.txt.

## Core 1: GS stage fast path and the IOP thread's priority (ext agent/sw-core1, 2026-10-19)
Base 9996088 (`update_dkd5`). Bot fight (`PLAY=1`, ship; logs `port/switch/logs/20261019_core1_*`), same windows as
above (title 25-40 s, heavy fight = the worst windows at 115-135 s, later 140-200 s).

- **PACKED fast path** (gs_frontend.cpp `processGIFPacket`, `GOW_GS_FASTPACK`, default 1): a tag's PACKED registers
  are decoded in one loop with RGBAQ/ST/UV/NOP inline, and an XYZ2/XYZF2 kick of a strip/fan in the middle of an
  open vertex-kick run (`GOW_GS_KICKS`, window full, debug history paused) is appended straight to the run: the
  vertex is built once, pushed, and the GS window updated as `vertexKick` leaves it. Everything else (the run's
  first draw and seed vertices, other primitives, A+D and other registers, XYZ3) still goes through
  `writeRegisterPacked`/`vertexKick`. It takes 88-90 % of the kicks in fights. `GOW_GS_FASTPACK=0` turns it off;
  `=2` is a check mode: the generic path runs and every kick the fast path would have taken is compared field by
  field (kick list entry, window, counters): 0 differences over ~200 M kicks on the console and ~12 M per 5 s in PC
  zone slot 2 (`[gs-stage-run] ... fast= check= diffs=`). The fast path is off by itself with `GOW_GS_KICKS=0`,
  `PS2X_GS_TRACE_TBP` or `PS2X_GS_FOG_DIAG`.
  GS stage (`gs_ms`) per 5 s, same build: fast path off 1.28-1.58 s in fights, on 0.80-1.17 s (about -0.4 s; the
  ~0.5 s of queue time to the deko3d thread is unchanged). The EE thread's own time drops by the same amount (the
  stage preempts it on core 1, so `ee_ms` contains the stage's time): 3.6-3.8 -> 3.1-3.4 s.
- **The frame rate does not move**: the EE's wait for VU1 (`guest_wait_ms`) grows by what was freed (0.3-0.4 ->
  0.6-0.9 s per 5 s in the later fight). The VU1 worker (core 2) is busy 3.8-4.1 s per 5 s and the EE waits for it
  in `sceGsSyncPath`; the frame is the EE -> VU1 chain, not core 1's sum. Core 1 now has ~0.6-0.9 s per 5 s spare.
- **IOP thread variants** (`GOW_IOP_THREAD=1`; `IopRun` now one priority step above `GSThread` by default,
  ThreadNaming.h, because Horizon does not time-slice equal priorities: before, `IopRun` ran only when the deko3d
  thread blocked; `GOW_PRIO_<thread>=<0-9>` sets any thread's step for tests):
  - core 0, boost 1, `GOW_IOP_SPIN=0`: EE-side `iop_ms` 1.0 -> 0.1-0.25 s, `IopRun` busy 0.6-0.8 s on core 0, lead
    waits 0.9-1.0 -> 0.4-0.6 s per 5 s, the deko3d thread busy 2.2-2.3 -> 2.5-2.7 s; the EE waits 1.4-1.5 s.
  - same with `GOW_IOP_LEAD=4194304` (ring now 16384 entries, lead clamp ~8.4 M cycles): lead waits ~0-30 ms, the
    thread lags up to the whole lead (`max_lead` 4.2 M); the EE still waits 1.3-1.5 s for VU1.
  - core 2 (`GOW_CORE_IopRun=2 GOW_PRIO_IopRun=0 GOW_IOP_SPIN=0`): lead waits 1.3-2.5 s per 5 s next to the VU1
    worker: worst.
  None beats inline: the thread moves ~0.9 s off core 1 but core 1 is no longer the limit, and every thread
  variant was 1-3 fps lower in the later fight (deko3d slower with `IopRun` beside it). **Inline stays the Switch
  default**; with the thread on, boost 1 + `GOW_IOP_SPIN=0` + a large `GOW_IOP_LEAD` is the best setting (spinning
  at a higher priority would hold the deko3d thread off core 0).

| build / env (package in port/switch/nsp/out) | title | heavy fight | later |
|---|---|---|---|
| before: 9996088 (`update_dkd5`, docs above) | 39.0-40.2 | 18.6-26.4 | 25.4-28.4 |
| sw-core1 (`update_core1b`), `GOW_GS_FASTPACK=0` | 38.6-40.8 | 19.2-23.0 | 24.4-27.0 |
| **sw-core1, defaults (fast path on, IOP inline), run a (`update_core1a`)** | **38.2-41.2** | **18.8-21.4** | **24.8-28.2** |
| **same, run b (`update_core1b`, installed, no env.txt)** | **38.2-39.6** | **19.0-21.8** | **25.6-32.4** |
| IOP thread core 0 boost 1 spin 0 | 36.0-38.8 | 17.0-21.0 | 24.4-27.9 |
| same + `GOW_IOP_LEAD=4194304`, run a / b | 37.0-41.0 / 36.4-40.2 | 17.4-20.4 / 17.2-20.6 | 24.0-26.4 / 23.4-27.0 |
| IOP thread on core 2 | 33.4-39.2 | 17.0-22.0 | 21.4-27.0 |

Per-core busy per 5 s, later fight, before (fast path off) -> after (defaults): core 1 EE `ee_ms` 3.6-3.8 -> 3.1-3.4
s (includes the GS stage, which preempts it), IOP inline 0.8-1.1 s (same), GS stage 1.28-1.58 -> 0.80-1.17 s, EE
waiting 0.3-0.4 -> 0.6-0.9 s; core 0 deko3d thread 2.1-2.4 s (same); core 2 VU1 worker 3.8-4.1 s (same).
Checks: PC zone gate `port/core1/gate/20261019_180634` 5/5 ok at 60 fps, speed 1.01, against
port/dkdraw/gate/20261019_165610; savetest (`port/core1/savetest`) 129 pages, card directory intact; check mode
`port/core1/gate-check/20261019_182616` (slot 2, `GOW_GS_FASTPACK=2`) 0 diffs; console fight screenshots
(scratchpad `core1/run_fp1a/fight_5.jpg`) look right (HUD, shadows, enemies).
Next lever: VU1 (core 2) and the EE -> VU1 dependency. The GS stage and the IOP are no longer on the critical path.
Handoff: worktree ext/PS2Recomp-kl-core1 (branch agent/sw-core1), PC build `out/build-pc` (VU1 blocks
`out/vu1pregen` = sw-vu1cyc's), WSL copy `/root/src-core1`, `/root/core1build.sh` -> `/root/gow-switch-core1`.

## Stream-buffer reuse and CLUT loads without a flush (ext agent/sw-vu1gpu 06b18e2, 2026-10-23, `update_con2`)
- **Stream storage pool** (gs_dk.cpp `storage()` / `retire()`): the backend's 8 MiB vertex stream is orphaned (GL
  `BufferData(nullptr)`) every ~8 MiB of vertices, ~90 times per 5 s in fights; each orphan used to be a
  `dkMemBlockCreate` of 8 MiB plus a `dkMemBlockDestroy` of the retired one (in the present path). Retired buffer
  storage of >= 1 MiB now goes to a pool of up to 4 blocks and is reused for the next storage of the same size.
  `[gs-dk-time] buf_new= buf_reused=` count it (fight: 0 new, ~90 reused per 5 s).
- **CLUT loads** (`GSHwBackend::LoadClut`): the pending batch reads the CLUT when it is drawn, so every CLUT load
  flushed it first (~130 per frame). When the palette comes from the memory image (no render target holds the CBP and
  the pending batch's target cannot span it: `clutSourceMayBeTarget`), the load goes first and the batch is flushed
  only when the palette changed (old palette restored for the flush). `[gs-hw-cpu] clut_kept=` counts the loads that
  kept the batch (fight ~2.7 k of 18.3 k). `GOW_GS_HW_CLUT_KEEP=0` restores the flush-first order.
- Console, same bot fight 150-190 s, update_con1 -> update_con2: GL thread busy 2.57 -> 2.17 s per 5 s; stream_ms
  541 -> 255, present_submit_ms 197 -> 35, draw calls 17.4 k -> 14.0 k, `[gs-gl]` clut op 836 -> 684 ms, gsq_wait 79 -> 3 ms.
  fps unchanged (28.0 / 27.2 vs 29.0 / 28.0; this run had ~4 % more VU1 runs per frame): the VU1 thread (core 2,
  4.35 s per 5 s) is the limit. Core 0 now has ~1.8 s per 5 s free.
- Note: the `[gs-gl]` `clut` op time is mostly the draws it flushes (the CLUT load itself is `[gs-hw-cpu] clut_ms`,
  ~10 us each); `?` is the `Kicks` op (strip/fan vertex runs, 360 k per 5 s).
