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
- Exploration on console (2026-11-01, port/switch/sweep/bossx/*, update_boss2 / update_boss3, screenshots every 4-6 s):
  - Engaged at the warp point, no teleport needed: ATHN08C (Ares, boss bar), ATHN08B (clones / family), ARENA27
    (trial 10/10), PAND02I (two Cerberus). PAND04Z: Kratos falls and the checkpoint restart puts him on the ship.
    PAND02DD, PAND04H (caves), PAND04EE (stone corridor with a gate), PAND03H (altar room): no boss at the warp point.
  - GOW_TELEPORT works from update_boss3 on (update_boss2 left a1 unset: the creature method 0x1DFE58 read a null
    matrix and Kratos landed at 0,0,0): ATHN01D 5:-1000,60,-1450 lands on the lower deck at -1000,-63,-1450 below the
    archers (the six y=94 creatures of the dump), not at a Hydra. A teleport over water or a pit is a fall death; the
    death screen then takes the cross presses and the run ends at the title (later teleports never fire).
  - Ship frame: ATHN01A start 1536,20,1957; ATHN01D warp -976,56,-393 (decks y 56 / -63, x about -700..-1120,
    x -1250 and z -1100 at x -2718 are overboard); ATHN01E warp -2664,672,-695 (save altar on the deck outside the
    cabin, x -2720..-2800, z -580..-730). ATHN01EE has a degenerate warp point (local 0,0,0) and no floor near it:
    it looks like the Hydra-2 boss zone that the deck of ATHN01E uses once the story triggers it.
  - Pandora's Guardian = the armoured undead minotaur of the Challenge of Hades (godofwar.fandom.com); R_MI4 loads in
    PAND03E (lava corridor, warp point -282,496,-553; Kratos falls into the lava at ~g15) and PAND04EE. No creature
    of it in the PAND03E dump at g12 (not spawned yet).
- **Handoff (sw-sweep4, 2026-11-01 17:00)**: BOSSES has ares, clones, challenge, cerberus (no teleport). Open: Hydra 1,
  Hydra 2, Pandora's Guardian. Next steps: (1) single-step teleports only (a fall ends the run); (2) Hydra 2: warp
  ATHN01E and walk / teleport forward along the ship axis (direction about -0.85,-0.52 in x,z from ATHN01A to 01E),
  or find the script trigger that starts the fight; (3) Hydra 1: GoW's first Hydra encounter is in ATHN01B
  (R_AHH/R_AHB; the warp into 01B is impossible because 01B is the start zone, but a teleport from the start is
  possible: walk trace xf in the first minute of a new game); (4) Guardian: PAND03E with a teleport past the lava
  (dump later than g12, or walk). Full run for the found four: `python -I tools/switch_sweep.py --boss --package
  update_boss3` (update_boss3 = dips32 + GOW_TELEPORT, md5 in port/switch/nsp/out/update_boss3).
- Fights from a save (sw-boss2, 2026-11-02): story-triggered fights start from a save slot instead of a warp.
  `SAVE_BOSSES` entries (key, card, slot, pad steps, fight-from g s, note) and ad-hoc `--boss-save
  "key|card|slot|<pad steps>|<fight from>"`: the card (port/switch/boss_cards/<card>, a PCSX2 .ps2 image) is pushed as
  sdmc:/switch/gow/boss.mcd and used via `GOW_MC0` (play.mcd untouched; boss.mcd deleted at the end), the pad script
  loads the save from the title (Start, down, cross, right per picker position, cross, cross; `slot` = position in the
  picker, which lists the saves newest first, not by data<n>.bin) and then runs the pad steps (e.g.
  `g3:lup*10,g15:lright*10`: the walk to the trigger) and from the fight-from second the boss pattern. fps count
  from the first trace line at g >= fight-from. `--boss-item` takes the same two extra fields after the teleport
  (`key|ZONE|<teleport>|<pad steps>|<fight from>`). Cards: console_athn01d.mcd (the console's play.mcd, 2026-11-02:
  one save, Athn01D altar, -1135,56,-447), ref_mcd002.mcd (ref/saves/Mcd002.ps2: slots Dest01, olymp01, Athn02B,
  Pand04AA, AtRet01B). Save slot data (data<n>.bin, 81920 bytes): magic 0xCAFEBAD1, zone name at +8, previous zone at
  +0x13, Kratos's transform at +0x1E (translation at +0x4E).
- Exploration sw-boss2 (2026-11-02, port/switch/sweep/bossx2):
  - Remote steering: usb-botbase's `setStick` does not reach the game (its button clicks do). Test option
    `GOW_PAD_LIVE` (update_boss5 = dips32 + teleport + live pad, md5 e43cfa1c) + `tools/switch_live.py` move Kratos
    step by step with a screenshot after each step (docs/diagnostics.md). Deaths are cheap: "Riparti dall'ultimo
    checkpoint" puts Kratos back at the warp point.
  - Hydra 1: the console's save (console_athn01d.mcd, picker position 1) loads at the Athn01D altar
    (-1135,56,-447). Stick up = -z there: 10 s up reaches the crate stacks under the archers' walkway (-1040,-63,-1430,
    the deck ends at z ~-1490), where Kratos stops; no Hydra yet (the way on is up the net on the crates). The 01D
    dump holds the fight's script names (enc2HydraEnt, HydraFightCams, NCV_HydFightRail, TrigVol): the fight is a
    scripted encounter of 01D.
  - Hydra 2: the ATHN01E warp point is not an altar but a boarded enclosure on the upper deck (x -2760..-2570,
    z -940..-578, y 650-672); the 10 s walks in the four directions trigger nothing.
  - Pandora's Guardian: R_MI4 (the minotaur variant only these zones load; common minotaurs are R_MI5) is in PAND03E
    and PAND04EE only. PAND03E = the lava rooms with blade traps (Challenge of Hades traps, `StatueCrank` in its
    strings): falling off the beams is a death. PAND04EE warp = a cave with a closed wooden gate (a chest beside it)
    and a round room with a patterned floor up the tunnel: the likely arena (the Guardian breaks through a gate).
- **Handoff (sw-boss2, 2026-11-02 05:00)**: BOSSES still ares, clones, challenge, cerberus; Hydra 1, Hydra 2 and
  Pandora's Guardian not found yet (most of the session went to console slots for sw-dips11/12 and sw-crash2).
  Ready: save items (`--boss-save`, `SAVE_BOSSES`), `GOW_PAD_LIVE` + `tools/switch_live.py`, the boss tree rebuilt on
  the dips35 sources (update_boss6 = default dips35 + GOW_TELEPORT + GOW_PAD_LIVE; `rsync -a --delete /root/src-mlang/
  /root/src-boss/` then `boss_sb.sh <tag>` whenever the default changes). Interactive session recipe: env.txt =
  default + `GOW_SKIP_FMV=1 GOW_GOD_MODE=1 GOW_PLAYER_TRACE=1 GOW_PAD_TEST=1 GOW_PAD_TEST_NO_CAPTURE=1
  GOW_PAD_GUION=30:start,39:x,45:x,51:x,57:x GOW_WARP=<zone> GOW_WARP_STAY=1500 GOW_PAD_LIVE=sdmc:/switch/gow/live.txt`
  (or the load keys of `load_keys()` + `GOW_MC0=sdmc:/switch/gow/boss.mcd` for a save), push, `sw_ready` +
  `sw_launch`, then `LIVE_HALF=1 python tools/switch_live.py <shot.jpg> x,y,s[,buttons] ...` (plain python: PIL for
  the half-size copy) and read the half-size shot; with `GOW_PAD_TEST` bot clicks are ignored, so menus (death screen:
  stick up + cross) go through live buttons too. Close: `sw_home_close`, default env.txt back, pull log.txt (its
  `[gow-player]` lines give the positions for `GOW_TELEPORT` / the pad steps), delete live.txt.
  Recipe verified on update_boss6 (md5 fd47027c, port/switch/sweep/bossx2/i_pand04ee2). PAND04EE floor y 880, the
  room spans about x 435..810, z -4510..-4195: a round floor pattern with a sparkling pick-up on it, a chest, a
  free-standing stone block (pushable?) and the closed wooden gate with a chest beside it.
  Next steps: (1) Guardian: PAND04EE, explore the round room up the tunnel and the gate (R1 at the gate did nothing);
  if nothing, PAND03E past the blade traps to the StatueCrank. (2) Hydra 1: load console_athn01d.mcd, walk up to the
  crates (-1040,-63,-1430), climb the net (cross + up) to the archers' walkway and on to the bow. (3) Hydra 2: from
  the 01E enclosure find the exit (boards may break with attacks; the deck lies toward -z). Record each working path
  as `SAVE_BOSSES` / `--boss-item` pad steps (or GOW_TELEPORT positions from the trace), then run all 7 with
  `--boss --package update_boss6`.
- Test package: `bash port/switch/boss_sb.sh <tag>` in WSL builds /root/src-boss (a copy of /root/src-mlang at the
  default's sources + port/overrides-usa/gow_overrides.cpp) in /root/gow-switch-boss, inside a private mount namespace
  where both appear at the mlang paths (incremental build, the mlang tree untouched). update_boss3 = dips32 +
  GOW_TELEPORT (a1 fix, md5 e143a9e1). Run: `python -I tools/switch_sweep.py --boss --package update_boss3`.

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

### Run 20261101_1211 (sw-sweep4, defaults update_dips25 -> dips32 + FAILGT)

- Packages: movie batch 1 (SEMO01 ... A06MP04) on update_dips25; all later items on update_dips32 (installed during
  the run after its checks; the sweep was paused for the d29-d32 benches and the boss exploration and resumed with
  `--resume`). Report port/switch/sweep/20261101_1211/report.md.
- Story movies: 53 PASS (4 batches), 29.7-30.0 pictures/s, all to their sequence end, every speaking movie read its
  Italian dub. Levels: 106 PASS, 2 FAIL: ATHN01B (warp into the start zone, test limit as in every run), PAND04E
  (launch failed: the bot's A press on the tile was lost; rerun below). Fights (9 boss zones at their warp points):
  9 PASS. No crash report, no hang, no frozen or black picture, no error line, game speed 0.98-1.00 everywhere.
  Warning only: "Kratos died after the warp" in 33 items (the square walk off the warp point, as before).
- fps (guest flips per s after the warp, capture windows excluded): mean of the zone means 58.1, median 59.7; no zone
  with a minimum < 25 any more (54 zones in run 20261030_1930). Zones with mean < 50: ATHN06C 38.2 (min 35.4),
  OLYMP02 42.7 (42.2), ATHN01D 43.4 (38.0), PAND02B 46.5 (38.6), fight_ATHN01D 48.3 (37.4), ATHN06B 48.9 (45.6),
  ATHN03D 49.0 (47.2), ARENA14 49.4 (48.8), ATRET01B 49.4 (45.2), ATHN04C 49.5 (48.2). **Mean < 50 and min < 40:
  ATHN06C, ATHN01D, PAND02B, fight_ATHN01D** (the deck and the ATHN06C hall are the known VU1 / GPU-bound scenes).
- PAND04E rerun (port/switch/sweep/rerun/20261102_0014): PASS, 59.2 / 59.8 fps.
- Boss fights (`--boss`, port/switch/sweep/boss/20261101_2357, dips32 default, 90 s each, screenshots every 6 s), fps
  while Kratos fights (state 11, capture windows excluded):

  | boss (zone) | fight s | fps min / mean | notes |
  |---|---|---|---|
  | Ares (ATHN08C) | ~100 | 56.4 / 59.1 | boss bar, Ares with blades, water: correct; Kratos dies later (grab), god mode does not stop it |
  | Kratos clones / family (ATHN08B) | ~90 | 45.0 / 54.3 | family health bar, embrace tutorial card, clones; mission failed at g92 |
  | Challenge of the Gods 10/10 (ARENA27) | ~30 | 54.8 / 57.4 | Kratos falls from the platform at g30; the cross presses then pick "abandon" (title) |
  | Cerberus pair (PAND02I) | ~130 | 49.8 / 57.9 | no death |

  All PASS, no crash, no error line; pictures correct (fog, water, particles, HUD, Italian cards). Hydra 1/2 and
  Pandora's Guardian not covered yet (handoff below).

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
