#!/bin/bash
# cp0.sh: chain copier off by default (GOW_CHAIN_COPIER=1 turns it on) in src-mlang, then build dips40
f=/root/src-mlang/ps2xRuntime/src/lib/ps2_memory.cpp
cp -n $f /root/dips39-orig/ps2_memory.cpp
python3 - "$f" <<'PY'
import sys
p = sys.argv[1]; s = open(p, newline='').read()
old = """                const char *e = std::getenv("GOW_CHAIN_COPIER");
                if (e && e[0] == '0')
                    return 0u;"""
new = """                // sw-dips16: off unless GOW_CHAIN_COPIER=1: with the copier the VU1 thread spent 4-5 % more per program
                // run in every zone (levels -1..1.5 fps, the fights' VU1-bound minimum lower too)
                const char *e = std::getenv("GOW_CHAIN_COPIER");
                if (!e || e[0] != '1')
                    return 0u;"""
assert s.count(old) == 1
open(p, 'w', newline='').write(s.replace(old, new))
PY
grep -n "GOW_CHAIN_COPIER=1" $f
bash '/mnt/c/Users/lovas/Claude/God Of War/port/switch/tools_dips14/build.sh' dips40
