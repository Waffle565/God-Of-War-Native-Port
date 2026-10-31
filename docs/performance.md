# Performance (USA build on the KIexster stack)

> **sw-lag (2026-10-26)**: `GOW_DK_FRAMES=3` is now on the console: default update_icons1 (= update_sws5 + Switch
> button icons, docs/status.md). Input lag measured below ("Input lag"); the sw-stall handoff that follows still holds
> for the open fps work (GOW_DK_FRAMES=5/8, GPU draw cost).

> **Handoff sw-stall (2026-10-26 ~00:30, stopped: user out of tokens).** Ship-fight stalls found and fixed in
> update_sws5 (ext agent/swstall 7519883 on agent/natspd; = the swfix3 default sources + `GOW_DK_FRAMES` default 3):
> the GS thread ran ~170 frames ahead of the GPU and stalled 70-230 ms whenever deko3d's queue ring filled ("The
> ship-fight stalls" below). Same-session results: play `sws5play` 240 fight windows 47.8 fps, speed 1.009, long frames
> 5.2/min (max 7 fields; baseline sws2play 46.8 fps, 71.5/min, max 219); bench `sws5b0` 44.8 / 46.9 / 47.4 / 48.6 vs
> default `swfix3b0` 47.5 / 46.6 / 45.9 / 47.2 / 47.4 (0-30 scene -2.7, fight scenes +0.3-1.5). **Not installed**
> (bench 0-30 lower; the user's decision). Console left on update_swfix3 + FAILGT (exefs md5 verified). GPU levers
> tried (all opt-in, gains within ~0.5 fps): `GOW_DK_COMPRESS`, `GOW_SCALER=fast`, `GOW_DK_LAZYBARRIER` (worse);
> upper bound of dropping all barriers +0.9-1.5 fps (`GOW_DK_NOBARRIER=2`, picture not valid). Next: try
> `GOW_DK_FRAMES=5/8` (envs sws_bf5/bf8: the 0-30 loss may be the short queue), then the GPU draw cost (models pass
> 203425 ~27 % of GPU time, shuffle 12 %, fail-only 204925 12 %, scale 9 %). Build: tools/switch_stall/ (private WSL
> tree /root/src-sws + /root/gow-switch-sws; sync.sh re-syncs it with /root/src-nv2 before a package).

Target (user, 2026-10-09): ≥ 60 fps, "see this game fly".

## Baseline (2026-10-09, port/perf1.log, OpenGL GS, interpreter VU1, VU1 capture on)
Loaded gameplay (state=11): 4.0 game frames/s. Game thread saturated; per game frame ≈ 250 ms:
VU1 155 ms, EE 77 ms, GS 10 ms, IOP 7 ms. 60 fps needs ≤ 16.7 ms → ~15x.

## Where the time goes / levers (verified in source unless marked)
1. **VU1 (61 %)**. The interpreter models the pipeline per instruction pair. Their per-block C++ generator
   (tools/vu1/generar_vu1.cpp, enabled with -DPS2X_VU1_MICROCODE_DIR) measured 2-3x on a bench frame.
   PCSX2's microVU (ref/pcsx2-src/pcsx2/x86/microVU_*.inl) resolves stalls at compile time: per-block static
   simulation with regCycleInfo (4-bit stall counter per VF component, microVU_IR.h:8), 4 flag instances
   (statFlag/macFlag/clipFlag[4], microVU.h:84-86), Q/P ping-pong instances, dead-flag elimination. The emitted
   code has no runtime pipeline bookkeeping. → model for a real VU1 block compiler.
2. **VU1 on its own thread (PCSX2 MTVU, ref/pcsx2-src/pcsx2/MTVU.{h,cpp})**: EE posts commands to a lock-free
   ring (MTVU_VU_EXECUTE, MTVU_VU_WRITE_MICRO/DATA/VIREGS/VFREGS, VIF unpack...); EE waits (VU_Thread::WaitVU,
   MTVU.cpp:435) only when it must read VU1 state or on VIF FLUSH*. Would overlap EE (77 ms) and VU1 (155 ms).
3. **EE recompiled code overheads** (seen in port/output-usa/sub_0017A0E0_0x17a0e0.cpp + ps2_runtime_macros.h):
   - `ctx->pc = …` stored before every instruction;
   - every JAL goes through PS2Runtime::dispatchGuestBranch (ps2_runtime.cpp:1463): atomic branch-trace ring
     (GOW-Port debug), eeCheckpointDue, hasFunction/lookupFunction, indirect call, ps2_perf::guestEntry;
   - every load/store checks isSpecialAddress + a contiguity check;
   - GPRs live as __m128i; 32-bit writes go through _mm_move_sd-style merges.
   (ps2TraceGuestWrite is an empty inline — no cost.)
Gains per lever above are estimates from the Gemini review (gemini-work/results/perf-research.md) — **unverified**.

## Log
- 2026-10-09: build with compiled VU1 (351 captured images) + /Zi (profiling) started (port/kl_build_vu1.log).
  Pitfall: in Git Bash, `-DCMAKE_CXX_FLAGS=/Zi …` gets rewritten to "C:/Program Files/Git/Zi" — set
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" when passing MSVC flags.
- 2026-10-09 14:20: build OK (compiled VU1: 13 generated programa*.cpp from 351 images; exe 154 MB + PDB).
  New tools: tools/perf_run.sh (save-load script + GOW_PERF_DIAG + optional muestrear profile, fresh copy of the
  user's card each run), port/prof/muestrear.exe (built from their tools/perfil/muestrear.cpp).
  Pitfall: `cmd //c x.cmd` with MSYS_NO_PATHCONV=1 leaves cmd interactive (hangs) — call cmd from PowerShell.
- Scheduler facts (EeScheduler.cpp): VBlank fires when counted EE cycles reach kVBlankPeriodCycles AND the host
  deadline passed (processDueDeadlines); checkpoints charge 32 cycles per backward branch and per guest call,
  each running advanceEeTimers + advanceIopEeCycles. VU1 runs synchronously in the VIF1 MSCAL callback on the
  game thread (ps2_runtime.cpp syncCoreSubsystems); VIF1/GIF DMA chains are snapshotted at kick (chainData) and
  processed immediately, completion flagged at once (ps2_memory.cpp processPendingTransfers).
- 2026-10-09 perf2 (compiled VU1, still with traces): gameplay ~7 game frames/s (was 4.0). Per frame: EE 77 ms,
  VU1 47 ms, GS 10, IOP 7.5. Profile (port/perf2.prof): **44 % of the game thread in WriteFile** —
  ps2_log.txt grew to 2.7 GB: per-function enter/exit trace (ps2_log.h, AGRESSIVE_LOGS, flush per line).
  Cause: our out/build-kl used CMake defaults PS2X_ENABLE_AGRESSIVE_LOGS / RUNTIME_LOGS / IOP_RPC_TRACE = ON;
  their compilar.ps1 sets them OFF unless tracing. **All earlier numbers (perf1, perf2) include this cost.**
- 2026-10-09 14:40: user supplied KIexster update (god-of-war-recomp-main(1).zip = GitHub main 8441486, extracted
  to ref/other-recomp-new; git clone in ref/other-recomp-git). 8 new patches applied cleanly to ext/PS2Recomp-kl
  (uncommitted — user declined the commit): gs-upload-ring, vu1-zero-factor, vu1-direct-store, vu1-cold-scalar,
  vu1-simd-zero, vu1-xgkick-flush, gs-shared-host, vu1-generator-dir (generator now takes the capture folder).
  Repo monitored for new commits (git ls-remote every 5 min).
- KIexster update content (Gemini summary gemini-work/results/kl-update-perf.md; env var + FPS figures verified):
  VU1 patches measured on their VIF1/VU1 replay bench (compiled VU1, ms/frame): zero-factor 103→99,
  direct-store 100→93, cold-scalar 93→91, simd-zero 90→83, xgkick-flush 82→78 (all automatic, no switch).
  gs-upload-ring = robustness only. gs-shared-host = opt-in `PS2X_GS_DIRECT_PRESENT=1` (WGL shared texture,
  no readback; log says "[gs-present] host mode: shared texture"), +0.9 % = noise. generator-dir = generator
  takes the capture folder (they now have 2302 micromemories incl. the final battle).
  Their latest loaded-gameplay figure (RENDIMIENTO_PRESENTACION.md): **6.85-6.91 game fps** on Ryzen 7 5800XT +
  RX 5700 XT, OpenGL, compiled VU1 (501 images), traces OFF. Their next bottleneck: GPU raster of FBP 0 flags
  0x4b (textured+blended+bilinear) ≈ 50-56 ms per frame in the fragment-shader-interlock path.
- **2026-10-09 perf3 (traces OFF + 8 new patches + compiled VU1 351 images): gameplay 13.6-14.6 game fps**
  (was ~7 with traces; KIexster's own figure 6.9 on a slower PC). Per frame ≈ 71 ms: VU1 36, GS 15, EE 13
  (was 77 — the trace was most of it), IOP 7. Profile port/perf3.prof (game thread): VU1 *interpreter*
  functions dominate the VU share (commitReadyPipelines, decodeInstructionPair, commitReadyFlags) → most
  Olympus microprograms are not in our 351 captures; 12.6 % GSThreadedBackend::Drain (waiting for the GS
  thread); ~10 % is GOW_PERF_DIAG itself (QueryPerformanceCounter, ps2_perf::switchTo).
  Next: capture VU1 micro-memories in gameplay with this build, merge, rebuild.
- 2026-10-09 VU1 capture round 2: 841 micro-memories (port/vu1_micro, +490 from 7 min of Olympus gameplay).
  perf4: 14.2-14.6 fps — no change. The profile's top-40 interpreter functions sum to only 6.6 % of the thread;
  the VU bucket (~48 %) is mostly compiled blocks spread over thousands of functions → compiled VU1 is simply
  expensive (consistent with KIexster's 78 ms/frame replay bench). GOW_VU1C_DIAG prints only from a static
  destructor, which this runtime's exit path skips (no output even with WM_CLOSE).
- **GS worker thread (PS2X_GS_WORKER=1, implemented 2026-10-09, building)** — PCSX2-MTVU-like:
  PS2Memory::processPendingTransfers copies GIF/VIF1 bytes at the kick (chains already were) into a job; a worker
  thread runs VIF1 → VU1 → PATH1/2/3 → GS frontend in order. DMA completion (CHCR.STR, D_STAT, handlers) stays
  at the kick on the EE thread, as before. Every EE entry into worker-owned state syncs first:
  MMIO read*/write*/read/writeIORegister for GS privileged regs, 0x10003000-0x10003FFF, VIF1/GIF FIFOs,
  VU1 memory; submitGifPacket / processVIF1Data / native GIF chains / flushMaskedPath3Packets when called off the
  worker; GS stubs (Support.h applyGsRegPairs, GS.cpp clear/store-image). VU1 MSCAL/MSCNT on the worker use the
  FBRST captured at the kick and do not write VPU_STAT (game has no CFC2/CTC2 — verified by grepping the
  recompiled code). Worker stopped before GS teardown. Shared-state map: gemini-work/results/vif1-shared-state.md
  (spot-checked).
- 2026-10-09 worker1 (PS2X_GS_WORKER=1): boots, loads the save, gameplay renders correctly
  (port/worker1/gow_pad_test_13.png identical scene to non-worker runs). 15.0-15.6 fps vs 14.2-14.6 without.
  ee_ms rose to ~4.4 s/5 s = the EE thread mostly waits in the worker sync → some sync point fires very often.
  Added PS2X_GS_WORKER_DIAG=1 (per-site sync/wait report every 5 s; sync helpers moved to the .cpp so future
  changes there avoid a PCH rebuild). Gemini review of the worker timed out (20 min, no output) — task too broad.
- worker2 (PS2X_GS_WORKER_DIAG): almost all waits at "mmio 0x70003F30" = **scratchpad** — needsGsWorkerSync
  masked 0x70003F30 to 0x10003F30 (VIF register range). Fixed (scratchpad excluded).
- worker3/4: no false waits any more (CSR 0x12001000 syncs wait ~0.2 ms/5 s), but 12.6-13.4 fps (< 14.5 serial).
  Profile: EE thread idle-spinning (34 % QueryPerformanceCounter + timers/IOP), worker 47 % waiting. Cause: the
  game waits for each DMA chain via the GS FINISH interrupt (vid::WaitForDMAComplete 0x183878 polls *0x29C7D0,
  set to 2 by the GS IRQ handler 0x182DD8 — KIexster ESTADO.md §7); EeScheduler::waitForEvent sleeps until the
  next timer/VBlank deadline and nothing woke it when the worker set FINISH. Fix (building): PS2Memory
  setGsWorkerWake → worker posts EeEvent ExternalWake (type 0xFFFFFFFF, matches no waiter) after a job that left
  CSR SIGNAL/FINISH set, so pollGsInterrupt runs at once.
- **2026-10-09 worker5 (worker + wake fix): 20.0 game fps** (was 14.5 serial), picture correct
  (port/worker5/gow_pad_test_13.png). Locked at 60/3: frames snap to VBlanks.
- Timeline (PS2X_GS_WORKER_TRACE=<s>, tools/gsw_trace.py): each frame the game kicks ONE ~1.95 MB DMA chain
  (0x13F730) right after vid::Flip; the worker needs **46.6 ms** for it (VU1 + GS frontend); the EE reaches
  vid::WaitForDMAComplete ~48 ms later and waits ~1.4 ms. Critical path = that one worker job.
  With GOW_GS_FINISH_ASINCRONO=1 the job drops to 42.7 ms (still 20 fps). 30 fps needs the job < ~33 ms.
  Next options: split GS frontend onto its own thread (pipeline VU1 → GS, ~11 ms), faster VU1 (microVU-style
  block compiler; VU1 ≈ 34 ms/frame).
- Stopped here by the user (2026-10-09). All runtime changes uncommitted in ext/PS2Recomp-kl.
- 2026-10-09 first real play session by the user (PS4 controller, port/play3.log, ~5 min): no errors or crashes.
  After the first 2 min (menus/loading): avg 24.5 game fps, min 15.6, max 52.1 per 5 s window. Title menu 30.
  The controller disconnected once near the end (Bluetooth); input fell back to keyboard as designed.

## EE checkpoint cost and the meaning of ee_ms (2026-10-11, ext branch agent/ee-speed 62dee0e, e96ee58)
- Profile (port/agent_ee/base2, muestrear, Olympus): the EE thread was ~55 % busy, and **~40 % of that was
  QueryPerformanceCounter**: `PS2Runtime::advanceIopEeCycles` opened a `ps2_perf::Scope` (two clock reads with
  GOW_PERF_DIAG=1) at *every* EE checkpoint (each backward branch and guest call, millions/s). iop_ms 1.3 s/5 s was
  mostly that scope. Fix: `IopSubsystem::accrueEeCycles` / `runPendingEeCycles`; the scope wraps only the batched IOP
  run (same IOP behaviour). iop_ms 1.3 s -> 0.15 s.
- EE timers (`PS2Memory::advanceEeTimers`) advance in batches up to the next timer interrupt
  (`cyclesUntilNextEeTimerInterrupt`); timer register reads/writes flush first, so counts/interrupts are exact
  (tick math carries its remainder; Gemini review gemini-work/results/ee-timer-batch-review.md agreed).
  Branch-trace ring: relaxed load/store instead of a locked fetch_add.
- `SET_GPR_*` write the low 64 bits with one scalar store (was 128-bit load + merge + store per GPR write).
- What the EE does in Olympus: after kicking the frame's DMA chain it **spins** in 0x17A2A0 (20000-iteration delay
  loop polling *0x29C7D0 == 1, the vid DMA-busy flag), each iteration an EE checkpoint. So the old ee_ms (~3.5 s/5 s)
  was mostly spin + the sleep until the host VBlank deadline (processDueDeadlines), not game logic.
  processDueDeadlines' sleep now counts as guest_wait: **ee_ms is EE work + spin (~1.4 s/5 s)**; thread busy ~31 %.
- Frame pacing: with cheap checkpoints the guest VBlank runs at exactly 60 Hz host-paced (GOW_VBLANK_DIAG=1:
  59.99 Hz, host lag < 2 ms) and Olympus locks at **30.0 fps** (2 VBlanks). The base build showed 31-35 fps
  (port/agent_ee/base2): unverified explanation — its slow checkpoints let guest time drift and VBlanks catch up
  faster than 60 Hz. Use the game-speed meter before reading fps above 30 as a win.
- Bottleneck now: the GS worker (VU1 + GS frontend) job, ~23-26 ms per frame (port/agent_ee/gap.py on
  PS2X_GS_WORKER_TRACE output); 60 fps needs < 16.7 ms. EE is no longer on the critical path on PC.
- Tools: port/agent_ee/prof_run.sh <bin_dir> <name> (zone slot 1 + muestrear profile + worker trace summary).

## 2026-10-11: native VU1 speed-up (ext branch agent/vu1-speed: 0055349, 994a514)
- **Cause found**: `vu1n::madd<>` (every MADD/MSUB of the generated blocks) was not inlined - its double-precision overflow
  fallback made it too big - and as a real call MSVC x64 passes its three `__m128` through memory: 12 % of all VU-thread
  samples, more than all blocks together. Fast path now `VU1N_INLINE` (MSVC `__forceinline`, GCC/Clang `always_inline`),
  fallback `maddWide` noinline. All other `vu1n::` helpers (divOp, clipVal, ...) forced inline too.
- XGKICK from native blocks walks the GIFtags in VU memory and copies the packet with at most two memcpy instead of
  `progressXgkick`'s per-qword loop (fallback to the loop for odd packets / `GOW_XGKICK` trace).
- Olympus, logs-off build, same session (port/agent_vu/base2 -> xgk1): vu_ms 2.58-2.70 s -> 1.27-1.39 s per 5 s while
  doing ~1.8x more VU work; VU cost per VU cycle 8.0 -> 2.2 ns; guest_flip_hz 31-36 -> 52-64.
- **Bit-exact proof**: `GOW_VU1N_RECORD=<file>` (`_N`, `_STRIDE`) stores VU1 run inputs + hash of the native output (state,
  data memory, GIF packets); `GOW_VU1N_REPLAY=<file>` reruns them with the current build at the first VU1 run and prints
  `[vu1n-replay] N records, M differ` (`GOW_VU1N_REPLAY_REPS=n` adds warm timing). 6000 records (menu + Olympus) from the
  original code: 0 differ after every change.
- Tried and dropped: keeping VF/VI/ACC/Q/I in C++ locals inside each generated block (bit-exact, but no measurable gain:
  MSVC already forwards the stores; draft at port/agent_vu/vu1n_gen_v2.py).
- Remaining worker-thread cost around VU1 (profile port/agent_vu/xgk1.prof): blocks ~20 %, runNative dispatch ~5 %,
  resetScheduler ~2.5 % per MSCAL, GifArbiter per-packet vector alloc/copy ~3-5 % (a Path1 direct path needs a change in
  ps2_gif_arbiter.h = full rebuild), VIF1 unpack ~13 %.
- Build note: a fresh configure defaults PS2X_ENABLE_RUNTIME_LOGS / AGRESSIVE_LOGS / IOP_RPC_TRACE / DEBUG_UI to ON, which
  writes a GB-sized ps2_log.txt (function enter/exit) and caps the game at ~10 fps; pass them OFF like build-kl.

## Game-speed meter (2026-10-11, ext branch agent/speedmeter 17488d4)
- What: `GOW_DELTA_DIAG=1` (with `GOW_PERF_FRAME_PC=0x001837B8`) sums the game's own per-frame time step (float at
  0x29C64C, seconds) at every vid::Flip entry and prints `[gow-speed] ... speed=` once per wall second: game seconds per
  wall second. 1.00 = real time; fps can be judged only together with it (fps above 30 with speed 1.00 is real; fps
  above 30 with speed > 1 means game time runs fast). Idea and the 0x29C64C address taken from KIexster's
  patches/ps2recomp-game-speed.patch, folded into `ps2_perf::guestEntry` (ps2_perf.h); zero cost when off.
- tools/zones.py sets it and reports "game speed (mean, min-max)" over the last 20 windows (warning outside 0.9-1.1).
- Measured, zones slots 1-4 (port/zones/20261011_232420, Release, all ok, windows after gameplay start):

  | slot | fps | speed in 60 fps windows | speed in 30 fps windows | overall |
  |---|---|---|---|---|
  | 1 Olympus | 60 (30 for ~11 s after load) | 1.031 | 0.999 | 1.027 |
  | 2 Desert | 60 | 1.024 | - | 1.024 |
  | 3 Cliffs | 30 (60 for ~20 s) | 1.026 | 0.999 | 1.004 |
  | 4 Final battle | 60 | 1.018 | - | 1.018 |
- Reading: the 60 fps is real: the game takes ~1/60 s steps (delta_min 0.0166); it is not a 30 fps game whose clock
  runs double. The 2-3 % excess at 60 fps was a runtime timing error, fixed (below).
- How the game measures a frame (0x17D778, also inlined in vid::WaitForDMAComplete 0x17A2A0): T0 (BUSCLK/256 =
  576 kHz) extended to 64 bit (wraps at 0x303628, last value 0x2C7908); ticks since its previous frame read -> 0x29C670;
  fields = ticks / 96.1 / 100 (115.2 in PAL), clamped to [1, 4], then max(fields, 60 / target fps); step = fields / 60.
  So a frame shorter than one field counts as one field and a longer one counts in full: any jitter of the read point
  only adds game time.
- Cause (2026-10-12): per-frame ticks alternated ~9000 / ~10200 in our build (`[gow-speed-hist]`: half the frames in
  bin 9, half in bin 10 at 1.06 fields), while PCSX2 reads 9572-9640 every frame (PINE, desert, speed 1.000). Trace:
  vid::WaitForDMAComplete spins on the GS-done flag 0x29C7D0 and the recompiled spin burns guest cycles ~10x faster
  than wall time (a FINISH 1 ms of wall time after a VBlank arrived 5800 ticks = 10 ms of guest time after it), so where
  the FINISH and the next frame-clock read land in guest time depended on how long the host GS worker took.
- Fix (EeScheduler::processDueDeadlines, ext agent/gamespeed): 15 no-op pacing deadlines per field on the VBlank grid
  (cycle deadline and host deadline in lockstep, like VBlank itself). The guest clock can no longer run ahead of the
  wall clock by more than 1/16 field inside a frame; real work runs at about wall rate already, so only spins are held.
  `[gow-speed-hist]` (same switch) prints the per-frame tick histogram in tenths of a field.
- Switch check (2026-10-25, nat-vu1s; console default update_nv5k + FAILGT, normal play = `tools/switch_bench.sh`
  with `NOBENCH=1`, no `GOW_BENCH`, bench `nv5kplay`): speed 1.011 mean over 209 s of gameplay (windows 0.80-1.11),
  57 guest flips per wall second, `[vblank-diag]` 60.0 Hz. The guest clock runs at wall rate on the console too (T0 from
  EE cycles held on the VBlank host grid by the pacing deadlines; the step from 0x17D778, fields clamped to [1, 4]).
  Benches (`GOW_BENCH=1`) force one field per frame on purpose (deterministic frames): at ~43 fps their game time runs
  at ~0.72 of real time, so a console watched during a bench shows slow animations; the default env.txt
  (`tools/switch/default_env.txt`) has no `GOW_BENCH`.
- Note: runs that never log `[memory card]` (stale git-ignored iopn_*/gow_overrides.cpp in a worktree) stay in the
  menu at 60 fps, speed 1.00; their numbers say nothing about gameplay.

### Below 60 fps: slow motion in the Switch ship fight (2026-10-25, nat-spd, ext agent/natspd f012118)
- Symptom: console default update_nv7foff, normal play (`nv7foffplay`): speed 1.00 at 57-60 fps but 0.927 over the
  169 windows at 40-52 fps (ship fight, 46.5 fps; single windows 0.75-1.08). The user saw slow animations.
- Cause, measured with `[gow-fclk]` / `[gow-fclk-long]` (below; logs port/switch/logs/bench_spd1-4play):
  1. Guest time lagged inside a frame. The VBlank rate stayed 60 Hz, but most frames read ~1 field
     (`[gow-speed-hist]` bins 9-10) while the wall frame was ~1.3 fields: the EE ran behind the VBlank wall grid during
     the frame and caught up in bursts at its waits, so a few frames read 2-6 fields.
  2. The ship fight really has long frames: about 3 per second of 70-230 ms (4-14 fields). In those the EE does
     ~13 ms of work and sleeps on the pacing grid, waiting for the GS (vid::WaitForDMAComplete) while the GS frontend
     is blocked on the full GL/deko3d queue (`gsq` 60-210 ms in the same frame). That is the fps limit itself; the
     average 46 fps is mostly 60 fps frames plus these stalls. The game cuts a frame to 4 fields (0x17D778 clamps
     fields to [1, 4]). A T0 gap above 65536 ticks (6.8 fields) even loses a whole wrap of its 16-bit extension
     (0x303628 / 0x2C7908), so a 7.5-field frame reads ~0.7 field.
- Fix, in two layers:
  - Runtime: `EeScheduler::catchUpToWall(maxCycles)` brings guest time up to the wall clock: the latest due point of
    the VBlank grid (VBlankStart and the pacing points have cycle and host deadline in lockstep), extrapolated to now,
    also past the pending VBlank; a lag below 1/8 field is left alone, so 60 fps frames keep reading exactly 1 field.
    catchUpToHost (the movie player's catch-up) is unchanged.
  - Game (gow_overrides `gowFrameClock`, replaces 0x17D778 unless `GOW_BENCH` / `GOW_FRAMECLOCK_TRACE`): before the
    game reads T0, catchUpToWall with a budget that keeps the frame being measured at most 3.75 fields
    (`GOW_FCLK_CAP`, hundredths of a field); after the read, the part of a long frame the game cut off (above 4
    fields, plus lost T0 wraps, found from the wall time between reads) becomes a debt, paid back as at most 1/2
    field per following frame by moving the game's frame start 0x29C674 back (read only by 0x17D778). The debt is
    capped at 6 fields (`GOW_FCLK_DEBT`, hundredths; 0 = no debt), so a loading pause is not replayed as fast motion.
    `GOW_FCLK_CATCHUP=0` turns both off. Benches (`GOW_BENCH=1`, fixed step) do not use it.
- Result (console, same package type, default clocks; bench rows in docs/benchmark.md "nat-spd"):

  | run | windows 40-52 fps: fps / speed | windows 52-57 fps | windows >= 57 fps |
  |---|---|---|---|
  | `nv7foffplay` (before) | 46.5 / **0.927** (169) | 54.1 / 1.010 | 59.9 / 1.015 |
  | `spd1play` (catch-up only, no cap / debt) | 45.3 / 0.961 (66) | 55.0 / 1.007 | 59.0 / 1.021 |
  | `spd4play` (catch-up + cap + debt) | 47.0 / **1.001** (237) | 53.7 / 1.031 | 59.9 / 1.015 |
  | `spd5play` (= spd4, catchUpToWall split out) | 47.0 / **1.003** (37) | 55.0 / 1.019 | 58.8 / 1.025 |

  The pad script is wall-timed, so how long a run stays in the heavy fight varies (window counts). Debt in spd4play:
  2.7 fields paid per second, mean 0.33, max 5.5. Bench `spd5g` 47.0 / 47.0 / 46.2 / 47.4 / 47.4 fps = `nv7foffg`
  (mean 47.0 vs 46.7). Console default from 2026-10-25 evening: update_spd5 (docs/status.md, nat-spd, also the PC
  check).
- What is left is the stalls themselves (smoothness, not speed): long GL/deko3d stretches every ~20 fields in the
  ship fight; finding them is GPU/GS work (`[gow-fclk-long]` prints the per-frame buckets).
- `[gow-fclk]` (with `GOW_DELTA_DIAG=1`, once per wall second): reads, reads caught up, fields added (sum / max),
  `t0_rate` (T0 fields the game measured per wall field), frames below 1 / above 4 fields, debt paid / left.
  `[gow-fclk-long]` (at most 8 per second): a frame above 4 fields or 4 wall fields: measured and wall fields, the
  frame's T0 before the catch-up, fields added, the VBlank count, and that frame's perf buckets (ms; ee, vu, gs, iop,
  guest wait, gsq wait; buckets need `GOW_PERF_DIAG=1`) and EE idle waits by wake-up cause.

### Game speed 1.03-1.09 in many zones: short frames and replayed EE stalls (2026-10-30, sw-bugs, update_bugs1)
- Symptom (console sweep port/switch/sweep/20261026_0256, `[gow-speed]` after the debug warp): ATHN03A, ATHN05B,
  ATHN06B, DEST02, PAND00A, ARENA06/10/12 at 1.055-1.082, most other zones 1.00-1.05. The warp itself is not the
  cause; two separate effects, measured per zone from `[gow-speed]` / `[gow-fclk]` / `[gow-speed-hist]`:
  1. **Short frames counted as a whole field** (normal play, any scene at ~45-59 fps with uneven frame times). The
     game's [1, 4] clamp (0x17D778) counts a 0.8-field frame as 1 field while a 1.3-field frame counts in full. In
     windows without any stall `t0_rate` (T0 per wall time) is 0.999 but game speed 1.03-1.09 (PAND00A 1.056 with
     no stall at all, 52 fps; ATHN03A 58 fps with `[gow-speed-hist]` bins 7-8 frequent). This is what a player sees:
     2-9 % fast motion wherever the frame rate hovers below 60.
  2. **Stalls of the EE thread replayed as fast motion** (the sweep's warp screenshots, 3.5 s each, written
     synchronously on the EE/GS path; in play any host stall that blocks the EE thread itself). Guest time stops,
     the VBlank grid's host deadlines keep their schedule, and `catchUpToWall` (nat-spd) then caught up the whole
     lag at up to 3.75 fields per frame: ATHN06B ran at 2.8x and 2.4x for the two seconds after each capture
     (`[gow-fclk] added=137 t0_rate=2.86`). Per-window means made it look like +6 %; over wall time it was a pause
     followed by a fast replay. Ordinary loads and long GPU frames are not affected: the EE sleeps on the pacing
     grid there, so T0 keeps wall time (the existing 4-field clamp + 6-field debt handle them).
- Fix (gow_overrides `gowFrameClockCatchUp`, EeScheduler `catchUpToWall`; port/switch/bugs1.patch):
  - Credit: the part a frame below 9600 ticks (one 60 Hz field) was counted too long is kept as a credit (at most
    `GOW_FCLK_CREDIT` hundredths of a field, default 200, 0 = off) and taken back from the next frames above 1 field
    by moving their frame start 0x29C674 later before the game reads T0 (never below 1 field). Credit and debt
    cancel each other. Simulation of the measured frame mix: 1.067 -> 1.000.
  - Lag bound: `catchUpToWall` drops a lag above `GOW_FCLK_MAXLAG` hundredths of a field (default 1500 = 250 ms,
    0 = no bound) by moving the host deadlines of the grid events (VBlankStart / VBlankEnd / pacing points) later;
    all grid events share one lag (cycle and host deadline in lockstep), so the grid stays consistent. 250 ms is
    above the longest normal GPU frame (70-230 ms in the ship fight), which is still caught up in full.
  - `[gow-fclk]` adds `credit=` (left), `taken=` (fields taken back this second) and `lag_dropped_ms=`.
- Console check (2026-10-30, port/switch/sweep/20261030_1723, update_bugs1): windows without a stall at 1.001-1.005
  (before 1.056-1.093): ATHN03A 1.001, ATHN06B 1.005, PAND00A 1.001, ARENA10 1.005; each 3.5 s warp capture shows
  `lag_dropped_ms` ~3150, credit taken back 1-4 fields per second. All four zones PASS. Console default from
  2026-10-30 evening: update_bugs1.
- Sweep judging: the per-window mean of `[gow-speed]` over-weights the windows right after a stall; game seconds over
  wall seconds of the windows without a stall is the better number (it was 1.00-1.09 before the fix).

### The ship-fight stalls: 170 frames queued on the GPU (2026-10-25, sw-stall, ext agent/swstall)
- Measured with the new GS-thread block diagnostics (docs/switch-deko3d.md "Stall diagnostics"; run `sws2play`,
  update_sws2 = spd5 code + diagnostics, normal play, default clocks): every long frame of the fight is the GS thread
  blocked inside `dkQueuePresentImage` (603 of 626 `[gs-dk-long]` blocks, 50-175 ms each, ~3 per second; the rest are
  waits for the 16 MB command ring), and at that moment **~170 presented frames are still unfinished on the GPU**
  (`submits_in_flight=170`, steady through the whole fight).
- Cause: nothing limited how far the GS thread runs ahead of the GPU. The swapchain image is acquired with a fence the
  GPU waits on (not the CPU), so the only back-pressure was deko3d's own 64 KB queue command memory (16 fence slices
  of 4 KB, deko3d `dk_queue.cpp` `flushRing` / `waitFenceRing`) and the 16 MB command ring. With ~380 bytes of queue
  commands per frame that is ~170 frames (~3.5 s at 47 fps): the GPU worked through a backlog seconds old (input lag
  of seconds), and whenever the ring filled, the flush in `dkQueuePresentImage` waited for the oldest 4 KB slice,
  i.e. ~10 frames of GPU work at once: 70-230 ms in which the GS frontend's 8-chunk queue filled and the EE waited
  (the `[gow-fclk-long]` stalls every ~18 VBlanks).
- Fix: `GOW_DK_FRAMES=n` (gs_dk.cpp `present`): after each present the GS thread waits until the frame n presents
  back has finished on the GPU, so the wait is spread over every frame and the GPU is at most n frames behind.
  Normal play `sws2f2` (n = 2): long frames 71.5 -> 3.4 per minute of play (mean 6.7 -> 5.0 wall fields, max 219 ->
  7.3), no present block over 25 ms (was 603), the per-frame wait (`frames`) at most ~21 ms. The pad script is
  wall-timed, and with seconds of input lag gone the run took another path through the fight (screenshots differ),
  so fps comes from the fixed-step bench (below).
  n = 3 (`sws2f3`, the fight as long as in the baseline): 238 windows at 47.7 fps (baseline `sws2play` 232 at 46.8),
  speed 1.008, long frames 5.4 per minute (max 8.1 fields), the per-second fps a steady 47-51 instead of 41-57.
  Fixed-step bench (`BENCH_GAME_S=150`, same session): `sws2b0` (no limit) 47.5 / 47.3 / 46.4 / 47.7 / 47.8,
  `sws2bf2` 44.7 / 47.0 / 47.5 / 48.6, `sws2bf3` 44.8 / 46.8 / 47.5 / 48.5 (the bench's 0-30 scene loses ~3 fps with a
  limit, the fight scenes gain up to 1).

## Input lag (sw-lag, 2026-10-26, tools/switch_lag/)
- Tool: `tools/switch_lag/lag_run.sh <name> [extra env]` (UPDATE=<pkg>): default env + SKIP_FMV + god mode, the bot
  starts a new game (bench menu path), then `lag_probe.py`: per trial a reference pixelPeek, `press <button>`, wait
  d ms, pixelPeek, release. pixelPeek takes 90-200 ms and seems to capture at its start (the d=0 and d=33 shots,
  whose windows overlap, differ), so the reaction time is bounded by d. Ends with the console default.
- Result update_sws5 / update_icons1 (`GOW_DK_FRAMES=3`, runs port/switch/logs/lag_sws5c, lag_icons3): in the
  upgrade menu (START in gameplay) D-pad down/up shows the new item already in the shot taken 33 ms after the press
  (cross-fade under way), not in the d=0 shot: the game reacts within ~2-4 frames, as on the PS2. The jump probe in
  the fight is not readable (enemies move between the shots). Before (swfix3, no limit): ~170 frames (~3.5 s)
  queued on the GPU (sw-stall measurement above), i.e. seconds.
- Queues left between the pad and the picture: the EE frame itself, the GS stage ring (4 MB, ~2 frames of GIF data
  when full), the GS frontend queue (8 x 64 KB chunks, under one frame), at most 3 presented frames on the GPU.
  `GOW_DK_FRAMES=2` would take one frame (~20 ms) off; the fight bench is the same with 2 or 3 (sw-stall), not changed.

## Presentation without readback (zero-copy, branch agent/present in ext/PS2Recomp-kl-present)
- **Before**: GSHwBackend::Present composed the CRTC circuits into presentTex, then glReadPixels into
  PresentationFrame::pixels; the host uploaded them again (UpdateTextureRec) and ran the bicubic scaler. The host
  waited for the GS worker on every latch: `upload_ms` ~0.7-1.1 s per 5 s window.
- **Now**: the GS GL context is created in GSHwBackend::Initialize (GS worker, while the host context is released)
  and shares objects with the raylib window (wglCreateContextAttribsARB with the host HGLRC, gs_gl.cpp).
  Present composes as before, forces alpha 1 on the GPU and copies presentTex into one of 8 shared slot textures
  with glCopyImageSubData (`GSGL::PublishSharedTexture`, fenced). The host draws the slot texture directly
  (`GSSharedPresent::Acquire`, gs_host_present.cpp). Present is queued (asynchronous) on the GS worker.
  On by default; `PS2X_GS_DIRECT_PRESENT=0` restores RAM frames.
- Zone captures (GOW_PAD_CAPTURAS) still get pixels: `copyLatchedHostPresentationFrame` sends a forceReadback
  request, which reads back once.
- Host scaler: the bicubic+sharpen pass only runs when the frame is magnified; a frame at least as large as its
  4:3/16:9 rectangle (e.g. `PS2X_GS_HW_SCALE=3` at 1920x1080, or a small window) gets one bilinear tap.
- Measured (Olympus slot 1, 1920x1080, scale 2, same build back to back, port/agent_present/r3_ram vs r3_shared):
  upload_ms 970-1100 → 23 ms per 5 s; game fps 36-38 in both (the game thread is the limit; the host
  thread is now idle in vsync, host_wait_ms 3.9 → 4.95 s). Captures identical (r2_ram vs r2_shared sheet.png).
- Check the window content with `GOW_HOST_SHOT=<host frame>` (writes host_shot.png = back buffer, next to the exe).
  Desktop grabs (tools/shot.ps1 PrintWindow, CopyFromScreen) of DEBUG_UI=OFF builds came back all white in
  both RAM and shared modes while host_shot.png is correct; cause **not found** (DEBUG_UI=ON builds grab fine).
- Switch: the mechanism is GL-generic (shared context + texture copy + fence sync); only context creation /
  host-context capture is WGL (gs_gl.cpp, `_WIN32` block). The non-Windows build has no GS GL context yet;
  an EGL/SDL version would capture eglGetCurrentContext in CaptureHostContext and pass it as share_context.
  glCopyImageSubData needs GL 4.3 / ARB_copy_image (or a glBlitFramebuffer fallback on GL 3.3).
- Worktree build pitfall: a fresh configure defaults to PS2X_ENABLE_AGRESSIVE_LOGS / RUNTIME_LOGS / DEBUG_UI /
  IOP_RPC_TRACE = ON and no PS2X_VU1_MICROCODE_DIR (interpreted VU1): ~3 fps and a GB-size ps2_log.txt.
  Copy those cache values from out/build-kl (logs/UI/trace OFF, VU1 dir port/vu1_micro + generator source).

## Cliffs of Madness 30 -> 60 fps: texture uploads (2026-10-12, ext branch agent/cliffs60)
- Cliffs (save slot 3) was GS-bound at ~32 fps: gs_ms 3330 per 5 s (~20 ms per frame) against 1150 in Olympus at 60 fps.
  The zone streams 4 bit (PSMT4) textures every frame. Sampling the GS worker (port/agent_ee/prof_run3.sh: zone slot 3 +
  muestrear) showed the time on the CPU, not the GPU: `GSMem::WriteP4` 25 %, `GSTextureStore::UploadAligned` ~20 %,
  `indexTextureFor` 16 % of the worker thread.
- Cause: host->local uploads were written one pixel at a time through a format switch and a non-inlined write call,
  with two integer divisions (pixel index -> x,y) per pixel; the index textures were decoded per texel through the
  generic `GSSwizzle` path.
- Fix: `GSMem::WriteRun` writes one row run of a transfer with the format resolved once and the page-table address
  math inlined (`PixelStorageTraits<psm>::Write`); `UploadAligned` splits the stream into row runs (no per-pixel
  division). `GSMem::ReadIndexRun` decodes a row of a paletted texture the same way for `indexTextureFor`. Same
  addresses as before (same page tables), so the GS memory contents and the textures are unchanged. Portable C++.
- Result: Cliffs 60 fps (gs_ms ~1640 per 5 s, GS worker busy 63 % instead of 85 %); the other zones stay at 60.


## Scratchpad fast path (2026-10-12, ext branch agent/scratchpad 96f61ab)
- The scratchpad (16 KB at 0x70000000, alias 0xF0000000) is plain memory, but `Ps2IsSpecialAddress` includes it, so
  every access in recompiled code took `runtime->Load/Store` (a call, the GOW_IO_HIST check, `syncGsWorkerFor`, a
  bounds-checked copy): ~40 M accesses per 170 s per zone, ~156 M in slot 4 (docs/native-port.md inventory).
- Now the READ*/WRITE* macros (include/ps2_runtime_macros.h) test for the scratchpad inside their special-address branch
  and access it directly (`Ps2ScratchpadFastPtr`); unaligned accesses and every other special address keep the runtime
  path (address error unchanged). RAM accesses are unchanged. No regeneration: the generated code already uses the
  macros (~150k access sites); only a full rebuild.
- Verified (port/spr/20261012_081449, baseline merged9): 4 zones ok, 60 fps everywhere, game speed 1.00-1.02, no image
  warnings; savetest ok. With `GOW_IO_HIST=60` in slot 4 the histogram window never closed: no access reached the
  runtime path after the first one for the whole run (before: ~156 M scratchpad accesses per 170 s).
- ee_ms did not drop in that run (slot 1 2088 vs 1920, slot 4 2989 vs 2683): ee_ms includes the GS-wait spin (see
  "EE checkpoint cost" above) and other forks were building at the same time, so it is not a cost measure here. The
  gain matters where the EE is the bottleneck (Switch); measure it there or with a profiler on an idle machine.

## Aegean Sea (new game) ship start: 43 fps, worker job bound (2026-10-12, ext branch agent/newgame)
- New-game test (`tools/zones.py --slots 0`): the first ship deck runs at ~43 fps while the camera looks over the rain
  and the sea (from gameplay +15 s until Kratos moves on); the rest of the fight holds 60 fps.
- Per frame there: VU1 ~6.7 ms + GS frontend ~8.2 ms (Olympus: ~4 + ~4), ~16 M primitives per 5 s. Worker trace
  (`PS2X_GS_WORKER_TRACE`, port/agent_ee/gap.py): job 17.9 ms + EE gap 4.5 ms = ~22 ms per frame.
- Not the GPU and not the GL queue: `PS2X_GS_HW_SCALE=1` and `PS2X_GS_QUEUE_CHUNKS=48` leave it at 43-45 fps.
  Sampling (muestrear) shows no thread saturated (EE 34 %, VIF/VU1/GS-frontend thread 28 %, GL thread 26 %): the
  stages run one after another. `decodeDirect` was 12 % of the GL thread; it now reads rows (`GSMem::ReadColorRun`,
  bit-identical, `GOW_GS_TEXCHECK=1` verifies) but fps did not change.
- Fix needed: run VU1 and the GS frontend as a pipeline (the GIF arbiter's path-packet callback is the boundary:
  VU1/XGKICK output queued to a GS-frontend thread), so the job costs max(VU1, GS) instead of the sum. Not done yet:
  it changes which thread owns `GS` state (EE-side syncs `syncGsWorkerIfOtherThread`, FINISH, transfers).

## VU1 / GS frontend pipeline (2026-10-12, ext branch agent/pipeline)
- With the GS worker (`PS2X_GS_WORKER=1`) the worker ran VIF1 + VU1 and, after every XGKICK, the GS frontend
  (`GS::processGIFPacket`) on the same thread, so a frame cost VU1 + GS frontend (ship start: ~6.7 + ~8.2 ms).
- Now the GIF arbiter's packet callback hands each packet (copied, in arbiter order) to a GS frontend stage thread
  (`ps2xRuntime/src/lib/gs/gs_stage.cpp`); VU1 of packet N+1 overlaps the GS frontend of packet N. The queue is bounded
  (4 MB) so the worker cannot run a frame ahead.
- Synchronisation is unchanged for everyone else: `PS2Memory::waitGsWorkerIdle` waits for the worker *and* the stage,
  so every EE access that synced with the worker (GS register stores, readbacks, display state, FINISH handshake)
  still sees all queued GS work done. The stage never waits for itself (`onStageThread`), and it wakes the EE after a
  batch when the GS raised an interrupt (FINISH/SIGNAL), as the worker did after a job.
- `PS2X_GS_PIPELINE=0` keeps the GS frontend on the worker thread (A/B and fallback). Portable C++ (std::thread).
- Queue (2026-10-24, sw-k7): a single-producer / single-consumer ring of 256 packet pointers. The worker gathers small
  packets in its current packet and publishes it (16 KB or at the job's end) by moving `head`; the stage frees a packet
  by moving `tail`; the worker takes freed packets back onto its own LIFO free list, so the buffer it fills next is the
  one the stage finished last (a plain ring of 256 x 16 KB buffers cycled 4 MB through the console's 2 MB L2: title
  VU1 time +4 %). The mutex and condition variables are only for sleeping and waking (seq_cst flag / index pairs on
  both sides). The idle stage polls `GOW_GS_STAGE_SPIN_US` (Switch default 100 us) before it sleeps. Console: the VU1
  thread's `pthread_mutex_lock <- flushStaged` share 2.1-2.5 % -> ~1.0 %.
- Measured (idle new-game run, port/newgame/b6idle_*: the ship start with the camera over the rain and sea):
  pipeline on 59.2-60.2 fps through the whole scene, `PS2X_GS_PIPELINE=0` 36-42 fps; game speed 1.00 both.
  Zones 0-4 (port/b6_zones/20261012_143318 + port/b6_zones2/20261012_145143 for slots 2 and 4, whose first runs
  were killed from outside by another test's `taskkill /IM`): 60 fps everywhere, speed 1.00, no image warnings;
  savetest ok.

## VIF1 chain streaming: the VU1 thread starts while the EE copies (2026-10-24, sw-k8, ext dfadf1a)
- Why: the VU1 thread idled ~1.9 ms per frame in two waits, the EE's path from one job's end to the next kick; part of
  it is `sendDmaChain` walking and copying the next chain (~430 KB, ~0.4 ms; `[gsw-chain]`) before the job is queued.
- Now (`GOW_CHAIN_STREAM_KB`, Switch default 64, PC default 0 = off): once 64 KB of a VIF1 chain are copied (checked
  at each tag's end) the job is queued with a `GsChainStream`; every further 64 KB is published (release store of
  `published`, `base` re-read after each). `runChainStream` (worker) parses the published bytes with
  `m_vif1Streaming` set: processVIF1Data then stops at a command that is not complete yet (a DIRECT whose payload is
  not all there, or anything within 4100 bytes of the end whose size says it does not fit) without touching it
  (`m_vif1LastPos`), and parses it again when more bytes arrive; once the EE is done the rest goes through the normal
  call (a truncated last command behaves as before). Same bytes, same order, each command executed whole: the
  results are those of one call over the whole chain. The EE reallocates its buffer only while the worker has
  acknowledged every published byte (`consumed`); the buffer is reserved at 8 MB first.
- Publish lag `GOW_CHAIN_STREAM_LAG_KB` (default 64): only bytes copied >= 64 KB ago are published. With no lag the
  worker read lines still dirty in the EE core's L1 and the VU1 time per run rose 4 % (13.76 -> 14.31 us).
- First version (EE-side command scan to find boundaries): the scan cost the EE ~170 ms per 5 s and, measured with
  streaming disabled behind it (`GOW_CHAIN_STREAM_KB=100000`), took 1.4-1.7 fps by delaying every kick; it also gave
  up at an UNPACK before the chain's first STCYCL, so only ~40 % of the chains streamed. The parser-side stop needs no scan:
  `[chain-stream] ... partial=0` on the console (tag ends always fall between commands in this game).
- Console bench (docs/benchmark.md; scenes heavy 10-20 / mid 30-60 / late 90-150 game s; per-run spread 0-0.2 fps in
  the identical part):
  | run | package / env | heavy | 20-30 | mid | 60-90 | 90-120 | 120-150 | chain_ms / 5 s | VU1 us/run |
  |---|---|---|---|---|---|---|---|---|---|
  | `b2a` / `b2b` | update_b2, stream off | 35.3 / 35.3 | 44.1 / 44.1 | 45.2 / 45.2 | 46.6 / 45.8 | 47.6 / 47.0 | 47.2 / 47.3 | 179-198 | 13.76 |
  | `sc1` | scan only (never publishes) | 33.9 | 42.4 | 43.5 | 43.9 | 46.7 | 45.6 | 333-387 | - |
  | `s2a` | first version (EE scan) | 36.1 | 45.2 | 46.4 | 46.8 | 48.0 | 48.2 | 368-461 | 13.92 |
  | `s3a` | parser-side, no lag | 35.5 | 44.4 | 45.4 | 45.7 | 47.0 | 47.4 | 207-229 | 14.31 |
  | **`s4a`** | **parser-side, lag 64 (default)** | **36.4** | **45.3** | **46.4** | **47.6** | **48.7** | **48.7** | 207-240 | 13.95 |
  | `s4b` | lag 192 | 36.0 | 44.8 | 45.9 | 46.4 | 47.6 | 47.8 | ~230 | 14.04 |
  VU1 thread idle 395-460 -> 220-290 ms per 5 s; the frames are the same (signatures identical to b2a for 63 s).
- Exactness: x86 replay dumps fight.rec / menu.rec, HLE=1 / 0, byte-identical (harness `s4x`; the VU1 path is not
  touched); the streamed parse is equivalent by construction (above) and the console bench's game state is identical.

## VU1 thread throughput: fewer and cheaper chain steps, leaner kernels, linear unpack (2026-10-24, sw-k9)
- Where the VU1 thread went (bench `k9p` = update_s4 + `GOW_PC_SAMPLE=1000`, heavy scene 75-85 s wall): generated blocks
  24 %, chainStepOut 13.9, kCullClip 5.6, kUnlitSt 5.6, idle 5.0, processVIF1Data 3.6, kLitSt 3.6, kSkinLitSt 3.1,
  hleChain 3.0, memmove 2.7, runNative 2.3, unpackFast ~7 (six formats), codeChanged 1.2, slotFor / hleMatch 0.75 each.
  memmove is `ps2_gs_stage::push <- GifArbiter::submitAndDrain` (`GOW_PC_SAMPLE_LR` on memcpy/memmove): the copy of
  each XGKICK packet into the GS stage queue. The mid scene (100-140 s) has the same shares.
- A chain step costs ~40 A57 cycles for ~128 instructions (3.4 M steps/s at ~14 % of the thread): instruction-bound,
  not miss-bound, so fewer instructions per step and fewer steps both pay.
- **Fewer chain steps (generator, main)**: a harness-only step profile (`EXTRA=-DVU1N_STEP_STATS`,
  `VU1N_STEP_FILE=<file>`: steps by from slot / key, to pc, result) against the generator's trace log
  (`VU1N_GEN_TRACELOG=<file>`: every trace head with its parts and why it stopped) showed where the 204 k steps of
  fight.rec (2 passes) come from: 41 % leave a block or trace at a JR / JALR (traces stopped there), 20 % leave blocks
  that never start a trace (a block that is some block's likely successor only ran inside that block's trace; at run
  time it is entered from side exits and other predecessors), 11 % are side exits, 8 % blocks the trace probe rejects,
  ~5 % stop before loop blocks, 3 % at the 48-pair limit; only 1.6 % go to blocks that read entry flags. The edge
  profile was also stale (from recordings lost with the distro). Now: `port/vu1n_profile.txt` = the old profile plus
  fight.rec + menu.rec (`tools/vu1_harness/edgeprof.sh`), traces through JR targets (`--jr-traces`), every block may
  start a trace (`--trace-all-heads`), 96 pairs per trace (`--trace-max-pairs`). Steps on fight.rec 204 k -> 147 k
  (gen4 -> g9; `--jr-traces` alone 182 k), menu.rec 307 k -> 266 k; generated code 8.46 -> 9.32 MB.
- **chainStepOut as a leaf (ext)**: the out-of-line step had a 160-byte frame with six callee-saved registers on every
  call (the rare paths call commitReadyPipelines, hleChain, blockTiming). The common case is now a leaf without a frame:
  history entry, the due FDIV/EFU retirement (`retireScalars`), stop / budget / kernel byte / key checks, successor
  set-up; a block that started FDIV/EFU operations, a commit with other queues busy, a kernel pc, a key mismatch or a
  successor that reads entry flags (`chainStepEntry`: the memo) continue out of line by tail call exactly where
  chainStep would (`chainStepSlow` / `chainStepSlowCommit` / `chainStepSlowTail`). On fight.rec 111 k of 143 k steps
  take the leaf (`-DVU1N_SLOW_STATS`); the rest are kernel entries (31 k, hleChain anyway) and FDIV/EFU blocks.
  aarch64 harness: chainStepOut + slow parts 2.06 -> ~1.4 M instructions per warm pass (ics_fight.rec).
- **Kernels (ext)**: kUnlitSt, kLitSt, kSkinLitSt ran every partial register write as a 128-bit blend, as the VU code
  does. A register whose other lanes stay untouched through the loop and are never read inside it (only bc<0..2>,
  xyz stores, xy converts) is now kept unblended in the loop and blended once at the exit; two partial writes that
  together cover the register are one blend. Six blends fewer per vertex in each (aarch64 harness: kUnlitSt 0.753 ->
  0.569 M, kSkinLitSt 0.754 -> 0.634, kLitSt 0.325 -> 0.262 instructions per warm pass). The A57 executes a 128-bit
  NEON op as two 64-bit micro-ops: these loops are FP-pipe bound (~29 cycles per vertex in kUnlitSt), not latency
  bound. kCullClip (flag-exact, branchy) is unchanged.
- **NEON helpers (ext, ps2_vu1_native.h, aarch64 only)**: `itofv<4/12/15>` is one SCVTF with fraction bits (was SCVTF +
  FMUL: scaling an integer conversion by 2^-n is exact, one rounding either way), `ftoiv<4/12/15>` one FCVTZS with
  fraction bits after the clamp (the product by 2^n is exact or saturates the same way), `lane<C>` one DUP (was a trip
  through a general register). Used by every generated block and kernel.
- **hleMatch (ext)**: a remembered kernel was re-confirmed at every code change near its pc by the byte-wise FNV of its
  region (up to 320 dependent multiplies); the bytes of a region that matched are kept per kernel and an equal region
  matches by memcmp (other bytes still go through the hash: the same decisions).
- **VIF1 UNPACK with CL == WL (ext ps2_vif1_interpreter.cpp `unpackLinear`)**: the destination is vuAddr + i (mod 1024),
  so vectors go out in groups of four between wraps without the per-vector cycle position, block multiply and branch.
  A unit test (scratchpad `k9/ut`) compares it with unpackLoop for every format, mode, sign and wrap: identical.
- Exactness: x86 replay dumps fight.rec / menu.rec, HLE=1 / 0, byte-identical to 529de89 and k3g with gen4 and with
  the new blocks (harnesses `c2x`..`c8x`, `g7x`, `g9x`); aarch64 (qemu) dumps of ics_fight / ic_menu, HLE=1 / 0,
  identical between the old and the new runtime (`aq.sh`). Bench signatures identical (see below).
- Console bench (heavy 10-20 / mid 30-60 / late 90-150 game s; `k9s` = update_s4 again today, the reference):
  | run | package | heavy | mid | late | VU1 busy heavy (ms / 5 s) |
  |---|---|---|---|---|---|
  | `k9s` | update_s4 (before) | 36.4 | 46.5 | 48.2 | 4797 |
  | `k9a1` / `k9a2` | g7 blocks (profile + `--jr-traces`) | 36.6 / 36.3 | 46.9 / 46.9 | 49.5 / 48.7 | 4797 / 4783 |
  | `k9c1` | + chainStep leaf (first form) | 36.8 | 47.3 | 49.9 | 4786 |
  | `k9e1` / `k9e2` | + kernels, NEON helpers, hleMatch, unpack | 37.9 / 37.9 | 48.4 / 48.2 | 50.9 / 50.2 | 4765 / 4778 |
  | **`k9g1` / `k9g2`** | **update_k9g (+ g9 blocks: all heads, 96 pairs; chainStep leaf incl. commit / entry memo)** | **38.3 / 38.9** | **49.1 / 49.7** | **50.9 / 51.7** | 4765 / 4755 |
  VU1 busy stays ~4.75 s per 5 s: the thread is still the limit, it just gets more frames out of it.

## VU1 thread throughput, part 2: fast traces, loaded values, VIF prefetch (2026-10-24, sw-k10)
- Profile of k9g (bench `k10p` = update_k9g + `GOW_PC_SAMPLE=1000`, heavy scene 75-85 s wall, VU1 thread): generated
  blocks 25.9 %, chainStepOut 7.0 (13.9 before sw-k9), kCullClip 6.1, idle 5.3, kUnlitSt 4.8, processVIF1Data 3.7,
  hleChain 3.3, kSkinLitSt 3.25, memmove 2.9 (+ `ps2_gs_stage::push` 0.6), kLitSt 2.7, runNative 2.5, chainStepSlow +
  SlowTail 3.0, kCull 1.9, unpack ~7.4 (unpackFast<3,2,0> 2.0, <1,1,0> 1.9, <2,2,0> 1.3, <3,1,1> 1.1, <3,1,0> 1.0,
  unpackLinear 0.8), VU1Interpreter::run 1.6, codeChanged 1.4, execEfu + queueP 1.4, setupTrace 0.8, slotFor 0.8,
  hleMatch 0.7, finishXgkick 0.7, syncInflight 0.6, flushPipelines 0.6, exportFlags 0.5. Hottest generated functions:
  t0ec8 (a matrix compose) 2.3 %, b1748 1.5 %, then < 1 % each.
- Generated code (main tools/vu1/vu1n_gen.py, docs/vu1-hle.md "Fast traces and loaded values"): traces get fast
  variants and JR trace share 0.3 (default since sw-k10), loaded values as range-analysis origins (default since
  sw-k11; its first console run `k10d1` "went black" from a flat battery, not the build). Bit-exact; qemu a57_cost -3.1 % of the VU1 work
  (-4.5 % with `--mem-origins`).
- **VIF prefetch** (ext 0927485, `GOW_VIF_PREFETCH`): the samples in the unpack loops sat on the source loads (V4-8:
  903 of 2141 samples on the `ld1`, 783 on the store waiting for it; processVIF1Data on its command-word loads): the
  DMA chain the EE thread has just copied on core 1 is not in the VU1 core's L1. The command loop and the unpack loops
  now prefetch 512 bytes ahead.
- Not done, with the reason: the GS stage copy (memmove 2.9 %) is the one copy of each XGKICK packet out of VU memory,
  which the next UNPACKs overwrite while the stage runs up to 4 MB behind, so it cannot be consumed in place; kCullClip is
  latency-bound on its loop-carried ACC/vf29 chain (the hottest samples: B2's first FMUL waiting for vf29, and the
  data-dependent path A/B branch at `vi14`); the per-run overhead (~5 %) is spread over a dozen functions with no hot
  spot; blocks with EFU ops (11.5 % of the chain steps on fight.rec, "headno:local") cannot join traces because
  efuOp's commit and the P reads depend on the scalar pipeline state the chain step updates between blocks.
- Console bench (GPU max; heavy 10-20 / mid 30-60 / late 90-150 game s; seed state A / B, docs/benchmark.md):
  | run | package | heavy | mid | late | VU1 busy heavy | state |
  |---|---|---|---|---|---|---|
  | `k9g1` / `k9g2` | update_k9g (sw-k9) | 38.3 / 38.9 | 49.1 / 49.7 | 50.9 / 51.7 | 4765 / 4755 | B / B |
  | `k10p` | k9g + sampler | 38.1 | 48.4 | 51.1 | 4708 | B |
  | `k10a1` / `k10a2` | fast traces (t1 blocks) | 39.3 / 39.0 | 50.1 / 49.8 | 51.9 / 52.2 | 4769 / 4764 | A / B |
  | `k10b1` | + VIF prefetch | 39.2 | 50.0 | 52.9 | 4753 | A |
  | `k10c1` / `k10c2` | + JR trace share 0.3 (t2) = **the sw-k10 default** | 39.3 / 39.2 | 50.1 / 50.2 | 52.6 / 52.6 | 4763 / 4739 | A / B |
  | `k10d1` | + loaded values (t3, `--mem-origins`) | - | - | - | - | - |
  `k10d1`: fighting normally at 11:21:17 (screenshot g5), black from 11:21:48 on, then no MTP and no bot (port 6000
  times out, ping answers); no log. `k10d2` and the k10e runs did not start. Bench signatures of every completed run
  match the reference of their state (k10a1 vs k9s: all 345 equal; B runs vs k9g2: equal up to the usual 64 s).
  So sw-k10 got heavy +0.5-0.9 fps, mid +0.6-1.0, late +0.9-1.7 over k9g (same states), VU1 still ~4.75 s busy per 5 s:
  still the limit everywhere; no scene became limited by another thread.

## The GPU at the default clocks (2026-10-24, sw-k11)
Target since 2026-10-24 (user): 60 fps in fights at the **default handheld clocks** (GPU 307.2 MHz, EMC 1331.2 MHz, CPU
at the game's 1785 MHz boost), then lower clocks. Bench at those clocks (docs/benchmark.md "Clocks"; heavy 10-20 / mid
30-60 / late 90-150 game s):
| run | package | heavy | mid | late | VU1 busy mid | GPU busy mid (`gpu`) | state |
|---|---|---|---|---|---|---|---|
| `dc1` | update_k10c (t2 blocks) | 29.1 | 35.0 | 37.6 | 3162 | - | A |
| `de1` | update_k10e (t4 blocks = the default generator now) | 29.3 | 35.2 | 37.4 | 3069 | - | A |
| `g1` | update_k11a (k10e + GPU timer, `GOW_DK_GPU_TIME=1`) | 29.0 | 35.0 | 37.5 | 3133 | 4930 | A |
| `x0` | k11b build, `GOW_GS_HW_EXPAND=0` (= k11a) | 29.1 | 35.0 | 37.6 | 3068 | 4923 | A |
| `g3` | update_k11a + `GOW_SCALER=bilinear` | 30.1 | **37.7** | **40.3** | 3414 | 4940 | A |
| `g2` | update_k11a at `GOW_GPU_MHZ=max` (460.8) | 40.3 | 51.7 | 54.1 | 4723 | 4877 | A |
- **The GPU is the limit at the default clocks, everywhere in the fight**: busy 4.9-5.8 s per 5 s (the heavy scene's
  windows overlap the load), the VU1 thread idles 1.6-1.9 s per 5 s in mid / late, the EE waits 1.7-1.9 s on the GS queue
  (`gsq`), and the GS stage's `present` op (dkQueuePresentImage) takes ~1 s per 5 s. The VU1 work of sw-k10/k11 (k10c ->
  k10e) changes nothing there (29.1 / 35.0 / 37.6 vs 29.3 / 35.2 / 37.4).
- **And still nearly the limit at GPU max**: `g2` has GPU busy 4.4-4.9 s per 5 s at 52-54 fps (VU1 4.7 s): the GPU
  work per frame scales with the clock (mid: 28 ms at 307 MHz, 18.8 ms at 460.8 MHz, ratio 1.5 = the clock ratio), so
  it is shader / raster bound on the GPU core, not memory bound.
- **Where the GPU time goes** (`[gs-dk-gpu]`, mid fight, per 5 s at 35 fps; docs/switch-deko3d.md): draws 3.9-4.0 s
  (~22.7 ms per frame), shuffles 0.46 s (2.6 ms), the present upscale 0.43 s (2.5 ms: bicubic + sharpen = 9 texture
  fetches per 1280x720 pixel), the present composition 0.06 s, clears 0.02 s, uploads / copies / readbacks ~0. Draw
  programs: the paletted, bilinear-filtered, alpha-tested variants (draw keys 203425 / 205425 = pass / fail pass of the
  same batches, 202925 / 204925, 202c25 / 204c25, 202825 / 204825; all `Indexed Linear Pass=1|2 Dual`) take ~2.9 s of the
  4 s; 203425 alone 1.1 s and its fail pass 205425 0.48 s.
- **Scaler**: `GOW_SCALER=bilinear` (one tap) cuts the upscale 0.43 -> 0.09 s and gives mid +2.7 fps, late +2.8 (`g3`
  vs `g1`); the picture is softer, so it is the user's choice (not made default).
- **Palette expansion tried and dropped** (`GOW_GS_HW_EXPAND`, scratchpad `k11/gs_hw_backend.expand.cpp`): filtered
  paletted draws sampled an RGBA8 copy of the index texture with the palette applied (made on the GPU once per index
  texture contents and palette, LRU within 48 MB) with the GPU's bilinear filter, one fetch instead of a gather and four
  palette fetches. It worked (112-497 copies, hits ~9 k per 5 s, made 0 per 5 s in steady scenes), but the draw time
  hardly moved (intro: variant 203421 1.31-1.33 s vs 203425 1.47 s per 5 s, -10 %), so these draws are not bound by
  their texture fetches; and both runs with it (`x1`, `x2`, seed state B) closed with a system error ~10 s into the
  fight (x2 held 48 MB, so not memory; not found). Reverted.
- **Fragment counts** (`f1` = update_k11d, the timer with fragment shader invocations; mid fight, per 5 s at 35 fps):
  | program | key | ms | M fragments | per frame | M frag/s |
  |---|---|---|---|---|---|
  | draw p10 | 203425 (pass 1) | 1031 | 252 | 1.43 M (5x the 640x448 screen) | 245 |
  | draw p11 | 205425 (its fail pass) | 474 | 133 | 0.76 M | 283 |
  | draw p31 / p30 | 204925 / 202925 (fail / pass) | 413 / 368 | 174 / 174 | 0.99 M each | 422 / 473 |
  | shuffle | - | 446 | 177 | 1.0 M | 397 |
  | scale (upscale) | - | 419 | 147 | 0.84 M (= 1280x720) | 352 |
  The GPU shades ~7 M fragments per frame at 250-470 M per second: it is fragment-throughput bound, by the number of
  fragments (overdraw, ~5x on the main alpha-tested variant) times a long fragment shader (~4 ns per fragment on the
  main one). The fail passes shade their batch again (p30 / p31: the same 174 M; p10 / p11: half, early Z helps there).
  Levers in order: shade each alpha-tested batch once (pass / fail in one draw where AFAIL allows, or a cheaper fail
  pass), shorter specialised shaders for the hot variants, fewer shuffle fragments, a cheaper upscale.

## Alpha-tested batches drawn once where AFAIL allows (2026-10-25, nat-vu1m, ext 9e1cf56)
The backend's alpha test drew every AFAIL != KEEP batch twice (pass draw, fail draw, each shading the whole batch).
`alphaPlanFor` (ext `gs_hw_backend.cpp`) now decides vertex-alpha batches per triangle on the CPU (the 202925 / 204925
pair: all fail, only the fail draw remains, pixel-identical) and merges texture-alpha batches whose two draws write the
same; the models (RGB_ONLY + Z write, 203425 / 205425) stay two passes, `GOW_GS_HW_FAILGT=1` lets early Z skip their
passing fragments in the fail draw (0.1 % of pixels differ, max abs 8). Details, data and next steps:
docs/native-render-study.md "step 7 on the renderer side". Console GPU time: not measured yet.

## Alpha test: RGB_ONLY in one draw, a stencil-split fail draw (2026-10-25, nat-vu1o, ext d32e48d)
RGB_ONLY batches without Z write that write alpha (6.5 k per 5 s in the bench fight, the second-largest two-pass
group) are now drawn once: the shader keeps every fragment and its dual-source output's alpha says pass / fail, the
alpha blend keeps the target's alpha for failing fragments (exact; the blend order inside the batch is now the GS's
primitive order, visible only in overlapping HUD glyphs). `GOW_GS_HW_ASEL=0` restores the two passes. The models
(RGB_ONLY + Z write, 9.3 M triangles per 5 s) get an experiment, `GOW_GS_HW_FAILSPLIT=1`: stencil-marked pass pixels,
the fail draw split into GEQUAL on unmarked and GREATER on marked pixels (7.8 k px over 36 bench shots differ, vs
9.3 k for `GOW_GS_HW_FAILGT=1`); it needs a depth-stencil format for every target. Console GPU time of all three:
not measured yet. Details: docs/native-render-study.md "step 7 on the renderer side, part 2".

## Shadow volumes counted in the stencil (2026-10-25, nat-vu1p, ext 76c64cd, experiment)
`GOW_GS_HW_STENCILVOL=1`: the character shadow volume (one batch per frame, ~3.1 k triangles, +1 / -1 per triangle)
counts in the stencil instead of the colour's G byte, and 15 stencil-tested constant draws over the volume's box (one
per bit) hand the count back to G for the game's shuffle and darken pass; the volume target's depth becomes
DEPTH32F_STENCIL8. Pixels and `[bench]` signatures equal the wrap groups on the PC bench. PC GPU time per 5 s: wrap
groups 26-31 ms, stencil path 35-42 ms (the count alone costs the same as the wrap groups; the bit draws add ~9 ms), so
it is off by default. Console: not measured; the 64-bit depth format already cost FAILSPLIT 2-4 fps there, so it can
only pay if the stencil-only count saves more than that format costs (`GOW_GS_GPU_TIME`: `draw:...W` vs `draw:...S`).
Details: docs/native-render-study.md "step 7 on the renderer side, part 3".

## Menu clock: the Load Game menu ran 8 % fast (2026-10-13, ext branches agent/menuclock, agent/menuclock2)
- Measured (port/menuclock, `GOW_DELTA_DIAG`, `GOW_VBLANK_DIAG`, `GOW_IOP_DRIFT`, `GOW_AUDIO_STATS`): title screen,
  main menu, pause menu and gameplay run at game speed 1.000, VBlank 59.99-60.00 Hz, IOP clock 1.000 +- 0.0004 of wall
  time, audio rate <= 1.002, in both the zones.py and the play.cmd environment (the ~3.7 % of 2026-10-12 is gone).
  Only the **Load Game (memory card) menu** ran fast: game speed 1.08, per-frame histogram alternating 0.8 / 1.1
  fields (`[gow-speed-hist] 8:30 11:30`).
- Cause (`GOW_FRAMECLOCK_TRACE=1`): in every second frame the menu sends three memory-card commands (MC2 socket
  0x8000131C, cmd 4, 3, 1) before the frame-clock read (0x17D778, called from the frame function 0x17A9E8 after the
  menu's update). Each command cost exactly 520 T0 ticks (0.9 ms) of guest time: libmc2's send (0x27B568) byte-copies
  the 0x2080-byte payload in an 8-instruction loop, and every taken backward branch of recompiled code charged a flat
  32 EE cycles (8320 x 32 = 266k cycles). The read moved 0.16 field late in those frames; the game counts a frame
  shorter than one field as one field and a longer one in full, so its step summed to 1.08 s per second.
- Fix (recompiler, ps2xRecomp control_flow_emitter.cpp): a backward edge charges the loop's straight-line length
  (target through the branch and its delay slot, ~1 cycle per instruction), at most the former 32. 1689 of 6418
  generated files change (only the `eeCheckpointDue(N)` argument; port/usa/recomp-clock.toml = recomp-movies2.toml with
  its own output dir). Nothing is charged more than before, so no frame can newly overrun its field.
- Result: Load Game menu 1.019-1.020 (histogram 0.9 / 1.0 fields), everything else unchanged at 1.000. The remaining
  ~2 % is the game's own design: the three card commands still cost ~0.04 field of real copy work in every second
  frame, and any read jitter only adds game time (expected on a console / PCSX2 as well; not measured there).
- Not the cause, tried and dropped: stepping guest time in IOP-batch steps while every EE thread waits (so an EE thread
  waiting for the IOP resumes when the IOP is done instead of at the next pacing point): no change, the 520 ticks were
  executed cycles, not waits.

## Ship deck ATHN01D: VIF1 UNPACK fast path, micro-memory changes (2026-10-16, ext branch agent/vu1perf)
- Scene: `tools/levels.py --zones ATHN01D` (ship deck in the rain, save altar), 37-41 fps. No thread is saturated:
  the game kicks one ~3 MB VIF1 chain per frame and waits for it before the next frame's work, so a frame is the worker
  job plus ~5-6 ms of EE work after it (`PS2X_GS_WORKER_TRACE`, port/vu1perf/gap.py). Job 21.4 ms, gap 6.0 ms.
- Profile of the VIF1/VU1 worker (tools/sample_prof.py, which now names DLL samples by module): native blocks 33 %,
  `processVIF1Data` 20 % (the per-vector, per-field UNPACK loop), the native dispatch loop 17 %, commit/reset 6 %,
  slot setup after micro-memory changes ~3 %. ~2400 VU1 programs and ~480k native blocks (7.7 pairs each) per frame.
- Changes (results unchanged; VU1 replay of 4000 ATHN01D runs bit-identical, 0 differ):
  - UNPACK without a write mask and with CL >= WL runs a loop specialised per (VN, VL, STMOD mode)
    (ps2_vif1_interpreter.cpp `unpackFast`); masked or CL < WL UNPACKs keep the general loop. `processVIF1Data` 20 % -> ~5 %.
  - An MPG that writes the bytes already in micro memory no longer bumps the code generation.
  - A real micro-memory change invalidates only the native slots whose extent overlaps a changed pair
    (docs/vu1-native-flags.md, "Code changes"); the game rewrites part of micro memory ~20 times per frame.
  - Slot layout: the per-block fields share one cache line, the idle table's length is in the slot.
- Result: worker job 21.4 -> 16.1 ms. ATHN01D mean fps +2.3 to +4 (A/B alone, two rounds: 40.8 -> 43.1,
  36.3 -> 38.6; the scene varies run to run). ATHN03A (45-50) and PAND04G (mean 59) unchanged: not job bound.
  Zones 0-4 60 fps, speed 1.00, savetest ok (port/vu1perf/gate, port/vu1perf/savetest).
- What is left in the job is VU1 (~10 ms/frame in the replay bench). The dispatch loop costs ~0.7x the block code
  itself, mostly the unpredictable indirect call per 8-pair block and the slot/hist cache lines; micro-changes to the
  loop measured no difference. The next lever is generator-side: blocks that chain to their successor (or larger
  blocks) so fewer pass through the loop. 60 fps here needs the job at ~11 ms.

## Ship deck ATHN01D: allocations and copies in the job and the EE gap (2026-10-16, ext branch agent/athn01d)
- Profile with DLL exports named (`tools/sample_prof.py --dll-exports`): part of the worker's "ntdll" time was heap
  work, not waiting: RtlAllocateHeap 6.8 % + RtlFreeHeap 0.6 % of the thread, plus memcpy. Every XGKICK packet was
  copied into a new vector by `GifArbiter::submit` before the GS stage copied it again into its own buffer, and the
  stage queue was a `std::deque` (MSVC allocates one block per 32-byte element).
- The EE gap after the job (serial in this game: it waits for the chain) held ~1.5 ms inside 0x13F730 building the
  next chain: `sendDmaChain` gathered ~3 MB into a fresh vector (~20 reallocations, new pages each time) that the
  worker freed after the job.
- Changes (output unchanged):
  - `GifArbiter::submitAndDrain`: with nothing queued the packet goes to the callback in place (XGKICK, PATH2).
  - GS stage queue and batch are vectors (capacity kept across swaps).
  - DMA transfer buffers are pooled (ps2_memory.cpp, up to 4): the worker returns each job's buffers.
- Result, interleaved A/B on an idle machine (port/athn01d/STATE.md): standing still (`levels.py --idle`) job 17.1 ->
  15.3 ms, gap 2.8 -> 1.6 ms, 51 -> 59.6 fps; moving and fighting job 17-18 -> 12-15 ms, 50-52 -> 59-60 fps (min 54-59).
- Left: the dispatch loop (~18 % of the worker) and per-program scheduler resets; the GS stage thread is now ~70 % busy
  (vertexKick) and is the next limit; the EE spins through the whole job (one core, matters on the Switch).

## Switch GS thread: texture parking cache, 4-lane range hash, one queue lock per chunk (2026-10-31, sw-dips, port/switch/dips4.patch)
- Cause: in the GS-thread-bound zones the game streams textures through a few GS memory areas, more contents per area
  than the three earlier contents a texture keeps, and the same texture lands at other addresses in other frames:
  1200-2450 texture decodes (+ glTexImage2D) per 5 s. The range hash was one latency-bound multiply chain, and the GS
  worker took the queue mutex twice per chunk (svcArbitrateLock ~9 % of its samples).
- Changes (gs_hw_backend.cpp, gs_threaded_backend.cpp; patch on the update_bugs1 sources):
  - a GL texture that would be deleted (oldest earlier content, evicted texture) is parked, keyed by its shape (format,
    TBW, size, TEXA, palette: all the decode reads except TBP0) + hash of its bytes; a texture about to be decoded
    takes a parked one with the same shape and bytes. LRU, `GOW_GS_HW_TEXPARK_MB` (default 48, 0 = off).
  - range hash in four independent lanes; the hash computed by the revalidation is reused by the decode that follows.
  - GS worker: the finished chunk is accounted in the same critical section that takes the next one.
- Result (same-session A/B vs update_bugs1, stay 60 s): ATHN02A mean 43.0 -> 47.0; ATHN07B 45.1 -> 46.4, frames
  > 67 ms 24.6 -> 5.1 /min; ATHN06C heavy part 25-29 -> 28-33; decodes 1200-2450 -> 8-15 per 5 s; speed unchanged.
- Package port/switch/nsp/out/update_dips4 (update_bugs1 + harness sp fix of a562e7c + dips4). The patch was
  recovered after a PC restart from the WSL tree; applied onto the bugs1 files (/root/dips-orig) it rebuilds .text,
  .rodata and .data byte-identical to the measured ELF (/root/elf_dips4.elf).
- port/switch/dips5.patch (NOT tested, full diff from bugs1): dips4 + per-256-byte-block hash cache (block hashes
  recomputed only after a transfer writes them; `GOW_GS_HW_BLOCKHASH=0` = off). Sources /root/dips5-src.
- Verified 2026-10-31 (sw-dips2), same-session A/B, min/mean fps bugs1 -> dips4: ATHN01D 33.8/35.5 -> 34.6/37.0,
  ATHN02A 43.6/43.9 -> 46.6/47.2, ATHN06C 27.4/28.5 -> 31.2/31.9, ATHN07B 44.5/45.3 -> 46.2/46.6; speed 0.98-0.99 both;
  screenshots (movie, title menu, HUD, ship deck fight, rain) identical. Installed as the console default.
- Next (sw-dips2, profile 2026-10-31, port/switch/logs/prof_lr4_ATHN06C): in ATHN06C's heavy part the GS thread is
  ~98 % busy at ~30 fps with ~6 M vertices/s: submitKicks 29 % (10 % the stall on `v[i].kick`, the stage thread's
  chunk; 10 % the `pendingIdx` range insert), addVertexOnly 11 %, memmove 15 % (draw's copy into the CpuUncached
  vertex buffer), draw 10 %, unchangedSinceDecode 7 % (range hashes). ATHN01D is VU1-bound instead (GSWorker ~93 %
  busy, GS thread 32 % idle). port/switch/dips6.patch (full, from bugs1) = dips5 + vertex path: per-run vertex
  constants, reserved vectors, push_back instead of the range insert, prefetch of the kicks (in A/B).
