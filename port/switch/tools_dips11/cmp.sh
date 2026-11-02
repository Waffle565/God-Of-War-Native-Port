#!/bin/bash
# cmp.sh <base> <new>: natcheck output diffs (x86 plain / fd, a64) and per-entry native us/run of some entries
b=$1; n=$2; H=/root/vu1h
for s in "" _fd; do [ -f $H/nca_$n$s.txt ] && echo "x86$s: $(diff $H/nca_$b$s.txt $H/nca_$n$s.txt | grep -c '^[<>]')"; done
[ -f $H/nca_${n}a.txt ] && echo "a64: $(diff $H/nca_${b}a.txt $H/nca_${n}a.txt | grep -c '^[<>]')"
for e in fbde7722 b66a3e7a 9ffe612e b45e3bab 93febeec; do
  for h in $b $n; do
    echo "$e $h: $(grep -h "entry $e:" $H/nca_$h/*.full 2>/dev/null | sed -E 's/.*native ([0-9.]+) us.*/\1/' | tr '\n' ' ')"
  done
done
