#!/bin/bash
# bh.sh <name>: build x86 <name> and a64 <name>a harnesses from /root/natvu1w
G=/root/natvu1w/out/vu1pregen/vu1n_generado
cd /root
taskset -c 0-7 bash /root/gowr/tools/vu1_nat/build.sh $1 /root/natvu1w $G > /root/vu1h/b_$1.log 2>&1 &
ARCH=a64 taskset -c 0-7 bash /root/gowr/tools/vu1_nat/build.sh ${1}a /root/natvu1w $G > /root/vu1h/b_${1}a.log 2>&1 &
wait
tail -n 3 /root/vu1h/b_$1.log /root/vu1h/b_${1}a.log
