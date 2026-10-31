# macOS (Apple Silicon)

Status (2026-10-31): the USA game (SCUS-97399) builds natively for arm64 with native VU1 and IOP code and a Metal
GS renderer (`src/lib/gs/metal/`, the default on macOS; `PS2X_GS_NULL=1` runs headless). Title, menu and gameplay
render with geometry and textures. The GL renderer needs OpenGL 4.3 and is not used on macOS (it stops at 4.1). Tracking: `odd/tasks/macos-port.md`.

## Layout
- Main checkout, branch `macos-port`. Runtime worktree `ext/PS2Recomp-kl`, branch `macos-runtime` (from `runtime`).
- `disc-usa/`: the extracted disc (`python3 -I tools/iso_extract.py <iso> disc-usa --layout`). The ISO is not needed
  after extraction.
- `port/usa/`: recompiler config for USA, taken from KIexster/god-of-war-recomp (`config/recomp.template.toml` with
  `@ELF@` = `disc-usa/SCUS_973.99`, `@MAP@` = `port/usa/funcmap.csv`, `@OUT@` = `port/output-usa/`, plus their
  `config/funcmap.csv`). Waffle565 does not publish this file.
- Git-ignored data: `port/output-usa/` (generated C++), `port/usa-vu1/` (VU1 micro-memory captures),
  `port/usa-iop/` (IRX module dumps).

## Build
Toolchain: Xcode (Apple clang), Homebrew CMake, Python 3. Build dir `ext/PS2Recomp-kl/out/build-mac`
(Unix Makefiles, Release).

1. Recompiler: `cmake -S . -B out/build-mac -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_ANALYZER=OFF`,
   `cmake --build out/build-mac --target ps2_recomp -j8`.
2. Recompile: from the project root,
   `ext/PS2Recomp-kl/out/build-mac/ps2xRecomp/ps2_recomp port/usa/recomp.toml` (6414 functions, 0 errors,
   ~2200 control-flow warnings are expected).
3. Runner inputs: copy `port/output-usa/*.cpp` and `port/overrides-usa/gow_overrides.cpp` into
   `ps2xRuntime/src/runner/`, the generated headers into `ps2xRuntime/include/`, and KIexster's
   `src/gow_pad2_packet.h` into the runner folder.
4. Configure with `-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=OFF` (otherwise the per-function trace
   writes hundreds of MB per minute) and `-DPS2X_VU1_MICROCODE_DIR=<abs>/port/usa-vu1` once captures exist, then
   `cmake --build out/build-mac --target ps2EntryRunner -j8` (~5 min full).

## Native VU1 and IOP
Both are generated from a real run of the game:
- Capture: `GOW_VU1_NATIVE_OFF=1 GOW_VU1_CAPTURA=port/usa-vu1 PS2X_IOP_DUMP_MODULES=port/usa-iop GOW_PAD_TEST=1
  GOW_SKIP_FMV=1` with a `GOW_PAD_GUION` script that reaches gameplay. `GOW_VU1_NATIVE_OFF=1` is required: captures are
  only written on the interpreter path.
- IOP: `python3 -I tools/iop/iopn_gen.py port/usa-iop ext/PS2Recomp-kl/ps2xIOP/src/native`.
- VU1: the build runs `tools/vu1/vu1n_gen.py` over the captures (1916 micro-memories, 1512 blocks). Without
  `PS2X_VU1_GENERATOR_SOURCE` (`generar_vu1.cpp` is not public) only this lean backend is built.

## App
`tools/macos/make_app.sh` assembles `dist/God of War.app` from this machine's runner build (details:
`tools/macos/README.md`). It is for local use only: the runner contains recompiled game code and must not be
redistributed. The launcher keeps everything in `~/Library/Application Support/God of War/` (`data/` = links to the
disc folder plus `IOP_MOD/`, `saves/GodOfWar.sav` with a `.bak` copy made before every start, `settings.ini`) and logs
to `~/Library/Logs/God of War/game.log`. `open "dist/God of War.app" --args --play` skips the window.

