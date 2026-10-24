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
