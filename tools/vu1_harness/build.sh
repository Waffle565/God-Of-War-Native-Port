#!/bin/bash
# VU1 replay harness (docs/switch.md "VU1 on the A57"): the runtime's VU1 sources + one generated vu1n_generado set,
# built as a standalone program that replays GOW_VU1N_RECORD files (main.cpp). Runs in WSL Ubuntu as root:
#   apt-get install g++ g++-aarch64-linux-gnu qemu-user llvm bc
#
# usage: tools/vu1_harness/build.sh <arch> <name> <ext worktree> <vu1n_generado dir>
#   arch: arm (aarch64 Linux, Switch compiler flags; runs under qemu-aarch64 through binfmt) or x86 (x86-64 Linux, SSE4.2)
#   output: /root/vu1h/<name>; run it as  /root/vu1h/<name> <records> [reps]  (GOW_VU1N_REPLAY_DUMP=<file> dumps results)
#   REBUILD=1 recompiles everything (the VU1 .inl files are not tracked as dependencies). EXTRA=<flags> adds compiler
#   flags (e.g. -DVU1N_FAST_STATS: fast-variant entry checks passed / failed, printed at exit).
# Then: icount.sh (dynamic aarch64 instruction profile), dumpdiff.py (compare two dumps record by record).
set -e
arch=$1; name=$2; wt=$3; gen=$4
H=/root/vu1h
mkdir -p $H
# paths without spaces for the compiler command lines
R=$H/rt_$name; G=$H/gen_$name
rm -f $R $G; ln -s "$wt/ps2xRuntime" $R; ln -s "$gen" $G
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
O=$H/obj_$name
[ -n "$REBUILD" ] && rm -rf $O
mkdir -p $O
COMMON="$EXTRA -pthread -std=c++20 -O2 -DNDEBUG -fsigned-char -ffp-contract=off -ffunction-sections -fdata-sections -I$R/include -I$R/include/runtime -I$R/src/lib"
if [ "$arch" = arm ]; then
    SSE2NEON=${SSE2NEON:-$(ls -d /root/gow-switch-build*/_deps/sse2neon-src 2>/dev/null | head -1)}
    [ -f "$SSE2NEON/sse2neon.h" ] || { echo "no sse2neon.h (set SSE2NEON=<dir>, e.g. a Switch build's _deps/sse2neon-src)" >&2; exit 1; }
    CXX="aarch64-linux-gnu-g++ $COMMON -march=armv8-a+fp+simd -mtune=cortex-a57 -DUSE_SSE2NEON -DSSE2NEON_PRECISE_MINMAX=1 -DSSE2NEON_PRECISE_DIV=1 -DSSE2NEON_PRECISE_SQRT=1 -DSSE2NEON_PRECISE_DP=1 -I$SSE2NEON"
    LD=aarch64-linux-gnu-g++
else
    CXX="g++ $COMMON -msse4.2"
    LD=g++
fi
objs=()
for f in $R/src/lib/vu/ps2_vu1_core.cpp $R/src/lib/vu/ps2_vu1_lower.cpp $R/src/lib/vu/ps2_vu1_upper.cpp "$HERE/main.cpp" "$HERE/stubs.cpp" $G/vu1n_part*.cpp; do
    o=$O/$(basename "$f" .cpp).o
    if [ ! -f $o ] || [ "$f" -nt $o ]; then
        $CXX -c "$f" -o $o &
    fi
    objs+=($o)
done
wait
$LD -static -pthread -Wl,--gc-sections -o $H/$name "${objs[@]}"
ls -la $H/$name
