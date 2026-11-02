#!/bin/bash
# enttime.sh <harness...>: per-entry native us/run summed over the natcheck recordings (x86 timing)
H=/root/vu1h
for e in 39f38df4 93febeec 6df5c750 3d0d1206 7ab32108; do
  for h in "$@"; do
    grep -h "entry $e:" $H/nca_$h/*.full 2>/dev/null | sed -E 's/.*: ([0-9]+) native runs.*native ([0-9.]+) us.*/\1 \2/' |
      awk -v e=$e -v h=$h '{n+=$1; t+=$1*$2} END {if(n) printf "%s %s runs %d us/run %.3f\n", e, h, n, t/n}'
  done
done
