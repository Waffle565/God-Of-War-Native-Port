#!/usr/bin/env bash
# Interleaved A/B on ATHN06C: dips37c vs dips38, N rounds; restores the default at the end.
cd "$(dirname "$0")/../../.."
N=${N:-3}
for i in $(seq 1 $N); do
  for p in dips37c dips38; do
    echo "=== round $i $p $(date +%T)"
    python -I tools/switch_sweep.py --levels --only ATHN06C --package update_$p --out port/switch/sweep/ab16_$p 2>&1 | tail -3
  done
done
TRANSFER=ftp bash tools/switch_default.sh
echo "=== done $(date +%T)"
