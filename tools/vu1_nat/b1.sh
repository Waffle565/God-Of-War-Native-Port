#!/bin/bash
# b1.sh <name>: build only the x86 harness <name> from /root/natvu1w (cores 8-15)
G=/root/natvu1w/out/vu1pregen/vu1n_generado
taskset -c 8-15 bash /root/gowr/tools/vu1_nat/build.sh $1 /root/natvu1w $G > /root/vu1h/b_$1.log 2>&1; tail -n 1 /root/vu1h/b_$1.log
