#!/bin/bash
# lines2.sh <bench> <elf> <func (exact demangled prefix)> [n]: top lines of func, one compact line
cd /root/gowr
python3 -I tools/switch_pcprof.py port/switch/logs/bench_$1/pcs_GSWorker.txt $2 --top 1 --lines "$3" 2>&1 | grep -A${4:-40} -- "--- " | tail -n +2 | awk '{sub("ps2_vu1_","",$2); printf "%s %s | ", $1, $2} END {print ""}'
