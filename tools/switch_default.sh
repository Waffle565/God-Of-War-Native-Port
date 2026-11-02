#!/usr/bin/env bash
# Restore the console default (docs/status.md): the default package's exefs + tools/switch/default_env.txt as env.txt.
# Run after every bench / cycle (they leave their own exefs and delete env.txt). The game must not be running.
# usage: tools/switch_default.sh   (DEFAULT_PKG=update_<x> overrides the package)
set -u
cd "$(dirname "$0")/.."
. tools/switch_lib.sh
PKG=${DEFAULT_PKG:-update_dips40}
sw_transfer
sw_push_update port/switch/nsp/out/$PKG || { echo "exefs push failed"; exit 1; }
mkdir -p port/switch/logs/default_env && cp tools/switch/default_env.txt port/switch/logs/default_env/env.txt
F put port/switch/logs/default_env/env.txt switch/gow || { echo "env put failed"; exit 1; }
echo "console default: $PKG + env.txt (FAILGT=1)"
