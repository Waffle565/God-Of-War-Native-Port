#!/bin/bash
# Native VU1 rendering harness (docs/native-render-study.md): the runtime's VU1 sources (with ps2_vu1_nat.inl) + one
# generated vu1n_generado set + tools/vu1_nat/main.cpp, x86-64 Linux (WSL, root). Same layout as tools/vu1_harness.
# usage: tools/vu1_nat/build.sh <name> <ext worktree> <vu1n_generado dir>    -> /root/vu1h/<name>
#   then: /root/vu1h/<name> check <vifrec> [exact|fast] [dump <file>] | liveout <vifrec> [n] | torec <vifrec> <out.rec>
#   REBUILD=1 recompiles everything, OPT=<level> replaces -O2.
set -e
name=$1; wt=$2; gen=$3
H=/root/vu1h
mkdir -p $H
R=$H/rt_$name; G=$H/gen_$name
rm -f $R $G; ln -s "$wt/ps2xRuntime" $R; ln -s "$gen" $G
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
O=$H/obj_$name
[ -n "$REBUILD" ] && rm -rf $O
mkdir -p $O
CXX="g++ -pthread -std=c++20 ${OPT:--O2} -DNDEBUG -fsigned-char -ffp-contract=off -ffunction-sections -fdata-sections -msse4.2 -I$R/include -I$R/include/runtime -I$R/src/lib"
objs=()
for f in $R/src/lib/vu/ps2_vu1_core.cpp $R/src/lib/vu/ps2_vu1_lower.cpp $R/src/lib/vu/ps2_vu1_upper.cpp "$HERE/main.cpp" "$HERE/../vu1_harness/stubs.cpp" $G/vu1n_part*.cpp; do
    o=$O/$(basename "$f" .cpp).o
    # the core includes the .inl files: rebuild it when any of them is newer
    newer=$(find $R/src/lib/vu -name '*.inl' -newer $o 2>/dev/null | head -1)
    if [ ! -f $o ] || [ "$f" -nt $o ] || { [ "$(basename $f)" = ps2_vu1_core.cpp ] && [ -n "$newer" ]; }; then
        $CXX -c "$f" -o $o &
    fi
    objs+=($o)
done
wait
g++ -static -pthread -Wl,--gc-sections -o $H/$name "${objs[@]}"
ls -la $H/$name
