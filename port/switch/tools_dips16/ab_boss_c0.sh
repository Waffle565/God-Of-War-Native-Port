#!/usr/bin/env bash
# ab_boss_c0.sh: as ab_boss.sh, out sweep/b16c0_<pkg> (copier-off runs)
cd "$(dirname "$0")/../../.."
R=$1; ENV=$2; shift 2
for i in $(seq 1 $R); do
  for p in "$@"; do
    for it in hydra1 clones; do
      echo "=== round $i $p $it $(date +%T)"
      python -I tools/switch_sweep.py --boss --only $it --package update_$p --env "$ENV" --out port/switch/sweep/b16c0_$p 2>&1 | grep -E "PASS|FAIL|SLOW|md5"
    done
  done
done
TRANSFER=ftp bash tools/switch_default.sh
echo "=== done $(date +%T)"
