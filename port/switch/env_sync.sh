#!/usr/bin/env bash
# Run in WSL (root). Copies an ext worktree (tracked files, git-ignored generated code, deko3d shader table, native IOP
# files, overrides: everything the Switch build reads) into the WSL file system and the VU1 pregenerated blocks next to
# it. Compiling from /mnt/c (9p) is slow and used to drop the WSL session (docs/switch-deko3d.md).
#
# usage: env_sync.sh <worktree on /mnt/c> <dest name> [vu1pregen dir on /mnt/c]
#   e.g. env_sync.sh "/mnt/c/Users/lovas/Claude/God Of War/ext/PS2Recomp-kl-vu1gpu" vgpu \
#        "/mnt/c/Users/lovas/Claude/God Of War/ext/PS2Recomp-kl-vu1disp/out/vu1pregen"
# Result: /root/src-<name>/ (worktree copy, no .git, no out/) and /root/vu1pregen-<name>/ (vu1_generado + vu1n_generado).
set -euo pipefail
WT="$1"; NAME="$2"; PRE="${3:-}"
DST=/root/src-$NAME
mkdir -p "$DST"
tar -C "$WT" --exclude=.git --exclude=./out --exclude=./build --exclude='.vs' -cf - . | tar -C "$DST" -xf -
if [ -n "$PRE" ]; then
    rm -rf /root/vu1pregen-$NAME
    mkdir -p /root/vu1pregen-$NAME
    cp -a "$PRE"/. /root/vu1pregen-$NAME/
fi
echo "synced $WT -> $DST"
