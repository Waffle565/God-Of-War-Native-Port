#!/bin/bash
# hsync.sh: list the VU files that differ between the source tree /root/src-mlang and the harness tree /root/natvu1x
for f in /root/src-mlang/ps2xRuntime/src/lib/vu/* /root/src-mlang/ps2xRuntime/include/runtime/ps2_vu1*.h; do
  g=${f/src-mlang/natvu1x}; cmp -s "$f" "$g" || echo "DIFF $f"
done
ls -t /root/vu1h | grep '^b_' | head -4
grep -c . /root/vu1h/nca_n1.txt; head -30 /root/vu1h/nca_n1.txt
