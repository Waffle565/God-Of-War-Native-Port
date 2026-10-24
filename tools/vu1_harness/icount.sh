#!/bin/bash
# icount.sh <records> <aarch64 harness...>: dynamic instruction profile of one warm replay pass per harness (qemu logs of
# 3 and 1 replay passes, difference halved: the first pass also sets up the block cache). Use a few hundred records
# (head -c $((33448 * 100)) game.rec > g100.rec): the qemu log of one pass is ~1 GB per 100 records.
# Columns: insns, q (128-bit NEON instructions: two micro-ops each on the A57's 64-bit FP pipes), mem, a57_cost = insns + q;
# then per function (generated blocks summed) with its call count over the 3-pass run.
# ICARGS=--blocks: generated blocks one by one; TOP=<n>: rows printed (16); PCS=1: per-instruction counts
# pcs_<harness>_<passes>.txt (annotate a function with icann.py).
cd /root/vu1h
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rec=$1; shift
for b in "$@"; do
    aarch64-linux-gnu-objdump -d --no-show-raw-insn -C $b > $b.dis
    sed -i 's/^\s*\([0-9a-f]*\):\t/  \1:\t00000000 /' $b.dis
    for r in 1 3; do
        qemu-aarch64 -d in_asm,exec,nochain -D q_${b}_$r.log ./$b $rec $r > /dev/null 2>&1
        python3 "$HERE/icount.py" q_${b}_$r.log $b.dis $ICARGS ${PCS:+--pcs=pcs_${b}_$r.txt} > ic_${b}_$r.txt
        rm -f q_${b}_$r.log
    done
    python3 - "$b" <<'EOF'
import sys
b = sys.argv[1]
def read(r):
    t = open(f'/root/vu1h/ic_{b}_{r}.txt').read().split('\n')
    head = {kv.split('=')[0]: int(kv.split('=')[1]) for kv in t[0].split()}
    rows = {}
    for line in t[1:]:
        p = line.split(' ', 1)
        if len(p) == 2:
            rows[p[1].split(' [calls ')[0]] = int(p[0])
    return head, rows
h1, r1 = read(1); h3, r3 = read(3)
warm = {k: (h3.get(k, 0) - h1.get(k, 0)) / 2 for k in h3}
print(f"{b}: per warm pass " + " ".join(f"{k}={v / 1e6:.2f}M" for k, v in warm.items()))
d = {k: (r3.get(k, 0) - r1.get(k, 0)) / 2 for k in set(r1) | set(r3)}
tot = sum(d.values()) or 1
for k, v in sorted(d.items(), key=lambda x: -x[1])[:int(__import__('os').environ.get('TOP', '16'))]:
    print(f"  {v / 1e6:8.3f}M {100 * v / tot:5.1f}%  {k[:100]}")
EOF
done
