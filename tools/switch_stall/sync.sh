#!/bin/bash
# /root/src-sws := /root/src-nv2 (the console default's sources) except sw-stall's own GS files
cd /root/src-nv2
for f in $(find ps2xRuntime ps2xIOP -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.inl' -o -name '*.c' -o -name 'CMakeLists.txt' \)); do
  case "$f" in ps2xRuntime/src/lib/gs/gpu/gs_dk.cpp|ps2xRuntime/src/lib/gs/gs_threaded_backend.cpp|ps2xRuntime/src/lib/gs/gpu/gs_dk_shaders_gen.cpp) continue;; esac
  cmp -s "$f" "/root/src-sws/$f" || { echo "sync $f"; cp -a "$f" "/root/src-sws/$f"; }
done
