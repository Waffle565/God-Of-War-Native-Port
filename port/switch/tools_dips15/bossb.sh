#!/bin/bash
# bossb.sh <tag>: copy the dips38 files from src-mlang to src-boss, build the boss package
for f in ps2xRuntime/src/lib/ps2_memory.cpp ps2xRuntime/include/runtime/ps2_perf.h ps2xRuntime/include/ThreadNaming.h ps2xIOP/src/emulator/core/sound_engine.cpp ps2xRuntime/include/ps2_runtime_macros.h; do cp /root/src-mlang/$f /root/src-boss/$f; done
while pgrep -x ninja >/dev/null || pgrep -f "^bash /root/dt/dips_sb.sh" >/dev/null; do sleep 20; done
bash "/mnt/c/Users/lovas/Claude/God Of War/port/switch/boss_sb.sh" $1 2>&1 | tail -3
