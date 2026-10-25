#!/bin/bash
# ent.sh <reps> <entry hex8> <rec> <harness>...: warm native us/run of one entry (GOW_VU1NAT_REPS), harnesses alternated, core 5
reps=$1; e=$2; r=$3; shift 3
for k in 1 2; do for h in "$@"; do
  echo "$h $(GOW_VU1NAT_REPS=$reps taskset -c 5 /root/vu1h/$h natcheck /root/nrs/$r.vifrec 2>&1 | grep "entry $e" | grep -o 'native [0-9.]* us/run')"
done; done
