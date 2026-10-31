# God of War.app (local macOS app)

`tools/macos/make_app.sh` builds `dist/God of War.app` (git-ignored) from this machine's runner build. Local use only:
the runner holds recompiled game code. Needs Xcode (`swiftc`, `codesign`, `iconutil`) and a built
`ext/PS2Recomp-kl/out/build-mac/ps2xRuntime/ps2EntryRunner` (`RUNNER=<path>` for another one). Re-run after every
runner build.

    Contents/MacOS/God of War   launcher (launcher/*.swift, AppKit)
    Contents/MacOS/GodOfWar     the runner
    Contents/Resources/AppIcon.icns  from port/switch/nsp/icon.jpg
    Info.plist                  local.godofwar.port; GOWDefaultDiscFolder = <repo>/disc-usa,
                                GOWImportCard = out/build-mac/run/Mcd001.ps2

Run: `open "dist/God of War.app"` (window) or `open "dist/God of War.app" --args --play` (straight into the game).

The game data is not copied: on Play the launcher prepares `~/Library/Application Support/God of War/`:

| Path | Content |
|---|---|
| `data/` | symlinks to every file of the disc folder (not its test cards), `IOP_MOD/<name>.irx` links; the runner's cwd |
| `saves/GodOfWar.sav` | the save file (`GOW_MC0`), copied to `GodOfWar.sav.bak` before every start |
| `settings.ini` | `[Video] Width, Height, Fullscreen, Scale`, `[Game] DiscFolder, ImportOffered` |

and runs `GodOfWar <data>/SCUS_973.99` with `GOW_WINDOW=<w>x<h>`, `GOW_FULLSCREEN=0|1`, `PS2X_GS_HW_SCALE=1-4`,
`PS2X_GS_WORKER=1`, `GOW_GS_FINISH_ASINCRONO=1`, `GOW_AUDIO_DEVICE=default` (unless `GOW_AUDIO` is set). Log:
`~/Library/Logs/God of War/game.log` (previous run: `game.log.1`). On the first start, when the build has a card at
`GOWImportCard` and there is no save yet, the launcher offers to copy it. Language, 120 Hz and graphics options are
shown greyed out until they exist (odd/tasks/macos-port.md T11-T13).
