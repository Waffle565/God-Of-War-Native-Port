#!/bin/bash
# cmp.sh <base> <new>: build <new> (x86 + a64), natcheck all (plain + fd) for both arches, diff vs <base>, x86 times
S=$(dirname "$0")
b=$1; n=$2
bash $S/bh.sh $n > /dev/null 2>&1 || { echo BUILD FAILED; tail -n 20 /root/vu1h/b_$n.log /root/vu1h/b_${n}a.log; exit 1; }
grep -l "error" /root/vu1h/b_$n.log /root/vu1h/b_${n}a.log && { grep -m5 error /root/vu1h/b_$n.log /root/vu1h/b_${n}a.log; exit 1; }
for h in $n ${n}a; do for m in "" fd; do bash $S/nca.sh $h $m > /dev/null; done; done
for s in "" _fd; do
  for a in "" a; do
    [ -f /root/vu1h/nca_${b}${a}${s}.txt ] || { for m in "" fd; do bash $S/nca.sh ${b}${a} $m > /dev/null; done; }
    d=$(diff /root/vu1h/nca_${b}${a}${s}.txt /root/vu1h/nca_${n}${a}${s}.txt | grep -c '^[<>]')
    echo "diff ${a:-x86}${s}: $d lines"
  done
done
# x86 native time sums (nrs 8)
for h in $b $n; do
  t=$(cat /root/vu1h/nca_$h/nrs_*.full | grep -o 'native [0-9]* us' | awk '{t+=$2} END {print t}')
  echo "$h native us (nrs 8): $t"
done
# x86 error-free check: any nonzero difference counts
grep -hE 'differ [1-9]|differs [1-9]|restored: [1-9]' /root/vu1h/nca_${n}.txt /root/vu1h/nca_${n}_fd.txt | sort | uniq -c | head
