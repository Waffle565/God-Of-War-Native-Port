#!/bin/bash
# hb17.sh <name> <hle.inl path>: harness from /root/natvu1x with the given ps2_vu1_hle.inl: natcheck x86 + a64 vs n1,
# forced decline vs n1_fd; with X=1 also the forced-exact harness <name>x (FastOps::ok() false) vs n1.
# natvu1x's hle.inl is restored from src-mlang at the end.
set -u
n=$1; H=/root/natvu1x/ps2xRuntime/src/lib/vu/ps2_vu1_hle.inl
while pgrep -x ninja >/dev/null || pgrep -f "^bash /root/dt/dips_sb.sh" >/dev/null; do sleep 20; done
cp "$2" $H
bash /root/dt/vu1_nat/dips_hb2.sh $n
[ -x /root/vu1h/$n ] && [ -x /root/vu1h/${n}a ] || { echo BUILD_FAILED; grep -m5 -i 'error' /root/vu1h/b_$n.log /root/vu1h/b_${n}a.log; cp /root/src-mlang/ps2xRuntime/src/lib/vu/ps2_vu1_hle.inl $H; exit 1; }
bash /root/dt/vu1_nat/nca.sh $n > /dev/null
bash /root/dt/vu1_nat/nca.sh ${n}a > /dev/null
bash /root/dt/vu1_nat/nca.sh $n fd > /dev/null
echo "diff x86: $(diff /root/vu1h/nca_n1.txt /root/vu1h/nca_$n.txt | grep -c '^[<>]')"
echo "diff a64: $(diff /root/vu1h/nca_n1a.txt /root/vu1h/nca_${n}a.txt | grep -c '^[<>]')"
echo "diff fd: $(diff /root/vu1h/nca_n1_fd.txt /root/vu1h/nca_${n}_fd.txt | grep -c '^[<>]')"
grep -h 'differ [1-9]\|differs [1-9]\|not restored: [1-9]' /root/vu1h/nca_$n.txt /root/vu1h/nca_${n}a.txt | head -5
if [ "${X:-0}" = 1 ]; then
  bash '/mnt/c/Users/lovas/Claude/God Of War/port/switch/tools_dips11/exact.sh' ${n}x 2>&1 | tail -2
  echo "diff x86 exact vs n1: $(diff /root/vu1h/nca_n1.txt /root/vu1h/nca_${n}x.txt | grep -c '^[<>]')"
  echo "diff a64 exact vs n1: $(diff /root/vu1h/nca_n1a.txt /root/vu1h/nca_${n}xa.txt | grep -c '^[<>]')"
fi
cp /root/src-mlang/ps2xRuntime/src/lib/vu/ps2_vu1_hle.inl $H
echo HB17_DONE
