# Console sweep: every movie, level and fight on the Switch

`tools/switch_sweep.py` runs the game on the console item by item and writes one report with a verdict per item:
`port/switch/sweep/<yyyymmdd_hhmm>/report.md` (+ `results.json`, per item `log.txt`, `envdir/env.txt`, bot
screenshots in `shots/`, the warp capture as PNG, Atmosphère crash reports).

```
python -I tools/switch_sweep.py                       # movies, then all 108 zones, then the fights
python -I tools/switch_sweep.py --levels --only ATHN02A,PAND04G --stay 40
python -I tools/switch_sweep.py --movies --movie-batch-s 900
python -I tools/switch_sweep.py --resume port/switch/sweep/<dir>   # continue a stopped run (done items skipped)
python -I tools/switch_sweep.py --dry                 # list the items
```

The console must be at the home menu or running nothing the user needs (the sweep closes whatever runs). It uses
WLAN only (sys-ftpd for files, usb-botbase for presses, screenshots and the running title; docs/switch.md "Remote
testing"), launches with the test user "claude" (tools/switch_lib.sh `sw_launch`), writes the default env.txt back
after every item and ends with `tools/switch_default.sh` plus an exefs md5 check against the default package. Stop
it by killing the python process and its bash/python children, then run `tools/switch_default.sh` with the game
closed.

## Boot options it uses (no new code in the game)

Everything is an existing test option of the console build, read from `sdmc:/switch/gow/env.txt`, off when unset
(docs/diagnostics.md has the full table):

| item | env.txt lines (on top of tools/switch/default_env.txt) |
|---|---|
| movies | `GOW_MOVIE_OVERRIDE=<m1>,<m2>,...` + `GOW_BONUS_MOVIE=24` + `GOW_BONUS_MOVIE_DELAY=8`: after the intro the title menu starts a bonus movie whose load takes the next name of the list, each time the player is idle again (docs/fmv.md). `GOW_PAD_GUION=99999:select` keeps the test presses from starting a new game; `GOW_MPEG_DIAG=1` logs pictures and A/V sync |
| levels | `GOW_SKIP_FMV=1`, new game by the runner's own pad script (`GOW_PAD_TEST=1`, `GOW_PAD_GUION_FILE=sdmc:/switch/gow/sweep_pad.txt`: Start, X x4 from 30 s; then from gameplay start a square walk with square/triangle attacks), `GOW_WARP=<zone>` (0.5 s after gameplay starts the level scripts' LoadForWarp + Warp move Kratos to the zone's warp point), `GOW_WARP_STAY`, `GOW_GOD_MODE=1`, `GOW_PLAYER_TRACE=1` |
| fights | as levels, `--fight-stay` (150 s) and circle mashing for the QTEs |

All items add `GOW_PERF_DIAG=1` (`[gow-perf] guest_flip_hz`) and `GOW_DELTA_DIAG=1` (`[gow-speed] speed=`, game
seconds per wall second). One warp per boot: chained warps can evict the zone Kratos stands in and crash
(docs/game-internals.md "Zones and the debug level warp"), so a level costs one launch (~2.5 min each).

## Judging

- FAIL: new Atmosphère crash report for the game, game no longer the running title, log stopped growing for 75 s
  (live FTP pulls every 30 s), game clock stopped (last 5 `[gow-speed]` 0) or fewer `[gow-perf]` windows than 60 %
  of the stay, warp not requested / timed out / not arrived, black warp capture (mean luminance < 6), three identical
  bot screenshots after 45 s (frozen picture), error lines (zones.py patterns, missing-target, null JALR,
  abort/fatal); movies: not played, no pictures, fewer than 80 % of the pictures the file size suggests.
- SLOW: mean guest fps after the warp < 25 (min < 25 is a warning), mean game speed outside 0.95-1.05, movie
  pictures per second < 28 (30 expected).
- PASS otherwise. Heap-guard lines are warnings.
- Limits: the screenshot check only sees a picture that stops changing (no JPEG decoder on the PC); the warp
  point is the zone start, so a boss whose fight starts further in is not fought (the fight list is a best guess:
  ATHN01D/ATHN01EE/ATHN01F Hydra, ATHN03A Ares in Athens, PAND05A/B, ARENA06/19/27, OLYMP04).

## Results

Filled in by the runs (latest first).

### Lessons from the first run (2026-10-26, port/switch/sweep/20261026_0256)

- The console's Auto-Sleep counts only HID input: the runner's pad script is not HID, and the game does not report
  media playback (appletSetMediaPlaybackState), so 10 min without a press the screen went black mid-movie
  (BONUS03) and FTP stopped answering. The sweep now nudges the right stick (0x600, below the roll threshold) every
  120 s. The same happened to a player who watches a long movie without touching the pad: fixed in update_bugs1
  (`appletSetMediaPlaybackState` while a movie plays, docs/fmv.md "Switch: Auto-Sleep during long movies").
- The log cannot be pulled while the game runs (the open log file is not readable over sys-ftpd), so items end by
  time budget and hangs are judged from the log pulled after closing.
- Movie length from the file size is wrong for the `.PSW` files (16:9, about 0.94 MB/s); a movie counts as played
  to the end when its segment has `[MPEG:wait] ... ended=1 ... seqEnd=1`. Still pictures between movies (title
  menu) made the screenshot frozen-check useless for movies: it is only used for levels and fights.
- `--rejudge <dir>` judges the pulled logs again with the current rules (no console).

### Run 20261026_0256 (stopped by the user at 08:31, partial)

- Coverage: all 67 movies; 46 of 108 zones (ATHN01A ... ARENA26, in the TOC order of `--dry`); fights not started.
- Result (rejudged): movies 67 PASS (30 pictures/s, every one to its sequence end); levels 37 PASS, 8 SLOW, 1 FAIL.
  ATHN01B FAIL = warp into the start zone Kratos already stands in (test limit, not the port). ATHN01EE and
  ATHN05AA: Kratos falls from the warp point and dies (warning).
- Bugs found: (1) Auto-Sleep during long movies (above). (2) game speed 1.055-1.082 (5-8 % fast) after the warp in
  ATHN03A, ATHN05B, ATHN06B, DEST02, PAND00A, ARENA06, ARENA10, ARENA12 (other zones 0.99-1.03); fps mean >= 25
  everywhere, minimum down to 15.4 (ARENA06). Cause of (2) (sw-bugs, docs/performance.md "Game speed 1.03-1.09"):
  not the warp but short frames counted as a whole field (normal play below 60 fps, 2-9 % fast) plus the sweep's
  own 3.5 s warp screenshots replayed by the frame-clock catch-up at 2-3x; fixed in update_bugs1. The per-window
  mean over-weights the windows after a stall.
- Report: port/switch/sweep/20261026_0256/report.md (+ results.json; logs and screenshots stay local, untracked).
- Resume: console at the home menu, then
  `python -I tools/switch_sweep.py --resume port/switch/sweep/20261026_0256` (skips the 113 done items, runs
  ARENA27 ... PAND00E and the fights, ~3 h; restores the default at the end). Stop: kill the python process tree,
  check no switch_sweep/switch_bot/switch_ftp process is left, run `TRANSFER=ftp bash tools/switch_default.sh`.
