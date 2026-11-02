#!/bin/bash
# mkpatch40.sh: port/switch/dips40.patch = dips40 against dips38 (windLoop, SoundMix boost 0, copier off by default)
cd /root/src-mlang
O='/mnt/c/Users/lovas/Claude/God Of War/port/switch/dips40.patch'
{ for f in ps2xRuntime/src/lib/vu/ps2_vu1_nat_misc.inl ps2xIOP/src/emulator/core/sound_engine.cpp ps2xRuntime/src/lib/ps2_memory.cpp; do
    diff -u --label a/$f --label b/$f /root/dips39-orig/$(basename $f) $f; done; } > "$O"
wc -l "$O"
