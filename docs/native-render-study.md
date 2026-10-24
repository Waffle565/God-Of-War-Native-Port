# Native rendering instead of the recreated VU1: study and migration plan (2026-10-24)

The question (user, 2026-10-24): replace the recreated VU1 path (VIF1 UNPACK into VU memory, the translated
microprograms with exact VU state, flags and cycle model, XGKICK'd GIF packets re-parsed by the GS stage) with
hand-written native code that does what each VU1 program does (transform, skinning, lighting, culling, packet
building) and feeds the deko3d renderer, judged by pixels instead of byte-exactness. The user's directive on the same
day: "you have to recreate everything, nothing has to be emulated or recompiled". So this is no longer one option
among several. The recompiled VU1 stays only as a temporary fallback and as the oracle (pixels, behaviour) while the
native code replaces it program by program, and then it leaves the shipped path.

This is a study. No code changed. The numbers come from the docs named below and from new x86 replay-harness runs
(WSL, `taskset -c 0-7`, harness `/root/vu1h/t4` = t4 blocks, `fight.rec` / `menu.rec`, 2000 records each, 2 passes;
scripts and outputs in the session scratchpad `nrs/`: `prof.sh`, `an.py`, `inv.py`, `fam.py`).

## 1. What the game runs on VU1

**One microprogram, streamed modules** (docs/vu1-gpu.md "The program"; confirmed here):
- Fixed code at 0x0000-0x2828. Three versions occur across the 3400 captured images in `port/vu1_micro` (2974 / 420 /
  6 images). fight.rec uses three (1973 / 23 / 4 records) and menu.rec one (a different version, in all 2000 records).
  Entries at 0x00 / 0x18 / 0x30 set vi1 = 0 / 341 / 683 (the triple-buffered input) and dispatch on a batch-header
  word (`ilw vi4, 69(vi12)`, `jr vi4`). Run shares on fight.rec are 45 / 31 / 24 % (runs) and 35 / 46 / 19 % (VU
  cycles). menu.rec is the same picture.
- From 0x2828 the micro memory holds **modules** in 0x350-byte slots, loaded by MPG while the game runs. A module is a
  prologue, a call of the batch-switch routine (XTOP, next batch's pointers / count, matrices vf20-23, lighting and
  bounding tests, XGKICK of the finished batch) and a software-pipelined vertex loop. fight.rec has 364 distinct full
  micro-memory images and 115 distinct module slot contents. menu.rec has 28 images and 8 modules. All 3400 captured
  images (PC zones + recordings) hold **322 distinct module contents**. Masking registers and immediates does not
  merge them into fewer families (the slot boundaries do not line up with the code), so the families below are by
  vertex-loop code.

**Where the VU cycles go, by vertex-loop code** (exact path, `GOW_VU1N_BLOCKPROF`, HLE off, one warm pass; the block
hash is position-independent; "modules" = how many distinct module contents in that recording contain the loop):

| loop code (pairs) | kernel today | what it does | fight | menu | modules (fight) |
|---|---|---|---|---|---|
| 570237771ea0bdea (31) | `skin_lit_st` | two matrices blended by a per-vertex weight, 3-light colour matrix, ST/Q | 31.9 % | - | 2 |
| 515a49593a9fb86f + 5797a1f6 (9+9) | `unlit_st` | transform, perspective divide, ST*Q, two-block cycle | 28.4 % | 15.9 % | 10 |
| 57dcf3d01a567c49 (22) | `lit_st` | lit textured, 3 lights | 20.0 % | 0.7 % | 6 |
| 9330611e7b80a713 / 4c9671fb (13) | `cull_swap` / `cull` | XYZ + strip winding cull (ADC), shadow volumes | 7.8 % | - | 14 |
| 5bb209bd93a479eb (9) | none | (unread) | 4.0 % | 9.2 %* | 10 |
| 476fc4cf0fe83040 (12) | none | (unread) | 2.9 % | - | 13 |
| 5181a0b08a6d0989 (28) | `skin_lit28` | two-matrix skinned lit, no divide (STQ.xy), menu Kratos | - | 55.9 % | 1 |
| a2eaa96b750883ce (11) | `unlit11` | unlit, Q of the previous pass (in the menu's fixed code at 0x2320) | - | 23.9 % | - |
| 40c1d3cda1601098 (13) | none | (unread) | 0.6 % | 2.6 % | 1 |
| batch front 0x60-0x45f | `scat*` (scatter loops) | XTOP, matrix loads, normal length, bounds tests | 3.9 % | 0.8 % | fixed |
| clip-flag cull loops 0cd8-0e2f, 1728-180f | `cull_clip` | clip test per strip point, accept / reject / partial | (in the module share) | | fixed |

\* with the HLE kernels on, what is left in the menu. With HLE on, what is left of the VU cycles: fight 476fc4cf 45 %,
5bb209bd 29 %, batch front 9 %, 40c1d3cd 9 %; menu 40c1d3cd 67 %, batch front 22 %, 5bb209bd 9 %.

**VU cycles are not wall time.** Console wall time of the VU1 thread (k10p sampler, heavy bench scene, docs/vu1-gpu.md
sw-k10 handoff): generated blocks/traces 25.9 %, chain steps 10 (chainStepOut 7.0 + slow parts 3.0), HLE kernels ~19
(kCullClip 6.1, kUnlitSt 4.8, kSkinLitSt 3.25, kLitSt 2.7, kCull 1.9), VIF1 decode + UNPACK 11.1, hleChain 3.3, the
GIF packet copy into the stage (memmove) 2.9 + 0.6, runNative 2.5, per-run entry / exit ~5, code-change handling ~4,
idle 5.3. The vertex math in the kernels is already plain NEON. **About 75 % of the thread is the emulation around
it**: dispatch, cycle and flag model, the batch code as VU instructions, VIF, packet copies. That 75 % is what a native
renderer removes. The 2026-10-23 "night 3" finding says the same from the other side: 93 % of the fight's VU1 thread
was outside the vertex loops, which is why the GPU vertex path was dropped.

**Thread budget now** (bench, k10c, GPU max): heavy 39.3 fps with the VU1 thread 4.75 s busy per 5 s (~24 ms per
frame); mid 50 fps and late 52.6 fps (~18-19 ms per frame). The VU1 thread is the limit in every scene. Core 1 (EE +
IOP, k10e1) is at 4.6-4.75 s per 5 s at 54 fps, and deko3d (core 0) at 3.2-3.4 s.

## 2. What a native path still has to provide

### EE side (game logic)
Nothing found that reads VU1 results back. A native path only has to keep the completion signals.
- **No VU registers or VPU_STAT read**: the game has no CFC2 / CTC2 (docs/performance.md, GS worker notes, verified by
  grep). MSCAL / MSCNT run on the FBRST captured at the kick.
- **No constant-address VU memory access in the EE code**: the only `lui 0x1100` in `port/output-usa` (7 sites:
  0x142618, 0x146258, 0x15de20, 0x15e698, 0x15ec80, 0x165970, 0x167a80) write 0x11000000 into a packet buffer
  (`sw` through the packet pointer kept at -0xE98(base)). That is the VIFcode FLUSH, not an address. Reads through a
  computed address are not ruled out yet (**unverified**: step 0 counts them).
- **The only EE-to-worker sync point that waits** in every `PS2X_GS_WORKER_DIAG` log is `sceGsSyncPath` (~300 waits
  per 5 s). No VU-memory or VIF-register site has ever shown up.
- **What the EE needs, then**: DMA completion of the VIF1 / GIF channels (CHCR.STR, D_STAT, the DMAC handlers) at the
  kick, as now; `sceGsSyncPath` returning once the chain's work is consumed; the **GS FINISH / SIGNAL / LABEL**
  register writes in the stream, in order. vid::WaitForDMAComplete (0x183878) waits for the FINISH interrupt
  (handler 0x182DD8 / 0x182EC8). So every non-vertex GIF register write the VU code or the VIF DIRECT path carries
  must still reach the GS state at its place in the stream.
- **VU cycle counts**: only diagnostics read them in this port (docs/vu1-gpu.md "CPU HLE kernels").
- **Cheapest end-to-end proof**: the deterministic bench's `[bench]` signatures (Kratos position, health, rand seed
  every 60 frames, docs/benchmark.md). If the game state stays identical for the 63 deterministic seconds with the
  native path on, the game logic does not depend on VU1 output in that scene.

### State carried between VU1 runs (what native code must reproduce while the hybrid lasts)
- VF / VI registers persist across MSCALs. Experiment (docs/vu1-hle.md, `GOW_VU1N_REPLAY_LIVEIN`): only 78 % of the
  next runs give the same output when the registers come from the previous run's start instead of its end. Matrices,
  light vectors and loop pipeline values live on between runs.
- The loops are software-pipelined: the first passes store values from before the loop, so the previous batch's
  last vertices go into the next batch's output.
- A shared data area (around qw 928-939) and the triple-buffered input areas (vi1 = 0 / 341 / 683) via TOPS / ITOP.
- XGKICK order relative to PATH2 / PATH3 (texture uploads, MSKPATH3) and to VIF DIRECT.

A fully native path keeps its own copy of this state (current matrices, lights, the strip's previous vertices). While
the recompiled VU1 is the fallback, a native run must leave the VU registers / memory that a later fallback run reads
the way the VU would. That is the main cost of the hybrid phase.

### GS stage / renderer: what it reads from the packets (ext `ps2xRuntime/src/lib/gs/gpu/gs_hw_backend.cpp`)
- **Vertex format**: XYZ ints (12.4 fixed, offset), 24/32-bit Z, ST / Q floats, RGBA (the VU emits 2^23-biased
  floats clamped at 255), the **ADC** bit (no kick: strip continue / culled triangle), the PRIM / register list of
  each GIF tag, A+D register writes (TEX0, CLAMP, ALPHA, TEST, FRAME, ZBUF, ...) copied from the batch header.
- **`rt.usedW / usedH`** (lines 2916-2927): the real drawn extent from the vertex maxima. findReinterpretSource uses it
  (1643-1645, 1771-1772) to decide texture reinterpretation of render targets. A native draw must report at least a
  conservative bounding box.
- **`isZInversion` / `flippedZ`** (2650-2662, 2947-2962): a full-screen bounding-box test that detects the game's Z
  inversion passes.
- **Shadow-volume wrap groups** (2491-2571): COLCLAMP=0 counting batches (the shadow volumes the `cull` / `cull_swap`
  modules build) drawn as a few constant-amount groups from per-triangle boxes, with texture barriers. Counting is
  sensitive to the exact set of triangles: a triangle more or less shows as a shadow error.
- **CLUT**: a palette change flushes the pending batch (~128 CLUT loads per frame, 85 % real changes). The native path
  must keep the order of draws vs CLUT / texture uploads.
- The soft-blend flat-colour check and the debug boxes also read vertex colours / positions.

## 3. Design options

All three replace the same thing: the VU1 run (dispatch + batch code + loop) and, as far as possible, the VIF1 UNPACK
into VU memory. The **input** stays the game's VIF1 DMA chain (the EE game code that builds it is a separate step). It
is read as data: native code decodes the commands this game uses (STCYCL, UNPACK formats, STROW / STMASK, ITOP /
BASE / OFFSET, MSCAL / MSCNT, MPG as "which module is loaded", DIRECT / DIRECTHL, FLUSH*, MSKPATH3) into batch
structures, without a VU memory image where none is needed.

### (a) Per-program native renderers emitting renderer draws directly
Native C++/NEON per module family: transform / skin / light / cull a batch (4 vertices per NEON op, SoA), build
`HwVertex` + draw state, hand them to the backend with the extents (bbox) the backend wants. No GIF packets, and the
stage does not parse the vertex data.
- VU1 thread (heavy fight, now ~24 ms per frame): the emulation share (~75 %) goes. What is left is the vertex math
  (today's kernels ~4.6 ms, ~2-3 ms in SoA form), the per-batch logic (~4000 batches per heavy frame at ~0.2-0.4 us:
  ~1-1.6 ms), chain decode (~1.5-2 ms instead of 2.7) and emission (~1 ms). **Estimate ~6-9 ms per heavy frame (2.7-4x)**,
  **unverified** until the first family runs.
- GS stage (core 3, ~60 % busy in the heavy phase): its vertex parsing for these draws goes (~180 ns per kick, 0.8-1.2
  s per 5 s in older fight measurements).
- GPU: no direct saving (same triangles), but the native path knows objects, so it can merge draws (14 k draws per
  5 s), drop ADC-culled vertices before upload, and render shadow volumes as a real stencil pass instead of the
  COLCLAMP counting emulation. **Unmeasured**: there is no GPU time split by kind yet (sw-k11 is adding
  `GOW_DK_GPU_TIME`).
- Risks: highest. The draw-state machine lives in the GS frontend (register writes in stream order), so the native
  draws must slot into the same ordered stream as the PATH2 / PATH3 uploads, CLUT loads and FINISH. The backend's
  heuristics (usedW/usedH, Z inversion, wrap groups, soft blend) are tuned to packet shapes. It is also the hardest to
  compare against the oracle (no common format).
- Effort: ~35-50 agent-days for full coverage.

### (b) Native code emitting compact GIF-equivalent records
The same native per-family code, but the output is what the VU program would XGKICK: GIF tags and PACKED vertices
(the same registers), or a compact "vertex block" record the stage expands. The stage and backend stay unchanged.
- VU1 thread: the same estimate as (a) minus the stage saving: ~7-10 ms per heavy frame. The memmove copy goes too:
  native code writes straight into the stage's packet buffer.
- GPU: unchanged.
- Risks: lowest. The output can be compared **per GIF write** with the oracle's XGKICK packets, using the existing
  tolerances (`tools/vu1_hle/hlecmp.py`: ST/Q rel 1e-4, RGBA +-2, XY +-2 = 1/16 px, Z +-4 or rel 1e-5, ADC exact).
  Ordering against other GIF traffic is preserved by construction.
- Effort: ~25-35 agent-days for full coverage. Moving from (b) to (a) later is ~5-8 days, since the per-family code is
  the same and only the emitter changes.

### (c) Hybrid: native for the top N families, recompiled VU1 for the rest
This is not an end state any more, but it is **how the migration has to run**. Per MSCAL, the native dispatcher looks
at the loaded module(s) and the batch headers. When every batch of the run goes to a covered family, the run is
native; otherwise the recompiled VU1 runs it, as today.
- Gain grows with coverage. In fights the top 4 families are ~88 % of the VU cycles and the top 6 ~95 %, but a run
  falls back when any one of its batches is uncovered, and the hybrid has to write back the live-out VU state (above).
  So the wall-time saving lags the cycle coverage until the tail is done.
- Risk: the live-out write-back. Mitigation: an oracle tool that records which registers and memory words each later
  run reads (an extension of `GOW_VU1N_REPLAY_LIVEIN`), so native runs write exactly those.

### At default clocks (GPU 307.2 MHz)
Native VU1 is necessary, not sufficient, for 60 fps at the default handheld clocks:
- With the VU1 thread at ~8-10 ms, the fight becomes bound by **core 1** (EE + IOP 4.6-4.75 s per 5 s already at
  54 fps, about 17 ms per frame, docs/vu1-gpu.md sw-k7 / k10e1) and by the **GPU**. At 307 MHz the fight was
  GPU-bound at 34-49 fps (avg 42, `la6d`, present_submit 0.75-1.8 s per 5 s) once the VU1 thread had got faster.
- **Expected fights at default clocks after a native VU1: ~42-50 fps (GPU-bound), not 60**, unless the renderer's GPU
  work drops by roughly a third and the EE + IOP drop by ~10-15 %. Option (a)'s draw merging and native shadow volumes
  are the only part of this work that can lower GPU time. Their size is unknown until the GPU time split exists.
- The title / menu already run at 60 fps; a native path saves power there.

## 4. Recommendation and migration plan

**Do (c) as the process with (b) as the output format, then switch the emitter to (a) once every family is native,
and then remove the recompiled VU1 from the shipped path.** (b) first because the oracle comparison stays per GIF
write and per batch, in the existing x86 / aarch64 harness on fight.rec and menu.rec, before any pixels are looked
at. (a) at the end because it is the only form that also takes work off the stage and possibly the GPU.

### Order of programs (by fight wall-time share, then reach)
0. **Step 0 (1-2 days)**: instrumentation and oracle.
   - A counter of EE loads / stores to 0x11000000-0x1100FFFF (VU0/VU1 memory) and EE reads of VIF1_STAT / TOP / ITOP
     (confirms "no readback", or names the sites).
   - Per-run live-out recording in the harness (which VU state later runs read).
   - A pixel-diff tool for bench screenshots at fixed frames (the bench is frame-deterministic for 63 s).
1. **Native chain decoder + batch framework (3-4 days)**: VIF1 commands into batch structs. The fixed code's batch
   switch / batch front (XTOP, header words, matrix loads vf20-23, bounds tests, scatter) as native C++. Dispatch by
   module hash to a native family with fallback. Output = GIF records into the stage buffer.
2. **`unlit_st` family (515a4959 + 5797a1f6; 10 modules, 28 % of fight / 16 % of menu VU cycles) end to end (2-3
   days)**: the simplest maths and the widest use, so it proves the framework.
3. `skin_lit_st` (32 %), `lit_st` (20 %): 2-3 days each.
4. The shadow-volume `cull` / `cull_swap` (14 modules, 8 %) and the clip-flag culling (`cull_clip`, 6 % of wall time,
   flag-exact today; a native version only needs the accept / reject / partial decision per strip point): 3-4 days.
   Check the shadows by pixels here (wrap-group counting).
5. Menu `skin_lit28`, `unlit11`, 40c1d3cd; fight 476fc4cf, 5bb209bd: ~1.5-2 days each.
6. **Long tail**: the rest of the 322 module contents seen in `port/vu1_micro`. First classify them by loop code over
   all captured levels (`port/levels`, PC zone captures). Keep a fallback counter per module hash in the shipped log
   until it reads zero over a full play-through. 5-15 days, depending on the number of families.
7. **Emitter to direct draws (a)**, with draw merging and native shadow volumes measured on the GPU (`GOW_DK_GPU_TIME`):
   5-8 days.
8. **Remove the recompiled VU1** (generator, native blocks, cycle model, kernels' exact-flag code) from the shipped
   build. Keep it in the harness as the oracle: 1-2 days.

Total about **30-45 agent-days**. Steps 2-5 can run in parallel forks once step 1 is in.

### How to verify each step
- **Harness, per family**: fight.rec / menu.rec (and the older ship / seq / PC-zone recordings) through the oracle and
  the native path. `hlecmp.py`-style comparison per GIF write with the tolerances above. ADC / triangle set exact
  (a different culled set is a visible error, especially for shadows). Live-out VU state exact where a fallback run
  reads it.
- **Game state**: bench `[bench]` signatures identical to the recompiled-VU1 run for the deterministic 63 s
  (`tools/bench_report.py --ref`). Any difference means the EE depends on VU1 output somewhere.
- **Pixels**: bench screenshots at the same game frames, native vs oracle. Mean absolute difference and the share of
  pixels off by > 8 levels, per scene. Thresholds to be set on the first family: differences should be edge pixels
  only. On PC the zone gate (5 zones, captures vs the previous gate).
- **Speed**: console bench (heavy / mid / late at default clocks, `[gsw-split]` vu_ms per run), plus the per-module
  fallback count.

### First concrete step
Step 0 plus the start of step 1 on the PC / WSL harness, without the console:
- the EE VU-memory / VIF-register access counter;
- the live-out recorder;
- a native decoder that turns fight.rec's records into batch structs and checks, per record, that it finds the same
  batch count / module per batch as the oracle's dispatch (`jr vi4` targets).

That decides the hybrid's granularity (per run or per chain) before any family is written. `unlit_st` is next.

## Sources
docs/vu1-gpu.md (handoffs sw-k10 ... night 3, "The program", "CPU HLE kernels", "GPU path"), docs/vu1-hle.md,
docs/performance.md ("VU1 / GS frontend pipeline", "VIF1 chain streaming", "VU1 thread throughput" 1 and 2, the GS
worker notes), docs/benchmark.md, docs/switch-deko3d.md, ext `ps2xRuntime/src/lib/vu/ps2_vu1_hle.inl` (kernel table,
lines 956-967), `ps2xRuntime/src/lib/gs/gpu/gs_hw_backend.cpp`, `ps2xRuntime/src/lib/ps2_memory.cpp`
(needsGsWorkerSync / waitGsWorkerIdle), `port/output-usa` (EE code grep), `port/vu1_micro`, the bench log
port/switch/logs/bench_k10e1.