## Run
The game loads its modules from `IOP_MOD/<name>` next to the ELF, so run from a folder that holds the disc files
plus `IOP_MOD/` (symlinks to `disc-usa/` are enough; `out/build-mac/run/` is such a folder):
`cd out/build-mac/run && ../ps2xRuntime/ps2EntryRunner $PWD/SCUS_973.99`.
Speed diagnostics: `GOW_PERF_DIAG=1 GOW_DELTA_DIAG=1 GOW_PERF_FRAME_PC=0x001837B8` print `[gow-perf]` and
`[gow-speed]`. `PS2X_GS_NULL=0` tries the GL renderer (fails on macOS until Metal).

## Performance (Apple M4, ship start, new-game script)
| scene | before | after |
|---|---|---|
| menu | 45.7 flips/s | 60.3 |
| ship idle | 27.9 | 60.0 |
| ship walk/fight | 20.8 (min 7.3) | 60.2 (min 59.4) |

What did it: field-wise draw-state batching (740k -> ~9k draws/s), zero-copy presentation into a `CAMetalLayer` with
vsync and 3 frames in flight, palette textures converted to RGBA and cached, shaders specialised with function
constants (compiled in the background, generic fallback), feedback copies limited to the sampled rectangle.

Presentation: the window is 16:9 letterboxed (1809x1018 inside a 1920x1018 content area on a 1080p display); on a
75 Hz display 60 fps content judders, set the display to 60 Hz. The debug panel (F1) starts closed on macOS
(`GOW_DEBUG_PANEL=1` opens it); while it is open the layer hides and frames go through RAM.

Switches: `PS2X_GS_DIRECT_PRESENT=0` (RAM presentation, as for captures), `GOW_GS_MTL_DEPAL=0` (no RGBA palette
cache), `GOW_GS_MTL_SPECIALIZE=0` (generic shader only), `GOW_GS_MTL_LABELS=1` (render-pass labels for Xcode's GPU
capture), `GOW_GS_HW_STATS=1` (adds `gpu_ms` and `feedbackMpx`), `GOW_HOST_SHOT` (writes `host_shot.ppm`).

## Graphics options
Each option has its own switch (the launcher's Graphics section sets them). Measured on an M4 at 1080p, ship
walk/fight:

| option | switch | result |
|---|---|---|
| internal resolution | `PS2X_GS_HW_SCALE=1..4`, per axis `PS2X_GS_HW_SCALE_X/_Y` (rounded up to eighths) | 1-3: 60 fps; 4: ~70% GPU, dips to 23-42 fps |
| native resolution | `GOW_GS_MTL_NATIVE=1\|WxH` | scales chosen from the output so no axis is upscaled: 3.75 x 2.5 = 1920x1120 for 1080p; 60.2 fps (min 59.4) fullscreen with FXAA, aniso 16 and the HD pack; sized at startup from the larger of window and screen |
| anti-aliasing | `GOW_GS_MTL_AA=fxaa\|off` | 60 fps, HUD stays readable |
| anisotropic filtering (3D textures only) | `GOW_GS_MTL_ANISO=0\|2\|4\|8\|16` | 60 fps |
| MetalFX spatial upscaling | `GOW_GS_MTL_METALFX=spatial\|off` | 60 fps; for Retina/4K output, falls back to the plain scaler when the frame is larger than the output |
| sharpening (contrast-adaptive) | `GOW_SHARPEN=off\|0..1` (default 0.5) | applied once, last; skipped after MetalFX |
| wider field of view (gameplay only) | `GOW_PANORAMICO=ampliado` | 60 fps; menus keep the original framing |

Recommended on an M4 with a 1080p screen (the launcher's defaults): fullscreen + native resolution (scale 3 in a
window), FXAA, anisotropic 16x, sharpen 0.5,
MetalFX off (use it to take scale 2 to a Retina or 4K output). Chain at present: composed frame -> FXAA (at render
resolution) -> MetalFX or the window scaler -> sharpen -> letterboxed drawable. Captures and the RAM path
(`PS2X_GS_DIRECT_PRESENT=0`) are taken before the chain unless `GOW_GS_MTL_CAPTURE_POST=1`.

## HD texture packs (PCSX2 format)
The Metal renderer computes PCSX2's texture replacement names (`<texhash>-<cluthash>-<bits>`, XXH3 over the texture's
GS memory blocks or expanded texels, palette hash, region, TEX0/TEXA bits; `gs_metal_texname.cpp`), so packs made for
PCSX2 load unchanged. With Huekage's "PS2 HD Textures Pack" (3,860 DDS) 98 of 107 textures seen from new game to the
ship are replaced, at 60 fps (~57 MB of GPU textures). Movie frames are new textures every frame and never match.
Packs are downloaded by the player and never shipped with the port.

