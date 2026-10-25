#!/bin/bash
# sws_runs.sh <pkg tag> <suffix:envfile>...: deterministic benches (BENCH_GAME_S 150) with update_<tag>.
# Always ends with the console default (tools/switch_default.sh), also when killed. To stop between runs:
# touch port/switch/logs/envs/STOP (only between runs; never kill it mid-run: a killed parent leaves its child bench running).
cd "/c/Users/lovas/Claude/God Of War"
restore() { trap - EXIT INT TERM; bash tools/switch_default.sh > port/switch/logs/default_sws.out 2>&1; tail -1 port/switch/logs/default_sws.out; }
trap restore EXIT INT TERM
rm -f port/switch/logs/envs/STOP
t=$1; shift
for se in "$@"; do sx=${se%%:*}; ef=${se#*:}
  [ -f port/switch/logs/envs/STOP ] && { echo "stopped before $t$sx"; break; }
  if [ "${PLAY:-}" = 1 ]; then
    NOBENCH=1 UPDATE=port/switch/nsp/out/update_$t bash tools/switch_bench.sh $t$sx 300 $ef > port/switch/logs/bench_$t$sx.out 2>&1
    python -I tools/switch_play_report.py port/switch/logs/bench_$t$sx/log.txt
  else
    BENCH_GAME_S=150 UPDATE=port/switch/nsp/out/update_$t bash tools/switch_bench.sh $t$sx 240 $ef > port/switch/logs/bench_$t$sx.out 2>&1
    echo "$t$sx: $(grep -E '^ +[0-9]+-[0-9]+ ' port/switch/logs/bench_$t$sx.out | awk '{printf "%s ", $2}')"
  fi
done
