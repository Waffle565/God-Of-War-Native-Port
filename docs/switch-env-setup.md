# Switch build environment: recreate it in one go

The whole Switch cross-build lives in one WSL2 distro, `Ubuntu`, as root. On 2026-10-23 that distro vanished (its
`ext4.vhdx` under `%LOCALAPPDATA%\wsl\{guid}\` was gone and `wsl.exe` failed with ERROR_PATH_NOT_FOUND; no copy in the
Recycle Bin or on any drive, no export). It was rebuilt from this repo and the docs; this page is how.

**Back the distro up.** The VHDX is the only copy of the WSL-side state (`/root/src-*` worktree copies, build trees, the
harness, reference dumps). After a good build: `wsl --export Ubuntu D:\wsl-backup\ubuntu-<date>.tar` (outside the repo
and not on the WSL disk; `wsl --import Ubuntu <dir> <tar>` restores it). Everything else is regenerable (below).

## One go
    powershell -ExecutionPolicy Bypass -File port\switch\env_setup.ps1      # distro + everything below
(`env_setup.ps1` installs `Ubuntu-24.04` under the name `Ubuntu` and runs `port/switch/env_setup.sh` in it; the .sh is
idempotent and can be run alone with `wsl -d Ubuntu -u root -- bash <path>/port/switch/env_setup.sh`.)

What it installs:
- apt: build-essential, cmake 3.28 (the old distro had 4.2; the build configures fine with 3.28), ninja, python3, git,
  `qemu-user` + `gcc/g++-aarch64-linux-gnu` + `libc6-arm64-cross` (the aarch64 VU1 harness `vga0`), `llvm`, `bc`,
  `pacman-package-manager`, and **GCC 14** as the default `g++`/`gcc` (+ the aarch64 cross pair): the harness sources do not
  compile with Ubuntu 24.04's GCC 13 (`ps2_vu1_native.inl:690` constexpr sizeof of a reference parameter). `/etc/wsl.conf`: default user root. Git `safe.directory '*'`.
- devkitPro through `dkp-pacman` (`/opt/devkitpro/pacman.conf`, no signature checks as before): `switch-dev` (devkitA64
  GCC 16.1.0, libnx, deko3d 0.5.0, **uam** = `/opt/devkitpro/tools/bin/uam`, switch-tools: elf2nso/npdmtool/nacptool),
  `switch-sdl2`, `switch-glfw`, `switch-mesa` 20.1.0, `switch-glad`, `switch-ffmpeg`, `switch-zlib`.
- **pkg.devkitpro.org returns 403 to Linux curl/wget** (Cloudflare, TLS fingerprint; every user agent), but Windows'
  `curl.exe` with user agent `pacman/6.0.2` passes. pacman's `XferCommand` is `port/switch/dkp-fetch.sh`, which calls
  `/mnt/c/Windows/System32/curl.exe` through WSL interop. `RootDir` must be `/` (packages hold `opt/devkitpro/...`).
- hacBrewPack (NSP packer) built from `github.com/rlaphoenix/hacBrewPack` into `/root/nsp-tools/hacBrewPack`.
- CMake fetches sse2neon and raylib from GitHub at configure time (works from WSL).

## Build the Switch package
1. Sync a worktree into the WSL file system (building from `/mnt/c` is slow and drops the session):
   `bash port/switch/env_sync.sh "/mnt/c/.../ext/PS2Recomp-kl-<wt>" <name> "/mnt/c/.../ext/PS2Recomp-kl-<wt2>/out/vu1pregen"`
   -> `/root/src-<name>` and `/root/vu1pregen-<name>`. The worktree must already hold the git-ignored generated inputs
   (EE code `ps2xRuntime/src/runner/`, native IOP `ps2xIOP/src/native/iopn_*.cpp`, `gow_overrides` files, the deko3d
   shader table `ps2xRuntime/src/lib/gs/gpu/gs_dk_shaders_gen.cpp`); they live in the Windows worktrees, so they
   survive a lost distro. A new worktree copies them from an old one (docs/vu1-gpu.md).
2. Build (keep one `wsl.exe` session attached: the distro stops when none is):
   `wsl -d Ubuntu -u root -- bash -c 'cd /root/src-<name>; VU1_PREGEN=/root/vu1pregen-<name> BUILD_DIR=/root/gow-switch-<name> taskset -c 0-13 ./switch/build.sh'`
   (the WSL VM has 15 GB: all 24 cores can run out of memory in the big unity batches, hence `taskset`).
3. Code-only package: `ELF=/root/gow-switch-<name>/ps2xRuntime/ps2EntryRunner.elf DEST=<dir> bash port/switch/nsp/make_update.sh`
   -> `out/update/atmosphere/...` (rename to `out/update_<name>`; copy the `atmosphere` folder to the SD root).
   Full NSP: docs/switch.md "Installable NSP".
4. Shader table after a GLSL change: `python -I tools/dk_shaders.py ...` (docs/switch-deko3d.md; it calls `uam` in WSL).
5. VU1 harness (vg0 x86, vga0 aarch64/qemu): `SSE2NEON=/root/gow-switch-<name>/_deps/sse2neon-src bash tools/vu1_harness/build.sh
   x86|arm <vg0|vga0> /root/src-<name> /root/vu1pregen-<name>/vu1n_generado` (build.sh only globs `gow-switch-build*`, hence
   `SSE2NEON`), see docs/vu1-gpu.md "Commands".

## What was lost with the distro, and what is regenerable
Regenerable from the repo / Windows side: the toolchain (this page), `/root/src-*` (env_sync.sh), the VU1 pregenerated
blocks (copies of `ext/PS2Recomp-kl-vu1*/out/vu1pregen`, also kept on Windows), build trees (`switch/build.sh`), the
harness binaries `/root/vu1h/*`, the packer. Not regenerable as such: the old helper scripts `/root/dkbuild2.sh`,
`/root/core1build.sh` (they were thin wrappers around `switch/build.sh` with `VU1_PREGEN`/`BUILD_DIR`, replaced by the
command in step 2), `/root/vu1gpu/sync.sh` (replaced by `env_sync.sh`), the old build trees (rebuilt, ~40 min cold), and
the reference dumps `/root/vu1gpu/ref_vg0_ship.dump` (re-made with `cmp.sh`: the exact path's replay dump of the ship
recording; the `.rec` recordings themselves live under the Windows scratchpad / `port/` folders, check there).

## Rebuild log (2026-10-23)
- Fresh Ubuntu 24.04 (WSL 2.7.10, 15 GB RAM VM), env_setup run, then env_sync of `ext/PS2Recomp-kl-vu1gpu` (branch
  agent/sw-vu1gpu 6a8114e + its untracked generated files) with `ext/PS2Recomp-kl-vu1disp/out/vu1pregen`.
- `switch/build.sh` (350 steps, `taskset -c 0-13`, ~1 h cold): `ps2EntryRunner.nro` 94 MB, ELF 1.1 GB; `make_update.sh` ->
  `port/switch/nsp/out/update_vgpu1/atmosphere/contents/0100D0D0600A0000/exefs/{main 47.2 MB, main.npdm 992 B}` (the
  vsplit5 package was 47.2 MB, same npdm size). Not run on the console in this task.
- Harnesses `/root/vu1h/vg0` (x86) and `vga0` (aarch64 under qemu-user) rebuilt and start.
- Backup: `wsl --export Ubuntu D:wsl-backupBuntu-20261023.tar` (9.2 GB), taken after the build.
