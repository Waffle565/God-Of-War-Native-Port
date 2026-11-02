#!/bin/bash
# annot.sh <elf> <pcs> <symbol regex> [lo_ms hi_ms]: objdump of the function with sample counts per instruction
B=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
cd "/mnt/c/Users/lovas/Claude/God Of War/port/switch"
read A S N <<< $($B-nm -S -C "$1" | grep -E " [tTwW] $3" | head -1 | awk '{print $1, $2, $4}')
echo "sym $N at $A size $S"
LO=${4:-0}; HI=${5:-999999999}
awk -v lo=$LO -v hi=$HI 'BEGIN{t=0} $1=="w"{t=$2; next} NF==2 && t>=lo && t<=hi {c[$1]+=$2} END{for(k in c) print k, c[k]}' "$2" > /root/dt14/cnt.txt
$B-objdump -d --no-show-raw-insn --start-address=0x$A --stop-address=$(printf "0x%x" $((0x$A + 0x$S))) "$1" | \
 awk 'NR==FNR{c[$1]=$2; next} /^ *[0-9a-f]+:/{a=$1; sub(":","",a); sub("^0*","",a); printf "%6s %s\n", (a in c)?c[a]:"", $0; next}' /root/dt14/cnt.txt -
