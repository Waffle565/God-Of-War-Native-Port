#!/bin/bash
# hbuild.sh <name>: copy the scratch misc.inl into src-mlang + natvu1x, build harness x86 + a64, natcheck all, compare with n1
set -u
n=$1
S='/mnt/c/Users/lovas/AppData/Local/Temp/claude/C--Users-lovas-Claude-God-Of-War/106a44df-0ee9-4f93-9244-cea230a4e186/scratchpad/misc.inl'
if pgrep -f "ninja|dips_sb.sh" >/dev/null; then echo "BUSY: a build runs"; exit 1; fi
cp "$S" /root/src-mlang/ps2xRuntime/src/lib/vu/ps2_vu1_nat_misc.inl
cp "$S" /root/natvu1x/ps2xRuntime/src/lib/vu/ps2_vu1_nat_misc.inl
bash /root/dt/vu1_nat/dips_hb2.sh $n
[ -x /root/vu1h/$n ] && [ -x /root/vu1h/${n}a ] || { echo BUILD_FAILED; grep -m5 -i 'error' /root/vu1h/b_$n.log /root/vu1h/b_${n}a.log; exit 1; }
bash /root/dt/vu1_nat/nca.sh $n > /dev/null
bash /root/dt/vu1_nat/nca.sh ${n}a > /dev/null
echo "diff x86: $(diff /root/vu1h/nca_n1.txt /root/vu1h/nca_$n.txt | grep -c '^[<>]')"
echo "diff a64: $(diff /root/vu1h/nca_n1a.txt /root/vu1h/nca_${n}a.txt | grep -c '^[<>]')"
grep -h 'differ [1-9]\|differs [1-9]\|not restored: [1-9]' /root/vu1h/nca_$n.txt /root/vu1h/nca_${n}a.txt | head
echo HB_DONE
