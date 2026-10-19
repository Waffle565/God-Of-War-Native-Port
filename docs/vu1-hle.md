# VU1 hot path: what the game runs and how it is compiled (2026-10-19, ext agent/sw-vu1-hle)

Goal: VU1 is the limit of the Switch build in gameplay (~4.0 s busy per 5 s at ~20 fps, docs/switch-deko3d.md); 60 fps
needs it ~2.4x faster. This page records what the VU1 time is spent on and what the generator does about it.

## Exact float handling out of the hot loops (2026-10-19, ext agent/sw-vu1fast)
**Handoff.** Branch `agent/sw-vu1fast` (ext 31a482d + nothing in the runtime; worktree ext/PS2Recomp-kl-vu1fast, own
build dir `out/build-pc` configured with `-DPS2X_VU1N_GEN_OVERRIDE=<root>/port/vu1fast/gen/vu1n_gen.py`, which is the
committed tools/vu1/vu1n_gen.py). Only the generator changed: `tools/vu1/vu1n_gen.py` emits a *fast variant* for 704 of
the 1704 block functions (all loop functions with a gain, plus plain blocks that save more than their entry check);
`--no-fast` gives the previous output. Not yet run on the console (see "Console" below for how to build it).

**What the exact handling was.** The native blocks already run in the VU's float mode: `runNative` sets round toward
zero and flush to zero (ARM64 FPCR RMode=RZ + FZ, x86 MXCSR RC + FTZ/DAZ) before the first block and restores it after,
so a plain `fmul`/`fadd` already gives the exact engine's result for finite operands (no denormals in or out, overflow
saturates to FLT_MAX instead of infinity). What the generated code still did for every FMAC:
- the operand view `nrm` (Inf/NaN -> +-FLT_MAX; BIC, UMIN, BSL per operand) for lanes not known finite;
- `clampv` after ADD/SUB/MUL (FMAXNM, FMINNM) - the identity for finite operands under round toward zero;
- MADD/MSUB through `maddc`: product and sum compared with FLT_MAX in **all four lanes** (two BIC, UMAX, UMAXV, FMOV,
  CMP, branch), the wide (double, one rounding) path when any lane reaches it;
- CLIP through two stores and six scalar compares, FTOI with a clamp of an already finite view.
DIV/SQRT/RSQRT already use the hardware divide and square root under RZ (`divv`: `qnorm` of the operands, a zero test,
`qfinish`); their cost is the divide latency, left as is.

**Fast variants (range analysis in the generator, `Bounds`).** For every VF lane, ACC lane, Q and I the generator tracks
a bound |x| < 2^e (or unknown: loads, FTOI results, MFIR/MFP, integer bits) through the generated code: MUL e_a+e_b,
ADD/SUB max+1, MADD max(e_acc, e_a+e_b)+1, MAX/MINI max, ITOF 32-shift, MOVE/MR32 copy, LOI from the constant, DIV
results finite. Where the bounds prove the exact handling is the identity it emits the plain op: the raw register for a
finite view, no clamp, `acc +/- a*b` for a MADD whose product and sum stay below 2^127 in all four lanes, CLIP in vector
compares (`vu1nf::clipv`), FTOI without the clamp (`vu1nf::ftoiv`).
- **Entry check.** The function's entry values get an assumed bound per lane (origin); the code records which origins it
  relied on and how - their bound (a plain MADD) or only their being finite (a skipped view or clamp: every value the
  analysis knows is finite when its origins are, whatever their size, because round toward zero never overflows to
  infinity). The function starts by testing exactly those lanes (magnitude bits against a per-lane limit, two vector ops
  per register, one compare per limit vector); if one fails it calls `<name>_x`, the exact function as generated before.
- **Loops** (self-loops, two-block cycles): the loop-carried bounds are iterated to a fixed point (join with the entry
  state, widening to the float range after six rounds; Q is unknown after `chainTo`, which reloads it).
- **Bounds per origin** (`_search`): all start at 2^64; while MADDs stay on `maddc` with every lane known, the origins
  behind the failing product (or ACC) with the highest bound are lowered by 4 (a chain of two matrix transforms ends
  with equal bounds, ~2^36-2^40), keeping the best map. Plain blocks get a variant only when the estimated saving exceeds
  the check by 8 instructions.
