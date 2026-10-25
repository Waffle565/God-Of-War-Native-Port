#!/bin/bash
# private Switch build tree for sw-stall: /root/src-sws + /root/gow-switch-sws (copies of src-nv2 / gow-switch-nv2)
set -e
rm -rf /root/src-sws /root/gow-switch-sws
cp -a /root/src-nv2 /root/src-sws
cp -a /root/gow-switch-nv2 /root/gow-switch-sws
cd /root/src-sws/ps2xRuntime/src/lib
# the other agent's in-progress language edit stays out of this tree
[ -f ps2_switch_runtime.cpp.pre_lang ] && cp -a ps2_switch_runtime.cpp.pre_lang ps2_switch_runtime.cpp
cd /root/gow-switch-sws
grep -rlI --exclude-dir=CMakeFiles/CMakeTmp -e /root/src-nv2 -e /root/gow-switch-nv2 . 2>/dev/null | grep -v '\.o$' | while read f; do
  sed -i 's#/root/src-nv2#/root/src-sws#g; s#/root/gow-switch-nv2#/root/gow-switch-sws#g' "$f"
done
grep -c src-sws CMakeCache.txt
# restore the shared tree's GS files (mine were copied in earlier)
for f in $(find /root/src-nv2 -name '*.pre_sws'); do cp -a "$f" "${f%.pre_sws}"; rm "$f"; echo "restored ${f%.pre_sws}"; done
