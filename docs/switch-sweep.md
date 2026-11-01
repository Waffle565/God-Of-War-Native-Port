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

## Boss fights (`--boss`)

The fight items above only load each boss zone at its warp point. `--boss` runs the real fights: items in
tools/switch_sweep.py `BOSSES` (key, zone, note, GOW_TELEPORT value), one boot each, `--boss-stay` (100 s) of short
steps, square / triangle combos, circle mashing (QTEs) and a cross every 10 s (dismisses trial and tutorial cards),
`GOW_GOD_MODE`, screenshots every `--shot-every` s. The fps of a boss item count from the `[gow-teleport]` line on
(the fight), without the warp-capture windows. A teleport item without that line is FAIL.

- Where the fight is (RAM dumps of the boss zones, run port/switch/sweep/bossdump, update_boss1): ATHN08C is the Ares
  boss fight itself (Ares's health bar right after the warp), ATHN08B the family defence against the Kratos clones
  (also engaged at the warp point), ARENA27 the last Challenge of the Gods trial (10 of 10; its card waits for cross).
  These three need no teleport. The warp points of ATHN01D (save altar before the Hydra deck), ATHN01EE (no point:
  Kratos falls to y -8955 and dies), PAND03H (altar room), PAND04H (empty cave) and ATHN07B (the portal door) are not
  in the arenas: those need GOW_TELEPORT positions.
- Positions: `GOW_PLAYER_TRACE` `xf=` while walking, `tools/gow_objscan.py <dump>` (objects with the creature
  transform layout: Kratos and the enemies share the class vtable 0x2BA528), `tools/gow_warppoints.py <dump>` (the
  warp points of the loaded zones, world positions). Resource WADs per zone (GODOFWAR.TOC) for the boss candidates:
  R_AHH/R_AHB ATHN01B, R_AHA/R_AH2 ATHN01D, R_AH3 ATHN01EE (Hydra), R_MI4 PAND04EE / PAND03E (Pandora's Guardian
  candidate), R_CEP/R_CEA PAND04Z, PAND04H, PAND02I (Cerberus), R_AB3/R_AB4 PAND03H.
- Test package: `bash port/switch/boss_sb.sh <tag>` in WSL builds /root/src-boss (a copy of /root/src-mlang at the
  default's sources + port/overrides-usa/gow_overrides.cpp) in /root/gow-switch-boss, inside a private mount namespace
  where both appear at the mlang paths (incremental build, the mlang tree untouched). update_boss2 = dips32 +
  GOW_TELEPORT (md5 4a1d17c6). Run: `python -I tools/switch_sweep.py --boss --package update_boss2`.

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
  tools/switch_sweep.py FIGHTS, from the boss resource WADs of each zone, run 20261030_1450 below).

## Results

Filled in by the runs (latest first).

### Runs 20261030_1754 / _1925 (sw-sweep3, test package update_sweep3 = update_bugs1 + harness sp fix)

- Package: port/switch/nsp/out/update_sweep3 (exefs md5 825bf22f), built in /root/gow-switch-mlang after the
  gow_overrides.cpp harness hunk of a562e7c was applied to /root/src-mlang; it differs from update_bugs1 only in
  gowMovieOverrideLoad (runs only with GOW_MOVIE_OVERRIDE / GOW_MPEG_DIAG), so it can become the default.
- Story movies (20261030_1754, 4 batches, 53 movies): 53 PASS, 29.7-30.0 pictures/s, every one to its sequence
  end; 36/36 speaking movies read their Italian dub (`voice_it/<movie>.DUB`, SEMO01 the `.PSW` overlay). The JALR 0
  crash of run 20261030_1450 is gone.
- SEMO01 with `--shot-every 3` (20261030_1925): PASS; the intro card shows "Tre Settimane Prima" / "Mar Egeo"
  (shots s016_48.jpg, s017_51.jpg).
- Levels (20261030_1930, all 108 zones, ~3.5 min each, 6.5 h): 107 PASS, 0 SLOW, 1 FAIL. ATHN01B FAIL = warp into
  the start zone Kratos already stands in (test limit, as in the first run). Game speed inside 0.95-1.05 in every
  zone (the 5-8 % fast zones of the first run are fixed). Warnings: minimum fps < 25 in 54 zones (mean >= 25 in all;
  lowest minimum ATHN01A 12.2, OLYMP02 12.8, ATHN06C 13.0, ATHN04C and PAND05A 15.0); "Kratos died after the warp" in
  24 zones: the sweep's square walk takes him off the warp point into a pit (position trace drops, state 14; ATHN05D
  did the same in the first run), the test, not the port. The `hp=` field of `[gow-player]` is always 0 on the
  console, so only `state=14` marks a death.
- Fights (20261031_0203, 9 boss zones, 150 s each): 9 PASS, minimum fps 15.8-24.8 (warning), falls in ATHN01EE,
  ATHN08B, ATHN08C. The screenshots show the zone start, not the boss (ATHN01D: the save point before the Hydra;
  ATHN07B: Kratos at the portal), so these items check that each boss zone loads and runs, not the fights
  themselves: the boss fights still need a save near each boss or a warp point inside the arena.
- After every run the default was restored and checked (exefs md5 a2f50ea0 = update_bugs1).

### Run 20261030_1450 (sw-sweep2, default update_mlangfog1 + FAILGT, movies only, paused for sw-bugs)

- Movies (all 67 incl. BONUS): 7 batches all FAIL, each ended by `[guest-branch:missing-target] JALR 0x161c68
  target 0` right after a dubbed movie's first read (7 of 22 dubbed starts, 0 of ~10 undubbed).
- Cause: sweep harness, not the game. gowMovieOverrideLoad (GOW_MOVIE_OVERRIDE / GOW_MPEG_DIAG only) moved sp 32
  bytes down for the override name, called 0x188CA0 and restored sp; 0x188CA0 returns to the dispatcher early at an
  EE checkpoint when the first disc read is slow (the dub decode made it slower), so sp was restored mid-function
  and its epilogue reloaded s0 from a shifted slot (s0 = 0x2c7084, crash in 0x1619E0). Normal play does not install
  the override. The dub files checked clean (all 43 range tables inside 0xBD audio payloads).
- Fix (port/overrides-usa/gow_overrides.cpp): the name goes 0x200 bytes below sp, sp is not touched. Needs a build
  on top of the next default (update_bugs1) before the story movies are rerun.
- Tool changes: dubbed movies must log `[disc] dub read: .../voice_<SWEEP_LANG>/<MOVIE>.DUB` (SEMO01: `overlay
  read: .../SEMO01.PSW`), else FAIL; BONUS* movies only with `--bonus`; fight list rebuilt from the boss resource
  WADs each zone loads in GODOFWAR.TOC (R_AHA/R_AH2 Hydra in ATHN01D, R_AH3 Hydra in ATHN01EE, R_AB3/R_AB4 in
  PAND03H, R_CEP/R_CEA Cerberus in PAND04H and ARENA27, R_A1A-C Ares in ATHN08B, R_AEV clones in ATHN08C, R_A2A-C
  Ares in ATHN07B; ARENA zones are the Challenge of the Gods).
- Levels and fights: not run yet.

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