- **Bit-exact by construction**, with one subtlety found by the replay: the MADD test must cover all four lanes as
  `maddc` does. Deciding on the DEST lanes only (what the exact engine and the hardware do) made 282 of the 1832 PC zone
  records differ: a NaN or huge lane outside DEST (integer bits) sends `maddc` to the wide path, whose single rounding
  gives other DEST values than the two-step fast path (`VU1N_FAST_DEST=1` reproduces it). So a function whose entry
  lane outside DEST holds such bits falls back to `_x`.

**Results** (replay harness, x86 and aarch64 builds, against the recorded hashes):

| recording | records | differ (x86 / aarch64) | fast entries / exact entries (`EXTRA=-DVU1N_FAST_STATS`) |
|---|---|---|---|
| ship (console, `vu1ship.rec`) | 2000 | 0 / 0 | 150077 / 36 |
| consecutive (console, `vu1seq.rec`) | 3000 | 0 / 0 | 309842 / 210 |
| PC zone walk (`game.rec`) | 1832 | 0 / 0 | 120610 / 18710 (b1748: F1.w holds 0xffff80xx) |

- The seven vertex-loop variants (0x2840 ... 0x3c20) have no `maddc`, `nrm` or `clampv` left.
- qemu aarch64 cost (instructions + Q instructions, one warm pass of 30 ship records, the replay's hashing left out):
  1.691 M -> 1.418 M (**-16 %**), generated blocks 1.359 -> 1.085 M (-20 %; e.g. b2ee0 0.169 -> 0.107 M, b2b90
  0.186 -> 0.127 M); 30 PC zone records: 2.343 -> 1.983 M (-15 %), generated blocks 1.601 -> 1.242 M (-22 %).
  What remains: `runNative` + `blockTiming` 0.57 M (40 %: the B halves of the loops read entry flags, so `chainTo` hands
  every pass back to runNative), then flags, blends, loads/stores and register spills (~30 vector registers live).
- x86 replay (WSL, noisy): ship 44.6-46.8 -> 39.0-41.3 ms, consecutive 84.5-90.6 -> 77.7-77.8 ms (5 warm passes).
- Expected on the console: qemu matched the A57 for sw-vu1-hle's change (-9.5 % predicted, -10 % measured), so the
  replay benchmark should drop from 137 ms by ~12-16 % (~115-120 ms), VU1 busy ~3.8-4.1 s -> ~3.3-3.6 s per 5 s.
- PC build (`out/build-pc`, generated parts identical to the replayed ones): zone gate port/vu1fast/gate/20261019_143226
  5/5 ok (60 fps, speed 1.00-1.01, no capture flagged against port/swvu1par/gate/20261019_130004), savetest 129 pages,
  card directory intact.
- Console build: `VU1_PREGEN=<root>/ext/PS2Recomp-kl-vu1fast/out/vu1pregen` (vu1_generado = sw-vu1-par's, vu1n_generado
  from this generator; or any folder regenerated with `python -I tools/vu1/vu1n_gen.py <dir>/vu1n_generado
  port/vu1_micro --heads port/vu1n_heads.txt --parts 6 --locals`) with switch/build.sh of the sw-vu1-par worktree (the
  runtime is unchanged); then the replay benchmark env.txt as above.
- Diagnostics: harness `EXTRA=-DVU1N_FAST_STATS` (fast / exact entry counts at exit, first misses by function),
  `-DVU1N_FAST_STATS_REGS` (the register, lane values and limits of each failed check); port/vu1fast/dbg_fast.py runs the
  search for one block (not in git).

## The VU1 thread on the console: split, overlap, sync points (2026-10-19, ext agent/sw-vu1-par)
**Handoff (where this stopped).** Branch `agent/sw-vu1-par` (worktree ext/PS2Recomp-kl-vu1par, own build dirs
`out/build-pc`, WSL `/root/gow-switch-par`, VU1 blocks `out/vu1pregen` = sw-vu1-hle's; agent/sw-ee5 merged, Switch
default `GOW_SOUND_BATCH=32`, docs/sound.md). Installed on the console: `port/switch/nsp/out/update_par4` (ext
31a482d, no env.txt): title 34.4-36.2 fps, fight 16.2-29.8, after the fight 26-29 (VU1 worker 4.1-4.4 s busy per 5 s).
Checks of 31a482d's runtime (PC build `out/build-pc`): zone gate port/swvu1par/gate/20261019_130004 5/5 ok (60 fps,
speed 1.00-1.01, no capture flagged against port/swvu1hle/gate/20261019_090208), savetest 129 pages, card directory
intact; VU1 replay (x86 harness) ship 2000 / consecutive 3000 / PC zone 1832 records, 0 differ. `PS2X_VU1_ASYNC` and
`PS2X_GS_SYNCPATH_DEPTH` (off) were only run on the console. Plan steps 1 and 2 are done and measured, step 3 was
assessed and not started (below). Next lever: none on the VU1 thread alone; see "What limits gameplay now".
A new PC build folder needs `-DPS2X_VU1_GENERATOR_SOURCE=<root>/ref/other-recomp-new/god-of-war-recomp-main/tools/vu1/generar_vu1.cpp
-DPS2X_VU1_MICROCODE_DIR=<root>/port/vu1_micro` (else VU1 runs interpreted: zone gate at 4-9 fps) and
`-DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_DEBUG_UI=OFF
-DPS2X_ENABLE_IOP_RPC_TRACE=OFF -DPS2X_ENABLE_RUNTIME_LOGS=OFF -DPS2X_ENABLE_SCCACHE=OFF` (the CMake defaults turn the
logs on: the EE thread then runs at ~0.2x game speed).
Console loop: scratchpad `par/cycle.sh <name> <s> [env.txt]` = the sw-vu1-hle `cycle.sh` (DBI MTP -> push update +
env -> start -> `PLAY=1` bot fight -> back to DBI -> pull log -> `tools/switch_log_summary.py`).

