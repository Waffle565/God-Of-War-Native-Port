#!/bin/bash
# nc8.sh <harness> (WSL root, /root/vu1h harness from build.sh; nat-vu1l): natcheck on the 8 recordings, one summary line each
cd /root/vu1h
for r in pcfight pcmenu pcwalk pcathn02 pcdesert pccliffs pcolymp pccliff2; do
  o=$(./$1 natcheck /root/nrs/$r.vifrec 2>&1)
  c=$(echo "$o" | grep "covered entries" | sed 's/.*covered entries: //')
  d=$(echo "$o" | grep "native vs oracle" | sed 's/.*oracle: //')
  n=$(echo "$o" | grep "not restored" | sed 's/.*restored: //')
  p=$(echo "$o" | grep "second-program runs where" | sed 's/.*reference: //')
  s=$(echo "$o" | grep -o "([0-9]* with the 16 KB snapshot)" | grep -o "[0-9]*" | awk '{t+=$1} END {print t+0}')
  echo "$r | $c | $d | unrestored $n | part $p | snaps $s"
done
