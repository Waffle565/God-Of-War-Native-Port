# Deterministic fight benchmark (GOW_BENCH, console and PC)

Why: the console bot's fight (`cycle4.sh PLAY=1`) takes a different course on every run (wall-clock button presses
over the network, AI, game time tied to the frame rate), so two packages could only be compared per scene, and a scene
reached in one run (e.g. the 57 fps "Kratos alone" scene) might not come in the next. The benchmark makes the game
compute the same frames on every run, whatever the frame rate, and reports fps and per-thread busy per scene.

## How it works (gow_overrides.cpp, `GOW_BENCH=1` with `GOW_PAD_TEST=1`)
- **Fixed frame step.** The game measures each frame (0x17D778: T0 ticks since the last frame -> fields, clamped,
  `max(fields, 60 / target fps)`; step = fields / 60 at 0x29C64C, scaled step at 0x29C658; docs/performance.md
  "Game-speed meter"). From the first frame on (menus and loading included), `gowBenchFrameClock` runs the original
  and then stores the step of exactly the target rate (`0x29C654` fields, 1 at 60 fps) with the same float operations.
  Game logic then advances by the same amount per frame on every run; below 60 fps the game runs in slow motion (game
  time = frames / 60). Forcing it only from the gameplay start was not enough: two runs then differed by one ulp in
  Kratos's position at frame 60 (state from the load frames) and drifted apart after ~70 game seconds.
- **Same random numbers.** At the gameplay start (game state 11, nothing pending: the existing pad-test detection) the
  newlib rand seed (`*(0x2A4294)+0x58`, the game's only LCG, overridden by `gowRand`) is set to a constant.
- **Input per frame.** The pad script's `g<s>` times count game frames / 60 (`g_benchFrames`, counted at each frame
  clock read) instead of wall seconds, so every press lands on the same frame. Menu presses (absolute times) stay on
  wall time; what happens before the gameplay start does not matter once the seed and the step are fixed.
- **Signature.** Every `GOW_BENCH_EVERY` frames (default 60) a `[bench] f=<frame> wall=<s> pos=... hp=... seed=...`
  line: Kratos's position, health (bits) and the rand seed. Equal lines in two runs = the same game state at that frame.
- `GOW_PAD_GUION_FILE=<path>`: the pad script from a file (comma or line separated, after `GOW_PAD_GUION`); env.txt
  lines on the Switch are limited to 512 bytes.

## The fight script (tools/bench_script.py)
New game from the title (start, then cross x4 from the menu delay on, 30 s by default on the console), then the
bot's pattern from gameplay start: square, square, triangle every 5.5 game seconds, no walking; god mode
(`GOW_GOD_MODE=1`) and `GOW_SKIP_FMV=1`. Default 240 game seconds.

## Console run (tools/switch_bench.sh)
    UPDATE=port/switch/nsp/out/update_<pkg> bash tools/switch_bench.sh <name> [wall secs, 480] [extra env file]
