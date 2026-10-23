# VU1 vertex loops off the exact path: CPU HLE kernels first, then the Switch GPU (ext agent/sw-vu1gpu)

User decision (2026-10-19): move the dominant VU1 vertex program to the Switch GPU (deko3d); not bit-exact, it must
look the same (pixel comparison against the CPU path, tolerances documented). Plan: (1) understand the program,
(2) a plain-float CPU reference (HLE) compared with the exact path on the recordings, which also says whether a fast
CPU HLE alone reaches the target, (3) GPU path, (4) console fps / per-core busy / screenshots.

## Handoff (2026-10-23 night 2: core 0 cut, vsync spin and VU1 helper assessed; ext agent/sw-vu1gpu 06b18e2, `update_con2`)
- **Core 0 (done, default)**: ext 06b18e2 (docs/switch-deko3d.md "Stream-buffer reuse"): the 8 MiB vertex-stream orphan reuses
  retired deko3d memblocks (was a dkMemBlockCreate/Destroy pair ~90 times per 5 s) and a CLUT load that leaves the palette unchanged
  no longer flushes the pending batch. Same bot fight 150-190 s: GL thread 2.57 -> 2.17 s per 5 s (stream 541 -> 255 ms, present
  280 -> 100 ms, draw calls 17.4 k -> 14.0 k), gsq_wait 79 -> 3 ms; core 0 total ~3.2 s per 5 s (~1.8 s free). fps 28.0 avg / 27.2 min,
  6 frames >= 2.3 fields per 5 s, title 55.4 (con1: 29.0 / 28.0, 1.2, 56.0): within run-to-run variance (this run had ~4 % more VU1 runs
  per frame); fps stays bound by the VU1 thread (core 2: gsw job 4.79 s, vu 4.42 s per 5 s = 96 %). Screenshots correct.
- **The `[gs-gl] clut` bucket was mostly draws**: the CLUT load itself is `[gs-hw-cpu] clut_ms` ~190 ms (10 us each); the rest of the
  44 us per op was the batch flush. Only ~15 % of the CLUT loads are unchanged palettes, so most flushes remain (the palette really
  changes per object). The remaining per-draw cost: draw_ms ~760 per 14 k draws (~54 us), Kicks op 0.8 s per 360 k strips.
- **Step 1 (vsync spin) is already in place**: `gowCountdownSkip` (gow_overrides.cpp, the two countdown back edges of 0x17A1D0 /
  0x17A2A0) jumps the countdown to the next scheduler deadline and the EE then sleeps in `processDueDeadlines` (condition variable,
  `guest_wait`). The profile's "39 % in the spin" are the IOP batches run at those checkpoints (iop bucket: real IOP work that has to
  run somewhere) plus the sleep (wait bucket); the spin's own code is ~3 %. Core 1's real slack is `guest_wait` ~1.3 s per 5 s, not 40 %.
  Nothing to change there.
- **Step 2 (coarse VU1 helper) assessed, not built**: earlier measurements already cover it: run-level speculation (`GOW_VU1N_REPLAY_LIVEIN`,
  docs/vu1-hle.md) gives the same output for only 78 % of consecutive run pairs (75 % with memory), so a helper running run k+1 early fails
  ~25-40 %; `PS2X_VU1_ASYNC=1` (VU1 programs on their own thread, VIF decoding overlapped) was slower (2 us VIF per 27 us run, every MSCAL
  waits); `GOW_VU1_SPLIT` (loop split) +24 %. A chain-level split has only ~2 chains per frame (`[gsw-chain]` chains=284 per 5 s) whose
  boundary is a run boundary with the same carried-state problem. Not worth a prototype; if someone wants a number first: per-chain
  vu_ms and whether chain 2's first run's live-ins match last frame's.
- **Next (per user decision 2026-10-19): the GPU vertex path** for the skin/lit kernels (kSkinLitSt is 22 % of the VU instructions; see
  "GPU path" below), judged by pixels. Core 0 now has ~1.8 s per 5 s free for the extra CPU-side work it needs. Core 1 (EE 2.05 s + IOP
  1.6 s + GS-stage share) caps ~45-50 fps even with VU1 free: 60 fps also needs EE/IOP time cut (EE 14 ms per frame, IOP ~5.5 ms at 60 fps).

## Handoff (2026-10-23 evening: log ring + GSStage core 0 = fights at 30 fps; ext agent/sw-vu1gpu ac30584, `update_con1`)
- ext ac30584 (on top of fast1 1b9a06d): GSStage thread on core 0 by default (`GOW_CORE_GSStage=1` restores core 1); stdout/stderr
  through a memory log ring written to the SD by a low-priority core-0 thread (`[switch-log]`); `[gsw-chain]`. Package
  `port/switch/nsp/out/update_con1`.
- Console, handheld, same bot fight: **fight 150-190 s 29.0 avg / 28.0 min** (fast1 24.0 / 22.2), frames >= 2.3 fields 1 per 5 s
  (was ~21); **title 51.5 fps** (47.6). `[switch-log]`: SD writes avg 4.8 ms, max 65 ms each, i.e. every log line used to stall
  the thread that printed it (EE speed lines every second, worker/stage/deko3d lines every 5 s): that was most of the hitches.
  With `GOW_EMC_MHZ=max GOW_GPU_MHZ=max` (opt-in, not default: battery/heat in handheld): title 57.0, fight 28.2 (no gain).
