#!/bin/bash
cd "/c/Users/lovas/Claude/God Of War"
for z in ATHN01D PAND02B ATHN06C; do
  python -I tools/switch_ftp.py rm switch/gow/pcs_GameThread.txt >/dev/null 2>&1
  python -I tools/switch_sweep.py --levels --only $z --package update_dips37 --env "GOW_PC_SAMPLE=1000,GOW_PC_SAMPLE_THREADS=GameThread,GOW_PC_SAMPLE_SKIP=2,GOW_PC_SAMPLE_LR=3926100-392632c" --out port/switch/sweep/prof_d37ee_$z
  mkdir -p port/switch/logs/prof_d37ee_$z
  python -I tools/switch_ftp.py get switch/gow/pcs_GameThread.txt port/switch/logs/prof_d37ee_$z/pcs_GameThread.txt && python -I tools/switch_ftp.py rm switch/gow/pcs_GameThread.txt
done
TRANSFER=ftp bash tools/switch_default.sh | tail -1
echo D14_DONE
