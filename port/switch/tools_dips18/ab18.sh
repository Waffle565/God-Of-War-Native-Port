#!/usr/bin/env bash
# ab18.sh <rounds> <pkg>...: interleaved boss runs (hydra1, clones) per package, out sweep/d18b_<pkg>; default restored
cd "$(dirname "$0")/../../.."
R=$1; shift
for i in $(seq 1 $R); do
  for p in "$@"; do
    for it in hydra1 clones; do
      echo "=== round $i $p $it $(date +%T)"
      python -I tools/switch_sweep.py --boss --only $it --package update_$p --out port/switch/sweep/d18b_$p 2>&1 | grep -E "PASS|FAIL|SLOW|md5"
    done
  done
done
TRANSFER=ftp bash tools/switch_default.sh
echo "=== done $(date +%T)"
