# VU1 hot path: what the game runs and how it is compiled (2026-10-19, ext agent/sw-vu1-hle)

Goal: VU1 is the limit of the Switch build in gameplay (~4.0 s busy per 5 s at ~20 fps, docs/switch-deko3d.md); 60 fps
needs it ~2.4x faster. This page records what the VU1 time is spent on and what the generator does about it.

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
the input magnitudes (matrices, positions) checked per batch - not done.
