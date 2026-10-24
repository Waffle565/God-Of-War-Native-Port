# Native rendering instead of the recreated VU1: study and migration plan (2026-10-24)

## Handoff (2026-10-24 night, nat-vu1c stopped here: ext agent/natvu1 head 1139670, package update_nv1)
The native runs are wired into the game (off by default) and the clip path of 7ab32108 is native. Details in
"Progress: runtime hookup + clip path" (end of this file).

- **Switch**: `GOW_VU1_NATIVE_RUN=1` (=`m`: measure only). ext ps2_vu1_core.cpp `VU1Interpreter::run` offers every MSCAL
  run to `VU1NatAccess::tryRun` (ps2_vu1_nat.inl): native when covered, else the recompiled VU1 as before. Declined runs
  leave VU memory unchanged (undo log), so no 16 KB copy per run. Counters per entry family every 5 s (`[vu1nat-run]`).
- **Clip path** 0x0358 -> 0x0858-0x0c18 (tag scan, per-tag set-up, vu1hle::kCullClip, matrix reload, module
  callbacks) native and byte-exact. Coverage with nat-vu1d's entries (merged, 6f9c60a): natcheck **fight 73.4 %**
  (2621 / 2625 covered runs; 4 decline at 0x0940, the triangle clipper), **menu 94.3 %** (28 decline at 0x1278, the
  multi-tag batch); 0 packet / memory / register differences; declined runs leave memory unchanged (checked).
- **PC game check** (bench fight, PC build, `GOW_PERF_DIAG=1`; runs off1-3, m4, on1-5 in the session scratchpad `nv/`):
  - `[bench]` signatures: the PC bench also has two start states (seed at frame 60 c2f11671 = A or f72fa8ad = B, at
    random, positions identical). Same state: **on5 (A, native, final build) = off3 (A) at every frame 60-3780** (the 63
    deterministic seconds), on4 (B) = off2 (B) through 3120 (all of on4's frames). on3 (A) left off3 at frame 3240 (rand
    seed, position from 3360) but on5 and off3 / m4 agree there, so that was run-to-run (on3 vs on5 differ at 3240).
  - **Pixels**: bench shots every 300 frames (`GOW_BENCH_SHOT=300`) differ in 80-92 % of the pixels (mean abs 2.6-4.1
    per channel) between ANY two runs, recompiled vs recompiled too (off3 vs m4): the rain streaks and the lightning
    brightness are not deterministic on PC. Native vs recompiled is inside that band at every frame before the
    divergence (off3 / on3, off2 / on4); scene, geometry, HUD look the same. A byte-level picture check needs the rain
    masked; the byte-exact packets (natcheck) are the real proof.
  - **VU1 time** (PC: the EE is the limit at ~10-13 fps, VU1 ~5 % of a core): per run, same build, measure mode vs
    native (m4 / on4, fight frames 60-3060): 3d0d1206 **2.76 -> 0.97 us**, 7ab32108 **3.86 -> 1.05 us**, 93febeec 2.84
    -> 2.66 us, 39f38df4 **4.60 -> 5.44 us (slower native)**; covered runs 11.8 s recompiled -> 6.8 s native + 2.0 s
    for the 23 k declined runs (0x0940 / 0x1278, ~90 us each in the recompiled VU1); the other entries cost 7.0-7.3 s
    (15 us per run: b12c8560, non-0 start pcs). VU1 time per game frame: off3 5.16 ms / on3 5.30 ms (quiet machine), m4
    6.31 / on4 5.80 ms (with the Switch build running), **off3 5.16 / on5 4.43 ms (-14 %, final build, quiet machine;
    3.48 vs 4.06 us per run)**; on5 per run: 3d0d1206 0.72, 7ab32108 0.80, 93febeec 2.09, 39f38df4 4.26 us, declined
    68 us, other entries 11.8 us. fps: EE-bound (off3 10.09 / on5 10.58), no loss.
- **Package** `port/switch/nsp/out/update_nv1` (ext 1139670, t4 blocks `/root/vu1pregen-t4`, build `/root/gow-switch-nv1`):
  built, **not run on the console**. Console test: env.txt `GOW_VU1_NATIVE_RUN=1` (+ `GOW_PERF_DIAG=1` for the
  counters), bench vs the same package with `GOW_VU1_NATIVE_RUN=m` (same code, all recompiled + per-family times).
- **Next**: (1) console bench nv1 on / m (the VU1 thread is the limit there); (2) skin_lit_st native is slower than
  the HLE kernel (5.44 vs 4.60 us on PC): its loop with exact semantics vs kSkinLitSt's plain float; (3) the remaining
  15 us-per-run entries (b12c8560 is the next: its 0x0858 path with loops 0x0b38-0x0db0 **overlaps the clip path
  code here**: 0x0858-0x0940 / 0x0b38-0x0c18 are the same fixed code, so reuse the clip path's transliteration and
  its `clipHash` gate), the clipper 0x0940 and the multi-tag path 0x1278 (~90 us per run, 23 k runs per 300 s);
  (4) a kick sink straight into the GIF stage instead of the packets vector (one copy less; only once no decline can
  follow a kick).

## Handoff (2026-10-24 night, nat-vu1b stopped here: ext agent/natvu1 head a2d6559, worktree ext/PS2Recomp-kl-natvu1)
First native runs, harness only (WSL x86; nothing is wired into the runtime yet, so there is no GOW_ switch and no
game run). Details in "Progress: step 1 walker / step 2 first native runs" (end of this file).

- **Done**: `vu1nat::runNative` (ext ps2_vu1_nat_run.inl): whole MSCAL runs of the entries **3d0d1206** (unlit_st
  loop, perspective divide) and **7ab32108** (no-divide loop, switches to the unlit_st loop through the module
  callbacks) as hand-written C++: the batch switch's paths these entries take (0x00d8-0x03a0, colour set-up
  0x03a8-0x07e0, matrix scale 0x1120-0x11c8, loop calls, kicks), the bounding-sphere / plane tests, the culled
  batch. The batch walker `vu1nat::walkBatches` (header 69(ITOP), GIF tags at TOP + vi1).
- **Verified** on pcfight / pcmenu (3000 consecutive runs each; harness `natcheck`): every native run is
  **byte-exact** against the oracle (native blocks): GIF packets, all 16 KB of VU memory, vf1-31 / vi1-15 / ACC / Q /
  P / I / CLIP / end pc. hlecmp.py on the mixed dump (native where covered, oracle elsewhere) vs `check ... fast
  dump`: 0 data words differ, 602 883 / 525 067 GIF words exact. The next-run check (the following run's oracle
  result from the native end state) caught the one bug seen during the work (a missing ISW), so it is live.
- **Coverage now**: fight 1430 runs native = **21.2 % of the VU1 pairs** (3d0d 719 / 719 runs, 7ab 711 / 906), menu
  1322 runs = **11.0 %** (3d0d 204 / 220, 7ab 1118 / 1201). Declines: 0x0358 (the clip-test path, `cull_clip`
  loops 0x0980-0x0e28: 195 fight / 83 menu runs), 0x1278 (a batch of several GIF tags: 16 menu runs).
- **Speed** (x86 harness, same runs): native 2.6 us per run vs 38.6 us for the oracle, but the oracle figure includes
  the harness's per-run interpreter copy: not a game-speed number. Measure in the runtime.
