# macOS (Apple Silicon)

Status (2026-10-30): the USA game (SCUS-97399) builds natively for arm64 and runs headless at full speed
(speed 1.00, 60 guest flips/s on an M4) with native VU1 and native IOP code. There is no picture yet: the GS
renderer needs OpenGL 4.3 and macOS stops at 4.1, so the GS runs as a null backend until the Metal renderer
exists. Tracking: `odd/tasks/macos-port.md`.

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

## Run
The game loads its modules from `IOP_MOD/<name>` next to the ELF, so run from a folder that holds the disc files
plus `IOP_MOD/` (symlinks to `disc-usa/` are enough; `out/build-mac/run/` is such a folder):
`cd out/build-mac/run && ../ps2xRuntime/ps2EntryRunner $PWD/SCUS_973.99`.
Speed diagnostics: `GOW_PERF_DIAG=1 GOW_DELTA_DIAG=1 GOW_PERF_FRAME_PC=0x001837B8` print `[gow-perf]` and
`[gow-speed]`. `PS2X_GS_NULL=0` tries the GL renderer (fails on macOS until Metal).

## macOS-specific build settings
In the top-level CMakeLists (`APPLE AND aarch64` only): `-fsigned-char` (x86 `char`), `-ffp-contract=off` (Apple clang
fuses `a*b+c`; x86 and the PS2 round twice), sse2neon precise min/max, division, square root and dot product.

## Known issues
- VU1 A/B check (`GOW_VU1N_VERIFY=1`): ~4% of runs differ between the native blocks and the interpreter on arm64
  (0 on x86): values near FLT_MAX and memory -0/+0, deterministic. Suspects: NEON min/max ordering of signed zeros
  versus SSE `minps`/`maxps`, and `fmacLaneT` / execUpper MADD near overflow. Not yet checked against a PCSX2 or
  Windows reference.
