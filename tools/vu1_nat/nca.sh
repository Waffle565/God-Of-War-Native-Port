#!/bin/bash
# nca.sh <harness> [fd]: natcheck all recordings (nrs 8 + nk + nq), 8 in parallel on cores 0-7.
# fd: GOW_VU1NAT_FORCE_DECLINE=1. a64 harness (name ends in a): under qemu-aarch64.
# Output: /root/vu1h/nca_<harness>[_fd].txt, one block per recording with the summary lines.
h=$1; tag=$h; [ "$2" = fd ] && tag=${h}_fd
out=/root/vu1h/nca_$tag; rm -rf $out; mkdir -p $out
recs=()
for r in pcfight pcmenu pcwalk pcathn02 pcdesert pccliffs pcolymp pccliff2; do recs+=(/root/nrs/$r.vifrec); done
recs+=(/root/nrs/nk/*.vifrec /root/nrs/nq/*.vifrec)
run() {
  f=$1; b=$(basename $(dirname $f))_$(basename $f .vifrec)
  pre=""; [[ $h == *a ]] && pre=qemu-aarch64
  env=""; [ "$tag" != "$h" ] && env="GOW_VU1NAT_FORCE_DECLINE=1"
  # ATHN01F particle runs need the large oracle budget (nat-vu1q)
  case $f in *ATHN01F*) env="$env GOW_VU1NAT_ORACLE_CYCLES=0x10000000";; esac
  env $env $pre /root/vu1h/$h natcheck $f > $out/$b.full 2>&1
  grep -E 'covered entries|native vs oracle|not restored|differ|declined at|not covered' $out/$b.full | grep -v '^ *$' > $out/$b.sum
}
export -f run; export h tag out
printf '%s\n' "${recs[@]}" | taskset -c 0-7 xargs -P 8 -I{} bash -c 'run {}'
for s in $out/*.sum; do echo "== $(basename $s .sum)"; cat $s; done > /root/vu1h/nca_$tag.txt
wc -l /root/vu1h/nca_$tag.txt
