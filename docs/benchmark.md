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
From DBI's MTP screen: pushes the package, the script (`switch/gow/bench.txt`) and env.txt (GPU max, audio stats,
pad test without captures, bench, god mode, skip FMV + the extra file's lines, e.g. `GOW_IOP_THREAD=1`), runs the game
(screenshot every 30 s into `port/switch/logs/bench_<name>/shots`), returns to DBI's MTP screen, pulls the log,
deletes env.txt from the SD and prints the report.

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
Run-to-run spread: 0.0-0.2 fps in the identical part, up to ~0.8 fps after 64 s. Wall time per run ~7 min (330 s
game + boot + menus + pull), `BENCH_GAME_S=150`.
