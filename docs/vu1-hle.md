# VU1 hot path: what the game runs and how it is compiled (2026-10-19, ext agent/sw-vu1-hle)

Goal: VU1 is the limit of the Switch build in gameplay (~4.0 s busy per 5 s at ~20 fps, docs/switch-deko3d.md); 60 fps
needs it ~2.4x faster. This page records what the VU1 time is spent on and what the generator does about it.

## Data-parallel vertex loops on a helper thread (2026-10-19, ext agent/sw-vu1split): works, bit-exact, does not pay
**Handoff.** Branch `agent/sw-vu1split` (ext f8d61b1 = sw-core1 + the split runtime cebb869, sw-vu1disp 665d3ce merged
in f4749a8, split off by default 20e04fe, sw-vu1jr 97a964b merged in b632e46; worktree ext/PS2Recomp-kl-vu1split, PC build `out/build-pc`, VU1 blocks `out/vu1pregen` =
sw-vu1disp's (sw-vu1cyc's kept in `out/vu1pregen_cyc`); WSL copies `/root/src-vsplt` -> `/root/gow-switch-vsplt`, cloned
from sw-core1's with the paths renamed, the changed runtime files copied in by scratchpad `vu1split/swsync.sh`).
**Off by default**: `GOW_VU1_SPLIT=1` turns it on. Installed on the console: `port/switch/nsp/out/update_vsplit5`
(b632e46: split off, sw-vu1disp + sw-vu1jr runtime with their defaults, no env.txt; `sdmc:/switch/gow/vu1ship.rec` is
on the SD card for the replay benchmark).

**What it does** (ps2_vu1_split.inl; hooks in runNative, the memory helpers and the callouts):
- *Probe.* A dispatch whose last run from that pc lasted >= `GOW_VU1_SPLIT_MIN` (600) VU cycles is broken after every
  block (ctx budget = now + 1: chainTo / chainDirect hand back exactly as at any budget end) until the block pcs repeat
  with period P (1-3 blocks) and two periods have equal VU cycles and VI deltas. The VI registers with a constant delta
  are the induction variables; one counting down gives the periods left R.
- *Plan.* Take-over at k = (R + warm + lat) / 2 periods (`GOW_VU1_SPLIT_WARM` 5, `GOW_VU1_SPLIT_LAT` 2), cycle Tk. The
  helper gets the worker's present state (registers, cycle, FDIV/EFU, flag rings, the 8 newest history entries) with the
  linear VI registers advanced and every absolute cycle shifted by (k - warm) periods, copies of the loop's dispatch
  slots, and copies the data-memory ranges its pointer registers will sweep (+-8 qwords).
