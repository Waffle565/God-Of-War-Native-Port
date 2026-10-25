#!/bin/bash
# lines.sh <bench> <elf> <func> [n]: prof.sh's top lines of func on one compact line each
bash /root/gowr/tools/vu1_nat/prof.sh "$1" "$2" "$3" "${4:-40}" | tail -n +2 | awk '{sub("ps2_vu1_","",$2); printf "%s %s | ", $1, $2} END {print ""}'
