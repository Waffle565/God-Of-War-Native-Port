#!/bin/bash
# dif.sh <base> <new>: diff line counts of the natcheck summaries (x86 / a64, plain / fd)
cd /root/vu1h
for a in "" a; do for s in "" _fd; do
  echo "${a:-x86}$s: $(diff nca_$1$a$s.txt nca_$2$a$s.txt | grep -c '^[<>]') ($(wc -l < nca_$2$a$s.txt) lines)"
done; done
