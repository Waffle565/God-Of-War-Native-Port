#!/bin/bash
# prof.sh <package> <tag> <extra env, comma-separated> <item...>: boss items with PC sampling of GameThread + GSWorker
# (+ the extra env, e.g. GOW_PC_SAMPLE_LR), pcs pulled per item to port/switch/logs/prof_<tag>_<item>; default restored
cd "/c/Users/lovas/Claude/God Of War"
PKG=$1; TAG=$2; EXTRA=$3; shift 3
for it in "$@"; do
  D=port/switch/logs/prof_${TAG}_$it; mkdir -p $D
  for t in GameThread GSWorker; do python -I tools/switch_ftp.py rm switch/gow/pcs_$t.txt >/dev/null 2>&1; done
  python -I tools/switch_sweep.py --boss --only $it --package $PKG --out port/switch/sweep/prof_$TAG \
    --env "GOW_PC_SAMPLE=1000,GOW_PC_SAMPLE_THREADS=GameThread,GSWorker,$EXTRA" > $D/sweep.out 2>&1
  for t in GameThread GSWorker; do python -I tools/switch_ftp.py get switch/gow/pcs_$t.txt $D/pcs_$t.txt; python -I tools/switch_ftp.py rm switch/gow/pcs_$t.txt; done
  echo "done $it"
done
TRANSFER=ftp bash tools/switch_default.sh
echo PROF_DONE
