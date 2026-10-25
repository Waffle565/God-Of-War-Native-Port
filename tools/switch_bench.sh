#!/usr/bin/env bash
# Deterministic console fight benchmark (docs/benchmark.md). Over WLAN (sys-ftpd, tools/switch_ftp.py) when it
# answers: start from anything (a running app is closed first). Over USB MTP otherwise: start on DBI's MTP screen.
# Pushes the package's exefs, env.txt (bench env + extra lines) and the pad script, runs the game SECS seconds
# (screenshots every 30 s), closes it, pulls the log, deletes env.txt and prints the per-scene table.
# usage: tools/switch_bench.sh <name> [secs, default 480] [extra env file]
#   UPDATE=port/switch/nsp/out/update_<pkg>   package to run (exefs pushed)
#   SHOTS=<dir>                               screenshot folder (default port/switch/logs/bench_<name>/shots)
#   TRANSFER=ftp|mtp                          force a transfer
#   NOBENCH=1                                 no GOW_BENCH (normal play: real frame steps; the pad script
#                                             times are wall seconds then)
#   BENCH_GPU=max (or MHz)                    GPU clock (GOW_GPU_MHZ); unset = the system's default clocks (handheld
#                                             GPU 307.2 / EMC 1331.2, the target since 2026-10-24); BENCH_EMC likewise
set -u
cd "$(dirname "$0")/.."
. tools/switch_lib.sh
NAME=$1; SECS=${2:-480}; EXTRA=${3:-}
D=port/switch/logs/bench_$NAME; S=${SHOTS:-$D/shots}
mkdir -p "$D" "$S"
: "${UPDATE:?set UPDATE=port/switch/nsp/out/update_<pkg>}"
sw_transfer
sw_ready "$S" || exit 1
sw_push_update "$UPDATE" || exit 1
# the PC sampler appends to its files: a run's pcs_*.txt must not carry an earlier run's samples
for t in GSWorker GameThread GSStage GSThread; do F rm switch/gow/pcs_$t.txt >/dev/null 2>&1; done
python -I tools/bench_script.py ${BENCH_GAME_S:-240} ${BENCH_MENU_S:-30} > "$D/bench.txt"
F put "$D/bench.txt" switch/gow || exit 1
mkdir -p "$D/envdir"
{ [ -n "${BENCH_GPU:-}" ] && echo "GOW_GPU_MHZ=$BENCH_GPU"
  [ -n "${BENCH_EMC:-}" ] && echo "GOW_EMC_MHZ=$BENCH_EMC"
  [ -z "${NOBENCH:-}" ] && echo GOW_BENCH=1
  cat <<'EOF'
GOW_AUDIO_STATS=1
GOW_PAD_TEST=1
GOW_PAD_TEST_NO_CAPTURE=1
GOW_GOD_MODE=1
GOW_SKIP_FMV=1
GOW_PAD_GUION_FILE=sdmc:/switch/gow/bench.txt
EOF
  [ -n "$EXTRA" ] && cat "$EXTRA"; } > "$D/envdir/env.txt"
F put "$D/envdir/env.txt" switch/gow || exit 1
sw_launch "$S" || exit 1
for i in $(seq 1 $((SECS / 30))); do sleep 30; B shot "$S/g$i.jpg" >/dev/null; done
sw_close "$S"
sw_pull "$D" || { B shot "$S/fail2.jpg" >/dev/null; exit 1; }
F rm switch/gow/env.txt >/dev/null 2>&1
python -I tools/bench_report.py $D/log.txt
# sw-fix 2026-10-25: a bench leaves the console in the default state (the user once played a bench build without
# env.txt: English, old controls); KEEP_BENCH=1 keeps the bench package installed (loops of benches)
[ "${KEEP_BENCH:-0}" = 1 ] || bash tools/switch_default.sh
