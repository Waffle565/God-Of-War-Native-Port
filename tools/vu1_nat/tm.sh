#!/bin/bash
# tm.sh <reps> <harness>...: native us summed over the 8 nrs recordings, sequential on core 3, harnesses alternated
reps=$1; shift
declare -A tot
for k in $(seq 1 $reps); do
  for h in "$@"; do
    t=0
    for r in pcfight pcmenu pcwalk pcathn02 pcdesert pccliffs pcolymp pccliff2; do
      u=$(taskset -c 3 /root/vu1h/$h natcheck /root/nrs/$r.vifrec 2>&1 | grep -o 'native [0-9]* us' | awk '{print $2}')
      t=$((t + u))
    done
    tot[$h]="${tot[$h]} $t"
  done
done
for h in "$@"; do echo "$h:${tot[$h]}"; done
