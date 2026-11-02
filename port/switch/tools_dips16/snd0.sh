#!/bin/bash
# snd0.sh: SoundMix default priority boost 1 -> 0 in src-mlang (sw-dips16: the boost cost ~3.5 fps in GL-bound views)
f=/root/src-mlang/ps2xIOP/src/emulator/core/sound_engine.cpp
cp -n $f /root/dips39-orig/sound_engine.cpp
python3 - "$f" <<'PY'
import sys
p = sys.argv[1]; s = open(p, newline='').read()
old = """        // 240 k samples per 5 s at boost 0, 40 k at boost 1; on core 3 it starved behind GSStage / ChainCopy)
        int core = 0, boost = 1;"""
new = """        // 240 k samples per 5 s at boost 0, 40 k at boost 1; on core 3 it starved behind GSStage / ChainCopy)
        // sw-dips16: default boost 0 again: above GSThread the mixer takes core 0 from the deko3d thread, and where that
        // thread bounds the frame (ATHN06C's corridor: GS GL busy ~4.6 s per 5 s) fps fell 38.5 -> 35.6 (GL 24 -> 26 ms
        // per frame); the fights, VU1-thread bound, gained ~1 fps of minimum from it (single runs)
        int core = 0, boost = 0;"""
assert s.count(old) == 1
open(p, 'w', newline='').write(s.replace(old, new))
PY
grep -n "int core = 0, boost" $f