Over WLAN (sys-ftpd, `tools/switch_ftp.py`; docs/switch.md "Remote testing over WLAN") from any console state, else
over USB MTP from DBI's MTP screen (`TRANSFER=ftp|mtp` forces one; steps in `tools/switch_lib.sh`): pushes the
package, the script (`switch/gow/bench.txt`) and env.txt (clocks, audio stats, pad test without captures, bench,
god mode, skip FMV + the extra file's lines, e.g. `GOW_IOP_THREAD=1`), runs the game (screenshot every 30 s into
`port/switch/logs/bench_<name>/shots`), closes it (MTP: back to DBI's MTP screen), pulls the log, deletes env.txt from
the SD and prints the report. The console stays on a wall charger: at GPU max it drains more than a PC USB port
charges (run `k10d1` ended with a flat battery).
**Clocks.** Since 2026-10-24 (user decision) the target is 60 fps at the **default handheld clocks** (GPU 307.2 MHz,
EMC 1331.2 MHz; CPU at the game's 1785 MHz boost), so the bench sets no GPU / EMC clock by default. `BENCH_GPU=max`
(or MHz; `GOW_GPU_MHZ`, handheld max 460.8) and `BENCH_EMC=...` (`GOW_EMC_MHZ`) give the old setting: every run up to
sw-k10 (`b2*` ... `k10*`, and `k10e1`) was at `GOW_GPU_MHZ=max` with EMC at the default. Do not edit
`tools/switch_bench.sh` while a run is going: bash reads it as it runs (run `k10d2` broke that way and lost its log).

## Report (tools/bench_report.py)
    python -I tools/bench_report.py port/switch/logs/bench_<name>/log.txt [--ref <other log>] [--scenes a-b,...] [--windows]
Scenes are ranges of game seconds; fps is exact over those frames (wall times of the `[bench]` lines); busy columns are
the mean per 5 s window lying inside the scene (ms per 5 s): `ee` (EE game code) + `iop` (inline IOP) = `core1`,
`vu1` (VU1 thread job time), `idle` (VU1 thread waits), `chain` (EE chain walk + copy), `gsgl` (GS stage), `pres`
(deko3d present_submit), `gsq` (EE waits on the GS queue). `--ref` checks the signatures against another run.

## How deterministic it is (console, 2026-10-24)
- Two runs of the same package (`b2a`, `b2b`: update_b2, `GOW_CHAIN_STREAM_KB=0`): the `[bench]` signatures (position,
  health, rand seed every 60 frames) are **bit-identical for the first 63 game seconds** (frames 0-3780); at frame 3840
  Kratos's position differs by a few units with the rand seed still equal, and the runs drift apart from there (same
  fight, same kind of scene). Not found yet: what game state reads a clock or the IOP at ~64 s (candidates: a sound /
  stream end the game waits on, a vblank-counter timer).
- Per-scene fps of the two runs: 53.4 / 53.6, **35.3 / 35.3**, 44.1 / 44.1, 45.2 / 45.2 in the identical part
  (0-10, 10-20, 20-30, 30-60 s), 46.6 / 45.8, 47.6 / 47.0, 47.2 / 47.3 after it (60-90, 90-120, 120-150 s).
- With the step forced only from the gameplay start (`b1a`, `b1b`, update_b1): identical seeds and positions within a
  hundredth for ~70 s, then apart.

## Scenes of the default script (game seconds from the gameplay start)
| scene | game s | what | b2a / b2b fps |
|---|---|---|---|
| intro | 0-10 | Kratos on the deck, first enemies arriving | 53.4 / 53.6 |
| **heavy** | 10-20 | the crowd of undead around Kratos (`b1a/shots/g3.jpg`); VU1 thread 4.61 s busy per 5 s, ~= the bot's 125-150 s phase (33-37 fps) | **35.3 / 35.3** |
| mid | 30-60 | the deck fight, 3-5 undead | 45.2 / 45.2 |
| late | 90-150 | the deck fight goes on (the script never walks, so the bot's "Kratos alone" 57 fps scene does not come) | 47.0-47.6 |
Two start states (sw-k9, 2026-10-24): a run gets one of two rand seeds at frame 60 (A `1e559a6a`, B `bd5bc50d`; same
position), apparently at random (`s4a`, `s4b`, `k9a2`, `k9e2` B; `k9p`, `k9s`, `k9a1`, `k9c1`, `k9e1` A), so compare
`--ref` against a run of the same start state: runs of one state are identical up to the 64 s divergence (A vs `k9s`:
63-73 s; B vs `s4a`: 70 s). Per-scene fps is not affected beyond the usual spread.
Run-to-run spread: 0.0-0.2 fps in the identical part, up to ~0.8 fps after 64 s. Wall time per run ~7 min (330 s
game + boot + menus + pull), `BENCH_GAME_S=150`.

## Results log (console, handheld, GPU max; logs `port/switch/logs/bench_<run>`)
| run | package / env | 0-10 | heavy 10-20 | 20-30 | mid 30-60 | 60-90 | 90-120 | 120-150 |
|---|---|---|---|---|---|---|---|---|
| `b2a` / `b2b` | update_b2 (bench from boot), `GOW_CHAIN_STREAM_KB=0` = before sw-k8 | 53.4 / 53.6 | 35.3 / 35.3 | 44.1 / 44.1 | 45.2 / 45.2 | 46.6 / 45.8 | 47.6 / 47.0 | 47.2 / 47.3 |
| `s4a` | update_s4 (chain streaming, default) | 54.0 | **36.4** | 45.3 | **46.4** | 47.6 | 48.7 | **48.7** |
| `e4p` | update_s4 + `GOW_EE_SAMPLE` | (0-30: 43.9) | | | 46.6 | 47.1 | 49.8 | 48.4 |
Later runs report heavy / mid / late only (`--scenes 10-20,30-60,90-150`); sw-k9 (docs/performance.md "VU1 thread throughput"):
| run | package | heavy 10-20 | mid 30-60 | late 90-150 | seed at frame 60 |
|---|---|---|---|---|---|
| `k9p` | update_s4 + `GOW_PC_SAMPLE=1000` (GSWorker) | 35.8 | 45.7 | 47.9 | A |
| `k9s` | update_s4 (reference of the day) | 36.4 | 46.5 | 48.2 | A |
| `k9a1` / `k9a2` | update_k9a (g7 blocks) | 36.6 / 36.3 | 46.9 / 46.9 | 49.5 / 48.7 | A / B |
| `k9c1` | update_k9c (+ chainStep leaf, first form) | 36.8 | 47.3 | 49.9 | A |
| `k9e1` / `k9e2` | update_k9e (+ kernels, NEON helpers, hleMatch, unpack) | 37.9 / 37.9 | 48.4 / 48.2 | 50.9 / 50.2 | A / B |
| **`k9g1` / `k9g2`** | **update_k9g (sw-k9 final: g9 blocks + runtime)** | **38.3 / 38.9** | **49.1 / 49.7** | **50.9 / 51.7** | B / B |
sw-k10 (docs/performance.md "VU1 thread throughput, part 2"):
| run | package | heavy 10-20 | mid 30-60 | late 90-150 | seed at frame 60 |
|---|---|---|---|---|---|
| `k10p` | update_k9g + `GOW_PC_SAMPLE=1000` (GSWorker) | 38.1 | 48.4 | 51.1 | B |
| `k10a1` / `k10a2` | update_k10a (fast traces) | 39.3 / 39.0 | 50.1 / 49.8 | 51.9 / 52.2 | A / B |
| `k10b1` | update_k10b (+ VIF prefetch) | 39.2 | 50.0 | 52.9 | A |
| **`k10c1` / `k10c2`** | **update_k10c (+ JR trace share 0.3; sw-k10 default)** | **39.3 / 39.2** | **50.1 / 50.2** | **52.6 / 52.6** | A / B |
| `k10d1` | update_k10d (+ `--mem-origins`) | flat battery ~2.5 min in (console asleep), no log | | | |
sw-k11 (docs/performance.md "The GPU at the default clocks"); `k10d2` lost its log (tools/switch_bench.sh edited while it ran):
| run | package / clocks | heavy 10-20 | mid 30-60 | late 90-150 | seed at frame 60 |
|---|---|---|---|---|---|
| `k10e1` | update_k10e (t4 = + loaded values), GPU max | 41.2 | 53.4 | 54.2 | A |
| `g2` | update_k11a (k10e + GPU timer on), GPU max | 40.3 | 51.7 | 54.1 | A |
| **`dc1`** | **update_k10c, default clocks (the new baseline)** | **29.1** | **35.0** | **37.6** | A |
| **`de1`** | **update_k10e, default clocks** | **29.3** | **35.2** | **37.4** | A |
| `g1` / `x0` | update_k11a + GPU timer / k11b with `GOW_GS_HW_EXPAND=0`, default clocks | 29.0 / 29.1 | 35.0 / 35.0 | 37.5 / 37.6 | A / A |
| `g3` | update_k11a + GPU timer + `GOW_SCALER=bilinear`, default clocks | 30.1 | 37.7 | 40.3 | A |
| `x1` / `x2` | k11b / k11c (palette expansion), default clocks | system error ~10 s into the fight | | | B / B |
Other runs (IOP thread, streaming variants): docs/iop-thread.md, docs/performance.md "VIF1 chain streaming".
nat-vu1g (native VU1 runs, docs/native-render-study.md; `GOW_PERF_DIAG=1`; vu1 = VU1 thread busy ms per 5 s, 90-150):
| run | package / env / clocks | heavy 10-20 | mid 30-60 | late 90-150 | vu1 | seed at frame 60 |
|---|---|---|---|---|---|---|
| `nv1on1` / `nv1on2` | update_nv1, `GOW_VU1_NATIVE_RUN=1`, default | 25.8 / 25.8 | 35.7 / 35.6 | 37.2 / 37.2 | 3847 / 3902 | B / B |
| `nv1m1` / `nv1m2` | update_nv1, `=m` (all recompiled), default | 28.5 / 28.5 | 35.1 / 35.2 | 37.3 / 37.5 | 3341 / 3372 | A / A |
| `nv1xon` / `nv1xm` | update_nv1 `=1` / `=m`, GPU + EMC max | 33.0 / 39.8 | 44.9 / 51.1 | 46.1 / 53.6 | 4778 / 4732 | A / B |
| `nv2ap` | update_nv2a `=1` + `GOW_PC_SAMPLE` (profile, 330 s) | (0-30: 34.6) | 35.0 | (90-150: ~37.1) | ~3700 | 4b2b0cc2 (new) |
| `nv2d1` / `nv2m1` | update_nv2 (Switch default: 3d0d + 7ab native) / same `=m`, default | 29.0 / 28.6 | 35.3 / 34.9 | 37.6 / 37.3 | 3249 / 3317 | A / B |
| **`nv2xd` / `nv2xm`** | **update_nv2 default / `=m`, GPU + EMC max** | **41.1 / 39.9** | **52.5 / 51.2** | **55.0 / 53.2** | 4700 / 4719 | A / A |
nv2 per run (A57): 3d0d1206 3.6 vs 4.2 us, 7ab32108 4.5 vs 6.0-6.1 us native vs recompiled. The nv2 builds' signatures
already part at frame 120 between two runs of the same package and env (`nv2d1` vs `nv2xd`; rand seed, positions still
equal), so the 63 s identity no longer holds there; `nv2ap` had a third frame-60 seed (4b2b0cc2). Not investigated.
nat-vu1j (ext agent/natvu1, docs/native-render-study.md; default clocks, `GOW_PERF_DIAG=1`, `BENCH_GAME_S=150`; vu1 = VU1
thread busy ms per 5 s, 90-150; all seed A at frame 60):
| run | package / env | heavy 10-20 | mid 30-60 | late 90-150 | vu1 |
|---|---|---|---|---|---|
| `nv3a1` | update_nv3a (npart lazy MAC + copy-on-write undo, loop-kind cache), `=1` | 28.0 | 35.2 | 37.3 | 3381 |
| `nv3b1` | update_nv3b (+ merge of agent/natvu1i, skin28 FastOps, UMAXV ok, vector rcpw), `=1` | 27.9 | 35.2 | 37.3 | 3377 |
| `nv3cp` | update_nv3c (rcpw scalar again) `=1` + `GOW_PC_SAMPLE` (profile) | 28.0 | 35.1 | 37.8 | 3382 |
| `nv3d1` | update_nv3d (+ canonicalGen), `=1` | 28.3 | 35.2 | 37.6 | 3425 |
| `nv3d2` | update_nv3d (Switch default mask 0x3) | 28.9 | 35.2 | 37.4 | 3268 |
| `nv3dp` | update_nv3d `=1` + `GOW_PC_SAMPLE` (profile: tryRun still hashed, fixed in nv3e) | | | | |
| **`nv3e1`** | **update_nv3 (= nv3e: + four-generation code caches; default mask 0x3)** | **28.8** | **35.1** | **37.5** | 3308 |
| (`f1`) | update_k11d (installed default, GPU timer on; sw-k11) | 29.3 | 35.1 | 37.7 | 3173 |
| (`nv2d1`) | update_nv2 (default mask) | 29.0 | 35.3 | 37.6 | 3249 |
Per run (us, nat for `=1`, rec from nv2m1 / nv3d2): 3d0d1206 3.6 / 4.2, 7ab32108 4.9 / 6.2, 93febeec 9.4 / 7.2-7.4,
39f38df4 20.0 / 13.3-13.5, 6df5c750 36.0 / 28.0-28.4, misc 47.0 / 44.4-45.2. update_nv3 does not beat update_k11d at the
default clocks (GPU-bound there): not installed, k11d's exefs pushed back to the console after the runs.
nat-vu1l (ext agent/natvu1, docs/native-render-study.md; `GOW_PERF_DIAG=1`, `BENCH_GAME_S=150`; fps by
`bench_report.py --scenes 10-20,30-60,90-150`; vu1 = VU1 thread busy ms per 5 s, 90-150; us = per run, run-weighted over
every full `[vu1nat-run]` window after the first two, nat / rec):
| run | package / env / clocks | heavy 10-20 | mid 30-60 | late 90-150 | vu1 | 3d0d / 7ab / 93fe / 39f3 / 6df5 / misc us |
|---|---|---|---|---|---|---|
| `nv3ep` | update_nv3 `=1` + `GOW_PC_SAMPLE` (profile of nv3e), default | | 35.1 | 37.2-37.5 | 3360-3412 | 3.75 / 4.99 / 9.55 / 20.0 / 36.5 / 42.5 |
| `nv4a1` | update_nv4a (fewer snapshots) `=1`, default | 27.9 | 35.1 | 37.4 | 3411 | 3.71 / 4.65 / 9.99 / 20.3 / 35.4 / 42.0 |
| `nv4b1` | update_nv4b (+ one GIF submit per run, exit lanes from s) `=1`, default | 28.1 | 35.2 | **35.4** | 3286 | 3.72 / 4.63 / 9.95 / 20.3 / 35.1 / 41.2 |
| `nv4e1` | update_nv4e (+ FastOps on results / 2^126, inlined FMAC helpers, merge of natvu1k; per-packet submit) `=1`, default | 28.6 | 35.1 | 37.4 | 3263 | 3.50 / 4.60 / **8.79** / **18.65** / **32.3** / 41.85 |
| `nv4em` | update_nv4e `=m` (all recompiled, measured), default | 28.4 | 35.1 | 37.2 | 3369 | 4.27 / 6.11 / 7.25 / 13.42 / 28.1 / 40.88 (rec) |
| **`nv4d1`** | **update_nv4 (= nv4e, Switch default mask 0x3), default** | **29.1** | **34.9** | **37.3** | 3216 | 3.74 / 4.95 / (7.13 / 13.25 / 28.1 / 40.1 rec) |
| **`nv4xd`** | **update_nv4 default mask, GPU + EMC max** | **41.2** | **52.7** | **54.7** | 4703 | 3.68 / 4.86 / (7.18 / 13.40 / 27.85 / 41.1 rec) |
| (`f1`) | update_k11d (installed default) | 29.3 | 35.1 | 37.7 | 3173 | |
| (`nv2xd`) | update_nv2 default mask, GPU + EMC max | 41.1 | 52.5 | 55.0 | 4700 | |
update_nv4 does not beat update_k11d at the default clocks (GPU-bound: 29.1 / 34.9 / 37.3 vs 29.3 / 35.1 / 37.7) and
equals update_nv2 at max clocks (the VU1 thread is the limit there, and lit / skin / misc still run recompiled): not
installed, k11d's exefs pushed back. nv4b1's late-scene drop (35.4) came with the one-submit-per-run GIF path, which is
therefore opt-in (`GOW_VU1NAT_MERGE_KICKS=1`); not repeated.

nat-vu1n (ext agent/natvu1, docs/native-render-study.md; same columns as nat-vu1l; `gpu` = GPU busy ms per 5 s at
90-150 (`GOW_DK_GPU_TIME=1`, from nv5f on); us per run, whole bench, nat / rec. From nv5f on the nat time starts after
the classification, like the rec time; earlier nat rows include it):

| run | package / env / clocks | heavy 10-20 | mid 30-60 | late 90-150 | vu1 | gpu | 3d0d / 7ab / 93fe / 39f3 / 6df5 / misc us |
|---|---|---|---|---|---|---|---|
| `nv5b1` | update_nv5b (typed accesses + natvu1m 9e1cf56 ATEST1) `=1`, default | 29.8 | 39.7 | 41.6 | 3540 | | 3.47 / 4.53 / 8.91 / 19.04 / 32.24 / 39.40 |
| `nv5f1` | update_nv5f (+ inlined set-up, Undo &, classification out of the timing, + sw-vu1gpu timer) `=1`, default | 30.2 | 39.3 | 41.8 | 3510 | 4905 | 3.34 / 4.20 / **8.13** / **16.61** / **29.97** / **36.39** |
| `nv5fa0` | update_nv5f `=1` + `GOW_GS_HW_ATEST1=0`, default | 29.1 | 35.3 | 37.6 | 3162 | 4912 | 3.35 / 4.18 / 8.07 / 16.62 / 29.96 / 36.03 |
| `nv5ffg` | update_nv5f `=1` + `GOW_GS_HW_FAILGT=1`, default | 31.3 | 43.4 | 44.9 | 3767 | 4887 | |
| `nv5fm` | update_nv5f `=m` (all recompiled, measured), default | 29.8 | 39.3 | 41.2 | 3723 | 4900 | 4.15 / 5.94 / 7.17 / 13.33 / 28.47 / 40.67 (rec) |
| `nv5jp` | update_nv5j (nv5f + `[[unlikely]]` redo, stq lane stores, MAC in two stores, natvu1m d32e48d) `=1` + `GOW_PC_SAMPLE` | 32.9 | 42.9 | 43.3 | 3684 | | 3.26 / 4.21 / 8.34 / 16.96 / 30.12 / 36.66 |
| `nv5j1` | update_nv5j `=1`, default | 32.8 | 42.6 | 43.6 | 3622 | 4900 | 3.23 / 4.13 / 8.19 / 16.73 / 30.14 / 36.38 |
| `nv5ja` | update_nv5j `=1` + `GOW_GS_HW_ASEL=0` | 30.1 | 39.5 | 41.8 | 3530 | 4903 | 3.23 / 4.14 / 8.20 / 16.71 / 30.09 / 36.32 |
| `nv5jg` | update_nv5j `=1` + `GOW_GS_HW_FAILGT=1` | 35.5 | 47.8 | 46.6 | 3932 | 4877 | 3.18 / 4.12 / 8.14 / 16.66 / 30.04 / 36.66 |
| `nv5js` | update_nv5j `=1` + `GOW_GS_HW_FAILSPLIT=1` | 33.7 | 43.9 | 44.6 | 3749 | 4894 | 3.22 / 4.13 / 8.15 / 16.71 / 30.13 / 36.43 |
| `nv5jm` | update_nv5j `=m` | 32.6 | 42.8 | 42.9 | 3849 | 4885 | 4.10 / 5.95 / 7.33 / 13.27 / 28.56 / 39.98 (rec) |
| **`nv5kd`** | **update_nv5k (ext 1ea385f + d32e48d, Switch default mask 0x23), default** | **33.4** | **42.1** | **43.7** | 3524 | 4892 | 3.24 / 4.14 / (rec) / (rec) / (rec) / 36.43 |
| `nv5kk1` | update_nv5k `=1` + `GOW_VU1NAT_MERGE_KICKS=1`, default | 33.3 | 42.4 | 43.6 | 3520 | 4892 | 3.07 / 3.92 / 7.82 / 16.38 / 29.97 / 35.45 |
| `nv5kk2` | same, repeated | 33.2 | 42.3 | 43.6 | 3554 | 4896 | 3.08 / 3.97 / 7.93 / 16.48 / 29.86 / 35.27 |
| **`nv5kx`** | **update_nv5k default mask, GPU + EMC max** | **43.1** | **56.2** | **57.8** | 4647 | 4420 | 3.22 / 4.15 / (rec) / (rec) / (rec) / 36.44 |

update_nv5k beats update_k11d at the default clocks (33.4 / 42.1 / 43.7 vs 29.3 / 35.1 / 37.7: the GS alpha-test work of
nat-vu1m / nat-vu1o) and update_nv4 at max clocks (43.1 / 56.2 / 57.8 vs 41.2 / 52.7 / 54.7), but it is not installed:
nat-vu1n's install condition was a `PS2X_VU1_ORACLE=OFF` package, which needs every family to win; k11d's exefs pushed
back. `GOW_VU1NAT_MERGE_KICKS=1` twice (`nv5kk1` / `kk2`): no late-scene drop this time (43.6 / 43.6 vs `nv5j1` 43.6),
per run 0.1-1.1 us less than `nv5j1` (`=1` without it): nv4b1's drop was not the merged submit.
| `nv5l1` | update_nv5l (nv5k + natvu1m 76c64cd) `=1` + `GOW_GS_GPU_TIME=1`, default | 33.2 | 42.4 | 43.6 | | 4933 | |
| `nv5lv` | update_nv5l `=1` + `GOW_GS_HW_STENCILVOL=1` | 33.2 | 42.3 | 43.5 | | 4935 | |
| `nv5lg` | update_nv5l `=1` + `GOW_GS_HW_FAILGT=1` | 35.7 | 47.6 | 47.2 | | 4903 | |
| `nv5lgv` | update_nv5l `=1` + `FAILGT=1` + `STENCILVOL=1` | 35.4 | 47.4 | 47.0 | | 4899 | |

nat-vu1r (ext agent/natvu1 adcb308, docs/native-render-study.md; `GOW_PERF_DIAG=1`, `GOW_DK_GPU_TIME=1`, default clocks,
**the default script length** (`BENCH_GAME_S` 240, 480 s wall: ~103 windows, not the 150 s of the rows above, so the
per-run numbers compare only with each other); fps per bench_report scene, `vu1` = VU1 thread busy ms per 5 s at 90-240;
us per run run-weighted over the whole bench (`famavg.py`), nat / rec of the same package):

| run | package / env | 0-30 | 30-60 | 90-120 | 210-240 | vu1 | 3d0d / 7ab / 93fe / 39f3 / 6df5 / misc us |
|---|---|---|---|---|---|---|---|
| `nv6a1` | update_nv6a (rcpv, non-aliasing MAC / undo stores, MERGE_KICKS default on) `=1` | 44.8 | 42.2 | 43.9 | 43.9 | 3533-3595 | 3.10 / 4.03 / **7.68** / **15.91** / **29.19** / 36.83 |
| `nv6am` | update_nv6a `=m` (all recompiled) | 43.0 | 42.8 | 42.4 | 42.8 | 3852-3915 | 4.10 / 5.97 / 7.16 / 13.15 / 28.64 / 42.55 (rec) |
| `nv6ap` | update_nv6a `=1` + `GOW_PC_SAMPLE=1000` (GSWorker profile) | 44.1 | 42.9 | 43.4 | 43.6 | 3604-3682 | 3.19 / 4.09 / 7.93 / 16.29 / 29.36 / 37.05 |

Native wins 3d0d / 7ab / misc (-24 / -32 / -13 %) and the VU1 thread (-8 %), loses lit +7 %, skin +21 %, skin28 +2 %:
`PS2X_VU1_ORACLE=OFF` still not built (condition: every family wins). Against nat-vu1n's `=1` benches (nv5kk2, 150 s
script) lit 7.93 -> 7.68, skin 16.48 -> 15.91, skin28 29.86 -> 29.19 (different scene mix: indicative only).

Console default from 2026-10-25 (user decision, nat-vu1s): update_nv5k + `GOW_GS_HW_FAILGT=1` (env.txt,
`tools/switch_default.sh`). Bench `nv5kdg` (`BENCH_GAME_S` 150, `GOW_PERF_DIAG=1`, `GOW_DK_GPU_TIME=1`, default
clocks; bench_report scenes): 0-30 46.8, 30-60 47.1, 60-90 45.8, 90-120 46.4, 120-150 47.2, 150-180 47.5 fps; vu1
3754-3833 ms per 5 s, gpu 4874-4905 (nv5lg, nv5l + FAILGT: 46.0 / 47.6 / 46.0 / 47.2 / 47.2 / 47.3).

nat-vu1s (ext agent/natvu1 aa491fe .. 86177ca, docs/native-render-study.md; same settings as nat-vu1r: `GOW_PERF_DIAG=1`,
`GOW_DK_GPU_TIME=1`, default clocks, default script length; fps per bench_report scene, `vu1` = VU1 thread busy ms per
5 s at 90-120 / 210-240, us per run run-weighted (`famavg.py`)):

| run | package / env | 0-30 | 30-60 | 90-120 | 210-240 | vu1 | 3d0d / 7ab / 93fe / 39f3 / 6df5 / misc us |
|---|---|---|---|---|---|---|---|
| `nv7a1` | update_nv7a (hashes by content) `=1` | 44.9 | 42.2 | 43.0 | 43.6 | 3375-3407 | 3.12 / 3.95 / 7.35 / 15.56 / 29.23 / 35.52 |
| `nv7am` | update_nv7a `=m` | 42.4 | 43.2 | 43.2 | 42.8 | 3872-3925 | 4.21 / 6.01 / 7.53 / 13.39 / 28.64 / 43.96 (rec) |
| `nv7b1` | update_nv7b (+ set-up groups on locals) `=1` | | | | | | 3.15 / 3.98 / 7.42 / 15.27 / 28.87 / 35.33 |
| `nv7bm` | update_nv7b `=m` | | | | | | 4.08 / 6.05 / 7.47 / 13.39 / 28.91 / 43.32 (rec) |
| `nv7c1` | update_nv7c (+ skin prime + main fused, helpers inline) `=1` | 44.9 | 42.2 | 43.7 | 42.9 | 3348-3358 | 3.32 / 4.09 / 7.40 / 13.78 / 28.89 / 35.46 |
| `nv7cm` | update_nv7c `=m` | 42.7 | 42.8 | 43.1 | 43.3 | 3833-3888 | 4.20 / 6.10 / 7.40 / 13.31 / 28.92 / 42.97 (rec) |
| `nv7d1` | update_nv7d (+ lit / skin28 fused) `=1` | 44.8 | 42.3 | 43.0 | 43.7 | | 3.21 / 3.98 / 7.08 / 13.66 / 28.25 / 35.54 |
| `nv7dm` | update_nv7d `=m` | 42.7 | 42.8 | 43.1 | 43.1 | | 4.04 / 6.06 / 7.36 / 13.31 / 28.59 / 44.82 (rec) |
| `nv7d6` | update_nv7d `=1` + `GOW_VU1NAT_PAIR=6` (lit unfused) | 44.9 | 42.3 | 42.9 | 43.6 | | 3.28 / 4.04 / 7.30 / 13.70 / 28.36 / 35.66 |
| `nv7e1` | update_nv7e (+ just-loaded rows, MINI as FMIN) `=1` | 44.9 | 42.2 | 43.8 | 44.2 | 3322-3343 | 3.16 / 3.96 / 7.21 / 13.62 / 27.79 / 35.35 |
| **`nv7f1`** | **update_nv7f (+ set-up groups in fast form) `=1`** | 45.0 | 42.2 | 43.8 | 43.9 | 3270-3294 | **3.12 / 3.98 / 6.99 / 12.43 / 27.63 / 35.33** |
| `nv7fm` | update_nv7f `=m` | 42.2 | 43.3 | 43.0 | 42.9 | 4005-4055 | 4.33 / 6.36 / 7.68 / 13.60 / 28.48 / 45.28 (rec) |
| **`nv7foffo`** | **update_nv7foff (nv7f, `PS2X_VU1_ORACLE=OFF`)** | 44.8 | 42.4 | 43.8 | 44.0 | 3306-3352 | 3.20 / 4.03 / 6.98 / 12.43 / 27.66 / 35.58 |

Console default from 2026-10-25 evening (user decision, nat-def): **update_nv7foff + `GOW_GS_HW_FAILGT=1`** (the nv7foff ELF
has the env switch, gs_hw_backend.cpp of /root/src-nv2). Bench `nv7foffg`, same settings as `nv5kdg` (`BENCH_GAME_S` 150,
240 s, `GOW_PERF_DIAG=1`, `GOW_DK_GPU_TIME=1`, FAILGT, default clocks): 0-30 47.7, 30-60 46.4, 60-90 45.9, 90-120 46.4,
120-150 47.2 fps (nv5kdg 46.8 / 47.1 / 45.8 / 46.4 / 47.2: mean 46.7 both, equal within noise); vu1 3521-3652 ms per
5 s (nv5kdg 3754-3833), gpu 4882-4893; boot to the title menu checked. Normal play `nv7foffplay` (`NOBENCH=1`,
`GOW_DELTA_DIAG=1`, `GOW_PERF_FRAME_PC=0x001837B8`): `[vblank-diag]` 60.2 Hz, speed 1.00 at 57-60 fps, 0.88-0.94 in the
ship fight at 46-48 fps (nv5kplay shows the same 0.89-0.96 at 45-51 fps; its later windows are a lighter scene, the
wall-timed pad script diverges, so the whole-run means 1.003 / 0.907 are not comparable).

### nat-spd (2026-10-25): game speed below 60 fps (frame-clock catch-up + long-frame debt)
Packages update_spd1-5 (the nv7foff sources + ext agent/natspd + gow_overrides gowFrameClock; ORACLE=OFF; spd5 is the
final one). Bench `spd5g`, same settings as `nv7foffg`: 0-30 47.0, 30-60 47.0, 60-90 46.2, 90-120 47.4, 120-150 47.4 fps
(mean 47.0; nv7foffg 46.7), vu1 3538-4509, gpu 4876-4899 (`spd4g`: 47.6 / 46.5 / 46.0 / 47.5 / 47.5). Benches do not run
the catch-up (fixed step); their `[bench]` lines equal nv7foffo / nv7f1 for the first 151 points, while nv7foffg / nv5kdg take the
other of two branches from f=3840 on: the bench is not bit-deterministic across runs at that point, independent of
this change (spd4g = spd5g). Normal play (`NOBENCH=1`, envs/nobench.txt, 300 s), speed in the 40-52 fps windows:
`nv7foffplay` 0.927 -> `spd4play` 1.001 (47.0 fps) / `spd5play` 1.003 (47.0 fps); 60 fps windows 1.007-1.025 as
before (nv5kplay 1.019). Table and cause in docs/performance.md "Below 60 fps".

Profiles (`GOW_PC_SAMPLE=1000`, `GOW_VU1_NATIVE_FAMS=8`: only skin native): `nv7as` (nv7a, skin 16.06), `nv7bs` (nv7b,
15.77), `nv7es` (nv7e, 13.93). **Every family's native run beats the recompiled one from nv7f on** (lit -9 %, skin -9 %,
skin28 -3 %, misc -22 %, 3d0d -28 %, 7ab -37 %); the ORACLE=OFF package has no skipped run (0 `[vu1nat-skip]`) and the
same times. Rec times vary ~±2 % between packages (7.36-7.68 lit, 13.31-13.60 skin): nat vs rec always in one package.