- **Next**: (1) wire runNative into the runtime's VU1 run (ps2_vif1_interpreter / VU1 thread: per MSCAL, on copies
  or directly, fallback to the recompiled VU1 on false), off by default (`GOW_VU1_NATIVE_RUN=1`), then a PC bench run
  for pixels / fps / `[bench]` signatures; (2) the clip path 0x0358 (+ loops 0x0980-0x0e28) for 7ab32108;
  (3) the next entries by share: 93febeec (lit_st, 22.2 % fight), 39f38df4 (skin_lit_st, 19.9 %), 6df5c750 (menu
  skinned, 80.5 % of the menu).
- Tools: `natcheck <vifrec> [dump f]`, `GOW_VU1NAT_COV=<entry hash>` (pairs executed + path signatures per entry),
  `GOW_VU1NAT_TRACEPC=<run>` (+ `GOW_VU1NAT_TRACEVF=<n>`), tools/vu1_nat/natdis.py (vudis.py misprints the ACC forms
  of the upper specials, e.g. SUBAz as `subz vf1`: use natdis.py for transliteration), natblk.py (block view of a
  trace). The harness build script breaks on the space in the repo path: WSL symlinks `/root/gowr` (repo) and
  `/root/natvu1w` (worktree). Dumps: `/root/nrs/pc{fight,menu}_{ref,nat}.dump`.

## Handoff (2026-10-24 evening, nat-vu1a)
Steps 0 and 1 of the plan below, PC / WSL only (no console). Everything new is off by default (env switches).
Details in "Progress" (end of this file).

- **Done**: step 0 (EE access counter `GOW_EE_VUACC`, VIF1 stream + run recorder `GOW_VIF1_REC`, live-out recorder
  and per-GIF-write oracle dumps in the harness `tools/vu1_nat`) and step 1's decoder + run classifier (`vu1nat`, ext
  ps2_vu1_nat.inl). The batch walker and the native batch switch are not done; step 2 not started.
- **Verified** (consecutive PC recordings: 3000 runs of the bench fight + 3000 of the menu): no EE access to VU memory
  / VIF registers at all; the native VIF decoder reproduces VU data memory, micro memory and VIF state byte-exact
  before every run; the native dispatch (entry from the batch header) equals the oracle's in every main-program run;
  every run reads state the run before it left (packet areas, qw 928-942, vf3-19 / vf24-31 / P / CLIP / Q / vi9).
- **Decided**: fallback per run (MSCAL), keyed by (module slot hash, entry offset); a native run writes its packet in
  place in the VU image and leaves the live-out registers. Reasons and numbers in "Progress".
- **Next**: the native batch walker (header words), then the batch switch + `unlit_st` entries 7ab32108 / 3d0d1206
  natively, checked per GIF write against `check ... fast dump` with tools/vu1_hle/hlecmp.py.
- Build / run: PC build in `ext/PS2Recomp-kl-natvu1/out/build-pc` (Release; the git-ignored generated runner files
  were copied from ext/PS2Recomp-kl-natiop, same base 0927485; full build ~45 min); bench-fight PC run script
  `nv/pcrun.sh` in the session scratchpad (copied to the archive folder below); harness in WSL:
  `bash tools/vu1_nat/build.sh nv1 <worktree> <worktree>/out/vu1pregen/vu1n_generado`, recordings in `/root/nrs`.

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

## Progress

### Step 0 / step 1 tooling (2026-10-24, ext agent/natvu1 85a77ca+)
All off by default. Runtime (ext `ps2xRuntime`):
- **`GOW_EE_VUACC=1`** (ps2_runtime.cpp `eeVuAccess`): every EE load / store that reaches VU0 / VU1 micro or data
  memory (0x11000000-0x1100FFFF), the VIF0 / VIF1 registers (0x10003800-0x10003FFF) or the VIF FIFOs, through any
  computed address, counted by (op, physical address, pc, ra) and printed every 5 s (`[ee-vuacc]`, cumulative). VU0
  micro and VU1 data memory are not special addresses for the recompiled code's memory macros (their fast path aliases
  them into RAM at 0x1000000+); with the switch on, `PS2Runtime::isSpecialAddress` also routes the whole
  0x110xxxxx window to the runtime, so those accesses are counted too (and then reach the real VU memory, as on the
  hardware).
- **`GOW_VIF1_REC=<file>`** (`GOW_VIF1_REC_AT=<s>` wall seconds after the first VU1 run, `GOW_VIF1_REC_SKIP=<runs>`,
  `GOW_VIF1_REC_N=<runs>`, default 3000; ps2_vif1_interpreter.cpp `vif1RecRun`): the VIF1 command stream exactly as
  processVIF1Data consumed it, cut at each MSCAL / MSCNT, and for each run the VU1 state it starts with (execute()'s
  setup applied), the VIF registers, micro and data memory. Recorded on the VIF thread after `vu1AsyncWait`, so the
  memories are exactly what the run starts with. Format in the source comment. ~33 KB per run.
- **`vu1nat`** (ps2_vu1_nat.h / ps2_vu1_nat.inl, included by ps2_vu1_core.cpp): `VifDecoder` (the native VIF1
  decoder: STCYCL / OFFSET / BASE / ITOP / STMOD / STMASK / STROW / STCOL / MPG / DIRECT(HL) / MSCAL / MSCNT / every
  UNPACK format with masks, STMOD and fill / skip cycles, into its own 16 KB input image and micro image; DIRECT
  payloads listed in stream order), `classifyRun` (per run: module entry from the batch header, module slot hash,
  fixed-code hash, known loops), `families()` (loop-code table of section 1, none native yet).
