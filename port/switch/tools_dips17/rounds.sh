#!/bin/bash
# rounds.sh (WSL): 3 rounds of tim.sh in both orders (n1/kc1, kc1/n1) plus n1/kc1r; per-entry us/run summed over rounds (x86)
D=$(dirname "$(readlink -f "$0")"); mkdir -p /tmp/kcc; cd /tmp/kcc; rm -f acc_*.txt
for i in 1 2 3; do
  for o in "n1 kc1" "kc1 n1" "n1 kc1r"; do
    bash "$D/tim.sh" $o > /dev/null
    for h in $o; do
      cat /root/vu1h/nca_${h}_t/*.full | grep -E "\[vu1nat-nat\] entry [0-9a-f]+:" | awk '{print $3, $4, $4*$(NF-7)}' >> acc_$h.txt
    done
  done
done
for h in n1 kc1 kc1r; do
  awk '{n[$1]+=$2; t[$1]+=$3} END {for (e in n) printf "%s %d %.3f\n", e, n[e], t[e]}' acc_$h.txt | sort > et_s_$h.txt
done
for b in kc1 kc1r; do
  echo "== n1 vs $b"
  join et_s_n1.txt et_s_$b.txt | awk '{a=$3/$2; b=$5/$4; ta+=$3; tb+=b*$2; printf "%s runs %d n1 %.2f B %.2f us/run (%+.1f%%) totA %.0f\n", $1, $2, a, b, (b/a-1)*100, $3} END {printf "TOTAL n1 %.0f B %.0f (%+.2f%%)\n", ta, tb, (tb/ta-1)*100}' | sort -k11 -g -r
done
