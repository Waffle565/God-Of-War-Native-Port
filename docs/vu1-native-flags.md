# VU1 native blocks: flag latency across blocks (character shadows)

Character shadows are drawn by a shadow-volume pass:
- the game clears G to 127 and inverts Z;
- it draws the volume twice (front faces G += 255 = -1, back faces G += 1, COLCLAMP=0 wraps);
- it copies G's top bits to alpha (8 px CT16 strips);
- it darkens wherever the count is not zero (DATE).

The two halves of the volume differ only by the ADC bits that the VU1 culling loop writes. For example, at 0x3C38
`FMEQ vi8` tests the sign of the `MSUBy.x` issued exactly 4 pairs earlier, across the loop branch, and `ISW.w` stores
the ADC word. So the culling is only right if MAC/clip flags become visible exactly 4 cycles (`kFmacLatency`) after
their writer, across block boundaries too.

## Why the former model had no shadows
The former model committed, at a block's exit, the newest flag writer of that block. The loop above then read the
flags of its own last pair, so both halves of the volume were culled the same way:
- Desert: 332 / 332 triangles;
- PCSX2: 311 / 381;
- PCSX2's interpreter in our build: 282 / 370.

Every pixel netted to zero. The exact engine (`GOW_VU1_SIN_COMPILAR=1 GOW_VU1_NATIVE_OFF=1`) draws the shadows.

## The model (the only one; ps2_vu1_native.h / .inl, tools/vu1/vu1n_gen.py)
**Flags inside a block.** A reader at pair i sees the newest writer of the block issued at least 4 cycles before it,
on the block's issue cycles `n.cyc`. Flags older than the block come from the flag rings.

**Flag rings** (`VU1FlagRing`, MAC and clip):
- They hold the last 4 writers. The VU issues at most one FMAC / CLIP per cycle, so 4 writers cover the latency
  window. Eight slots: the newest four at next-1 .. next-4, the writer pushed out last at next-5, so a push copies
  nothing.
- A native block pushes only its newest writer. MAC writers are pushed with their unclamped FMAC result and DEST
  mask, and the flags are computed only when something reads them (`vu1n::macValRt`).
- The older writers that can still be the visible one after the exit are those whose successor lies in the block's
  last three pairs. Their values go unconditionally into the block's tail record (`VU1FlagTail` in its history
  entry), and a static `VU1TailDesc` from the generator says which pairs and masks they are.
- A reader that meets an invisible ring entry checks that entry's tail record (`tailVisible`).
- Entry and exit go through the exact engine's flag pipeline (`importFlags / exportFlags`, tail records expanded).

**Entry timing.** A block that reads flags written before it gets its exact issue cycles from the pipeline state at
entry. Every other block uses its idle-pipeline table.
- Lane readiness (VF/VI/ACC) is not tracked per block. The last 64 native blocks are kept in `NativeState::hist`, and
  a lane's ready cycle is derived from its newest writer (`walkHistory`).
- Only blocks that ended less than `maxLat` cycles ago can still hold a lane in flight.
- Lanes not written since native entry come from the exact engine's readiness arrays. These are only read while
  `arraysReadyBy` (cleared at every program start) is in the future.
- The stall model runs once per distinct in-flight state (cached per block, 16 entries).
- When the previous block ran for `maxLat` cycles or more and nothing from the exact engine is in flight, the
  predecessor (block + timing table) alone decides the entry state. Its result is memoized in the dispatch slot; the
  shadow loop at 0x0d00 hits this every iteration.

**Exit.** When the exact engine takes over, the readiness arrays get the lanes still in flight (`syncInflight`).

**Dispatch.** One 128-byte `Slot` per pc. Its first cache line holds everything the loop touches for every block:
function, idle table and its length in cycles (`idleEnd`, so a block run with idle timing never reads its table),
tail descriptor, FDIV/EFU starts. The entry-timing memo, used only by blocks that read entry flags, is in the second
line.

