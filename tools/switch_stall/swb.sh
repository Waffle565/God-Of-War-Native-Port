#!/bin/bash
# swb.sh <tag>: copy the swstall worktree's changed GS / runtime files into /root/src-sws (no backups:
# the tree is private; tools/switch_stall/sync.sh re-syncs it with the default sources /root/src-nv2), build /root/gow-switch-sws, package port/switch/nsp/out/update_<tag>
# usage (WSL root): [EXTRA=ps2xRuntime/src/lib/gs/gpu/gs_dk_shaders_gen.cpp] bash swb.sh <tag> (WSLENV=EXTRA from Windows)
set -e
export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64
W=/root/gowr/ext/PS2Recomp-kl-swstall
cd "$W"
for f in ps2xRuntime/src/lib/gs/gpu/gs_dk.cpp ps2xRuntime/src/lib/gs/gs_threaded_backend.cpp ps2xRuntime/src/lib/gs/gpu/gs_dk_shaders.h ps2xRuntime/src/lib/gs/gs_frontend.cpp $EXTRA; do
  d=/root/src-sws/$f
  if ! cmp -s "$f" "$d"; then

    echo "copy $f"; cp "$f" "$d"
  fi
done
cd /root/gow-switch-sws
taskset -c 0-15 ninja -j16 > /root/gow-switch-$1.log 2>&1 || { grep -E "error|Error" /root/gow-switch-$1.log | cut -c1-300 | head -20; exit 1; }
tail -1 /root/gow-switch-$1.log
cp ps2xRuntime/ps2EntryRunner.elf /root/elf_$1.elf
bash /root/gowr/port/switch/nsp/make_update_to.sh /root/elf_$1.elf /root/gowr/port/switch/nsp/out/update_$1 | tail -1
