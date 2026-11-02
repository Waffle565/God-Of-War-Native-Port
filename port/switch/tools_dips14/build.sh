#!/bin/bash
# build.sh <tag>: one WSL build at a time (waits for other agents' ninja / dips_sb.sh)
while pgrep -x ninja >/dev/null || pgrep -f "^bash /root/dt/dips_sb.sh" >/dev/null; do sleep 20; done
bash /root/dt/dips_sb.sh $1 2>&1 | tail -3
