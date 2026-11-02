#!/bin/bash
# lv.sh <dir> <shot name> <steps...>: idle nudge (keeps Auto-Sleep off), live steps, half-size shot
cd "/c/Users/lovas/Claude/God Of War"; D=port/switch/sweep/bossx3/$1; n=$2; shift 2
python -I tools/switch_bot.py cmd setStick RIGHT 0x600 0x0 >/dev/null 2>&1; python -I tools/switch_bot.py cmd setStick RIGHT 0x0 0x0 >/dev/null 2>&1
LIVE_HALF=1 python tools/switch_live.py $D/$n.jpg "$@" | grep pos; echo "$D/${n}_h.jpg"
