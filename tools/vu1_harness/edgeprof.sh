#!/bin/bash
# edgeprof.sh <x86 or aarch64 harness> <out file> [recordings...]: edge profile for vu1n_gen.py --profile (the traces' likely
# successors, docs/vu1-native-flags.md "Traces"): dispatches from block pc a to block pc b over one warm replay pass of
# each recording (default ship.rec seq.rec game.rec in /root/vu1h), run without direct chaining and traces so that every
# edge outside the generated loops is a dispatch; summed into "e <from> <to> <count>" lines (hex pcs). The output holds
# only pcs and counts (port/vu1n_profile.txt); the recordings themselves hold game data and stay out of git.
cd /root/vu1h
b=$1; out=$2; shift 2
recs=("$@"); [ ${#recs[@]} -eq 0 ] && recs=(ship.rec seq.rec game.rec)
tmp=$(mktemp -d)
for r in "${recs[@]}"; do
  GOW_VU1N_DIRECT=0 GOW_VU1N_TRACES=0 GOW_VU1N_BLOCKPROF=$tmp/$(basename $r).txt ./$b $r 2 > /dev/null 2>&1
done
cat $tmp/*.txt | awk '$1=="e" {c[$2" "$3]+=$4} END {for (k in c) print "e", k, c[k]}' | sort > "$out"
rm -rf $tmp
wc -l "$out"
