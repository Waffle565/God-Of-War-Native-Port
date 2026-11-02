#!/bin/bash
# waitlog.sh <log> <pattern> <seconds>
for i in $(seq 1 $3); do grep -q "$2" "$1" && break; sleep 1; done; tail -c 160 "$1"
