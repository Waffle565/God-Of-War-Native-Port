#!/bin/bash
# mkpatch.sh: port/switch/dips39.patch = diff of dips39 against dips38 (originals in /root/dips39-orig)
cd /root/src-mlang
O='/mnt/c/Users/lovas/Claude/God Of War/port/switch/dips39.patch'
{ diff -u --label a/ps2xRuntime/src/lib/vu/ps2_vu1_nat_misc.inl --label b/ps2xRuntime/src/lib/vu/ps2_vu1_nat_misc.inl /root/dips39-orig/ps2_vu1_nat_misc.inl ps2xRuntime/src/lib/vu/ps2_vu1_nat_misc.inl
  diff -u --label a/ps2xIOP/src/emulator/core/sound_engine.cpp --label b/ps2xIOP/src/emulator/core/sound_engine.cpp /root/dips39-orig/sound_engine.cpp ps2xIOP/src/emulator/core/sound_engine.cpp; } > "$O"
wc -l "$O"
