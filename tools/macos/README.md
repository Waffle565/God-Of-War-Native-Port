# God of War.app (local macOS app)

`tools/macos/make_app.sh` builds `dist/God of War.app` (git-ignored) from this machine's runner build. Local use only:
the runner holds recompiled game code. Needs Xcode (`swiftc`, `codesign`, `iconutil`) and a built
`ext/PS2Recomp-kl/out/build-mac/ps2xRuntime/ps2EntryRunner` (`RUNNER=<path>` for another one). Re-run after every
runner build.

    Contents/MacOS/God of War   launcher (launcher/*.swift, AppKit)
    Contents/MacOS/GodOfWar     the runner
    Contents/Resources/AppIcon.icns  from port/switch/nsp/icon.jpg
    Contents/Resources/keyprompts/   keyprompts/*.py + tools/lang_pack.py, gow_wording.py, gow_text.py
    Info.plist                  local.godofwar.port; GOWDefaultDiscFolder = <repo>/disc-usa,
                                GOWImportCard = out/build-mac/run/Mcd001.ps2

Run: `open "dist/God of War.app"` (window) or `open "dist/God of War.app" --args --play` (straight into the game).

The game data is not copied: on Play the launcher prepares `~/Library/Application Support/God of War/`:

| Path | Content |
|---|---|
| `data/` | symlinks to every file of the disc folder (not its test cards), `IOP_MOD/<name>.irx` links; the runner's cwd |
| `saves/GodOfWar.sav` | the save file (`GOW_MC0`), copied to `GodOfWar.sav.bak` before every start |
| `settings.ini` | `[Video] Width, Height, Fullscreen, Scale`, `[Game] DiscFolder, ImportOffered, Text, Voice, ButtonPrompts` |

and runs `GodOfWar <data>/SCUS_973.99` with `GOW_WINDOW=<w>x<h>`, `GOW_FULLSCREEN=0|1`, `PS2X_GS_HW_SCALE=1-4`,
`PS2X_GS_WORKER=1`, `GOW_GS_FINISH_ASINCRONO=1`, `GOW_AUDIO_DEVICE=default` (unless `GOW_AUDIO` is set). Log:
`~/Library/Logs/God of War/game.log` (previous run: `game.log.1`). On the first start, when the build has a card at
`GOWImportCard` and there is no save yet, the launcher offers to copy it. Language, 120 Hz and graphics options are
shown greyed out until they exist.

## Languages
With the European disc (SCES-53133) extracted next to the USA one, `python3 -I tools/lang_pack.py disc-usa disc-eu`
writes `disc-usa/lang/` (text + voice overlays for en, es, fr, de, it; docs/settings.md). The launcher's Text and
Voice menus are enabled when the disc folder has `lang/`; on Play it sets `PS2X_DISC_OVERLAY=lang/<text>_<voice>.layout`
(settings.ini `[Game] Text`, `Voice`). Menus and HUD screens stay in English. The voice also picks the title sequence's
sound bank (`lang/shell_<voice>.bin`: Kratos's line before the intro movie, "Los dioses del Olimpo me han abandonado."
for es), taken from the European `R_SHELL<letter>.WAD`.

Movies: `lang_pack.py` also writes `lang/movies_<code>.bin` (fr, de, it, es; ~240 MB each), the dubbed sound of the 44
movies the European disc dubs (the intro included). When Voice is not English and the file exists, the launcher sets
`PS2X_MOVIE_VOICES=lang/movies_<voice>.bin`; the runtime keeps the USA movies' picture and puts the dubbed track in
place of their PCM while demuxing (`[movie-voice]` lines in game.log). Other movies have no speech or are not dubbed.

## Button prompts (keyboard keys)
The game writes buttons in its texts as tokens (`[XButton]`, `[R2Button]`, ... in R_PERM.WAD's message table and the
Power Up move list) and draws them as PS2 icons. `keyprompts/make_kb_overlays.py` makes a keyboard variant of every
language overlay: `lang/kb_<text>_<voice>.layout`, the same overlay with `kb_text_<text>.bin`, whose texts name the
keys of the keyboard layout (WASD, arrows, left/right click, Space, E, Q/R, F/G, 1-4, Enter, Backspace):

    python3 -I tools/macos/keyprompts/make_kb_overlays.py disc-usa [--show 590,705]   (--show prints those messages)

It needs the language overlays first (`python3 -I tools/lang_pack.py disc-usa [disc-eu]`; English only without the
European disc). The launcher's "Button prompts" row (settings.ini `[Game] ButtonPrompts=Keyboard|Controller`, default
Keyboard) passes `PS2X_DISC_OVERLAY=lang/kb_<text>_<voice>.layout` or the plain one. On Play it runs the copy bundled
in the app (`/usr/bin/python3`, ~0.2 s) when the chosen pair's kb_ overlay is missing or older than its language
overlay; `make_app.sh` also refreshes them when the disc folder has `lang/`. The output stays in the disc folder
(git-ignored game data).

Rules (`keyprompts/keymap.py`): key words per language (es: Espacio, clic izquierdo/derecho, Intro, flechas); a stick
is replaced as a phrase ("[Joystick] izquierdo" -> WASD, right stick -> arrows), messages about L3/R3 keep their
icons. Lines that would be wider than the game's own longest lines (hint boxes 53 characters, move list rows ~40 for
name + keys) use shorter words (es "clic izq.", then LMB/RMB, de LMT/RMT). Key words are plain ASCII: the menu font
has no accented letters. Not changed: the legends drawn as Flash clips ("✕ = Select", "△ = Back", the ✕ under a
hint), which are shapes, not text.
