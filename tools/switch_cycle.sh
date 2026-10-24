#!/usr/bin/env bash
# One console run (docs/switch.md "Remote testing"): push an update package's ExeFS and env.txt, run the game SECS
# seconds (screenshot 15 s after start and every 30 s), close it, pull the log and print the summary.
# Over WLAN (sys-ftpd) when it answers: start from anything (a running app is closed first). Over USB MTP otherwise:
# start on DBI's MTP screen, ends there again.
# usage: tools/switch_cycle.sh <name> [secs, default 60] [env file | -]   (no env file / "-": env.txt deleted)
#   UPDATE=port/switch/nsp/out/update_<pkg>   package (default: none, the ExeFS on the SD stays)
#   TRANSFER=ftp|mtp                          force a transfer
set -u
cd "$(dirname "$0")/.."
. tools/switch_lib.sh
NAME=$1; SECS=${2:-60}; ENVF=${3:--}
D=port/switch/logs/run_$NAME; S=$D/shots
mkdir -p "$S"
sw_transfer
sw_ready "$S" || exit 1
if [ -n "${UPDATE:-}" ]; then sw_push_update "$UPDATE" || exit 1; fi
for t in GSWorker GameThread GSStage GSThread; do F rm switch/gow/pcs_$t.txt >/dev/null 2>&1; done
if [ "$ENVF" != - ]; then mkdir -p "$D/envdir"; cp "$ENVF" "$D/envdir/env.txt"; F put "$D/envdir/env.txt" switch/gow || exit 1
else F rm switch/gow/env.txt >/dev/null 2>&1; fi
sw_launch "$S" || exit 1
sleep 15; B shot "$S/title.jpg" >/dev/null
for i in $(seq 1 $(( (SECS - 15) / 30 ))); do sleep 30; B shot "$S/g$i.jpg" >/dev/null; done
sw_close "$S"
sw_pull "$D" || exit 1
python -I tools/switch_log_summary.py "$D/log.txt"
