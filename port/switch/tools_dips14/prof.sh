#!/bin/bash
# prof.sh <elf> <pcs> <lo-hi s> [N]: per-function (funcs.py) and per-category (cat.py) shares; output also in /root/dt14/last_funcs.txt
mkdir -p /root/dt14; T="/mnt/c/Users/lovas/Claude/God Of War/port/switch/tools_dips14"
cd "/mnt/c/Users/lovas/Claude/God Of War/port/switch"
TOPN=100000 python3 "$T/funcs.py" "$1" "$2" "$3" > /root/dt14/last_funcs.txt
echo "== categories"; python3 "$T/cat.py" /root/dt14/last_funcs.txt ${4:-30}
