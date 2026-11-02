#!/bin/bash
# etime.sh <harness A> <harness B> [entries regex]: native us/run per entry, summed over the recordings (x86 timings)
re=${3:-1dcc471c|2611f8a2|e4f3b5c3|ea6b4a01}
for h in $1 $2; do
  echo "== $h"; cat /root/vu1h/nca_$h/*.full | grep -E "entry ($re):" | awk '{n[$3]+=$4; t[$3]+=$4*$(NF-7)} END {for (e in n) printf "%s runs %d native %.2f us/run\n", e, n[e], t[e]/n[e]}' | sort
done