- *Helper* (thread `VU1Split`, pre-spawned, spins `GOW_VU1_SPLIT_SPINUS` 2000 us then waits on a condvar; on the Switch
  the spin yields: core 0 at the default priority, which is already the lowest an application thread gets, so a step
  below fails). Its own VU1Interpreter + NativeState in `specMode`: no timing-cache / memo writes (a miss aborts),
  no exact-engine callouts, XGKICK aborts. It runs the restricted dispatch loop (the loop's slots only) to its first
  block boundary >= Tk, publishes a snapshot, then runs to the loop exit (or the run's budget) with `ctx.spec` set:
  `ldq/ldWord/stq/stWords` log the words read before written and mark the words written.
- *Take-over.* The worker runs to Tk itself (budget break), compares its state with the snapshot (registers, cycle,
  FDIV/EFU, flag values the rings still show by issue cycle and tail records, history entries in the maxLat window),
  waits for the helper (stall limit `GOW_VU1_SPLIT_STALLUS` 40 us without progress), checks every logged read against
  the real memory (which is the memory at Tk), checks that no timing cache / slot changed meanwhile (`timingEpoch`),
  then copies the written words and takes the helper's final state over. Anything else: the worker just goes on.
- Warm-up: 3 periods leave the cull loops' carried cross-product registers (vf27/vf28) wrong in ~5 % of plans; 5 fixes it.
- Diagnostics: `GOW_VU1_SPLIT_STATS=1` (or `GOW_PERF_DIAG=1`): `[vu1-split] window=... probes plans reached adopted
  blocks_saved ... mismatch mem wait_ms helper_ms helper_state jobs` every 5 s (and at the end of a replay;
  tools/switch_log_summary.py prints them); `GOW_VU1_SPLIT_DIAG=<n>`: the first n state mismatches with the differing
  registers; `GOW_VU1_SPLIT_SYNC=1`: the helper job runs inline at the post (deterministic test of the logic);
  `GOW_VU1_SPLIT_PROBE` (24): block boundaries a probe looks at.

**Bit-exact.** Replay harness (tools/vu1_harness, now linked with -pthread), all three recordings with 3 passes (cold +
2 warm), x86 and aarch64 (qemu), split off / on inline / on threaded: ship 2000, consecutive 3000, PC zone 1832 records,
**0 differ, warm passes 0 differ** in every combination (before and after the sw-vu1disp merge). On the console the
replay benchmark with the split on also reports `2000 records, 0 differ`. PC zone gate with the split **on** (merged
build, before the default went off): port/vu1split/gate/20261019_203831 5/5 ok, 60 fps, speed 1.01; final build (split
off, 20e04fe): port/vu1split/gate/20261019_214655 5/5 ok, 60 fps, speed 1.01 (slot 0 `gow_pad_test_5` flagged: another
moment of the same fight, renders right), savetest (`port/vu1split/savetest`) 129 pages, card directory intact. The
sw-vu1jr merge (b632e46): x86 replay 0 differ (all three recordings, split off and on threaded), console replay 0 differ.

**Why it does not pay.** The vertex loops are short: ~5-6 loop entries per program run, ~300 VU cycles (~25 vertices,
~5 us on the A57) each. With the default threshold 30-40 % of the probes plan a split and ~85 % of those are taken over,
yet the helper's committed blocks are only ~4-7 % of all loop blocks (`blocks_saved`), while every split pays the probe
(5+ broken dispatches), three state snapshots (~2.5 KB each), the cross-core hand-off and a wait (the helper starts
late and runs its blocks slower: logging, cold caches, and on core 0 it only runs when deko3d's thread blocks).
Console replay benchmark (ship recording, 6000 warm runs, same build `update_vsplit3`):

| env | ms | |
|---|---|---|
| split off (default), traces + direct chaining on | 122.9 / 122.7 | |
| split off, `GOW_VU1N_TRACES=0 GOW_VU1N_DIRECT=0` | 119.0 / 118.6 | sw-vu1disp's traces/chaining are ~3 % **slower** on the A57 |
| split on (min 600 cycles) | 152.2 | +24 % (2240 take-overs, 82881 blocks saved, wait 32 ms, helper 40 ms) |
| split on, `GOW_VU1_SPLIT_MIN=2000` | 127.0 | +3.5 % (183 take-overs, 20638 blocks) |
| `update_vsplit5` (+ sw-vu1jr's chain step), split off | 119.5 / 121.3 | -1.7 % against the line above it without sw-vu1jr |
| `update_vsplit5`, `GOW_VU1N_TRACES=0 GOW_VU1N_DIRECT=0` | 116.6 | still ~3 % below traces + direct chaining on the A57 |

Gameplay (bot fight, `PLAY=1`, logs port/switch/logs/20261019_vsplit_*), guest fps per 5 s window:

| build / env | title | heavy fight (115-135 s) | later (140-200 s) | VU1 worker vu_ms per 5 s |
|---|---|---|---|---|
| before: sw-core1 `update_core1b` (docs/switch-deko3d.md) | 38.2-39.6 | 19.0-21.8 | 25.6-32.4 | 3.8-4.1 s job |
| this build, split off (`s3off_a`) | 37.2-38.2 | 15.8-22.0 | 19.6-27.6 | 3.5-4.0 s |
| final default build `update_vsplit4` (split off, no env, `s4def_a`) | 38.4-39.8 | 16.2-27.8 | 21.6-28.4 | 3.6-3.9 s |
| split off, traces/direct off (`s3old_a`) | 35.8-36.0 | 17.2-27.4 | 22.0-30.4 | 3.5-4.0 s |
| split on, min 600 (`s3on_a`) | 31.8-32.6 | 14.0-23.0 | 18.2-22.1 | 3.6-3.8 s (no drop) |
| split on, min 1500 (`s3m1500`) | 34.8-36.0 | 16.0-23.8 | 20.0-22.2 | 3.7-3.9 s |

Per-core busy did not change with the split (core 2 VU1 worker 3.5-4.0 s per 5 s either way; core 0 deko3d 1.7-1.9 s;
the helper adds ~0.3-0.4 s on core 0 and the worker waits ~0.25-0.3 s for it). Open issue (moot while off): in both
gameplay runs with the split on, the take-overs stopped after ~40 s of fighting and never resumed (the worker saw the
helper busy for good: probes 0 in every later window); `helper_state` / `jobs` in the stats line were added to find
out on the next run.

**Next lever.** Not more threads for VU1 at this granularity: a split needs the per-split cost well under ~2 us to pay
on 5 us loops. What could: (1) the vertex loop as straight data-parallel code inside one thread (the steady state of the
cycle model is a fixed per-vertex pattern: once two periods match, the remaining passes could run as a tight loop
without per-block history / flag-ring bookkeeping, validated the same way the split validates; this is the
"cheaper dispatch" lever above); (2) traces/direct chaining off on the Switch (-3 % VU1 on the replay benchmark; gameplay A/B within the noise of one
run each), or finding why they are slower on the A57 than qemu predicts (branch prediction of the tail calls?).

## JR / JALR exits and the chain step (2026-10-19, ext agent/sw-vu1jr)
**Handoff.** Branch `agent/sw-vu1jr` (ext 665d3ce + 97a964b; worktree ext/PS2Recomp-kl-vu1jr, own build dir
`out/build-pc` configured like sw-vu1disp's with `-DPS2X_VU1N_GEN_OVERRIDE=<root>/port/vu1jr/gen/vu1n_gen.py` = the
committed tools/vu1/vu1n_gen.py). What pays is a runtime change (the chain step); the generator's JR chaining works and
is bit-exact but measured no gain, so it is opt-in (`--jr`) and the default output is byte-identical to 3254106's (the
vu1disp output). Not yet run on the console.

**What was tried (JR / JALR exits).** On the ship recording ~27 dispatches per run were left (800 per warm pass of
s30), nearly all at JR / JALR exits: the vertex-loop dispatcher blocks (0x1228 / 0x1248: `JALR vi15` into the loop
chosen per batch, 24-26 different targets) and the subroutine returns (`JR vi14` / `JR vi15` back to the dispatcher).
- Profile: with `GOW_VU1N_BLOCKPROF` the runtime also writes `j <JR pc> <target> <count>` for every block a function
  returns from that ends in a JR / JALR (the `e` lines attribute the second block of a two-block cycle to the
  dispatched one); tools/vu1_harness/edgeprof.sh sums them into port/vu1n_profile.txt (now with 599 `j` lines; the `e`
  lines are unchanged). 181 sites; the most frequent target takes 58 % of the JR dispatches, the two most frequent 74 %.
- `--jr`: a JR exit tries the site's 1-2 most frequent targets by block key (as chainDirect), then any target whose
  slot is set up and has a function (`chainJr` with key 0: what runNative would dispatch there); the model in
  docs/vu1-native-flags.md "JR / JALR exits". Ship dispatches per warm pass of s30: ~800 -> ~100 (left: chains that ran
  their 64 steps); runNative ship 0.079 -> 0.047 M, PC zone 0.090 -> 0.045 M.
- `--jr-traces`: traces continue through a JR (call/return pair inside the trace, else the profile's target when it
  takes >= 60 % of the site): 1057 -> 1230 traces.
- Result: no gain. A chain step does what runNative's dispatch does (history entry, FDIV/EFU, commit, budget, entry
  timing, tail record; ~95 vs ~110 qemu instructions, sw-vu1disp), so chaining a JR moves the cost instead of removing
  it, and the JR traces' side exits (returns to another caller) cost more than the merges save.

**What pays (runtime, bit-exact by construction).**
- `entryTimingAfter`: chainTo (self-loops and the two-block cycles of the vertex loops: every pass) and chainDirect /
  chainJr give the successor's entry timing from the block they just finished (timing data, table id, length) instead of
  reading the newest history entry back; `entryTiming` is that read plus the same memo test, and the entry they read
  is by construction the one just written, ending at the successor's start.
- chainTo counts `nativeBlocks` / `nativePairs` (diagnostics) only with `GOW_VU1N_DIAG`, as chainDirect already did.

**Results.**
- Replay harness x86 and aarch64 (qemu): ship 2000, consecutive 3000, PC zone 1832 records, 0 differ, warm passes 0
  differ; the dumps (state incl. VU cycles, data memory, GIF packets) are byte-identical to sw-vu1fast's x86 dumps
  (aarch64 identical to x86) for all four variants below.
- qemu, VU1 functions per warm pass of 30 records (icb.sh "VU1 functions", corrected counting):

| build | ship (s30) | PC zone (g30) |
|---|---|---|
| sw-vu1disp (665d3ce runtime, 3254106 output) | 1.626 M | 1.932 M |
| **+ chain step (97a964b runtime, default output)** | **1.594 M (-2.0 %)** | **1.904 M (-1.4 %)** |
| + `--jr` | 1.597 M | 1.907 M |
| + `--jr --jr-traces` | 1.598 M | 1.910 M |
| `--jr --jr-traces`, env `GOW_VU1N_JR=0` | 1.602 M | 1.910 M |

- x86 replay (WSL, 5 warm passes, 7 alternating rounds, ms): ship 22.7-25.4 (vu1disp) / 22.6-24.8 (chain step) /
  23.5-25.4 (`--jr`), consecutive 44.4-45.8 / 43.5-45.4 / 43.7-46.5, PC zone 23.3-24.5 / 22.1-24.5 / 22.2-25.6: within the
  noise, `--jr` not faster.
- PC build (`out/build-pc`, default output = the vu1disp parts): zone gate port/vu1jr/gate/20261019_212540 5/5 ok (60
  fps, speed 1.01; slot 0 flags one ship-fight capture against port/vu1disp/gate/20261019_191036: another moment of the
  fight, a tutorial prompt on screen, rendered correctly), savetest 129 pages, card directory intact;
  an earlier build with `--jr --jr-traces` also passed the gate (port/vu1jr/gate/20261019_205810, 5/5 ok, 60 fps,
  speed 1.01, no capture flagged against port/vu1disp/gate/20261019_191036).
- Expected on the console: ~-2 % VU1 time (qemu), small against the 2.4x VU1 needs for 60 fps.
- Console build: `VU1_PREGEN=<root>/ext/PS2Recomp-kl-vu1jr/out/vu1pregen` (a copy of sw-vu1disp's: vu1_generado =
  sw-vu1-par's, vu1n_generado = the default output) with `switch/build.sh` of the **vu1jr** worktree (runtime changed;
  copy the untracked build files of the sw-vu1-par worktree into a new WSL build dir). Optional A/B of JR chaining on
  the A57 (its indirect-branch prediction is what qemu and x86 do not show): `out/vu1pregen-jr` (vu1n_generado from
  `--jr`), with `GOW_VU1N_JR=0` in env.txt as the off case on the same build.

**Vertex-loop bookkeeping.** What is left per pass of a two-block cycle is the chain step itself: the pending-queue
check, the history entry, the FDIV start (the loops hold a DIV), commitDue, the slot check, the memo lookup with the
FDIV key (these blocks are not `memoable`), the tail record, and in the generated code the flag push and Q store /
reload around chainTo. The earlier probe that drops flag rings, history and FDIV timing altogether (wrong results)
was -7.6 % on the Switch replay benchmark (docs/vu1-native-flags.md "Cost"): the upper bound of what the bookkeeping
can still give. Small next steps: chainTo already tested the pending queues, so its commitDue could go straight to
the FDIV/EFU part; a cycle's A -> B -> A steps could keep the successor slot and its memo entry in registers.

## Dispatch: direct chaining and traces (2026-10-19, ext agent/sw-vu1disp)
Update 2026-10-24 (sw-k9, docs/performance.md "VU1 thread throughput"): traces now also go through JR targets, start at
every block and run up to 96 pairs, with an edge profile that includes fight.rec / menu.rec (generator defaults; chain
steps on fight.rec 204 k -> 147 k), and the runtime's chain step is a leaf function in its common case.

**Handoff.** Branch `agent/sw-vu1disp` (ext 823cf54 + 665d3ce; worktree ext/PS2Recomp-kl-vu1disp, own build dir
`out/build-pc` configured like sw-vu1cyc's with `-DPS2X_VU1N_GEN_OVERRIDE=<root>/port/vu1disp/gen/vu1n_gen.py` = the
committed tools/vu1/vu1n_gen.py). Runtime (ps2_vu1_native.h / .inl) and generator changed together: the new default
output needs this runtime (`--no-direct` gives the former output, which also runs on it). Not yet run on the console.

**Measuring fix first.** `tools/vu1_harness/icount.py` printed only the 25 most expensive functions per run, and the
warm-pass cost (`icb.sh`: run with 2 passes minus run with 1 pass) was summed over those lists: a function in one list
but not the other counted with its whole run, startup ones negative. The totals of the sections below ("1.418 M",
"1.210 M", ...) are off by that (e.g. sw-vu1cyc's ship figure is 1.690 M, not 1.210 M; the PC zone 2.291 M, not 1.950 M).
`icount.py --all` prints every function; port/vu1disp/icb.sh (now with `--all`) also prints "VU1 functions" (runNative,
the generated blocks and the runtime's VU1 code, without the replay's hashing and record copies). All figures here are
that, one warm pass of 30 records (s30.rec / g30.rec), aarch64 under qemu, instructions + Q instructions.

**What it was.** runNative's dispatch: ~110 qemu instructions per block (ship 0.377 M, PC zone 0.701 M per pass), plus
each block's register loads and stores. A chain step through chainTo costs about the same (~95 instructions: history
entry, FDIV/EFU, commit check, slot check, entry timing, tail record, plus the function's prologue and register
traffic), so chaining alone (direct chaining below) moves the cost into the generated code: ship 1.690 -> 1.638 M,
PC zone 2.291 -> 2.248 M. Only merging blocks removes it.

**Changes (bit-exact by construction; the model in docs/vu1-native-flags.md "Direct chaining" and "Traces").**
- Direct chaining (`chainDirect`): a block function that has stored its registers goes on to the block at its static
  successor's pc itself (a tail call into whatever the slot holds for that block key: its function or a trace).
- Traces: a block and its likely successors (blocks without loop functions, D/T/E bits or callouts; parts after the
  first do not read entry flags) as one native block: one function with the registers in locals throughout, the parts'
  idle tables one after the other, one history entry, one flag push; a branch that leaves the trace exits as the prefix
  block (`tracePart`). 1057 traces (2676 parts, 6 loops, 36 whose part 0 reads entry flags). The PC zones' hot loop
  (0x1788-0x1748, six blocks) is one trace that loops in place.
- Likely successors from an edge profile: `GOW_VU1N_BLOCKPROF` also writes `e <from> <to> <count>`;
  tools/vu1_harness/edgeprof.sh sums them over the three recordings (run with `GOW_VU1N_DIRECT=0 GOW_VU1N_TRACES=0`)
  into port/vu1n_profile.txt (pcs and counts only), which the generator reads next to the heads file (`--profile`,
  `--no-profile`: backward branches taken, forward not).
- Entry-timing memo with two entries per slot (a trace entered from its loop and from a side exit in turn).
- Flags: `--no-direct` (former output), `--no-traces`, `--no-trace-entry`; env `GOW_VU1N_DIRECT=0`, `GOW_VU1N_TRACES=0`
  (same results, A/B on one build), `GOW_VU1N_TRACE_PC=<hex>` / `GOW_VU1N_TRACE_ENTRY=0` (diagnostics).

**Results.**
- Replay harness x86 and aarch64 (qemu): ship 2000, consecutive 3000, PC zone 1832 records, 0 differ, warm passes 0
  differ; the result dumps (state incl. VU cycles, data memory, GIF packets) are byte-identical to sw-vu1fast's x86
  dumps (aarch64 dumps identical to x86).
- qemu, VU1 functions per warm pass of 30 records:

| build | ship (s30) | PC zone (g30) |
|---|---|---|
| sw-vu1fast (vf4a) | 1.902 M | 2.321 M |
| sw-vu1cyc (base of this branch; its section's "1.210 M / 1.950 M" with the old counting) | 1.690 M | 2.291 M |
| + direct chaining | 1.638 M (-3 %) | 2.248 M (-2 %) |
| + traces, static guess (`--no-profile`) | 1.626 M (-3.8 %) | 2.104 M (-8.2 %) |
| + traces, edge profile (default) | **1.628 M (-3.7 %)** | **1.935 M (-15.5 %)** |

  PC zone: runNative 0.701 -> 0.093 M, the loop trace t1788 0.45 M replaces six blocks (0.65 M with their dispatch).
  Ship: what is left are the vertex-loop cycles (already one function each) and the JR-driven calls (~13 dispatches per
  run: `jr vi4` dispatch, subroutine returns): no static successor, so neither chaining nor traces reach them.
- x86 replay (WSL, 5 warm passes, three alternating rounds, very noisy): ship 46.4-57.6 -> 49.8-60.0 ms, consecutive
  90.0-112.3 -> 86.5-109.2 ms, PC zone 45.2-62.5 -> 40.7-57.5 ms: the PC zone faster, the ship within the noise (maybe a
  little slower: the trace functions add ~2x generated code).
- PC build (`out/build-pc`, generated parts identical to the replayed ones): zone gate port/vu1disp/gate/20261019_191036
  5/5 ok (60 fps, speed 1.01, no capture flagged against port/vu1cyc/gate/20261019_155028), savetest 129 pages, card
  directory intact. The Switch compiler (devkitA64 GCC 16) builds the generated parts with `musttail`.
- Expected on the console: the A57 gain should follow qemu roughly for the PC-zone-like work (-10 to -15 % VU1 there);
  for the ship fight ~-3 %, plus whatever the direct branches save in indirect-branch mispredictions that qemu does not
  count (runNative's one `blr` served every block before).
- Console build: `VU1_PREGEN=<root>/ext/PS2Recomp-kl-vu1disp/out/vu1pregen` (vu1_generado = sw-vu1-par's, vu1n_generado
  regenerated with this generator: `python -I tools/vu1/vu1n_gen.py <dir>/vu1n_generado port/vu1_micro --heads
  port/vu1n_heads.txt --parts 6 --locals`, which picks up port/vu1n_profile.txt) with `switch/build.sh` of the
  **vu1disp** worktree (runtime changed; copy the untracked build files of the sw-vu1-par worktree into a new WSL build
  dir); then the replay benchmark env.txt (docs/vu1-native-flags.md). `GOW_VU1N_TRACES=0` / `GOW_VU1N_DIRECT=0` in
  env.txt give the A/B on the same build.

**Next.** The ship's remaining dispatches are JR targets: a per-JR-site guess (the profile's most frequent target,
checked against the slot's key like chainDirect) would chain them too. The vertex-loop cycles have their own fast
variants; traces exclude them. Traces have no fast (range-analysis) variants yet: the PC zone loop trace runs the exact
float handling of b1748 (which failed its fast entry check often anyway).

## Loops that read entry flags stay in their function (2026-10-19, ext agent/sw-vu1cyc)
**Handoff.** Branch `agent/sw-vu1cyc` (ext 31a482d + 823cf54; worktree ext/PS2Recomp-kl-vu1cyc, own build dir
`out/build-pc`, configured like sw-vu1fast's with `-DPS2X_VU1N_GEN_OVERRIDE=<root>/port/vu1cyc/gen/vu1n_gen.py`).
Only the runtime changed (ps2_vu1_native.h / .inl); the generator's default output is byte-identical to sw-vu1fast's
(tools/vu1/vu1n_gen.py only gained the opt-in `--regions`, below). Not yet run on the console.

**What it was.** About 40 % of what was left on the ship recording (qemu) was `runNative` + `blockTiming`. The hot
self-loops and two-block cycles of the vertex loops (e.g. b3598, b2858: `FMEQ`/`FMAND` read the MAC flags of the
previous pass, so the block "reads entry flags") could not chain: `chainTo` refused every successor that reads entry
flags, so each pass returned to runNative, which ran `blockTiming` for it. Those blocks also hold a DIV (FDIV unit read
at entry), so they were not `memoable` either: every pass did the full history walk, signature and cache search.

**Change (bit-exact by construction).**
- `VU1NativeAccess::entryTiming` (inline): the predecessor memo, then `blockTiming`. runNative and chainTo both use it.
- The memo also serves blocks that read FDIV/EFU state: `scalarKey` packs the FDIV/EFU part of blockTiming's in-flight
  signature (slack per unit, 0 = not busy) into one word, stored with the memo (`Slot::memoScalar`). Under the memo's
  conditions (predecessor ended at this block's start after >= maxLat cycles, nothing from the exact engine in flight)
  the lane part of the signature depends on the predecessor's timing table alone, so equal (predecessor, table, scalar
  key) means equal signature, hence the same cached table and id that blockTiming would return.
- `chainTo` enters a successor that reads entry flags with that entry timing (and its tail record on it), exactly as
  runNative would set it up. The self-loops and cycles of the vertex loops now run every pass inside their function.
- `GOW_VU1N_CHAIN_FLAGS=0`: the former behavior (same results; checked on all three recordings).
- Replay: warm passes are checked too now (`(warm passes: N differ)`: hash or VU cycle count unlike the cold pass).

**Results.**
- Replay harness, x86 and aarch64 (qemu), against the recorded hashes: ship 2000, consecutive 3000, PC zone 1832 records,
  0 differ, warm passes 0 differ; the result dumps (state incl. the VU cycle count, data memory, GIF packets) are
  byte-identical to sw-vu1fast's x86 dumps. With `GOW_VU1N_CHAIN_FLAGS=0` likewise.
- qemu aarch64 (one warm pass, replay hashing left out): ship 30 records **1.418 M -> 1.210 M (-14.7 %)**; blockTiming
  0.199 M -> ~0, generated blocks 1.085 -> 1.064 M (b3598 0.167 -> 0.135 M: no entry check and register reload per
  pass), runNative unchanged (0.372 -> 0.377 M: it now dispatches mostly the straight-line blocks and the multi-block
  loops). PC zone 30 records 1.983 -> 1.950 M (-1.7 %): its hot loop (0x1728-0x1bc8) has six blocks per pass.
- x86 replay (WSL, 5 warm passes, three alternating rounds; too noisy to rank): ship 33.9-36.5 -> 32.5-36.3 ms,
  consecutive 64.3-76.3 -> 59.8-66.3 ms, PC zone 34.9-36.7 -> 36.2-37.3 ms.
- PC build (`out/build-pc`, generated parts identical to the replayed ones): zone gate port/vu1cyc/gate/20261019_155028
  5/5 ok (60 fps, speed 1.00-1.01, no capture flagged against port/vu1fast/gate/20261019_143226), savetest 129 pages,
  card directory intact.
- Expected on the console: qemu predicted the A57 well for the last two changes, so the ship replay benchmark should drop
  by ~12-15 % against sw-vu1fast, and VU1 busy in fights by about as much (~3.3-3.6 -> ~2.9-3.2 s per 5 s, estimate).
- Console build: `VU1_PREGEN=<root>/ext/PS2Recomp-kl-vu1cyc/out/vu1pregen` (a copy of sw-vu1fast's: vu1_generado =
  sw-vu1-par's, vu1n_generado = the default generator output) with `switch/build.sh` of the **vu1cyc** worktree (the
  runtime changed; copy the untracked build files of the sw-vu1-par worktree into a new WSL build dir); then the replay
  benchmark env.txt (docs/vu1-native-flags.md). `GOW_VU1N_CHAIN_FLAGS=0` in env.txt gives the A/B on the same build.

**Loop regions (`--regions`, measured, off by default).** The dispatches left are straight-line blocks and loops of
three or more blocks (the shadow loop at 0x0cd8-0x0e10: 9 blocks, ~15 passes per run on the ship; the PC zones' loop
0x1728-0x1bc8: 8 blocks, ~40 passes per run). `gen_region` gives every block of such a loop (strongly connected
components of the static block graph, split into inner loops: `loop_regions`, at most 10 blocks / 64 pairs, blocks that
call out to the exact engine end a loop) a function that runs the loop from it, going from block to block through
chainTo. Bit-exact (0 differ on all recordings, x86 and aarch64), but slower in qemu:
- all the loop's registers in locals across the function (first version, not kept): ship 1.213 M (+0.2 %), PC zone
  1.914 M (-1.8 %); GCC spills the ~30 vector registers of the loop (b1748's region 0.79 M);
- each block's own body (fast variant and entry check included) in its own scope, registers through the VU1State between
  the blocks (current `--regions`; a compiler barrier per block changes nothing): ship 1.296 M, PC zone 2.066 M. A chain
  step costs more instructions than runNative's dispatch it replaces (runNative keeps its state in registers; in the
  large function every step reloads it).
x86 timings looked better for the second version (PC zone ~30-34 ms), but within the noise. Not used.

**Next.** What is left on the ship: the generated blocks, then runNative (~0.38 M, ~170 dispatches per run: the
straight-line program prologue, ~25 blocks, and the multi-block loops above). A cheaper dispatch would have to drop work
per block (history entry, FDIV/EFU starts, commitDue), not just move it into chainTo.

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
