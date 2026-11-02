#!/usr/bin/env python3
"""Per-instruction PC-sample annotation of one function: annot.py <pcs.txt> <elf> <symbol substring> [lo_ms hi_ms]
Prints the disassembly with sample counts (needs aarch64-linux-gnu-nm / objdump)."""
import collections, subprocess, sys
path, elf, fn = sys.argv[1:4]
lo, hi = (int(sys.argv[4]), int(sys.argv[5])) if len(sys.argv) > 5 else (0, 1 << 62)
hist = collections.Counter(); use = False
for line in open(path):
    p = line.split()
    if not p: continue
    if p[0] == 'w': use = lo <= int(p[1]) <= hi; continue
    if use and p[0] != 'c': hist[int(p[0], 16)] += int(p[1])
out = subprocess.run(['aarch64-linux-gnu-nm', '-C', '-S', '--defined-only', elf], capture_output=True, text=True).stdout
for l in out.splitlines():
    p = l.split(' ', 3)
    if len(p) == 4 and fn in p[3]:
        a, sz = int(p[0], 16), int(p[1], 16); break
dis = subprocess.run(['aarch64-linux-gnu-objdump', '-d', '--no-show-raw-insn', '--start-address=%#x' % a,
                      '--stop-address=%#x' % (a + sz), elf], capture_output=True, text=True).stdout
tot = sum(n for o, n in hist.items() if a <= o < a + sz)
print('total', tot)
for l in dis.splitlines():
    s = l.strip()
    if ':' in s:
        try: o = int(s.split(':')[0], 16)
        except ValueError: print(l); continue
        n = hist.get(o, 0)
        print('%6d %s' % (n, s))
