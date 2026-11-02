#!/usr/bin/env bash
# after.sh: once the levels A/B ends: SEMO01 on dips39, 4 title boots of dips39, then a hydra1 profile of boss17 with
# memmove callers (GOW_PC_SAMPLE_LR) and SoundMix boost 0; each step restores the default
cd "$(dirname "$0")/../../.."
until grep -q "=== done" port/switch/tools_dips16/ab_lev.out; do sleep 15; done
echo "=== movie $(date +%T)"
python -I tools/switch_sweep.py --movies --only SEMO01 --package update_dips39 --out port/switch/sweep/mov_dips39 2>&1 | grep -E "PASS|FAIL|SLOW|md5"
echo "=== boots $(date +%T)"
bash port/switch/tools_dips16/boot4.sh update_dips39 port/switch/sweep/boot_dips39 4
echo "=== prof $(date +%T)"
bash port/switch/tools_dips16/prof.sh update_boss17 b17lr "GOW_PC_SAMPLE_LR=392e7c0-392e9ec,GOW_PRIO_SoundMix=0" hydra1
echo "=== after done $(date +%T)"
