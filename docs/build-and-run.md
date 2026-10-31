# Build and run

## Toolchain (Windows)
- Visual Studio Build Tools 2026 (MSVC 14.51), Windows SDK 10.0.26100, CMake 4.3 bundled at
  `C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`.
- Generator "Visual Studio 18 2026", x64, build dir `ext/PS2Recomp/out/build`.
- Python 3.11 (+ `capstone` installed with pip) for the tools.
- PS2Recomp: github.com/ran-j/PS2Recomp, cloned at commit 2c5fbb9 (2026-10-05) into `ext/PS2Recomp`.

## Recompile pipeline
1. (once) `ps2_analyzer disc/SCES_531.33 port/gow.toml` — already done; `port/gow.toml` is now hand-maintained.
2. `python -I tools/find_functions.py disc/SCES_531.33 port/output port/functions.csv` — function map
   (`gow.toml` → `ghidra_output = "port/functions.csv"`). Needs an existing `port/output` for the known starts.
3. `rm -rf port/output && ext/PS2Recomp/out/build/ps2xRecomp/Release/ps2_recomp.exe port/gow.toml > port/recomp.log`
   Expect: 6482 functions, 0 errors, ~2240 control-flow warnings (indirect jumps → runtime dispatch, harmless).
4. `tools/sync_runner.sh` — copies changed `port/output/*.cpp` + `port/overrides/*.cpp` into
   `ext/PS2Recomp/ps2xRuntime/src/runner/` and headers into `ps2xRuntime/include/`; deletes stale runner files.
   Copies only changed files so builds stay incremental. Slow-ish on Windows (~2 min, 6500 cmp calls).
5. Build: `cmake --build out/build --config RelWithDebInfo --parallel --target ps2EntryRunner > port/build.log`
   (rebuild the recompiler itself with `--config Release --target ps2_recomp` after changing ps2xRecomp).

## Build cost (important for planning)
- Use **RelWithDebInfo** for iteration. Release uses /GL + /LTCG, making every relink a full link-time codegen.
- Changing any header in the runner's precompiled header (`ps2_runtime.h`, `ps2_call_list.h`, `ee_scheduler.h`,
  `gs_frontend.h`, generated `ps2_recompiled_functions.h`, …) → full rebuild ≈ 20–40 min.
- `.cpp`-only changes in ps2xRuntime/ps2xIOP → seconds to ~1 min.
- Never start a second build while one runs: an overlapped build once ended rc=1 and forced a full recompile.
- `grep " error "` on build.log gives false "Binary file matches"; use `grep -aE 'error C|error LNK'`.
- The link prints `/FORCE` warning LNK4088; that is a configured option, not an error.
- Re-run `cmake -S . -B out/build` after adding source files (globs / CMakeLists edits).

## Run
- Executable: `ext/PS2Recomp/out/build/ps2xRuntime/RelWithDebInfo/ps2EntryRunner.exe <abs path>/disc/SCES_531.33`
  run with cwd = `disc/`. No second argument: the disc comes from `disc/.disc_layout` (see disc.md).
  (argv[2] = path to an .iso is still supported but the user does not want ISO reads.)
- Test runs play sound (user 2026-10-12, "enable the audio"): `tools/zones.py` and `tools/savetest.py` mute only with
  `--silent` (GOW_AUDIO=0).
- `play.cmd` plays the movies (cutscenes; docs/fmv.md). The zone and save tests set `GOW_SKIP_FMV=1` to keep runs
  short; `tools/movies.py` plays every movie of the disc (muted, sound recorded) and judges picture and sound
  (docs/fmv.md; needs numpy).
- Game data comes from the extracted disc in `disc-usa/` (docs/disc.md); `play.cmd` and the test tools never read the
  ISO (`--iso` in zones.py/savetest.py, or `GOW_ISO=<iso>`, for comparisons).
- `tools/run.sh <seconds> <name> [ENV=VALUE ...]` — runs from disc/, waits, screenshots, kills;
  log → `port/<name>.log`, screenshot → `port/<name>.png`.
- `tools/shot.ps1 <process> <out.png>` — PrintWindow capture of the game window (works with overlapping windows).
- The runtime debug panel (F1) is ImGui; its state is saved in `disc/imgui.ini` (set `Collapsed=1` to hide).
- Window 640x448; keyboard + mouse layout in ps2xRuntime/include/runtime/ps2_pad_keyboard.h (read by
  ps2xRuntime/src/lib/ps2_pad.cpp): WASD = left stick (move), arrows = right stick (evade), left click = □,
  right click = △, Space = ✕, E = ○, Q/R = L1/R1, F/G = L2/R2, 1-4 = d-pad up/right/down/left (magic),
  Enter = Start, Backspace = Select. Keyboard/mouse and the gamepad work together: buttons add up, each stick
  takes the source deflected further. Gamepad: the one whose button was pressed last, else the first one.

