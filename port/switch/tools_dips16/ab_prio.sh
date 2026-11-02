#!/usr/bin/env bash
# ATHN06C on dips38 with GOW_PRIO_SoundMix=0 (SoundMix at the default priority on core 0, as dips37c), N runs
cd "$(dirname "$0")/../../.."
N=${N:-3}
for i in $(seq 1 $N); do
  echo "=== run $i $(date +%T)"
  python -I tools/switch_sweep.py --levels --only ATHN06C --package update_dips38 --env GOW_PRIO_SoundMix=0 --out port/switch/sweep/ab16_dips38p0 2>&1 | tail -2
done
TRANSFER=ftp bash tools/switch_default.sh
echo "=== done $(date +%T)"