- Harness (main repo **tools/vu1_nat**: `build.sh <name> <ext worktree> <vu1n_generado>` -> `/root/vu1h/<name>`):
  - `check <vifrec> [exact|fast] [dump <file>]`: per run, the decoder's image / micro image / VIF state against the
    recording (decoder state chained from the oracle's output of the run before + the stream in between); the native
    dispatch against the oracle's `jr` into the module; batch-switch calls and XGKICKs per run; oracle end state
    against the next run's recorded start state (registers persist); loops / module coverage. `dump` writes the
    oracle's per-run results in the `GOW_VU1N_REPLAY_DUMP` format, so `tools/vu1_hle/hlecmp.py ref test` compares a
    native path with the oracle per GIF write (ST/Q, RGBA, XY, Z, ADC tolerances).
  - `liveout <vifrec> [n]`: for each run, which registers (field poisoning) and which memory qwords written by the
    previous run and not rewritten by the VIF (qword poisoning) change its result: the state a native run must leave
    for a fallback run.
  - `torec <vifrec> <out.rec>`: runs as `GOW_VU1N_REPLAY` records for the older tools (blockprof, icount).
  - `rec2vifrec.py <rec> <out>`: an older `GOW_VU1N_RECORD` file (fight.rec, menu.rec: every 7th run, no stream) as a
    runs-only vifrec for the dispatch / classification part.

### Results on fight.rec / menu.rec (runs only, 2000 each)
- **Dispatch**: every main-program run dispatches exactly once, at its start (`xitop vi12; ilw.y vi4, 69(vi12);
  jr vi4`). The native entry (header word read from the input image) equals the oracle's jump into the module in
  **1973 / 1973** fight runs and **2000 / 2000** menu runs. The other 27 fight runs are not main-program entries
  (startPC outside 0 / 0x18 / 0x30, or a different microprogram: 4 runs dispatch through `jalr vi15, vi14` with
  vi12 = XTOP) and fall back. The dispatch site is found per fixed-code version (scan for the first JR after the
  entries, then the ILW that loads its register).
- **One module per run**: in the fight all 2000 runs return from the batch switch only into the entry's module slot;
  in the menu 102 of 2000 runs also jump into a second module slot. So the run is the unit of dispatch, but the
  per-run key must be the set of modules the run's batches use (the batch switch re-reads the header), not only the
  entry. Batch-switch calls per run are odd (1, 3, 5, ... up to 59): most runs 1-5.
- **The dispatch key is the entry code, not the module.** A module slot holds several loop variants and the batch
  header's word picks the entry (e.g. module 40c144db: 0x32f0 = transform + ST + RGBA without divide, 0x3218 = the
  `unlit_st` loop with the perspective divide). Each entry calls the fixed batch switch (`iaddiu vi14, vi0, 27;
  jalr vi15, vi14`, pair 27 = 0xd8: XTOP, header, matrices vf20-23, vi2 / vi3 / vi4 / vi5 = input, output, stride,
  count), runs its loop and returns with `jr vi14`. `classifyRun` hashes the entry's code up to that JR + delay slot
  (`entryHash`, position independent). fight.rec: **18 distinct entry codes** for 58 modules; by oracle VU pairs
  39f38df4 (skin_lit_st, 2 modules) 28.2 %, non-main programs 19.9 %, 7ab32108 (`unlit_st`, 9 modules) 15.8 %,
  93febeec (lit_st, 6 modules) 12.4 %, 3d0d1206 (unlit transform without divide, 9 modules) 10.4 %, then 13 entries
  for the last 13 %. menu.rec: 8 entry codes, 6df5c750 (skinned) 82.5 %, 7ab32108 11.4 %; 7 of the 8 also occur in
  the fight. The first 4 entries cover ~67 % of the fight's VU pairs and ~96 % of the menu's.
  But the entry code alone is not a safe key: the batch switch calls back into the module **relative to the return
  address** (`isubiu vi15, vi15, 5; jalr vi14, vi15` ...). The oracle's fixed-code jumps into the module land at
  entry -224, -208, -192, -184 ... -16, +0, +24, +112 bytes in the fight (the module's other routines: the second
  loop variant, the lane writers). So the safe dispatch key is **(module slot hash, entry offset in the slot)**, with
  the entry hash as the grouping of keys that share code; a native family is registered for a list of such keys,
  each verified in the harness. In the menu, the 102 runs that touch a second module jump far outside the entry's
  slot (-5000 ... +5632): the batch header names more than one module there, and the native batch walker has to read
  those header words before it can decide (open).
- **Loop table is not a family table**: the 28-pair block 5181a0b08a6d0989 (`skin_lit28`) occurs in modules that also
  hold `skin_lit_st`, `cull`, `5bb209bd` and `476fc4cf` loops. Coverage by loops found per module: fight runs
  unlit_st 55.9 %, skin_lit_st(+5181) 17.8 %, lit_st 15.2 %, others < 2.3 % each, none 2.0 % (but 20 % of the VU
  pairs: the non-main programs are long); menu unlit_st 44 %, skin_lit_st(+5181) 45.8 % (82.5 % of pairs). Native code
  will register per module hash (58 distinct modules in fight.rec, 6 in menu.rec) with a family behind it.
- **Oracle**: the exact interpreter and the native blocks (PC pregen of 0927485) agree: fight.rec 382 300 GIF words
  identical, data memory identical, only the sticky STATUS flags differ (the native blocks do not keep them; nothing
  reads them); menu.rec 342 142 GIF words identical, 45 within tolerance (1-ulp ST / Z, the known native-vs-exact
  rounding cases of docs/vu1-native-flags.md), 253 data words and a few vf15-17 lanes 1 ulp apart. The harness must
  set the VU rounding mode (round toward zero) itself, as execute() does: without it the exact interpreter gave
  1-ulp differences in every run.

