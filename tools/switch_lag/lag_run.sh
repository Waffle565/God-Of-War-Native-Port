#!/usr/bin/env bash
# Input-lag run (docs/performance.md "Input lag"): default package (or UPDATE=...), env = default env + SKIP_FMV
# (+ extra env file), bot presses the bench's menu path (new game), waits for gameplay, runs lag_probe.py, closes,
# pulls the log and restores the console default.  usage: tools/switch_lag/lag_run.sh <name> [extra env]
set -u
cd "$(dirname "$0")/../.."
. tools/switch_lib.sh
NAME=$1; EXTRA=${2:-}
D=port/switch/logs/lag_$NAME; S=$D/shots; mkdir -p "$S" "$D/envdir"
sw_transfer; sw_ready "$S" || exit 1
[ -n "${UPDATE:-}" ] && { sw_push_update "$UPDATE" || exit 1; }
{ grep -v '^#' tools/switch/default_env.txt; echo GOW_SKIP_FMV=1; echo GOW_GOD_MODE=1; [ -n "$EXTRA" ] && cat "$EXTRA"; } > "$D/envdir/env.txt"
F put "$D/envdir/env.txt" switch/gow || exit 1
sw_launch "$S" || exit 1
T0=$(date +%s)
at() { while [ $(( $(date +%s) - T0 )) -lt $1 ]; do sleep 0.5; done; }
at 25; B shot "$S/title.jpg" >/dev/null; at 30; B click PLUS; for t in 39 45 51 57; do at $t; B click A; done
at 70; B shot "$S/intro.jpg" >/dev/null; at ${GAMEPLAY_S:-90}; B shot "$S/gameplay.jpg" >/dev/null
python -I tools/switch_lag/lag_probe.py "$S" ${DELAYS:-} | tee "$D/probe.txt"
sw_close "$S"; sw_pull "$D"
bash tools/switch_default.sh
