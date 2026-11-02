#!/usr/bin/env bash
# copier.sh: after after.sh: ATHN01D + PAND02B + ATHN06C on dips39 with the chain copier off / on, 2 rounds (VU1 us/run question)
cd "$(dirname "$0")/../../.."
until grep -q "=== after done" port/switch/tools_dips16/after.out; do sleep 15; done
for i in 1 2; do
  echo "=== $i off $(date +%T)"
  python -I tools/switch_sweep.py --levels --only ATHN01D,PAND02B,ATHN06C --package update_dips39 --env GOW_CHAIN_COPIER=0 --out port/switch/sweep/cp16_off 2>&1 | grep -E "PASS|FAIL|SLOW"
  echo "=== $i on $(date +%T)"
  python -I tools/switch_sweep.py --levels --only ATHN01D,PAND02B,ATHN06C --package update_dips39 --out port/switch/sweep/cp16_on 2>&1 | grep -E "PASS|FAIL|SLOW"
done
TRANSFER=ftp bash tools/switch_default.sh
echo "=== copier done $(date +%T)"
