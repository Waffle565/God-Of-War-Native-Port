#!/bin/bash
# switch_build.sh <tag> [dry] (WSL root; nat-vu1l): copy the worktree's VU sources into /root/src-nv2, build /root/gow-switch-nv2, ELF -> /root/elf_<tag>.elf, package update_<tag>
set -e
cd /root/natvu1w/ps2xRuntime
for f in src/lib/vu/*.inl src/lib/vu/*.cpp include/runtime/ps2_vu1*.h; do
  if ! cmp -s "$f" "/root/src-nv2/ps2xRuntime/$f"; then echo "copy $f"; [ -z "$2" ] && cp "$f" "/root/src-nv2/ps2xRuntime/$f"; fi
done
[ -n "$2" ] && exit 0
cd /root/gow-switch-nv2
taskset -c 0-7 ninja -j8 > /root/gow-switch-$1.log 2>&1 || { tail -30 /root/gow-switch-$1.log; exit 1; }
tail -2 /root/gow-switch-$1.log
cp ps2xRuntime/ps2EntryRunner.elf /root/elf_$1.elf
bash "/root/gowr/port/switch/nsp/make_update_to.sh" /root/elf_$1.elf "/root/gowr/port/switch/nsp/out/update_$1" | tail -2
