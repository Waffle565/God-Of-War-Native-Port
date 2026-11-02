#!/bin/bash
# filler crash hunt: filler.sh <tag>  (one new-game ATHN03D boot on update_crash2d)
cd "/c/Users/lovas/Claude/God Of War"; n=$1; O=port/switch/sweep/bossx3/filler.out
python -I tools/switch_sweep.py --levels --only ATHN03D --package update_crash2d --env "GOW_POOL_CHECK=1,GOW_CRASH_DUMP=sdmc:/switch/gow/crash2" --out port/switch/sweep/crash2f_$n > port/switch/sweep/bossx3/filler_$n.log 2>&1
d=$(ls -d port/switch/sweep/crash2f_$n/*/ | tail -1); log=$d/ATHN03D/log.txt
fault=$(grep -c 'gow-pool\] FAULT' $log 2>/dev/null); jalr=$(grep -c 'missing-target' $log 2>/dev/null)
if [ "${fault:-0}" != 0 ] || [ "${jalr:-0}" != 0 ]; then
  mkdir -p port/switch/logs/crash2/f_$n
  for f in $(python -I tools/switch_ftp.py ls switch/gow | awk '{print $NF}' | grep '^crash2'); do
    python -I tools/switch_ftp.py get switch/gow/$f port/switch/logs/crash2/f_$n/$f && python -I tools/switch_ftp.py rm switch/gow/$f
  done
  cp $log port/switch/logs/crash2/f_$n/
fi
echo "[crash2] f_$n: verdict=$(grep -oE '\| ATHN03D \| [A-Z]+' $d/report.md | awk '{print $4}') fault=$fault jalr0=$jalr dir=$d" >> $O
