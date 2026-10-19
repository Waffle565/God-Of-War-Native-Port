#!/usr/bin/env bash
# Builds the God of War native port for Nintendo Switch homebrew (ps2EntryRunner.nro), in WSL with devkitPro.
#
# Usage: ./switch/build.sh [target...]        (default: the NRO)
# Environment:
#   DEVKITPRO      devkitPro root (default /opt/devkitpro)
#   BUILD_DIR      build folder (default ~/gow-switch-build: the WSL file system, much faster than /mnt/c)
#   VU1_PREGEN     host build folder with vu1_generado/ and vu1n_generado/ (default: ../PS2Recomp-kl/out/build-kl/ps2xRuntime)
#   BOOT_ELF       game ELF on the SD card (default sdmc:/switch/gow/disc-usa/SCUS_973.99)
#   CLEAN=1        wipe the build folder first
# Prerequisites: dkp-pacman -S switch-dev switch-sdl2 switch-mesa switch-glad switch-zlib (docs/switch.md).
# The generated game code (src/runner, ps2xIOP/src/native/iopn_*.cpp) must be present as for the PC build.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BUILD_DIR="${BUILD_DIR:-$HOME/gow-switch-build}"
VU1_PREGEN="${VU1_PREGEN:-$REPO_ROOT/../PS2Recomp-kl/out/build-kl/ps2xRuntime}"
BOOT_ELF="${BOOT_ELF:-sdmc:/switch/gow/disc-usa/SCUS_973.99}"

[ -f "$DEVKITPRO/cmake/Switch.cmake" ] || { echo "error: missing $DEVKITPRO/cmake/Switch.cmake (switch-dev)" >&2; exit 1; }
[ -d "$VU1_PREGEN/vu1_generado" ] || echo "warning: no pregenerated VU1 code in $VU1_PREGEN (VU1 runs on the interpreter)" >&2
# The native blocks must come from vu1n_gen.py --locals (registers in locals, in-place self-loops; ~26 % less VU1 work on
# the A57, docs/switch.md "VU1 on the A57"). A PC build folder configured before that flag holds the older form, which
# still runs but slower: the deko3d console builds of 2026-10-19 morning used build-kl's from 2026-10-13 that way.
if [ -d "$VU1_PREGEN/vu1n_generado" ] && ! grep -q "chainTo" "$VU1_PREGEN"/vu1n_generado/vu1n_part*.cpp; then
    if [ "${ALLOW_OLD_VU1N:-0}" != "1" ]; then
        echo "error: $VU1_PREGEN/vu1n_generado was not generated with --locals (regenerate: python -I tools/vu1/vu1n_gen.py <dir>/vu1n_generado port/vu1_micro --heads port/vu1n_heads.txt --parts 6 --locals; ALLOW_OLD_VU1N=1 builds anyway)" >&2
        exit 1
    fi
fi

export DEVKITPRO DEVKITA64="$DEVKITPRO/devkitA64"
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"

[ "${CLEAN:-0}" = "1" ] && rm -rf "$BUILD_DIR"

cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
    -DPS2X_ENABLE_SCCACHE=OFF -DPS2X_ENABLE_DEBUG_UI=OFF \
    -DPS2X_ENABLE_RUNTIME_LOGS=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=OFF \
    -DPS2X_ENABLE_FFMPEG=OFF \
    -DPS2X_VU1_PREGENERATED_DIR="$VU1_PREGEN" \
    -DPS2X_DEFAULT_BOOT_ELF="$BOOT_ELF"

TARGET_ARGS=()
for t in "$@"; do TARGET_ARGS+=(--target "$t"); done
cmake --build "$BUILD_DIR" -j"$(nproc)" "${TARGET_ARGS[@]}"

NRO="$(find "$BUILD_DIR" -name '*.nro' -print -quit)"
[ -n "$NRO" ] && echo "==> $NRO"
