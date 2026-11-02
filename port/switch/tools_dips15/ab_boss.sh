#!/bin/bash
# ab_boss.sh <package> <outtag> "<item>|<env>" ...: boss items, one sweep each (each pushes the package, restores the default)
cd "/c/Users/lovas/Claude/God Of War"
PKG=$1; TAG=$2; shift 2
for spec in "$@"; do
  it=${spec%%|*}; env=${spec#*|}
  python -I tools/switch_sweep.py --boss --only $it --package $PKG --out port/switch/sweep/$TAG --env "$env" 2>&1 | grep -E "PASS|FAIL|SLOW|restored|md5|report" 
  echo "== $it [$env] done"
done