### Results on consecutive PC recordings (2026-10-24, natvu1 build, bench fight script)
Recorded with the PC build of ext 42f2f0a (out/build-pc of the worktree; `GOW_VIF1_REC_N=3000`): `pcfight.vifrec`
(gameplay, bench frames ~600-700 = the heavy 10-20 s scene; 3000 consecutive runs, 3.9 MB VIF stream) and
`pcmenu.vifrec` (title / main menu, 3000 runs, 5.0 MB stream). Copies: WSL `/root/nrs/`, archive
`D:\Claude Archive\GoW-Archive\natvu1-2026-10-24\` (game data: keep out of git). Oracle dump of the fight:
`/root/nrs/pcfe.dump` (exact interpreter, REPLAY_DUMP format).
- **EE readback (step 0): none.** `GOW_EE_VUACC=1` over the fight run (160 s: boot, menus, gameplay) and the menu
  run (95 s): no EE load or store reached VU0 / VU1 memory, the VIF0 / VIF1 registers or the VIF FIFOs (no
  `[ee-vuacc]` line; the counter prints the first access at once). With the constant-address grep of section 2 this
  closes "no readback": the EE needs only the DMA completion and the GS FINISH / SIGNAL / LABEL writes. (No positive
  control was run: the game makes no such access to trigger the counter.)
- **Native VIF decoder: byte-exact.** Fight and menu: the decoder's input image equals the recorded VU data memory
  before **every one of the 2 x 2999 runs** (0 qwords differ), micro images (MPG) identical, VIF state (CYCLE, MASK,
  MODE, ROW, COL, BASE, OFST, TOPS, ITOPS, TOP, ITOP, DBF) identical, every segment ends exactly at the recorded MSCAL
  / MSCNT with the recorded start pc. Fight: 17 639 UNPACKs (643 770 qw), 48 DIRECT (40 KB), 62 MPG; menu: 17 257
  UNPACKs, 213 DIRECT (1.19 MB), 69 MPG. (The decoder's image for run k+1 is the oracle's output memory of run k plus
  the decoded stream in between, so this also shows the VIF writes never race the running program here.)
- **Oracle**: the exact interpreter's end state equals the next run's recorded start state in 2999 / 2999 fight runs;
  in the menu in 2464 / 2999 (535 runs 1 ulp apart in vf15-17, 366 runs with a data qword 1 ulp apart: the game ran
  the native blocks, and these are the known native-vs-exact rounding cases). With the native blocks as the oracle
  (`check ... fast`) the menu is 0 / 2999 on both. hlecmp exact vs native: fight 602 883 GIF words identical; menu
  525 035 identical + 32 within tolerance. So per-GIF-write checks use the native-block oracle (`fast dump`).
- **Dispatch**: fight 2987 / 2987 native entries equal the oracle's (13 runs of other programs fall back), menu
  3000 / 3000. Fight: one module per run in all 3000 runs; menu: 121 runs also use a second module. Entry codes:
  fight 13 (93febeec lit_st 22.2 % of VU pairs, 7ab32108 unlit_st 21.2 %, 39f38df4 skin_lit_st 19.9 %, 3d0d1206
  unlit transform 10.9 %, other programs 8.3 %, b12c8560 5.1 %, ...), menu 10 (6df5c750 skinned 80.5 %, ...).
- **Live-out (what a run reads of the runs before it)**: every run reads memory the run before it wrote (fight and
  menu 2999 / 2999): the three output / packet areas qw 0-243, 341-585, 682-926 (the packet a run kicks is built in
  place over the previous one: the software-pipelined loops' first passes and the parts a batch does not rewrite),
  the shared words qw 928-929 (2986 runs: the batch switch's `isw.z vi13, 928(vi0)` etc.) and 931-942. Registers read
  across runs: vf11-vf19 and vf24-vf31 (lights / colour matrices / camera, 1300-2700 of 2999 runs), vf3-vf9 (pipeline
  values, 60-660 runs), P and CLIP (1490 / 2690 fight runs), vi9 (490), Q (68); vf20-23 / vi4 / vi11 / vi13 only in
  the other programs' runs. (vi0 / vf0 show as live only because they are constants: ignore.)

### Decision: fallback granularity = per run (MSCAL)
- Every main-program run dispatches once, at its start, from the batch header in the input the VIF just delivered,
  and the decoder reproduces that input exactly, so the native side can decide per MSCAL before the run starts.
- Runs of one DMA chain mix many entries (fight: 13 entry codes in 3000 consecutive runs), so a per-chain fallback
  would send a whole chain to the recompiled VU1 for one uncovered entry and cut the coverage to the share of chains
  without any uncovered run.
- The live-out does not argue for chains either: registers and the packet areas flow from run to run across chain
  boundaries too. What it requires instead: **a native run must leave the VU state a later recompiled run reads**:
  its GIF packet written into the image in place (option (b) output = the packet in the VU memory layout, handed to the
  GS stage as the XGKICK is today), the shared words 928-942, and the registers above (vf3-19, vf24-31, P, CLIP, Q,
  vi9) as the VU leaves them. The harness's `liveout` mode lists them; the hybrid's check is the next run's oracle
  result after a native run.
- Per-run key: (module slot hash, entry offset), plus all modules the run's batches name (menu: 4 % of runs use a
  second module).

### Next steps
1. Native batch walker: the header words at 69(ITOP) .x / .z / .w and the sub-batch headers at TOP + vi1 (count,
   matrix offset in vf1.w, flags), so the native side knows the batches, their modules and counts before the run
   (check: the oracle's batch-switch calls per run, odd 1-59 in the histograms, and the menu's second-module runs).
2. Native batch switch (fixed code 0xd8-0x12a8 of the main version: XTOP, matrices vf20-23, bounds / clip tests, GIF
   tag and A+D, XGKICK) for one entry first: 7ab32108 (`unlit_st`, 21 % of the fight's pairs) and 3d0d1206 (the
   no-divide variant in the same modules, 11 %), checked with `check ... fast dump` + hlecmp per GIF write on pcfight /
   pcmenu, and with the next run's oracle result after the native run (live-out).
3. Then the step 2-5 order of the plan, with the entry hashes above as the list to cover.

### Progress: step 1 walker / step 2 first native runs (2026-10-24 night, nat-vu1b, ext a2d6559)
**Run structure** (main fixed-code version of the fight and the menu, the same in the ranges below):
- Entry 0x00 / 0x18 / 0x30: vi1 = 0 / 341 / 683, vi12 = XITOP; `ilw.y vi4, 69(vi12)`; `jr vi4` into the module
  entry, which does `iaddiu vi14, vi0, 27; jalr vi15, vi14` (batch switch at 0x00d8, vi15 = entry + 0x18 = its loop)
  with the loop's prime count in the delay slot (`iaddiu vi8, vi0, 3` / `1`).
- Header qw ITOP + 69: .x flags (low nibble cleared and written back; bit 1 = 2: other path at 0x0188, bit 13 =
  0x2000: second plane pair / clip test, sign: the header-negative path to 0x1120, else the colour set-up at 0x03a8),
  .y module entry (pairs). The block at ITOP (from qw ITOP) is itself a GIF packet (A+D state) kicked before the
  vertices; 64-68(ITOP) hold the scale / bounds vectors, 73-78 colour constants, 79-85 are written by the batch
  switch, 16-19(ITOP + tag.w) the matrix.
- **Sub-batch header = the vertices' GIF tag** at TOP + vi1: .x NLOOP | EOP (bit 15), .y low nibble = qwords per
  vertex (vi4, the loop stride), .w = matrix offset. The vertices follow it and the loop writes the output in place
  over the input (the packet kicked is this tag + its NLOOP vertices). qw 244 after the tag: the bounding sphere.
- The loops run twice per batch: a prime pass of vi8 vertices into the scratch qw 930+ (fills the software
  pipeline; this is why qw 931-942 are live between runs), then NLOOP vertices from the tag (vi3 = vi11 = tag qw).
- Bounding sphere (0x01c8-0x02a0): centre through the matrix, radius * |row 0| (SQRT), then two plane pairs tested
  by MAC-flag equality (`fmeq vi, vi6` with vi6 = 16: "only w negative" of madd / msub results; the flags read are
  those of the FMAC op 4 pairs earlier). Outside -> 0x0088: kick the ITOP packet and an empty EOP tag, end. With bit 13,
  inside the second pair too -> the module callbacks at 0x02d8 / 0x02f8 (7ab32108: they switch vi15 to the
  3d0d1206 entry's unlit_st loop, which sits 0xd8 before it in the same module), straddling -> the clip path 0x0358.
- Ends: E bit at 0x00c8 (culled), 0x1698 (header-negative path), 0x1340 (colour path); end pc = E pair + 16.
- **Walker check** (`check ... exact`, histogram (GIF tags at TOP + vi1, oracle batch-switch calls)): the calls
  are 1 + 2 x tags on the main diagonal (fight 2/5, 3/7 ... 18/37; menu up to 29/59): every tag is one batch with
  its prime and main loop call. Off the diagonal: 1/1 (culled: no loop call; fight 203, menu 575 runs) and +2
  (1/5: fight 1081, menu 776; 1/7, 2/7, 3/9 ...): one extra loop call per run, the clip path's second pass (not
  read yet). So the walker knows the batches before the run; the per-batch path (culled / clip) needs the sphere test.

**Native runs** (`vu1nat::runNative(state, mem, code, startPC, packets, declinePc)`, ext ps2_vu1_nat_run.inl):
- Hand-written C++ per VU instruction group of the paths above, with the native blocks' float semantics (vu1n:
  operands normalised, results clamped, MAC of the unclamped result, round toward zero set by the caller), the
  loops as plain functions (kUnlitSt of ps2_vu1_hle.inl for the unlit_st loop, `noDivLoop` for the 7ab32108 loop;
  the lower unit's store of a register the same pair's upper op rewrites stores the old value). Pipeline effects
  that matter are written out: the MAC flags read 4 pairs after the op, SQRT Q ready at the MULQ 7 pairs later, the
  DIV in the loop's delay slot read by the next block.
- Gates: the FNV of the fixed-code ranges transliterated (0x0000-0x0420, 0x1120-0x11d0, 0x1648-0x16a8 =
  57882ece8537340c, fight and menu), the entry code hash, for 7ab32108 also the 3d0d1206 code 0xd8 before it. Any
  other branch returns false with the pc (the caller works on copies and falls back to the recompiled VU1).
- Results: see the handoff at the top (byte-exact on every covered run, 21.2 % / 11.0 % of the VU pairs).

### Progress: runtime hookup + clip path (2026-10-24 night, nat-vu1c, ext agent/natvu1)
**Runtime hookup** (`GOW_VU1_NATIVE_RUN=1`, off by default; ext ps2_vu1_core.cpp `VU1Interpreter::run`, ps2_vu1_nat.inl
`VU1NatAccess::tryRun`):
- Every run that comes from `execute()` (an MSCAL, marked by a thread_local in ps2_vu1_core.cpp; MSCNT resumes never)
  is offered to `vu1nat::runNative` first, when the VU is idle (no pipeline / XGKICK / branch pending, start pc 0x00 /
  0x18 / 0x30). It works on a copy of the VU1State and **in place on VU memory**; true: the state is taken, the kicked
  packets go to the GIF (PATH1, `submitGifPacket`, one call per EOP-terminated packet, in kick order, after the run),
  the program counts as ended (E bit) and `m_cycle` moves on by 256 so the native blocks' readiness / flag state of the
  run before is in the past. false: the recompiled VU1 (native blocks / exact engine) runs the MSCAL as before.
- **Declined runs leave memory unchanged** (`nrun::Undo`): the batch code's single stores (`Vu::isw` / `Vu::sq`) log
  the old qword; the vertex loops' stores are not logged, so every decline after a loop is decided before the first
  loop (the second loop's count, the EOP / multi-tag test 0x1278, header bit 13 at 0x12d8; vi9 / vi10 are not written
  by the loops). The clip path, whose decline at 0x0930 depends on the cull loop's result, snapshots the 16 KB first
  (only clip runs, ~7 %). natcheck checks it: a declined run's memory equals the input (0 bad on pcfight / pcmenu).
- Float mode around the native run: round toward zero + FTZ / DAZ, as the native blocks (`kRoundZero | kFlushZero`;
  natcheck the same).
- Hashes cached per micro-memory generation (`PS2Memory::getVU1CodeGeneration`): the fixed-code gate (~1.5 KB of FNV)
  and the entry kind per entry pc (~550 bytes), so a run costs no hashing unless the code changed.
- Counters every 5 s on stderr: `[vu1nat-run] window native N runs ms (culled, kicks) | recompiled N runs ms + other
  entries N runs ms | not offered N | decline attempts ms | <family> nat N x us, rec N x us [declined pc:runs];` per
  family (3d0d1206, 7ab32108, 93febeec, 39f38df4, 6df5c750: the entry kind cached per generation). "rec": the
  recompiled VU1's time for the family's runs (declined ones in mode 1); "other entries": entries without native code;
  "not offered": MSCALs the hookup does not take (a pipeline still pending, ...). **`GOW_VU1_NATIVE_RUN=m`**: measure
  only, every run recompiled and timed per family: the baseline for the native times on the same build.
- `GOW_BENCH_SHOT=<n>` (gow_overrides.cpp, main repo port/overrides-usa): every n bench frames the VIF1 / VU1 worker and
  the GS stage are drained and the latched frame is written to `bench_shot_<frame>.ppm` in the working directory.

**Clip path** (entry 7ab32108, header bit 13 with a sphere straddling the second plane pair; 0x0358 -> 0x0858):
- 0x0358: `vi1 = vi10 + vi10; ibltz vi1, 0x0388` (header bit 14: no clip test) else `b 0x0858`.
- 0x0858-0x08e8: vi8-11 saved to 929.xyzw, tag scan (vi11 = sum of NLOOP into 69(ITOP).w, vi2 + 1 into 69(ITOP).z,
  vi6 = tags - 1). Per tag 0x0c20-0x0cd0: stride vi4 = tag.y & 255, count vi5 = tag.x & 255, matrix vf24-27 at
  16-19(ITOP + tag.w), vf20-23 = clip constants from 64-66(ITOP) (I = 2047.5 / 0x3a000800), first vertex in vf31 /
  vf29; then the cull loop 0x0cd8-0x0e10 = `vu1hle::kCullClip` (entry 0: B1 first, so no flag ring is read; local
  rings take its last writers; its clip flag becomes the state's CLIP), exit 0x0e18 (vi2 -= 2 x stride), 0x08f8 next
  tag. The loop writes the ADC word 0x8001 into the w of vertices outside and sets 929.z bit 1 for a triangle partly
  outside.
- 0x0908: 929.z bit 1 -> 0x0940, the triangle clipper (0x0940-0x0b30 and the big paths 0x1700-0x2380 seen in run 81):
  **declined** (4 of 1625 covered fight runs). Else 0x0b38: the batch switch's matrix set-up again (vf1 = tag, vf20-23
  from 16-19(ITOP + tag.w), vf3 = row 0 squared), the module callbacks as at 0x02c8 (vi15 -> the 3d0d1206 entry's
  unlit_st loop, vi8 = 3, vi10 bit 13 cleared), back to 0x0388 (header-negative matrix path or colour path).
- Gate: its own FNV (0x0858-0x0940 + 0x0b38-0x0e30 = b0e2569eee8738b3), so a version differing only there still
  runs the rest natively. 3d0d1206 runs never take the clip path in the recordings (declined if they do).
- **natcheck** (pcfight / pcmenu, 3000 runs each): fight 1621 of 1625 covered runs native (declined: 4 at 0x0940),
  **31.3 % of the VU pairs** (was 21.2 %); menu 1393 of 1421 (28 at 0x1278, the multi-tag batch), **13.8 %** (was 11.0
  %); 0 packet / memory / register differences, 0 declined runs with memory changed; hlecmp.py of the mixed dumps vs
  the reference dumps: 0 data words differ (602 883 / 525 067 GIF words exact). natcheck also prints the first three
  runs per decline pc (`run <k> declined at <pc>`, GOW_VU1NAT_TRACEPC targets).

### Progress: lit and skinned entries (2026-10-24 night, nat-vu1d, ext branch agent/natvu1d, worktree ext/PS2Recomp-kl-natvu1d)
Branch agent/natvu1d = agent/natvu1 (c606296, nat-vu1c's runtime hookup) + 3 commits (head 6f9c60a). Own file
`ps2_vu1_nat_lit.inl`, included by ps2_vu1_nat_run.inl after the nrun helpers; runNativeBody hands every entry it does
not cover itself to `vu1nat::runLit` (one line), the natcheck gate asks `vu1nat::nlit::covers` (one line).
- **Entries**: **93febeec** (lit_st), **39f38df4** (skin_lit_st), **6df5c750** (skin_lit28, the menu Kratos). All three
  prologues are `iaddiu vi14, vi0, 28; jalr vi15, vi14; iaddiu vi8, vi0, 1`: the batch switch is entered at
  **0x00e0**, one pair after `iaddiu vi7, vi0, 1`, so vi7 stays 0 (set at 0x0070) and 0x0388 always goes to the colour /
  lighting set-up at 0x03a0, whatever the header sign.
- **Fixed code** as a pc state machine (blocks 0x03a0-0x0850, 0x0e30-0x1118, 0x11d0-0x1350, 0x13f8-0x14e8, the clip
  pass 0x16a8-0x1800 / 0x1bb0 / 0x1570 / 0x2310): colour set-up with the light matrix (0x0500-0x0698: lights vf16-18,
  the second set vf24-26 for skinning; scale from P = 1/|row 0| or from ERSQRT / RSQRT of the light vectors), the second
  matrix vf12-15 (0x0428 / 0x07e8), the same set-up again for every further GIF tag of a batch (0x0e30, entered from the
  tag merge 0x1278 / 0x1468: the next tag's NLOOP is added to the first tag, its .w names the matrix), the
  doubled-stride loop calls 0x13f8 (skin modules), the kicks 0x12b0-0x1348. Version gate: FNV of those ranges
  (`nlit::kFixedValue` 356ebd77e6dbab0c: the fight's and the menu's fixed code are identical there; they differ only in
  0x24d8-0x2820).
- **Menu clip pass** (6df5c750, header bits 13 + 14; 0x0358 -> 0x0388 when bit 14 is set): after the ITOP kick
  (0x12e0) the output strip is tested again per vertex (0x1728 loop): the position against two scaled frusta (vf22 /
  vf23 from qw 64-66(ITOP)), CLIP of both into the clip flags, FCGET; the triangle of the last three vertices outside one
  plane of the first test gets its ADC bit (`sq.w vf1` with 0x8000, 0x1bc8); a triangle partly outside goes to the
  clipper at 0x1808 (never seen: declines). The FMAND at 0x1770 reads the MAC flags of 0x1750 (addz.w, sign of w);
  the zero case (0x17a0) never occurs and declines. Then `jr` to the return address in qw 928.x (1049 = 0x20c8, left by
  the loop's exit delay slot), `bal` 0x2310 -> 0x1570: two passes of the fixed-code loop 0x2320 (perspective divide, Q
  of the previous pass), xitop, the final kick at 0x1338.
- **Loops**: own exact versions (`nlit::litLoop`, `skinLoop`, `skin28Loop`, `unlit11Loop`: operands normalised,
  results clamped, DIV as vu1n::divv), same pass structure as vu1hle::kLitSt / kSkinLitSt. The HLE kernels are plain
  float and differ where the prime pass reads stale input (huge / denormal words): its scratch output qw 931+ is read by
  later runs.
- **FTZ + DAZ**: the native blocks run with flush-to-zero and denormals-are-zero (`vu_fpmode::kFlushZero`) besides
  round toward zero, and a native run must too (a denormal vertex word times a negative matrix lane gave -0 instead of
  +0, so Q = -FLT_MAX instead of +FLT_MAX in a prime pass). natcheck and the runtime hookup (`VU1Interpreter::runNative`
  in ps2_vu1_nat.inl) now set it.
- **Undo** (nat-vu1c's nrun::Undo): the batch code's stores are logged; before the first loop call memory is snapshot
  unless no decline can follow (one tag with EOP, NLOOP > 0, no clip pass).
- **Result** (natcheck after the rebase, every covered run byte-exact: GIF packets, 16 KB VU memory, all registers;
  hlecmp of the mixed dumps vs `pc*_ref.dump`: 0 data words differ, every GIF word exact; no declined run left memory
  changed): **fight 73.4 % of the VU pairs native** (2621 / 2625 covered-entry runs: 93febeec 692 / 692, 39f38df4
  308 / 308, plus 3d0d / 7ab with nat-vu1c's clip path; 4 declines at 0x0940), **menu 94.3 %** (2644 / 2672: 6df5c750
  1251 / 1251; 28 declines at 0x1278 in 3d0d / 7ab runs). Before this round: 21.2 % / 11.0 %.
- **Left** (by share of VU pairs): fight: other programs (startPC not 0 / 0x18 / 0x30) 8.3 %, b12c8560 5.1 %,
  b66a3e7a 3.7 %, e4f3b5c3 3.1 %, ea6b4a01 3.1 %, c79fec55 1.5 %, three small ones 0.9 %; menu: b45e3bab 2.3 %,
  9ffe612e 0.9 %, b12c8560 0.8 %, 29a9fc10 0.5 %, the multi-tag path 0x1278 of 3d0d / 7ab (28 runs).

### Progress: the smaller entries + a third recording (2026-10-24 night, nat-vu1e, ext branch agent/natvu1e, worktree ext/PS2Recomp-kl-natvu1e)
Branch agent/natvu1e = agent/natvu1d 6f9c60a + 4 commits + a merge of agent/natvu1 1139670 (head c611ec4). Own file
`ps2_vu1_nat_misc.inl` (namespace `vu1nat::nmisc`), included by ps2_vu1_nat_lit.inl before runLit; runLit reaches it
through one-line hooks, so these entries run on nat-vu1d's pc state machine (same batch front, colour set-up, tag
merge, kicks).
- **Entries** (all with the lit prologue `iaddiu vi14, vi0, 28; jalr vi15, vi14; iaddiu vi8, vi0, N`: batch switch at
  0x00e0, vi7 = 0; only the prime count N differs, `nmisc::kEntries`). The module code that runs is mostly the loop of
  the *other* entry of the same module, reached through the batch switch's module callbacks (`vi15 -= 5; jalr`,
  `vi15 += 3; jalr` into the module: `jr vi14` / `jalr vi15, vi14` with an IADDIU / ISUBIU in the delay slot), which
  `nmisc::moduleCall` decodes from the code instead of hard-coding (nat-vu1c hard-coded the 7ab32108 case):

  | entry | N | path | loop (by the FNV of its code from vi15) |
  |---|---|---|---|
  | b66a3e7a | 1 | second plane pair inside -> callbacks (0x02a8); texGen 0x06d8 | texDivLoop: divide, ST = vf19 + vf16-18 * normal (env mapping) |
  | e4f3b5c3 / ea6b4a01 | 2 | clip path 0x0858 -> callbacks | windLoop<false / true>: divide, ADC from the triangle winding |
  | 1dcc471c / 2611f8a2 | 3 | direct | the same windLoop (the other entry of those modules) |
  | c79fec55 | 2 | clip path -> callbacks | unlitAbLoop: two blocks, divide, ST not divided |
  | 29a9fc10 | 3 | direct | unlitAbLoop |
  | b45e3bab | 1 | clip path -> callbacks | colDivLoop: divide, RGBA = min(vf30 * rgba + vf31, I) |
  | 9ffe612e | 2 | direct | colDivLoop |
  | b33a96d5 / 5e1e18b3 | 4 / 1 | direct / clip path -> callbacks | posAbLoop: two blocks, position only |

  The entries' own no-divide loops are never reached in the recordings and decline (`nmisc::loop` knows only the
  loops above).
- **Fixed-code paths added** (hooks in runLit): 0x02a8 (header bit 13, sphere inside the second plane pair: vi10 bit
  13 cleared, callbacks), 0x0858 (the cull_clip pass of nat-vu1c for lit entries, copied as `nmisc::clipPath` with the
  generic callbacks, back to 0x0388 -> 0x03a0; gate `CodeInfo::clipOk`; memory snapshot first), 0x06d8-0x0718
  (0x06c8: header bit 9 set, vi1 = vi10 << 6 negative: I = -0.5, vf19 = (0.5, 0.5, 0, 0), vf16-18 / vf24-26.xy = matrix rows * vf10.x: the ST matrix of
  the env-mapped loop), 0x0740-0x0758 (vi4 >= 0 at 0x0730: vf29 / vf7 (/ vf30) += I).
- **Timing facts the loops depend on** (exact against the oracle): windLoop's FMEQ (p2) reads the MAC flags of the
  previous pass's `msuby.x vf0` (p11, 4 pairs earlier); in the first pass the flags of the last FMAC before the loop
  call (runLit's `Op::mac`: the loop prologue's `addi.z vf7` is only 3 pairs before the FMEQ, too recent); its ADC word
  is `FMEQ(MAC, (w + 127) & 0x8080) - 1` stored by ISW.w. colDivLoop's DIV in p5 is read by the MULQ in p12 (exactly 7
  pairs: the new Q) and by p1 of the next pass. The two-block loops' MULQ reads the other block's DIV (7 pairs).
- **Runtime**: `nlit::entryKind` returns 3 + `nmisc::entryKind` for these (CodeInfo::kind 7..), cached per generation
  as the lit ones; `[vu1nat-run]` counts them as family `misc` (without it the hookup would not offer them).
  Not built on MSVC this round (the harness g++ build compiles the same ps2_vu1_core.cpp TU).
- **Third recording** `pcwalk.vifrec` (the natvu1 PC exe, new game from the title, a pad script that walks with the
  left stick and attacks (`walk.py`), `GOW_VIF1_REC_AT=150`: bench frames ~1100-1140; 3000 runs; archive
  `D:\Claude Archive\GoW-Archive\natvu1-2026-10-24\pcwalk.vifrec`, scripts in `nat-vu1e\`, WSL `/root/nrs/`). It is
  the same ship deck as pcfight at a later moment, not another level: 15 entry codes, the mix differs (other programs
  24.7 %, 93febeec 21.7 %, 7ab32108 17.1 %, 39f38df4 15.0 %, 3d0d1206 8.0 %, c79fec55 3.2 %, 1dcc471c / 2611f8a2 2.9 %
  each, e4f3b5c3 / ea6b4a01 2.0 % each). A recording of another level is still to do.
- **Result** (natcheck, every covered run byte-exact: GIF packets, 16 KB VU memory, registers; no declined run left
  memory changed; hlecmp of the mixed dumps vs the reference dumps: 0 data words differ, every GIF word exact):

  | recording | native share of VU pairs | was (nat-vu1d) | declines | left |
  |---|---|---|---|---|
  | pcfight | **85.8 %** (2875 / 2879 covered runs) | 73.4 % | 4 at 0x0940 | other program 8.3 %, b12c8560 5.1 % |
  | pcmenu | **98.4 %** (2930 / 2958) | 94.3 % | 28 at 0x1278 (3d0d / 7ab multi-tag) | b12c8560 0.8 %, 60641204 0.2 % |
  | pcwalk | **74.2 %** (2939 / 2945) | - | 6 at 0x0940 | other program 24.7 % (53 runs), 541e797b (2 runs) |

  pcwalk: in 2 runs (825 / 860, e4f3 / ea6b clip path with a one-vertex tag) the native CLIP (0x659590) differs from
  the harness oracle's (0x000590) but equals the recorded start state of the next run: the harness oracle does not seed
  its clip-flag chain from the recorded state, so this is an oracle artifact, not a native error (natcheck and check
  now print the CLIP values).
- **The "other programs" (fight 8.3 %, walk 24.7 %)**: not other entries of the main program but a **second
  microprogram** (MPG; 1376 of 2048 code pairs differ from the main program), run in bursts (fight runs 1557-1573, walk
  1070-1104 and 2203-2234), start pc 0x00 / 0x18 with no module entry in the batch header (entry word 0), 1.3-34 k VU
  pairs and 5-93 XGKICKs per run. Data driven: control words at 20-31(XTOP), per-element keys unpacked with
  ITOF12 / FTOI4 / MR32 / MTIR, per-track routines called through `jalr vi15, vi1` (0x04c8, 0x05a0, 0x0668, 0x06e8:
  interpolation snippets, EEXP), then a quad builder at 0x16d8-0x1d00 (OPMULA / OPMSUB cross products, ERLENG,
  CLIP, a DIV per corner, SQI into the packet): a particle / ribbon (trail) program, which effect is not identified.
  In pcwalk even the oracle (native blocks) does not reproduce the game for it: end state != next recorded start
  state in 8 of its runs (vf4) and the decoder image differs in 12 runs (22 qw, from qw 569), so the reference for it must be
  settled first (exact interpreter vs native blocks vs the recording).
- **Next**: (1) the particle program (the largest share left, 8-25 % of the gameplay VU pairs), reference first;
  (2) b12c8560 (fight 5.1 %: the A entries of the e4f3 / ea6b modules with nat-vu1c's clip code); (3) the 0x0940
  clipper (4-6 runs per recording), the 0x1278 multi-tag path of 3d0d / 7ab (menu 28 runs), 60641204 / 541e797b;
  (4) a recording of another level; (5) a runtime bench run with `GOW_VU1_NATIVE_RUN=1` (pixels, fps, `[vu1nat-run]`
  family `misc`), after an MSVC build of this branch.

### Progress: the second microprogram + b12c8560 (2026-10-24 night, nat-vu1f, ext branch agent/natvu1f, worktree ext/PS2Recomp-kl-natvu1f)
Branch agent/natvu1f = agent/natvu1e c611ec4 + 2 commits (07edeef, e6eec1e); agent/natvu1 (1139670) is already in it.
New file `ps2_vu1_nat_part.inl` (namespace `vu1nat::npart`); hooks: one include + one line in runNativeBody
(ps2_vu1_nat_run.inl), the family line of `VU1NatAccess::tryRun` (ps2_vu1_nat.inl: the program counts as family `misc`),
one entry + one loop hash in ps2_vu1_nat_misc.inl, the harness (check / natcheck) in ps2_vu1_nat.inl.

**The reference first** (pcwalk: oracle end state != next recorded start in 8 runs (vf4), decoder image != recording in 12
runs near qw 569). Cause: the program copies integer words with `max.x vf4, vf17, vf17` / `max.y vf4, vf18, vf18`
(vf17.x / vf18.y = 0x400, a denormal as a float; 0x1688 / 0x1690 of the rotated-sprite builder) and stores vf4.xy into the
packet (qw 569 / 799 = `sq.xy vf4, 5(vi11)`). Both harness engines give 0: the exact engine compares normalised operands
(denormal -> 0), the native blocks' `_mm_max_ps` runs under DAZ (`maxps(0x400, 0x400)` = 0 with DAZ, 0x400 without:
checked on this CPU). The game keeps 0x400, as the hardware does (MAX / MINI are an integer compare of the raw words:
PCSX2 mVU MIN_MAX_PS); most likely the MSVC build folds `max(a, a)` to `a` in the native block. Not a seeding, VIF or
recording problem. Fix of the reference: `g_vuHwMinMax` (ps2_vu1_upper.cpp, harness only, the runtime never sets it):
the exact engine returns the raw register for MAX / MINI of a register with itself. `check <rec> exact hwminmax`:
pcwalk end state != next start in 2 runs (the known CLIP seeding artifact of runs 825 / 860), decoder 0 differences;
pcfight 0 / 0. Only the same-register form is changed: it is the case observed, and there the hardware and the game
agree; for MAX / MINI of two different registers the game runs the native blocks' float compare, which the main
program's runs match. natcheck's reference for the second program is therefore
the exact engine + hwminmax; the native blocks differ from it in 12 pcwalk runs (counted, `second-program runs where
the native blocks differ`). The main program's reference stays the native blocks (pcmenu's exact engine differs from
the recording in 535 runs; fast in 0).

**The program** (MPG'd between main-program runs; start pc 0x00, or 0x18 to keep the packet double buffer of the run
before; gate: FNV of 0x0000-0x0770 + 0x0db0-0x0e48 + 0x10b8-0x1ed0 = 860b4468fbef0880, the same in pcfight and pcwalk;
their code differs only beyond 0x2c00). Data driven from the batch at XTOP:
- 20 random seed (vf30), 21.xy element range (+ XTOP), 21.zw / 31 the age terms, 22.w element stride, 22.y the builder's
  set-up (`jalr vi15`; it returns the builder's element routine in vi14, stored to 22.x), 22.z the builder's constants
  (GIF tag + 2-4 qw), 0-6 matrix / projection / size, an A+D packet at XTOP + 8 (kicked first).
- Per element: key words 23-30 are lists of 16-bit fields, one per lane (a negative field shifts the lane by 16 bits
  with itof12 / ftoi4 twice, 0 ends the list). 23 / 24 copy element qwords (xyzw / w) to XTOP + key; 25 / 26 the same
  with a random value per key (0x04c8: vf1 = a + b * rnd, vf30 = the next seed: a float LCG with the 128 / 2^16
  rounding constants); age vf31 = (t, t^2 / 2, ...) from 31(XTOP) and 21.zw, P = EEXP; w < 0 skips the element;
  29 / 30 pick a per-track snippet (`jalr vi15, vi1`: 0x0530 constant, 0x05a0 linear a + b t, 0x0600 the same clamped
  to 0-255, 0x0668 quadratic a + b t + c t^2/2, 0x06e8 clamped) on the data at XTOP + key 30, results to qw 265+;
  27 / 28 scatter them to the builder's inputs (absolute qw 546-549).
- Builders (set-up -> element routine; GIF PRIM of their tag): 0x10b8 -> 0x1128 two-vertex streak (sprite 0x156),
  0x13d0 -> 0x1488 rotated sprite (sin / cos polynomial 0x1540-0x1620 with range reduction; tri strip 0x154),
  0x16d8 -> 0x1748 velocity-aligned view quad (cross product of position and direction, ERLENG; 0x154), 0x19f8 -> 0x1a68
  quad from two axes (0x154), 0x1d08 -> 0x1db0 strip of vf17.z quads (untextured Gouraud fan 0x14d). CLIP of the
  corners against the guard band: culled -> 0x0db0 (no kick, vi6 -= 1); else EOP on the last tag (vi7), XGKICK,
  vi13 = 1356 - vi13 (563 <-> 793). E at 0x04b8 (end pc 0x04c8).
- Which effect (inferred from the packets, not from pixels): all textured, alpha-blended, UV-addressed particles
  (TEX0 per run in the A+D packet). The view quad builder dominates both recordings (fight 1000 elements in 10 runs,
  ~100 per frame; walk 2113): velocity-aligned streaks on the rainy ship deck, most likely the **rain**; rotated sprites
  (walk 548, splashes / sparks / smoke), two-axis quads, and the untextured fan strips (walk 6 runs, fight 1, when
  attacking: most likely the **Blades of Chaos swing trail**).

**Native** (`npart::run`): the element loop, the key lists, the LCG, the snippets, the age / EEXP, the five builders,
cull, EOP and kicks as sequential C++ with the vu1n float semantics (as the rest of ps2_vu1_nat_*), MAX of a register
with itself as a raw copy, EEXP / ERLENG with the exact engine's formulas (in SSE scalar ops). The pipeline effects the
program relies on are all "value ready" ones (WAITQ / WAITP, DIV 7 pairs before its MULQ, CLIP / MAC flags at least 4
pairs before FCAND / FMAND), so only the same-pair reads (a lower op reading a register its upper op writes: the old
value, e.g. EEXP of vf31.z beside `madd vf31`, `sqi vf5` beside `sub.w vf5`) are written out. Memory is snapshot first
(declines anywhere restore it); declines: other snippet / builder addresses, a strip of 0 segments.

**b12c8560** (fight 5.1 %): the B entry (module + 0xe8, lit prologue, vi8 = 1) of the 93febeec-like modules; its runs
take nat-vu1e's clip path (nmisc::clipPath) and the callbacks into the module's A loop, which is the lit_st loop
(`nlit::litLoop`, code hash 7c187fbcd9b8ba29 over 192 bytes): one `nmisc::kEntries` line + one `nmisc::loop` case.

**Result** (natcheck, every covered run byte-exact: GIF packets, 16 KB VU memory, registers; no declined run left memory
changed):

| recording | native share of VU pairs | was (nat-vu1e) | declines left |
|---|---|---|---|
| pcfight | **99.2 %** (2996 / 3000 runs) | 85.8 % | 4 at 0x0940 (the triangle clipper) |
| pcmenu | **99.1 %** (2963 / 2991 covered) | 98.4 % | 28 at 0x1278 (3d0d / 7ab multi-tag); 9 runs of 60641204 / other entries |
| pcwalk | **98.9 %** (2992 / 2998) | 74.2 % | 6 at 0x0940; 541e797b (2 runs) |

Native time on these runs (x86 harness, cold caches per run): fight 33 ms vs 121 ms for the oracle (native blocks).

**Another level** `pcathn02.vifrec` (natvu1 PC exe, new game, `GOW_WARP=ATHN02A` 0.5 s after gameplay start (arrived
11.8 s later, "The Gates of Athens"), `GOW_VIF1_REC_AT=110`, pad script zone.py; Kratos did not move after the warp
(`[bench] pos` constant), so it is the level start; 3000 runs; archive `D:\Claude Archive\GoW-Archive
atvu1-2026-10-24\pcathn02.vifrec`,
WSL `/root/nrs/`). Mix: 3d0d1206 32.1 % of the pairs, skinned 12.1 %, the second program 10.5 % (29 runs: 1271 two-vertex
streak elements of builder 0x10b8, 4 strip and 2 sprite runs), lit 6.4 %, c79fec55 2.1 %. Reference: decoder 0
differences; the exact engine + hwminmax ends != the next start in 8 runs (2004-2019: MAC / CLIP only, which natcheck
does not compare except CLIP), the native blocks in 2. natcheck: **94.9 % of the VU pairs native** (2975 of 2999 covered
runs), every covered run byte-exact except run 2019's CLIP (native 820000 = the recorded next start, the exact
engine 000000: the oracle's CLIP seeding artifact again); declines: **15 runs at 0x0940** (the triangle clipper, ~3 %
here) and 9 at 0x1278 (the 3d0d / 7ab multi-tag path); 541e797b 1 run.

**Tools**: `check <rec> exact hwminmax` (reference with the hardware's same-register MAX / MINI), `GOW_VU1NAT_DIFF=1`
(check prints the differing qwords: decoded / recorded / oracle end / previous start, and the differing registers),
`GOW_VU1NAT_COV=1` (coverage of the runs without a module entry, i.e. the second program), natcheck prints the second
program's code hash (`[vu1nat-part]`). Scratch: `partsum.py <vifrec>` (per run: builder, elements, GIF PRIM, the A+D
packet), `fdis.py` (disassembly with nops and branch targets), `vrx.py` (one run's code / data / state), `zone.py` +
`zrun.sh` (new game, GOW_WARP, walk, recording), archived in `D:\Claude Archive\GoW-Archive\natvu1-2026-10-24\nat-vu1f\`.
Not built on MSVC this round (the harness g++ build compiles the same TU).

**Next**: (1) the 0x0940 triangle clipper (4-6 runs per ship recording, 15 in Athens: the largest share left;
0x0940-0x0b30 + 0x1700-0x2380); (2) the 0x1278 multi-tag path of 3d0d / 7ab (menu 28 runs, Athens 9: nat-vu1c's code in ps2_vu1_nat_run.inl;
runLit's tag merge 0x1278 / 0x0e30 is the model); (3) 60641204 / 541e797b (lit prologue, vi8 1, own loops at entry + 0x18 not written yet: registering them alone
declines at the 0x1228 loop call); (4) an MSVC build and a runtime bench with
`GOW_VU1_NATIVE_RUN=1` (pixels: the rain / trails of the second program, fps, `[vu1nat-run]` family misc).