**Code changes.** Slots are derived from the micro memory. When its generation changes (VIF MPG, EE stores), the
native state compares the micro memory with its own copy and only slots whose extent overlaps a changed pair (the
block's pairs plus the pair after it, which ended the extent scan) are set up again; the rest stay valid. The VIF
does not bump the generation for an MPG that writes the bytes already there. In the ship-deck scene the game rewrites
part of the micro memory ~20 times per frame.

## Cost
Replay benchmark: 4000 recorded VU1 runs at the end of the Olympus zone walk, 5 warm passes.

| Build | Time | Notes |
|---|---|---|
| Former model | 62.7 ms | |
| First latency version (opt-in, shadows2) | 122 ms | |
| This model | 77-79 ms | |

In game (`GOW_VU1_STATS`, zone gate port/shadows3/gate): 3.3-3.6 ns per VU1 cycle in slots 1-4 (former model 2.7,
shadows2 opt-in 4.8); all zones 60 fps, game speed 1.00.

What the remaining cost is: the newest-writer ring push and tail stores per block (≈ 1/3), the history entry per
block, and the entry-timing walk for flag-reading blocks that miss the memo.

How it was measured:
- `GOW_VU1N_RECORD=<file> GOW_VU1N_RECORD_SKIP=8000000` records runs from the heavy part of a zone run.
- `GOW_VU1N_REPLAY=<file> GOW_VU1N_REPLAY_REPS=6` replays them and prints the time (port/shadows3/bench.sh).
- `tools/sample_prof.py` samples the runner's threads by function, with a /MAP link.

## Verification
`GOW_VU1N_VERIFY` compares native with the exact engine per run. Integer-like words (GIF data, the ADC bit) are
compared exactly, and bisection targets ADC mismatches.

Mismatches with this model:

| Zone | Mismatches | Runs | Rate | Shadows2 opt-in |
|---|---|---|---|---|
| Olympus | 8527 | 890k | 0.96 % | 0.95 % |
| Cliffs | 133 | 852k | 0.016 % | 0.016 % |
| Desert | 3135 | 750k | 0.42 % | 0.34 % |

In Desert almost all mismatches come from one scene late in the walk (5 in the first 400k runs, then ~9 per 1000), so
the rate depends on how much of it a slow verify run reaches.

The mismatch kinds are the same as before: 1-unit rounding (`0000822e/0000822f`) and the few `ffff/0000` and
`0400/0000` words on near-degenerate triangles. No reserved instructions.

Diagnostics:
- `GOW_GS_HW_VOLTRACE=<n>`: triangles per half, and G after the volume.
- `GOW_VU1_KICKPRIM=<prim>`.
- `GOW_VU1_FMANDPC=<pc>`: ours and PCSX2's interpreter.
- `GOW_VU1_STARTMAC=1`.
- `GOW_VU1N_VERIFY` (`GOW_VU1N_DUMP=1` writes the run's micro memory).
- `GOW_VU1N_DIAG=1`: native pairs, misses and native entries per 5 s.
- RT dumps also write alpha (`_a.pgm`).

## VU1 on the console (Switch, 2026-10-19, ext agent/sw-vu1b)
In gameplay (ship deck, deko3d build) the VU1 thread (`GSWorker`, core 2) is busy ~4.0 s of every 5 s at ~20 fps:
VU1 is the limit, then the EE (~2.9 s). Everything runs native (`GOW_VU1N_DIAG`: no misses, nothing interpreted),
~7.4 pairs per block, 17-18 ns per VU1 cycle (`GOW_VU1_STATS`).

**Measuring on the A57 itself.** qemu instruction counts (tools/vu1_harness `icount.sh`) overstate gains about
threefold on the real core, so changes are timed on the console with the replay benchmark:
- record runs in game: env.txt `GOW_VU1N_RECORD=sdmc:/switch/gow/vu1ship.rec GOW_VU1N_RECORD_SKIP=1400000
  GOW_VU1N_RECORD_STRIDE=60 GOW_VU1N_RECORD_N=2000` (ship gameplay; `STRIDE=1` for consecutive runs), pull it over MTP;
- time a build: env.txt `GOW_VU1N_REPLAY=sdmc:/switch/gow/vu1ship.rec GOW_VU1N_REPLAY_REPS=4`; at the first VU1 run
  the log gets `[vu1n-replay] timing: <ms> for 6000 warm runs` and `2000 records, 0 differ` (hashes of the build that
  recorded). Run-to-run noise ~0.5 %. Records hold game data: keep them out of git.
- bit-exactness: the harness replays the console recordings and the PC zone recording (`game.rec`) on x86 and
  aarch64 builds against an x86 reference dump (`dumpdiff.py`).

| console replay, 2000 ship records x 3 warm passes | ms | |
|---|---|---|
| blocks generated without `--locals` (what the deko3d builds of 2026-10-19 morning had: `VU1_PREGEN` defaulted to build-kl's 2026-10-13 output) | 170.5 | |
| `--locals` (registers in locals, in-place self-loops) | 155.9 | -8.6 % (qemu predicted -26 %) |
| + per-lane clean operands (below) | 152.2 | -2.4 %, bit-exact, kept |
| + clamp behind a predicted branch (`clampr`) | 160.0 | slower: dropped |
| probe: no flag rings / block history / FDIV timing (wrong flags) | 140.6 | upper bound of the cycle-model bookkeeping: -7.6 % |
| probe: no operand normalisation, no clamp, no wide MADD | 108.2 | -29 %, but not exact (below) |
| sw-vu1-hle (two-block cycles, maddc, invariant views) | 137.0 | docs/vu1-hle.md |
| sw-vu1fast: fast variants (range analysis + entry check, bit-exact) | not measured yet | qemu -16 %; docs/vu1-hle.md top section |

- In game: 18.2 -> 16.9 ns per VU1 cycle with `--locals`; gameplay stays ~20-21 fps (VU1 still saturated).
  `switch/build.sh` now refuses pregenerated blocks without `--locals` (`ALLOW_OLD_VU1N=1` overrides).
- **Per-lane clean operands** (`vu1n_gen.py`): the operand view `nrm()` is skipped when every lane the operation uses
  (its DEST lanes, or the broadcast lane) was last written in the block by a result that cannot be Inf/NaN: clamped
  FMAC results, MAX/MINI of normalised operands, ITOF, ABS. Before, only full-DEST FMAC results counted, and a later
  FTOI (integer bits) into such a register did not clear it. 7503 -> 6993 `nrm` calls. Bit-exact on all recordings
  (PC zone walk 1832, ship 2000, consecutive 3000; locals and memory forms).
- **Exact float semantics cost ~29 %**, but cannot go: without them 1 of 2000 ship runs differs (vertex values and
  clip flags of one batch), yet 23-27 % of the PC Olympus runs do (each of operand normalisation, clamp and wide MADD
  matters there). sw-vu1fast removes them where a range analysis proves them the identity, guarded by a per-call entry
  check (docs/vu1-hle.md, "Exact float handling out of the hot loops").
- **Running VU1 programs in parallel does not pay.** Each MSCAL resets the pipeline state (`resetScheduler`), so
  run k+1 depends on run k only through registers and data memory. Replaying 3000 consecutive ship runs: between
  runs only the pc changes (registers flow from one program into the next), and started with the registers of run
  k's start instead of its end, only 343 of 2999 runs give the same output (with run k's memory writes undone instead:
  2647). Register speculation would fail ~9 times in 10. VIF unpack overlapping VU1 (as the hardware does) is worth at
  most the ~15 % of `GSWorker` the VIF takes on PC.
- Left: the A57 time per VU1 cycle is spread over the float emulation (~30 %), the block bookkeeping (~8 %) and the
  arithmetic itself; no single change found here gives the ~2.4x VU1 needs for 60 fps.
