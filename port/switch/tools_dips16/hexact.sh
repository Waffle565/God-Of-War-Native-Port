#!/bin/bash
# hexact.sh <name>: forced-decline natcheck of harness <name> vs n1_fd, then the forced-exact harness <name>x (every
# FastOps pass redone exactly; tools_dips11/exact.sh) vs n1
set -u
n=$1
if pgrep -f "ninja|dips_sb.sh" >/dev/null; then echo "BUSY: a build runs"; exit 1; fi
bash /root/dt/vu1_nat/nca.sh $n fd > /dev/null
echo "diff fd: $(diff /root/vu1h/nca_n1_fd.txt /root/vu1h/nca_${n}_fd.txt | grep -c '^[<>]')"
bash '/mnt/c/Users/lovas/Claude/God Of War/port/switch/tools_dips11/exact.sh' ${n}x 2>&1 | tail -4
sed -i 's/nca_f1/nca_n1/' /dev/null
echo "diff x86 exact vs n1: $(diff /root/vu1h/nca_n1.txt /root/vu1h/nca_${n}x.txt | grep -c '^[<>]')"
echo "diff a64 exact vs n1: $(diff /root/vu1h/nca_n1a.txt /root/vu1h/nca_${n}xa.txt | grep -c '^[<>]')"
echo HX_DONE
