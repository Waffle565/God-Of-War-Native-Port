#!/bin/bash
# b39.sh <tag> <bosstag>: Switch package from src-mlang, then the boss package (misc.inl copied to src-boss)
bash '/mnt/c/Users/lovas/Claude/God Of War/port/switch/tools_dips14/build.sh' $1
cp /root/src-mlang/ps2xRuntime/src/lib/vu/ps2_vu1_nat_misc.inl /root/src-boss/ps2xRuntime/src/lib/vu/ps2_vu1_nat_misc.inl
bash '/mnt/c/Users/lovas/Claude/God Of War/port/switch/tools_dips15/bossb.sh' $2
ls -la "/mnt/c/Users/lovas/Claude/God Of War/port/switch/nsp/out" | grep -E "$1|$2"
echo B39_DONE