**Split of the GS worker thread** (`GSWorker`, core 2; `[gsw-split]` every 5 s with `GOW_PERF_DIAG`, on by default on
the Switch; `tools/switch_log_summary.py` prints it). Gameplay (ship fight), per 5 s:

| | job_ms (busy) | VU1 runs (vu_ms) | of which XGKICK hand-off (kick_ms) | VIF1 + rest (vif_ms) | runs | XGKICKs |
|---|---|---|---|---|---|---|
| before (ext 3b9fc78 + counters) | 4.0-4.3 s | 3.7-4.0 s | 0.26-0.37 s | 0.25-0.33 s | 115-150k | 380-470k |
| kick batching (below) | 4.1-4.4 s | 3.8-4.1 s | 0.15-0.23 s | 0.29-0.34 s | 135-155k | 415-500k |

- A run averages ~27 us, an XGKICK packet ~0.7 us of hand-off (arbiter + GS stage queue) before the batching.
- VIF1 decoding and UNPACKs are only ~7 % of the thread: overlapping them with VU1 (plan step 2) can win at most that.

**VU1 runs on their own thread (`PS2X_VU1_ASYNC=1`, off by default).** The worker hands each MSCAL/MSCNT to a
`VU1Run` thread (core 2) and goes on decoding VIF1; it waits for the program before the next MSCAL/MSCNT, MPG, FLUSH*,
DIRECT/DIRECTHL, MSKPATH3, PATH3 data, a GIF arbiter drain, and before it reports idle (the hardware's VIF stalls,
plus everything that touches the arbiter, which the VU1 thread drives through XGKICK). `PS2X_VU1_ASYNC=2` runs each
program at that next wait point on the worker itself (worst-case overlap, deterministic; for checking that the game
never unpacks into a running program's data). Console (`GOW_CORE_GSWorker=1`, worker next to the EE):
title 24-25 fps (35-36 without), gameplay 10-16 (15-28): **slower**. vu_ms drops to ~2.5 s but the worker waits
~3.6 s per 5 s for the VU1 thread: with ~2 us of VIF work per ~27 us run, every MSCAL waits, and each wait/wake pair
costs more than the overlap gains. Only a spinning hand-off on both sides could pay, and no core has a spare ~3.5 s
per 5 s to spin. Kept as an option, not used.

**Kick batching (default).** The GS stage queue (gs_stage.cpp) took one lock and one queue entry per XGKICK packet;
small packets now gather on the producer side and are queued together (16 KB, or at `publish()` after each worker
job). kick_ms 0.26-0.37 -> 0.15-0.23 s per 5 s.

**Relaxed `sceGsSyncPath` (`PS2X_GS_SYNCPATH_DEPTH=<n>`, off by default).** `PS2X_GS_WORKER_DIAG=1` shows the EE's
waits for the worker are all in `sceGsSyncPath` (~300 waits, ~2.9 s per 5 s in a fight; one CSR read waits ~0.1 s).
The worker copies DMA data at the kick, so the game could reuse its buffers at once; with n > 0 the call returns once
fewer than n jobs are queued (every GS/VIF/VU register or memory access still syncs fully). With n = 1: no fps change
(gameplay 22-24, title 36), `guest_wait_ms` 0.4-0.7 s as before: in fights core 1 (EE + IOP + GS stage) is full.

**`GOW_CORE_GSStage=0` does not hold up.** Same build, same bot fight: default (stage on core 1) 25.4-27.6 fps after
the heavy part, stage on core 0 20.6-22.0, with `gsq_wait_ms` 1.1-2.4 s (the stage blocks on the deko3d thread's
queue: core 0 is full in fights, 167-211k draws per 5 s). The default stays.

**Plan step 3 (data-parallel vertex loop), assessed, not started.** Replay experiment `GOW_VU1N_REPLAY_LIVEIN=1` on the
consecutive recording (`vu1seq.rec`, 2999 pairs of consecutive runs of the main program):
- across runs: with every register the next run reads taken from the previous run's *start* instead of its end, 2348
  of 2999 outputs are identical (78 %); with the bytes the previous run wrote (and the VIF did not rewrite) reverted,
  2252 (75 %; a shared area around qw 928-939 and XGKICKed data). Speculating run k+1 before run k ends would fail
  ~40 % of the time: no.
- inside the loop: the vertex loop carries the previous vertex (vf11/vf28: strip culling cross product) and the
  pipelined loads from one or two iterations; a second worker starting a few iterations early is possible in
  principle, but needs per-run hand-offs (2 per ~27 us run, ~135k runs per 5 s) to a thread on core 0 or 1, the
  cycle model (`chainTo` timing, flag history, FDIV/XGKICK progress) merged at the split, and spare time on those
  cores, which in fights they do not have (below).

**What limits gameplay now** (fight, ~20 fps, per 5 s): core 2 VU1 worker 4.1-4.4 s busy; core 1 EE ~3.0 s + IOP ~1.0 s
+ GS stage ~1.1-1.4 s (full, `ee_ms` is wall time incl. preemption); core 0 deko3d thread 2.4-3.0 s + present. Every
core is near full, so 60 fps (or a steady 30) needs less work per frame on all three, not a better split.

## One program, one loop
Profiled with the replay harness (tools/vu1_harness) on the console ship recording (2000 runs, `port/swvu1b/vu1ship.rec`,
not in git):
- `GOW_VU1N_REPLAY_PROFILE=<file>`: per record and warm pass `record startPc vuCycles ns`. 99 % of the runs start at
  0x00, 0x18 or 0x30 (41 / 39 / 19 % of the time). Those are three entries of **one** microprogram: each sets `vi1` to
  0 / 341 / 683 (the triple-buffered input area) and branches to the common code at pair 11, which dispatches on a word
  of the batch header (`ilw vi4, 69(vi12)`, `jr vi4`) into flag-controlled paths.
- `GOW_VU1N_BLOCKPROF=<file>`: VU cycles per dispatched block; `GOW_VU1N_TRACEREC=<n>`: the dispatch sequence of one
  record. 98 % of the VU cycles are 7 block pairs at 0x2840, 0x2b90, 0x2ee0, 0x3230, 0x3580, 0x38d0, 0x3c20 - the same
  vertex loop, loaded at seven addresses in different micro-memory images.
- The loop (software pipelined, unrolled twice with renamed registers): load position + a packed word, transform by
  two 4x4 matrices (vf12-15, vf20-23; ACC chains of ADDA/MADDA/MADD), DIV/MULQ perspective, FTOI4, a 2D cross product
  (vf27/vf28) whose sign FMEQ reads into the ADC bit, SQ of xyz and the flag word. Block A ends `ibne -> B`, block B
  `ibne -> A`.

## Cost on the A57 (qemu instruction counts, 100 ship records, one warm pass; the replay's own hashing left out)
| | generated blocks | runNative | blockTiming | total |
|---|---|---|---|---|
| `--locals` (sw-vu1b) | 5.17 M | 1.78 M | 0.61 M | 7.56 M |
| + two-block cycles | 5.49 M | 1.22 M | 0.61 M | 7.32 M (-3 %) |
| + maddc + invariant operand views | 5.01 M | 1.22 M | 0.61 M | 6.84 M (-9.5 %) |

qemu counts overstated real A57 gains about threefold before (docs/vu1-native-flags.md "VU1 on the console"): the
console replay benchmark decides.

## Generator changes (tools/vu1/vu1n_gen.py), all bit-exact
Replay against the recorded hashes: ship 2000, consecutive 3000, PC zone walk 1832 records, 0 differ.
- **Two-block cycles** (`gen_cycle`, `--no-cycles` turns it off): A's function runs A, then B, then A again while
  `chainTo` agrees, with the registers of both blocks in locals (all loaded at entry, all stored at every exit). Before,
  each pass of the loop left through `runNative` twice and each block loaded and stored its ~20 registers. B's function
  stays as is (entered directly by runNative) and is not `static` (A's function compares the slot with it).
- **maddc** (ps2_vu1_native.h): MADD/MSUB return the clamped result and the raw one (for the MAC flags) from one call;
  the clamp only runs on the wide path. On the fast path product and sum are below FLT_MAX in magnitude, so clampv was
  the identity: 2 NEON instructions less per FMAC.
- **Invariant operand views** (`Regs.inv`): in loop functions (self-loops and cycles) a VF register the function never
  writes gets its operand view (`nrm`: Inf/NaN -> +-FLT_MAX) once at entry (`N<r>`), not at every use in every pass.
  GCC did not hoist it (register pressure, the matrix rows are spilled).

## Console (2026-10-19, ext agent/sw-vu1-hle = sw-vu1b + sw-ee4 merged, deko3d, scale 1)
- Replay benchmark (env.txt `GOW_VU1N_REPLAY=sdmc:/switch/gow/vu1ship.rec GOW_VU1N_REPLAY_REPS=4`, method in
  docs/vu1-native-flags.md): **137.0 ms** for 6000 warm runs, 2000 records, 0 differ (sw-vu1b's final build: 152.2 ms):
  **-10 % VU1 time on the A57**, bit-exact (qemu predicted -9.5 %: here the counts did not overstate).
- Gameplay, same bot script (`PLAY=1`, ship fight; logs `port/switch/logs/20261019_hle_*`), guest fps per 5 s window:

| build | 115-145 s (heavy) | 165-200 s | VU1 busy per 5 s |
|---|---|---|---|
| sw-vu1b final (`out/update_vu1b_clean`) | 14.2-17.0 | 23.2-26.6 | ~3.9-4.1 s |
| this branch (`out/update_hle_g3`) | 17.4-27.8 | 28.2-30.2 | ~3.8-4.0 s |
| this branch + env.txt `GOW_CORE_GSStage=0` | 16.2-29.6 | 27.0-31.6 | ~3.7-3.9 s |

  The title menu ran ~38 fps (28-29 before). With the GS frontend on core 0 the EE thread waits more (`guest_wait_ms`
  1.3-1.5 s per 5 s instead of 0.4-0.6 s): core 1 is no longer full, VU1 (core 2) alone limits. No crash in four runs
  (200 s each).

## What is left (and why not 2.4x this way)
Per vertex the loop is ~18 VU pairs of exact float semantics: operand views, overflow check per FMAC (product and sum
magnitude, one UMAXV and a branch), clamps, blends for DEST masks, masked VU memory accesses. With every bookkeeping
overhead gone the arithmetic stays; the probe without exact float handling (sw-vu1b) gave -29 % and changes 23-27 % of
the PC Olympus runs' output. A native rewrite of the loop with plain float math would only pay with a per-batch range
check proving that no operand or result can reach FLT_MAX (then the exact and plain results are equal), which needs
the input magnitudes (matrices, positions) checked per batch - done in the generator since sw-vu1fast (top section:
range analysis and an entry check per function call).
