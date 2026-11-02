#!/usr/bin/env bash
# check40.sh: dips40 default checks. Levels A/B vs dips37c (interleaved, R rounds), SEMO01, 4 title boots, boss runs
# boss18 (dips40 + test options) vs boss10 (dips37c + test options), interleaved; the default is restored at the end
cd "$(dirname "$0")/../../.."
R=${R:-3}
for i in $(seq 1 $R); do
  for p in dips37c dips40; do
    echo "=== lev $i $p $(date +%T)"
    python -I tools/switch_sweep.py --levels --only ATHN01D,PAND02B,ATHN06C,ATRET01B --package update_$p --out port/switch/sweep/d17l_$p 2>&1 | grep -E "PASS|FAIL|SLOW"
  done
done
echo "=== movie $(date +%T)"
python -I tools/switch_sweep.py --movies --only SEMO01 --package update_dips40 --out port/switch/sweep/mov_dips40 2>&1 | grep -E "PASS|FAIL|SLOW|md5"
echo "=== boots $(date +%T)"
bash port/switch/tools_dips16/boot4.sh update_dips40 port/switch/sweep/boot_dips40 4
for i in 1 2; do
  for p in boss18 boss10; do
    for it in hydra1 clones; do
      echo "=== boss $i $p $it $(date +%T)"
      python -I tools/switch_sweep.py --boss --only $it --package update_$p --out port/switch/sweep/d17b_$p 2>&1 | grep -E "PASS|FAIL|SLOW|md5"
    done
  done
done
TRANSFER=ftp bash tools/switch_default.sh
echo "=== done $(date +%T)"