- Fight frames now sit at 1.9-2.1 fields (the game's 30 fps step). Budget per 5 s at 29 fps: VU1 thread (core 2) 4.35-4.44 s busy
  (~30 ms per frame: cap ~33 fps); core 0: deko3d 2.58 s (clut 0.82 s / 18.6 k ops, unnamed draw-op bucket 0.82 s / 360 k,
  texflush 0.32, vrun 0.29, present 0.28) + GS stage ~1.0 s = 3.6 s (72 %: cap ~40 fps); core 1: EE 2.1 s + IOP 1.6 s +
  pacing 1.3 s (a large part of ee+iop is the vsync spin with its IOP checkpoints, 39 % of EE samples in the profile), real
  game EE code ~14 ms per frame. EE chain walk+copy only ~110 ms / 5 s (~0.8 ms per frame): the EE->VU1 overlap prototype was
  dropped (<= 2-3 %).
- Next for 60 fps: VU1 needs ~2x AND core 0 needs ~1.8x. Recommendation: (1) VU1 work onto the core-1 slack (the vsync spin
  and pacing are ~40 % of core 1: make the spin sleep until the vblank event, then a coarse VU1 helper there, or the GPU
  vertex path for the skin/lit kernels if per-run splitting stays as unprofitable as GOW_VU1_SPLIT was); (2) core 0: the CLUT
  op (44 us each, ~128 per frame) and the per-draw op cost in the deko3d backend; the GS stage could share core 1 slack too.

## Handoff (2026-10-23 afternoon: console env tests, console agent)
- Same bot fight each run (cycle.sh PLAY=1 BOOT=45, 150 s), update_ee3, windows 150-190 s (later windows are often the death screen):
  baseline 23.2 fps avg / 22.0 min, 30 frames >= 2.3 fields per 5 s (`[gow-speed-hist]` bucket 23 is the cap);
  `GOW_CORE_GSStage=0` 25.8 / 22.4, 15 hitches; `GOW_IOP_THREAD=1 GOW_IOP_SPIN=0 GOW_IOP_LEAD=4194304` 24.4 / 22.7, 9 hitches;
  both 22.5 / 19.4 (core 0 overloaded: deko3d 2.0 -> 3.0 s / 5 s, gsq_wait doubles); GSStage=0 + IOP thread on core 1
  (`GOW_CORE_IopRun=1`) 24.2 / 17.4; GSStage=0 + `PS2X_GS_QUEUE_CHUNKS=32` 24.2 / 22.6 (gsq_wait 0.36 -> 0.29 s, no fps gain).
  Kept: GSStage on core 0 (now ext agent/sw-vu1gpu ac30584).
- update_fast1 (PIE, VU1 -O3, raw-FPCR div/sqrt) vs update_ee3, no env, console handheld (`[switch] mode handheld`, EMC
  1331 MHz, GPU 307 MHz): title 47.6 vs 47.3 fps, VU1 19.2 vs 21.0 us per run (-9 %); fight 150-190 s 24.0 / 22.2 vs 23.2 / 22.0,
  25.1 vs 25.7 us per run. fast1 + `GOW_EMC_MHZ=max GOW_GPU_MHZ=max` (EMC 1600, GPU 460.8 handheld): title 48.4, fight
  26.2 / 25.2 (150-250 s: 27.1 / 25.2, no death), deko3d 2.09 -> 1.80 s / 5 s, hitches 21 -> 18 per 5 s.
- `[ee-wait]` (update_ee3, `GOW_EE_SAMPLE`): in the fight ALL EE idle is cause 7 = pacing (1.0-1.6 s / 5 s), none is DMAC/vblank
  wait. With the VU1 thread 3.6 s / 5 s busy (~29 ms per frame) the VU1 path is the per-frame limiter; the EE gets ahead and
  sleeps in the pacing wait.
- `PS2X_GS_WORKER_TRACE` on the console is unusable as is: consecutive trace events from three threads are ~56 ms apart
  (only 30 events in 2 s). stderr is unbuffered to the SD file, so each line seems to block the printing thread for tens of
  ms. Fix prepared (85bd55d): stdout/stderr go to a 1 MiB memory ring that a priority-0x3B thread on core 0 writes every
  100 ms; `[switch-log]` every 30 s reports the SD write cost (avg/max ms). If confirmed, every speed line on the EE, stage or
  worker thread was a potential hitch. Also new: `[gsw-chain]` (EE time to walk+copy the DMA source chains before the worker
  job is queued, per 5 s).

## Handoff (2026-10-23 late night: cheap compiler/code fixes, package `update_fast1`; ext agent/sw-vu1gpu 1b9a06d)
Console-free round (the console was asleep): four audit items, each its own ext commit, verified off the console; they
need the console A/B (fight + title menu fps, `gsw-split` vu_ms per run, `[ee-prof]` not needed).
- e9daace + PIE fix **-fPIE instead of -fPIC** for `ps2_runtime`, `ps2_iop`, `libmpeg2` (Switch; plain non-PIC fails the
  `-pie` link: "read-only segment has dynamic relocations", vtables in .rodata): the cause was devkitPro's
  `Platform/NintendoSwitch.cmake:21` (`CMAKE_POSITION_INDEPENDENT_CODE ON`), not raylib (its line 69 is the Android branch).
  build.ninja: 121 `-fPIC` compile lines -> 7 (raylib only); before: `libps2_runtime.a` 3852 `ADR_GOT_PAGE` relocations,
  `libps2_iop.a` 1146, .got 0x13398 bytes; with -fPIE 2240 / 185, .got 0x12d28 (the rest are extern data, which -fPIE
  still reaches through the GOT). VU1 harness with the core files `-fPIC` vs not: +0.4 % instructions only
  (the VU1 core has few interposable calls); the gain, if any, is in the GS stage / IOP / runtime helpers (console only).
- 77976fe **-O3** on `ps2_vu1_core/upper/lower.cpp` and `vu1n_part*.cpp` (`PS2X_SWITCH_VU1_OPT`). aarch64 harness under qemu
  (`icount.sh`, 100 records, HLE+DIRECT+TRACES, minus the harness's own `silentNativeHash`): fight 2.36 -> 2.24 M insns per
  warm pass (-5 %; runNative -12 %, generated blocks -2.4 %), menu 4.17 -> 4.05 M (-3 %). Replay exact (400 records fight +
  menu, HLE off: 0 differ; HLE on: dumps byte-identical to the -O2 build). Generated EE code not tried (no harness for it,
  and 88 MB of text). `-fno-plt` is moot (static link, no PLT); `-fomit-frame-pointer` is already the -O2 default.
  The -O3 vu1n parts need more compiler memory: the 14-job build was OOM-killed once (cc1plus on vu1n_part4 beside the
  unity batches); resumed with `taskset -c 0-7` (scratchpad `bp8.sh`).
- d5c14fa **EE FPU div/sqrt** (`ps2FpuDivideNearest` / `ps2FpuSqrtNearest`, ps2_runtime_macros.h): on AArch64 one `mrs fpcr`
  + two `msr` around an asm `fdiv`/`fsqrt` (sse2neon did fegetround/fesetround calls + extra FPCR reads, 4 FPCR writes per
  divide); no write when the thread already rounds to nearest (only with `GOW_EE_FPU_NEAREST=1`: the EE runs RZ+FZ, so the
  two writes stay). Exactness: 10 M random + edge-case pairs (div and sqrt) under RZ+FZ and RN+FZ, old vs new bit-identical,
  FPCR restored (aarch64 test under qemu, scratchpad `fpt/`). qemu time for 1 M divides 194 -> 46 ms (instruction count
  only; the real cost is the FPCR-write serialisation, console only). `PS2X_FPU_CSR_SSE2NEON` brings back the old code.
- b0d4e2a + 82a4a63 **Clocks**: start-up line `[switch] mode docked|handheld; clocks (Hz, start -> now): cpu .. emc .. gpu ..`.
  `GOW_EMC_MHZ` / `GOW_GPU_MHZ` (MHz or `max`: EMC 1600, GPU 768 docked / 460.8 handheld) set those rates and the 2 s keeper
  re-applies them (a re-apply is logged); default stays the system's rates (not verified on hardware, so not on by default).
- env.txt: no package carries one (make_update.sh writes exefs only). The SD's `sdmc:/switch/gow/env.txt` still holds
  `GOW_EE_SAMPLE=200 GOW_CORE_EeSampler=1 GOW_PRIO_EeSampler=3` from update_ee2: the console agent must replace it
  (scratchpad `c/env_fast1.txt` = defaults, `c/env_fast1_clk.txt` = `GOW_EMC_MHZ=max GOW_GPU_MHZ=max`).
- A/B for the console agent: `update_ee3` (c7337ec, the same code minus these four; sampler off without the env) vs `update_fast1` (exefs main 49.3 MB, built 15:44) with env_fast1.txt, then
  update_fast1 with env_fast1_clk.txt; read the `[switch] mode` line first (docked or handheld decides the GPU max).

## Handoff (2026-10-23 night: EE thread profile, structural-lever study; ext agent/sw-vu1gpu c7337ec)
- New tool (ext c7337ec, `update_ee2`/`update_ee3`): `GOW_EE_SAMPLE=<hz>` starts a sampler thread that every 5 s prints `[ee-prof]` (exclusive guest
  function = the innermost function entered through `dispatchGuestBranch`, or the resume pc when the scheduler re-enters a function after a
  yield, plus the EE thread's perf bucket 0 ee / 3 iop / 4 wait) and `[ee-wait]` (EE idle waits by wake cause). Cost: two relaxed stores per guest call.
  On the Switch use `GOW_EE_SAMPLE=200 GOW_CORE_EeSampler=1 GOW_PRIO_EeSampler=3` (std::this_thread::sleep_for and thread::detach are ENOSYS
  on this libc: svcSleepThread via `ps2xSwitchSleepNs`, the thread is leaked). Read with scratchpad `prof.py <log> <first window> <last window>`.
- Fight profile (bot fight, update_ee2, ~22-24 fps; per 5 s: ee 2.7-3.0 s, iop 0.8-1.0 s, guest_wait 1.1-1.8 s): of the EE thread's samples
  **~39 % are in the frame loop's vsync spin** (resume pc 0x17a32c in sub_0017A2A0: `*(0x29C7D0)==1` polled with 20000-iteration countdown loops,
  every iteration an `eeCheckpointDue`): bucket iop 14.6 % (the IOP batches the checkpoints run) + wait 21.7 % + 3 % own. The game's real EE
  code is a flat tail: 0x17a114 (sub_0017A0E0 after the Flip call) 7.5 %, 0x162f18 3.9 %, 0x27b9f0 3.0 %, 0x254310 2.1 %, 0x27a210 1.7 %,
  0x158020 1.1 %, everything else below 1 % each (60 entries listed): no hot function to rewrite. `guest_wait` is the pacing sleep
  (EeScheduler.cpp:1946, the guest clock may not run ahead of the wall clock) plus `waitForEvent` when every guest thread is blocked.
- `[gow-speed-hist]` in the fight: most frames take 1.6-1.9 fields (27-31 ms), 3-8 frames per second take >= 2.4 fields: the 22-25 fps average is
  a hitch problem as much as a throughput one.
- Arithmetic at 24 fps (41.7 ms/frame): core 1 = EE game code ~20 ms + GS stage ~7.5 ms (it preempts `ee_ms`) + IOP ~8 ms (per wall second, 3 ms at
  60 fps); VU1 thread ~30 ms. 60 fps (16.7 ms) would need EE game code + stage < 13 ms and VU1 < 16 ms: 2x on both, and the EE tail is flat.
- Decision (numbers above): no VU1-on-GPU prototype this round. It removes at most the VU1 thread (3.6 s/5 s) and the GS stage still consumes
  the GIF stream the VU1 programs produce (0.9 s/5 s, core 1) so the stage and the EE (4.3 s/5 s on core 1) stay; without a core-1 cut the
  fps ceiling stays ~30. Cheapest tests first (coordinator audit, 2026-10-23): (1) `[ee-wait]` by cause (update_ee3, built, not yet run) and the
  EE->VU1 chain walk (`PS2X_GS_WORKER_TRACE`); (2) `GOW_CORE_GSStage=0`, then `GOW_IOP_THREAD=1 GOW_IOP_SPIN=0 GOW_IOP_LEAD=4194304` now that deko3d is
  ~1.8 s busy; (3) gsq_wait split; (4) -O3 on the VU1 files; (5) PIC off for ps2_runtime/ps2_iop; (6) FPU divide csr writes; (8) clock/docked logging.
- State: console `update_ee2`'s `env.txt` (the three sampler variables) is still on the SD. At 14:00 the console stopped answering on Wi-Fi
  (<switch-ip>: no ping, probably asleep after the cycle): needs a physical wake before any further bot/MTP use.

## Addendum (2026-10-23 evening: HLE becomes the Switch default, where the rest goes)
- `GOW_VU1_HLE=1` is now a Switch default (ext 70c9fbb, `update_vgpu5`; env.txt `GOW_VU1_HLE=0` turns it off). Console bot fight windows
  (`gsw-split` vu_ms / runs): **21.5 us per VU1 run** with HLE+DIRECT+TRACES vs 27.6-28.2 (DIRECT+TRACES only) and 33.1 (no env); title menu
  44-47 fps (up to 55 in the quiet windows). Bot fight fps still 17-55 (avg ~29, scenes differ; the VU thread stays ~3.6 s / 5 s busy because
  the faster runs let the EE queue more of them: judge by us/run, not by fps). The kernels' per-call clock reads are now opt-in
  (`GOW_VU1_HLE_TIME`; they were on whenever GOW_PERF_DIAG was, which the Switch has by default).
- Where the remaining VU1 time goes (aarch64 harness under qemu, `icount.sh`, fight.rec first 100 records, HLE+DT, excluding the harness's own
  hashing; script: scratchpad `ic.sh`): kernels ~30 % (kSkinLitSt alone 22 %: ~100 NEON ops per vertex, already at the A57's 128-bit throughput),
  generated blocks and traces ~37 %, runNative's dispatch loop ~13 % (~4 k instructions per VU1 run), exact-engine entry/exit ~6 %. Dispatch
  counts (x86 harness, `GOW_VU1N_BLOCKPROF` + `krank.py`): after the kernels the cycles sit in the batch front (pc 00d8/00e0/0458/0350/0240
  ~45 % of the remaining VU cycles), the clip-flag culling loops (0cd8..0df8 and 1728..17d8, ~19 %), and the batch-switch routine (1258, 9.5 %).
  Each of these is a 5-10 % item of the VU thread; no single block or kernel is a big lever any more.
- Thread budget at ~30 fps in the fight (5 s windows): EE 2.7-2.8 s busy (IOP inline ~0.85 s of it) + 1.1-1.4 s `guest_wait`, VU1 thread 3.6 s,
  GS stage 0.7 s, deko3d thread 1.75 s. 60 fps needs EE < 16 ms per frame as well (it is 18.7 ms at 30 fps) and VU1 < 16 ms (24 ms): both
  threads need 1.5-2x, which the remaining micro-optimisations of the CPU path do not give; the structural options are the GPU vertex path
  (step 4) or fewer VU1 runs per frame (840 per frame now).
- `SSE2NEON_PRECISE_MINMAX=0` (harness `PMM=0`) in the VU1 sources: bit-identical replay on 800 records (fight, menu; HLE off) and -4 % A57 cost
  of the VU work, but NOT applied: the precise min/max exists because PS2 data holds NaN patterns (0x7FFFFFFF) that the VU treats as large numbers
  and that the clamps must map to the bound (x86 minps/maxps return the second operand on NaN, NEON fmax propagates the NaN); 4 % is not worth that risk
  without a recording that exercises those patterns.
- Next candidates (each ~5-10 % of the VU thread, in order of certainty): (1) a kernel for the batch front (pc 00d8..0398: XTOP, matrix loads, normal
  length, bounding tests with MAC-flag reads) so that the 4-6 dispatches per batch become one; (2) kernels for the clip-flag culling loops
  (0cd8.. / 1728..): they need the clip flag ring pushed at exit; (3) a cheaper runNative dispatch (hist entry, slot checks) for the ~20 dispatches per run;
  (4) the GPU vertex path or fewer VU1 runs per frame for the real step towards 60 fps.
- Harness: `tools/vu1_harness/build.sh` takes `PMM= PDIV= PSQRT= PDP=` (0/1) for the SSE2NEON_PRECISE_* defines (default 1, as the Switch build).

## Addendum (2026-10-23, step 1 + fight replay A/B)
- Second fight run (DIRECT+TRACES, HLE off, update_vgpu3): menu 40 fps again (31 without), fight 16-26 (scenes vary); not worse than the baseline 15-24 -> `GOW_VU1N_TRACES=1 DIRECT=1` is now the Switch default (ext 3df92c2).
- Deterministic fight replay on the console (`fight.rec`, 6000 warm runs): no env 137.2 ms, DIRECT+TRACES 128.6, DIRECT+TRACES+HLE 108.4 (-21 %), HLE alone 118.1.
- Leftover on fight.rec with HLE on (x86 harness block profile): the batch switch / per-object code (pc 00d8/00e0/0458/1258/1748/0350), ~73 dispatches and ~10 pairs per block per record, ~315 A57 cycles per block: this is the next target (step 3), not another loop kernel.

## Handoff (2026-10-23 later: menu kernels, ext agent/sw-vu1gpu)
- Two new kernels in ps2_vu1_hle.inl: `skin_lit28` (hash 5181a0b08a6d0989, 28-pair self-loop, no perspective divide: STQ.xy, position
  M_B*p + w*(M_A*p) as floats, lit colour min 255.0; entered after the batch-switch jalr) and `unlit11` (a2eaa96b750883ce, 11-pair
  self-loop, Q of the previous pass). Both transcribe the pairs in order (reads before writes inside a pair), cycles = pairs per pass
  (identical to the exact path's total on menu.rec: 4557336 / 1714647). Note: the title menu profile is 5181a0b0 34 %, eae77e32 (the
  batch-switch routine itself, pc 1258, 11 pairs, 23 %), a2eaa96b 13 %, 515a4959 8 %.
- Verification (x86 harness, menu.rec, reference dump with HLE off): GIF 342142 writes exact, 0 beyond tolerance; 482 data words
  differ by 1 ulp (float results in the VU buffers, the exact path rounds wide MADDs differently).
- Console, replay benchmark of menu.rec (6000 warm runs): no env 191.6 ms, DIRECT+TRACES 146.9, + `GOW_VU1_HLE=1` 115.2 (fight.rec before
  this change: 137 / 128.6 / 108.4). Live title menu with DIRECT+TRACES+HLE: **44-46 fps** (38-40 with DIRECT+TRACES only), screen correct;
  the VU thread is still ~3.3 s / 5 s busy: the batch switch (eae77e32 and the pc 00d8/00e0/0458 blocks) is what is left (step 3).
- Switch default DIRECT=1 TRACES=1 is set (ext commit "Switch default"); `GOW_VU1_HLE` is still off by default (flag in env.txt).
- Package: port/switch/nsp/out/update_vgpu4 (local only).

## Handoff (2026-10-23, console measurements of the HLE kernels; ext agent/sw-vu1gpu 556db41 + the diagnostics commit)
- State: `update_vgpu3` (= `update_vgpu1` + the intro-movie fix 3873093 merged as 556db41 + diagnostics) is on the console. Build:
  `bash port/switch/env_sync.sh ... vgpu`, `VU1_PREGEN=/root/vu1pregen-vgpu BUILD_DIR=/root/gow-switch-vgpu ./switch/build.sh` (docs/switch-env-setup.md).
  Console loop = the 4c236500 `cycle.sh` (scratchpad, copy it): bot menu -> New Game -> fight, `PLAY=1 BOOT=45` (BOOT = seconds before the
  first B; 25 was too early now); after a failed cycle the console may sit at the user picker (A) or hbmenu ordering differs: DBI is the 12th tile
  (DDOWN, DRIGHT x5), then A, A, DDOWN x9, A.
- Baseline `update_vgpu1`, no env: title menu 31-33 fps (VU thread 3.6 s / 5 s, EE 1.7 s, guest waits 2.8 s), bot fight 15-24 fps (VU 3.5-3.9 s,
  EE 2.4-2.8 s), movie 53-58 fps. (Old docs: menu 28-29, fight 19-28: the bot fight differs from run to run.)
- `GOW_VU1_HLE=1` changes NOTHING measurable (menu 31, fight 18-25, VU thread still 3.6-3.8 s). Why (measured with the new diagnostics):
  - the kernels themselves are cheap: `[vu1-hle]` now prints calls/VU cycles/wall ms: unlit 135 ms, lit 78, skin 111, cull 22+22 per 5 s
    (~0.35 s for ~110 M VU cycles); native pairs drop 225 M -> 120 M and blocks 28 M -> 22 M (`GOW_VU1N_DIAG`),
  - what is left costs ~3 s: ~22 M small blocks per 5 s (batch switch / prologue code, avg 5.4 pairs) at ~120-250 ns each, i.e. the
    per-block dispatch (the Switch default `GOW_VU1N_DIRECT=0 GOW_VU1N_TRACES=0` of c985a11 makes every block a runNative round trip),
  - code changes (MPG) 600-1500 per 5 s and slot set-ups 33-47 k per 5 s cost only ~70 ms + ~95 ms (`codeMs`/`slotMs` in `[vu1n]`),
    so they are not the problem (the HLE `tried` reset is not either).
- The title menu is 94 % two loops with NO kernel yet: `5181a0b08a6d0989` (28 pairs, 66 %: two-matrix lit/skinned two-block cycle, vf14 weight,
  loi 0x4b0000ff clamp) and `a2eaa96b750883ce` (11 pairs, 28 %: unlit ST-less vertex loop family), seen with `GOW_VU1N_LIVEPROF=1
  GOW_VU1N_DIAG=1` (`[vu1n-prof]` top 14 code hashes per 5 s; with the cached hash it still halves the fps: use it for a look only). A menu
  recording exists only on the console SD (`switch/gow/menu.rec`, pulled to the scratchpad; `fight.rec` too; keep out of git); disassembly
  from `tools/vu1_hle/findblk.py` (want.prof lines `k <hash> <pc> <pairs> 1 9000000`).
- **Measured win, no code: `GOW_VU1_HLE=1 GOW_VU1N_DIRECT=1 GOW_VU1N_TRACES=1` in env.txt: title menu 38-40 fps (31 without), bot fight
  22.2-23.6 fps (same as before).** The menu is the steady, deterministic scene: use it as the A/B workload (the fight varies per run).
  Same with HLE off (`GOW_VU1N_DIRECT=1 GOW_VU1N_TRACES=1` only, run `20261023_dt`): menu 37-39, fight 22.4-23 then 25.6-30.5 in the later
  windows (the HLE run showed 22-23.6 there; scenes differ per run). So DIRECT+TRACES are the win, HLE adds nothing yet. To do: confirm
  in a second fight run, then make them the Switch default (reverts the measurement of c985a11, which was without the kernels / on the ship scene).
- Next (in order): (1) A/B DIRECT/TRACES x HLE in the menu and fight (4 runs, same package); (2) kernels for `5181a0b0` and `a2eaa96b`
  (`kUnlitSt` in ps2_vu1_hle.inl is the pattern; verify with the harness `cmp.sh`/`hlecmp.py` on `menu.rec`: the old reference dump is lost,
  re-make it with HLE off); (3) shrink the batch-switch / prologue blocks (the ~22 M leftover blocks) as a kernel of their own;
  (4) the GPU path only after these: the CPU side is dispatch-bound, not arithmetic-bound.
- Intro movie fix (3873093) is in this build but the glitch frame has not been checked on the console yet (the bot skips the movie with +).

## Handoff (2026-10-20, stopped on request after step 2 started)
- Worktree `ext/PS2Recomp-kl-vu1gpu`, branch `agent/sw-vu1gpu` from b632e46 (sw-vu1split), untracked build files
  copied from the vu1split worktree (generated EE code, native IOP files, overrides, deko3d shader table). Commits:
  c985a11 (Switch defaults `GOW_VU1N_TRACES=0 GOW_VU1N_DIRECT=0`, measured ~3 % faster on the A57), ad33516 (HLE
  framework + `unlit_st`, profiling aids), 6a8114e (WIP: `lit_st`, `skin_lit_st`, `cull`, `cull_swap`).
- Nothing new is on the console: it still has `port/switch/nsp/out/update_vsplit5` (b632e46, sw-vu1split's build)
  and was not touched in this session. No Switch build of this branch exists yet, no PC build folder either.
- WSL: copy of the worktree `/root/src-vgpu` (all tracked files synced; sync script below), VU1 blocks
  `/root/vu1gpu/vu1pregen` (= ext/PS2Recomp-kl-vu1disp/out/vu1pregen), x86 harness `/root/vu1h/vg0`, aarch64 harness
  `/root/vu1h/vga0`; reference dumps `/root/vu1gpu/ref_vg0_ship.dump`.
- Verified: all five kernels are **bit-exact** in data memory and GIF packets on the ship recording (2000 records,
  `hlecmp.py`: 334053 GIF writes exact, 0 data words differ); only the MAC flag register at the program's end differs
  (kernels do not keep the flag pipeline). qemu (aarch64, s30, with `unlit_st` only): VU1 functions per warm pass
  2.487 M -> 1.956 M (-21 %); `unlit_st` itself 0.107 M where the generated cycle + its dispatches cost ~0.42 M.
- Not yet done: the seq and PC-zone recordings (`seq.rec`, `game.rec`; the PC zone's hot loops `5181a0b0` and
  `a2eaa96b` have no kernel yet), qemu totals with all kernels, a PC build + zone gate, the Switch build and the
  console replay benchmark / bot fight, anything GPU.

### Next steps (in order)
1. Kernels for the remaining hot blocks (table below): `5181a0b0` (PC zone 56 %), `a2eaa96b` (PC zone 24 %),
   `5bb209bd`, `40c1d3cd`, `476fc4cf`; check each with `cmp.sh` on ship, seq and game.
2. qemu cost with all kernels (`icb2.sh vga0 s30.rec` and `g30.rec`), then a Switch build (copy of
   docs/switch-deko3d.md's WSL flow from `/root/src-vgpu`, `VU1_PREGEN=/root/vu1gpu/vu1pregen`) and the console
   replay benchmark with `GOW_VU1_HLE=1` vs 0 (env.txt, docs/vu1-native-flags.md "Measuring on the A57"), then the
   bot fight (scratchpad `core1/cycle.sh` style script with `UPDATE=<package>`).
3. If the CPU kernels make VU1 (core 2) no longer the limit, the GPU step pays only by also removing the GS stage's
   per-vertex work (core 1); see "GPU path" below. Retest traces/direct chaining with the kernels on (the loops no
   longer go through them; runNative's dispatch of the remaining code is then the main VU1 cost: s30 runNative 1.03 M
   with traces/direct off, 0.52 M with them on).
4. Vectorise the kernels' steady state (4 vertices per SIMD op) only if the console says the scalar kernels are not
   enough.

### Commands
- Sync worktree -> WSL copy: scratchpad `vu1gpu/sync.sh` (Git Bash: tracked + modified + untracked-not-ignored files
  of the worktree tarred into `/root/src-vgpu`; `ALL=1` every tracked file). wsl.exe expands `$` in `bash -c '...'`:
  run scripts from files.
- Harness: `bash <scratch>/vu1gpu/hb.sh x86|arm <name>` (tools/vu1_harness/build.sh on `/root/src-vgpu`, prints
  compile errors; touch `ps2_vu1_core.cpp` first: the .inl files are not dependencies).
- Exact vs HLE: `bash <scratch>/vu1gpu/cmp.sh vg0 ship` (reference dump once with the HLE off, HLE dump, then
  `python3 tools/vu1_hle/hlecmp.py ref hle [n]`: GIF writes by register with tolerances: ST/Q rel 1e-4, RGBA +-2,
  XY +-2 (1/16 px), Z +-4 or rel 1e-5, ADC exact; final VI / pc; data words).
- Profile by block code (pc independent): `GOW_VU1N_BLOCKPROF=<file>` now also writes `k <hash> <pc> <pairs> <runs>
  <cycles>`; `python -I tools/vu1_hle/krank.py <file>` ranks them; `tools/vu1_hle/findblk.py <prof> <rec> <dir>`
  writes a listing (tools/vu1_hle/vudis.py) of each hot block from the first record holding it;
  `tools/vu1_hle/reghash.py <rec> <index> <start> <pairs>` gives a kernel's region hash.
- `GOW_VU1N_PEEK=<block hash>[:n]`: registers and the data memory around vi2 / vi3 before and after the first n
  dispatches of that block (reading a loop's buffer layout).

## The program (step 1)
- 99 % of the VU1 runs start at 0x00 / 0x18 / 0x30 of one microprogram (fixed code 0x0000-0x2828 in 1973 of 2000 ship
  records) that sets vi1 = 0 / 341 / 683 (triple-buffered input) and dispatches on the batch header (`jr vi4`).
- From 0x2828 the micro memory holds **modules** in 0x350-byte slots (123 different slot contents across the three
  recordings, position independent, loaded at different slots: the game streams them in with MPG). A module is a
  prologue, a call of the batch switch routine (pair 27/28: `xtop`, next batch's pointers and count, matrices
  vf20..23 from the batch, lighting / bounding tests, XGKICK of the finished batch) that returns into the module's
  vertex loop, and the loop; the loop's fall-through calls the batch switch again, which returns into the loop for the
  next batch. The loops are software pipelined: the first passes store values from before the loop (the pipeline
  carries the previous batch's last vertices), so a kernel must run the passes exactly as the VU does, not "per
  vertex" from the inputs.
- Vertex buffers (from vi2 + 1, vi4 qwords per vertex): ST ints (12 fractional bits), [normal ints (15 fractional bits),
  w = 2^23], RGBA ints, XYZ ints (4 fractional bits, w = flags: bit 15 no kick, bit 0 winding, or a skinning
  weight). Output (from vi3 + 1, often another buffer): STQ floats, RGBA as 2^23-biased floats (colour in the
  mantissa, clamped at 255), XYZ ints with the ADC word in w; some lanes are written by a second loop of the module.
- Where the cycles go (`k` profile, one warm pass, cycles attributed to the dispatched block incl. its chained passes):

| block code | pairs | kernel | ship | consecutive | PC zone |
|---|---|---|---|---|---|
| 57dcf3d01a567c49 | 22 | `lit_st`: lit textured (3 lights, colour matrix) | 27.5 % | 24.8 % | 0.1 % |
| 515a49593a9fb86f (+ 5797a1f6) | 9+9 | `unlit_st`: ST/Q + transform, two-block cycle | 25.0 % | 18.2 % | 15.2 % |
| 570237771ea0bdea | 31 | `skin_lit_st`: two matrices blended by a weight, lit | 21.7 % | 24.1 % | - |
| 4c9671fb01ff6c3b | 13 | `cull`: XYZ + strip winding cull (ADC) | 9.8 % | 11.1 % | - |
| 9330611e7b80a713 | 13 | `cull_swap`: the other winding | 6.6 % | 11.1 % | - |
| 5bb209bd93a479eb | 9 | none yet | 2.6 % | 2.6 % | 0.8 % |
| 5181a0b08a6d0989 | 28 | none yet | - | - | 55.6 % |
| a2eaa96b750883ce | 11 | none yet | 0.2 % | - | 23.8 % |
| 40c1d3cda1601098 | 13 | none yet | 0.1 % | - | 2.3 % |
| 476fc4cf0fe83040 | 12 | none yet | 1.3 % | - | - |

## CPU HLE kernels (step 2, ps2_vu1_hle.inl, `GOW_VU1_HLE=1`, off by default)
- A kernel names its loop region by FNV-1a 64 of the region's bytes (no game code in the source) and its entry
  blocks by offset; runNative checks each dispatch pc once per code change (`VU1HleState`, reset in the code-change
  path) and then calls the kernel instead of the block: it runs all remaining passes in plain float (the native blocks'
  round-toward-zero mode, no operand normalisation / clamps / wide MADD, no cycle model: no history entries, flag
  ring pushes or FDIV timing per pass), writes VU memory and registers as the loop leaves them, adds the passes'
  idle cycles and returns the fall-through pc; runNative continues there with `histCount = 0`, `arraysReadyBy = 0`
  (nothing in flight) and the FDIV entry cleared (else commitDue writes an older Q over the kernel's: the first cull
  bug). A kernel declines (`kDecline`, the native block runs) when the count register would wrap (vi5 <= 0).
- Q: an instruction reading Q sees the newest DIV (the native model stalls on it), so a kernel's MULq uses the DIV
  of its pipeline stage; the cull loops' FMEQ reads the MSUBy.x of the pass before (4 cycles back, steady state also
  with the pass's 1-cycle stall at FTOI4), the first pass reads the MAC ring at entry + 2 (`ringAt`), and the kernel
  pushes its last two MAC writers when it ends.
- `GOW_VU1_HLE_MASK=<hex>`: only the kernels whose bit is set; `[vu1-hle]` (with `GOW_PERF_DIAG` /
  `GOW_VU1_HLE_STATS`, and at the end of a replay): calls / VU cycles per kernel.
- VU cycles: the kernels add the idle pass length (e.g. 31 per skinned vertex) where the exact engine also counts
  stalls; the cycle counts differ a little (only timing diagnostics read them in this port: unverified for XGKICK
  ordering, check in game).

## GPU path (step 3, not started): what the data says so far
- The loops' output goes into GIF packets that the batch switch XGKICKs; the CPU GS stage decodes them (fast PACKED
  path) and the backend batches them for deko3d. A GPU kernel that writes VU memory would need a GPU round trip per
  run (~135k runs per 5 s); the useful form keeps the inputs (VIF-unpacked vertex buffers + the batch's matrices /
  lights) on the GPU and has a vertex or compute shader produce the backend's vertex format, with the GS stage
  receiving a placeholder for the vertex block (PRIM/registers from the packet, vertices by reference).
- The kernels above are the reference for those shaders (same pass structure); the strip culling needs the previous
  two vertices (a compute pass per batch, or per-triangle work in the shader).
- Decide after the console numbers of the CPU kernels: if VU1 stops being the limit, the GPU step is worth it only
  for the GS stage's per-vertex cost on core 1 (~180 ns per kick, 0.8-1.2 s per 5 s in fights).
