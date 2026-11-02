#!/usr/bin/env bash
# boot4.sh <package> <out dir> [n=4]: n title boots of an update package with the default env.txt (FAILGT); shots at
# 25 / 50 / 75 s, log pulled, error lines counted; the console default is restored at the end
set -u
cd "$(dirname "$0")/../../.."
. tools/switch_lib.sh
PKG=$1; OUT=$2; N=${3:-4}
sw_transfer
for i in $(seq 1 $N); do
  D=$OUT/boot$i; mkdir -p "$D"
  sw_ready "$D" || exit 1
  [ $i = 1 ] && { sw_push_update port/switch/nsp/out/$PKG || exit 1; }
  cp tools/switch/default_env.txt "$D/env.txt"; F put "$D/env.txt" switch/gow || exit 1
  sw_launch "$D" || { echo "boot$i: launch failed"; continue; }
  for t in 25 50 75; do sleep 25; B shot "$D/t$t.jpg" >/dev/null; done
  sw_close "$D"; sw_pull "$D" || exit 1
  echo "boot$i: $(grep -ciE 'error|exception|terminate|abort' "$D/log.txt") error-like lines, $(wc -l < "$D/log.txt") lines"
done
TRANSFER=ftp bash tools/switch_default.sh
echo BOOT_DONE
