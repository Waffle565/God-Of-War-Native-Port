#!/bin/bash
# tr.sh <harness> <rec>...: natcheck each recording, print its [trace] lines (temporary instrumentation builds)
h=$1; shift
for r in "$@"; do echo "== $r"; /root/vu1h/$h natcheck /root/nrs/$r.vifrec 2>&1 | grep '^\[trace\]' | paste -sd ' ' | cut -c1-1500; done
