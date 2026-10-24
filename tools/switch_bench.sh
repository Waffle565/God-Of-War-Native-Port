#!/usr/bin/env bash
# Deterministic console fight benchmark (docs/benchmark.md). Start: console on DBI's MTP screen.
# Pushes the package's exefs, env.txt (bench env + extra lines) and the pad script, runs the game SECS seconds
# (screenshots every 30 s), returns to DBI's MTP screen, pulls the log and prints the per-scene table.
# usage: tools/switch_bench.sh <name> [secs, default 480] [extra env file]
#   UPDATE=port/switch/nsp/out/update_<pkg>   package to run (exefs pushed)
#   SHOTS=<dir>                               screenshot folder (default port/switch/logs/bench_<name>/shots)
set -u
cd "$(dirname "$0")/.."
NAME=$1; SECS=${2:-480}; EXTRA=${3:-}
D=port/switch/logs/bench_$NAME; S=${SHOTS:-$D/shots}
mkdir -p "$D" "$S"
B() { for k in 1 2 3; do timeout 60 python -I tools/switch_bot.py "$@" 2>/dev/null && return 0; sleep 3; done; return 1; }
M() { for k in 1 2 3; do timeout 300 python -I tools/switch_mtp.py "$@" && return 0; [ "$1" = rm ] && return 1; sleep 10; done; return 1; }
E=${UPDATE:?set UPDATE=port/switch/nsp/out/update_<pkg>}/atmosphere/contents/0100D0D0600A0000/exefs
t=$(B title); [ "$t" = "0197BB45716E0000" ] || { echo "not in hbmenu/DBI (title $t)"; exit 1; }
M put $E/main atmosphere/contents/0100D0D0600A0000/exefs || exit 1
M put $E/main.npdm atmosphere/contents/0100D0D0600A0000/exefs || exit 1
python -I tools/bench_script.py ${BENCH_GAME_S:-240} ${BENCH_MENU_S:-30} > "$D/bench.txt"
M put "$D/bench.txt" switch/gow || exit 1
mkdir -p "$D/envdir"
{ cat <<'EOF'
GOW_GPU_MHZ=max
GOW_AUDIO_STATS=1
GOW_PAD_TEST=1
GOW_PAD_TEST_NO_CAPTURE=1
GOW_BENCH=1
GOW_GOD_MODE=1
GOW_SKIP_FMV=1
GOW_PAD_GUION_FILE=sdmc:/switch/gow/bench.txt
EOF
  [ -n "$EXTRA" ] && cat "$EXTRA"; } > "$D/envdir/env.txt"
M put "$D/envdir/env.txt" switch/gow || exit 1
B click HOME; sleep 2; B click X; sleep 2; B click A; sleep 3
B click DRIGHT; sleep 1; B click A; sleep 6; B click A; sleep 6
t=$(B title); [ "$t" = "0100D0D0600A0000" ] || { echo "game did not start (title $t)"; B shot "$S/fail.jpg"; exit 1; }
for i in $(seq 1 $((SECS / 30))); do sleep 30; B shot "$S/g$i.jpg" >/dev/null; done
for k in 1 2 3 4; do B click HOME; sleep 5; B shot "$S/h_$k.jpg" >/dev/null; [ $(stat -c %s "$S/h_$k.jpg") -gt 150000 ] && break; done
B click X; sleep 2; B click A; sleep 3
B click DRIGHT; sleep 1; B click A; sleep 6; B click A; sleep 6
B click DDOWN DRIGHT DRIGHT DRIGHT DRIGHT DRIGHT; B click A; sleep 5; B click A; sleep 1
B click DDOWN DDOWN DDOWN DDOWN DDOWN DDOWN DDOWN DDOWN DDOWN; B shot "$S/dbi.jpg" >/dev/null; B click A; sleep 5
M get switch/gow/log.txt $D/log.txt || { echo "MTP not up"; B shot "$S/fail2.jpg"; exit 1; }
for t in GSWorker GameThread GSStage GSThread; do M get switch/gow/pcs_$t.txt $D/pcs_$t.txt 2>/dev/null; done
M rm switch/gow/env.txt 2>/dev/null
python -I tools/bench_report.py $D/log.txt
