# hb2.sh <name> [base]: build x86 + a64 harness from /root/natvu1x
cd /root; G=/root/natvu1w/out/vu1pregen/vu1n_generado
taskset -c 0-7 bash /root/gowr/tools/vu1_nat/build.sh $1 /root/natvu1x $G > /root/vu1h/b_$1.log 2>&1 &
ARCH=a64 taskset -c 0-7 bash /root/gowr/tools/vu1_nat/build.sh ${1}a /root/natvu1x $G > /root/vu1h/b_${1}a.log 2>&1 &
wait; tail -n 2 /root/vu1h/b_$1.log /root/vu1h/b_${1}a.log
