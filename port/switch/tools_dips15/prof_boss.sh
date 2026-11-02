#!/bin/bash
# prof_boss.sh <package> <tag> <item...>: boss items with PC sampling of GameThread + GSWorker, pcs pulled per item
cd "/c/Users/lovas/Claude/God Of War"
PKG=$1; TAG=$2; shift 2
first=1
for it in "$@"; do
  D=port/switch/logs/prof_${TAG}_$it; mkdir -p $D
  for t in GameThread GSWorker; do python -I tools/switch_ftp.py rm switch/gow/pcs_$t.txt >/dev/null 2>&1; done
  P="--package $PKG"
  python -I tools/switch_sweep.py --boss --only $it $P --out port/switch/sweep/prof_$TAG \
    --env "GOW_PC_SAMPLE=1000,GOW_PC_SAMPLE_THREADS=GameThread,GSWorker" > $D/sweep.out 2>&1
  for t in GameThread GSWorker; do python -I tools/switch_ftp.py get switch/gow/pcs_$t.txt $D/pcs_$t.txt; python -I tools/switch_ftp.py rm switch/gow/pcs_$t.txt; done
  echo "done $it"
done