| switch | effect |
|---|---|
| `GOW_TEXTURES_DIR` | the pack's folder of replacement files, scanned recursively (point it at the files for SCUS-97399, not at a pack root whose `dumps/` hold originals under the same names); default `~/Library/Application Support/God of War/textures/SCUS-97399/replacements` |
| `GOW_GS_MTL_TEXREPL=0` | replacement off |
| `GOW_GS_MTL_TEXREPL_MB` | GPU budget in MB (default 2048) |
| `GOW_GS_MTL_TEXDUMP=<dir>` | writes every new texture as a PNG under its PCSX2 name (for making packs) |
| `GOW_GS_MTL_TEXREPL_LOG=1` | logs every texture seen/matched |

The launcher's Textures row turns a pack on and chooses its folder. DDS (BC1/BC3/BC7, RGBA8) loads straight into GPU
formats; PNG through ImageIO; loading is asynchronous (the original is drawn until the replacement is ready). Not
reproduced: PCSX2's region guess from recent uploads (two full-screen textures of the pack).

## macOS-specific build settings
In the top-level CMakeLists (`APPLE AND aarch64` only): `-fsigned-char` (x86 `char`), `-ffp-contract=off` (Apple clang
fuses `a*b+c`; x86 and the PS2 round twice), sse2neon precise min/max, division and dot product. Not
`SSE2NEON_PRECISE_SQRT`: on AArch64 it swaps the exact `vsqrtq_f32` for an estimate plus Newton steps (1 ulp off for
~45% of inputs). The EE FPU's round-to-nearest div.s/sqrt.s are computed in double and rounded in software on arm64
(`ps2_runtime_macros.h`), because clang does not order float code against FPCR writes.

## Known issues
- Fixed (macOS only, shared race): the intro movie's VBlank handler writes DISPFB2 twice per run (0x26E798 sets the
  double-buffer entry 104/208, 0x26CF20 overwrites it with 104), and the host thread could latch the display between
  the two stores, showing the stale in-engine picture for single frames. While an interrupt or GS callback handler runs,
  presentation now uses a snapshot of the display registers taken at its start (`GS::holdDisplay`, at most two fields).
  Windows and Linux have the same race in shared code.
- Not checked yet: a few short bursts of frames in the in-engine cliff shot after the intro shift red towards
  blue/green at the same point in every run (possibly scripted, e.g. lightning); compare with PCSX2.
- The USA config's widescreen patches read a camera aperture factor from 0x000FFFF0; the override must write it
  (9/16) every frame, else the projection is +-FLT_MAX and VU1 culls every vertex (fixed in port/overrides-usa).
- VU1 A/B check (`GOW_VU1N_VERIFY=1`): 0.038% of runs differ (383 in 998k), the same classes docs/vu1-native-flags.md
  lists for x86 (1-unit rounding, ffff/0000 words) plus end-MAC 12/0.
- Test suite on macOS (`-DPS2X_BUILD_TEST=ON -DPS2X_TEST_SKIP_STALE=ON`): 452 tests, 62 fail, none in VU or float
  math: 53 PS2GS + 1 PS2VU1 read CPU-side GS VRAM that this runtime no longer rasterises, the rest are stale upstream
  tests.
- Short dips at the start of the title screen and rare EE-side dips in gameplay (ee/vu/iop time spikes) also happen
  with `PS2X_GS_NULL=1`, so they are not the renderer.
- The GL renderer has the same uninitialised-padding problem in its draw-state comparison that made every primitive
  its own batch on Metal (fixed here with a field-wise `SameDrawState`); worth reporting upstream.
