#!/bin/bash
# gentest.sh <harness>: GOW_VU1NAT_GENTEST=1 natcheck (x86 + a64), compared with the plain run of the same harness
n=$1; H=/root/vu1h
for h in $n ${n}a; do
  cp $H/nca_$h.txt $H/nca_${h}_plain.txt
  GOW_VU1NAT_GENTEST=1 bash /root/dt/vu1_nat/nca.sh $h > /dev/null
  echo "gentest $h vs plain: $(diff $H/nca_${h}_plain.txt $H/nca_$h.txt | grep -c '^[<>]')"
done
echo GT_DONE