## PC release folder
`powershell -ExecutionPolicy Bypass -File tools\pc_release\make_pc_folder.ps1 [-Dest <folder>]` (default
`dist\God of War (PC)` in the repo, gitignored; user 2026-10-18: builds stay here, never on Z:) makes the folder a
player gets, laid out like a shipped PC game:

    God of War.exe   launcher (tools/pc_release/launcher.cpp + launcher.rc: icon, version info)
    bin\             GodOfWar.exe (the Release ps2EntryRunner) + its DLLs
    data\            disc-usa without the test cards (Mcd001.ps2, play.mcd, mc0/, mc1/), hard links on the same volume
    saves\           GodOfWar.sav (+ GodOfWar.sav.bak)
    settings.ini     written by the launcher
    README.txt       tools/pc_release/README.txt: controls, saves, folders

- Launcher: a small window (resolution from the primary monitor's modes, Fullscreen checkbox, Play/Quit); the choice
  is saved in `settings.ini` (`[Video] Width, Height, Fullscreen`). `God of War.exe --play` skips the window. It sets
  play.cmd's variables (GS worker, async FINISH, default audio device unless GOW_AUDIO is set) plus
  `GOW_WINDOW=<w>x<h>`, `GOW_FULLSCREEN=0|1` and `GOW_MC0=saves\GodOfWar.sav`, and runs the game with `bin\` as working
  directory. Languages: `[Game] Text, Voice` (en fr de it es) in the same window and file; the launcher sets
  `PS2X_DISC_OVERLAY=lang\<text>_<voice>.layout`, made by the script with tools/lang_pack.py in `data\lang\`
  (always: the save wording; with the European disc extracted in `disc\`: the other languages). docs/settings.md.
- Runner (ext ps2_runtime.cpp): `GOW_FULLSCREEN=1` starts in exclusive fullscreen at the `GOW_WINDOW` size; F11
  switches window/fullscreen at run time; Esc no longer quits (raylib's default exit key; Esc is the pad's circle).
  The window title is the game's name from games_database.cpp ("God of War" for SCUS-97399).
- Icon and version info of the runner: ext CMake option `PS2X_RUNNER_ICON` (set in out/build-kl to
  `tools/pc_release/icon.ico`, made from port/switch/nsp/icon.jpg) generates a resource script with `GLFW_ICON` (GLFW
  takes it for the window class: window + taskbar) and a VERSIONINFO ("God of War", shown by Task Manager).
- Saves: the game keeps its PS2 save format, one card image (`saves\GodOfWar.sav`, PCSX2 .ps2 layout,
  docs/memory-card.md); per-save files would need a folder-backed card (PCSX2's FolderMemoryCard synthesises the
  FAT on the fly), which the native card layer does not have. The launcher copies the file to `.bak` before every
  start. Nothing is written outside the game folder (the LOCALAPPDATA audio_device.txt is only read when
  GOW_AUDIO_DEVICE is unset, which the launcher always sets).
- Re-running the script replaces the program and relinks changed data files; `saves\` and `settings.ini` are never
  overwritten (the first layout's `disc\play.mcd` is moved to `saves\GodOfWar.sav` once).
- Tested 2026-10-18: title "God of War", icon, F11 both ways, fullscreen start, Esc keeps running, savetest
  (`--exe dist/.../bin/GodOfWar.exe --elf dist/.../data/SCUS_973.99 --silent`) writes 129 pages, card directory intact.

## Project tools (`tools/`)
| Tool | Purpose |
|---|---|
| iso_extract.py | List/extract ISO (both layers); `--layout` writes .disc_layout/.disc_meta |
| find_functions.py | Build function map CSV (see elf-and-recompilation.md) |
| dis.py ADDR | Print R5900 disassembly of the generated function containing ADDR (from port/output comments) |
| irxdis.py IRX [--find STR] [--at HEX N] | Disassemble an IRX (R3000, base 0) / find string references |
| peek.py LOG | Decode heartbeat peek lines into hex + ASCII |
| run.sh / shot.ps1 | Run + screenshot |
| key.ps1 | Post a held key to a window without focus (WM_KEYDOWN/UP): `key.ps1 proc delay holdMs vk scan [repeat]` |
| pine.py | Read EE memory of a running PCSX2 (PINE) |
| irxfind.py | Locate IRX modules inside an IOP RAM dump (PCSX2 save state) |
| vramdiff.py | Page map of our GS VRAM vs PCSX2's GS.bin |
| ppm2png.py | Convert PS2X_FRAME_DUMP output to PNG |
| sync_runner.sh | Sync generated code + overrides into the runner |
| switch/ | Switch smoke-link helpers (see switch.md) |

Note: `dis.py` reads comments in port/output; a few delay-slot lines are missing there. For exact bytes use capstone
on the ELF (file offset = vaddr − 0xFF000). Capstone mis-decodes R5900 128-bit ops (lq/sq/MMI).
- Pitfall (2026-10-09): editing C++ through a Python heredoc in the Bash tool turned "\n" inside C string
  literals into real newlines (C2001). Use the Edit tool for lines containing escapes.
