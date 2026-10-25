#!/bin/bash
# prof.sh <bench> <elf> <func> [n]: top source lines of func in the bench's GSWorker (VU1) samples
cd /root/gowr
python3 -I tools/switch_pcprof.py port/switch/logs/bench_$1/pcs_GSWorker.txt $2 --top 1 --lines $3 > /tmp/prof_$3.txt 2>&1
grep -A ${4:-40} -- "--- $3" /tmp/prof_$3.txt
