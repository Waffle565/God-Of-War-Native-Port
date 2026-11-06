#!/usr/bin/env bash
# lev1.sh: one interleaved levels round dips37c / dips40s, level by level (pair per level), no new run after STOP (HHMM)
cd "$(dirname "$0")/../../.."
STOP=${STOP:-1611}
for l in ATHN06C ATHN01D PAND02B ATRET01B; do
  for p in dips37c dips40s; do
    [ "$(date +%H%M)" -ge "$STOP" ] && { echo "=== stop before $l $p $(date +%T)"; break 2; }
    echo "=== lev $l $p $(date +%T)"
    python -I tools/switch_sweep.py --levels --only $l --package update_$p --out port/switch/sweep/c40l_$p 2>&1 | grep -E "PASS|FAIL|SLOW|md5"
  done
done
echo "=== done $(date +%T)"
