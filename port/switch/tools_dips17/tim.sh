#!/bin/bash
# tim.sh A B: x86 timing, every recording natchecked by A then B back to back (same core load), GOW_VU1NAT_REPS=5,
# output dirs /root/vu1h/nca_<A>_t and nca_<B>_t (*.full), for et.sh <A>_t <B>_t
A=$1; B=$2
for h in $A $B; do rm -rf /root/vu1h/nca_${h}_t; mkdir -p /root/vu1h/nca_${h}_t; done
recs=()
for r in pcfight pcmenu pcwalk pcathn02 pcdesert pccliffs pcolymp pccliff2; do recs+=(/root/nrs/$r.vifrec); done
recs+=(/root/nrs/nk/*.vifrec /root/nrs/nq/*.vifrec)
run() {
  f=$1; b=$(basename $(dirname $f))_$(basename $f .vifrec)
  env=""; case $f in *ATHN01F*) env="GOW_VU1NAT_ORACLE_CYCLES=0x10000000";; esac
  for h in $A $B; do env $env GOW_VU1NAT_REPS=5 /root/vu1h/$h natcheck $f > /root/vu1h/nca_${h}_t/$b.full 2>&1; done
}
export -f run; export A B
printf '%s\n' "${recs[@]}" | taskset -c 0-7 xargs -P 8 -I{} bash -c 'run {}'
echo TIM_DONE
