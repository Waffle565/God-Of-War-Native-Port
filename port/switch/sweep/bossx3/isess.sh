#!/bin/bash
# interactive boss session (sw-boss3): isess.sh start <dir> <env.txt> [card.mcd] [package] | isess.sh stop <dir> [keep-mcd-as]
cd "/c/Users/lovas/Claude/God Of War"; . tools/switch_lib.sh; TRANSFER=ftp; sw_transfer >/dev/null
cmd=$1; D=port/switch/sweep/bossx3/$2; mkdir -p $D
if [ $cmd = start ]; then
  P=${5:-update_boss6}
  sw_ready $D; sw_push_update port/switch/nsp/out/$P || { echo push failed; exit 1; }
  cp $3 $D/env.txt; F put $D/env.txt switch/gow || exit 1
  if [ -n "$4" ]; then mkdir -p $D/mc; cp "$4" $D/mc/boss.mcd; F put $D/mc/boss.mcd switch/gow || exit 1; rm -r $D/mc; fi
  F rm switch/gow/live.txt >/dev/null 2>&1
  sw_launch $D && echo launched
else
  sw_home_close $D
  sw_pull $D
  [ -n "$3" ] && F get switch/gow/boss.mcd "$3"
  F rm switch/gow/live.txt >/dev/null 2>&1; F rm switch/gow/boss.mcd >/dev/null 2>&1
  TRANSFER=ftp bash tools/switch_default.sh > $D/default.out 2>&1; tail -2 $D/default.out
fi
