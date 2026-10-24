"""icann.py <harness> <function substring> [min count]: a function's disassembly (/root/vu1h/<harness>.dis, written by
icount.sh) annotated with the executed count of each instruction per warm replay pass (icount.sh with PCS=1: the
difference of the 3-pass and 1-pass counts, halved). Prints every instruction with its count; lines below the minimum
count (default 1) are folded into '...'. Ends with the function's total and the counts by mnemonic."""
import collections
import re
import sys

h, fn = sys.argv[1], sys.argv[2]
lo = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
D = '/root/vu1h'


def counts(r):
    c = {}
    for line in open(f'{D}/pcs_{h}_{r}.txt'):
        a, n = line.split()
        c[int(a, 16)] = int(n)
    return c


c1, c3 = counts(1), counts(3)
re_f = re.compile(r'^([0-9a-f]+) <(.*)>:$')
re_i = re.compile(r'^\s+([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)$')
inside = False
tot = 0.0
ops = collections.Counter()
folded = False
for line in open(f'{D}/{h}.dis', errors='replace'):
    m = re_f.match(line)
    if m:
        if inside:
            break
        inside = fn in m.group(2)
        if inside:
            print(m.group(2))
        continue
    if not inside:
        continue
    m = re_i.match(line)
    if not m:
        continue
    a = int(m.group(1), 16)
    w = (c3.get(a, 0) - c1.get(a, 0)) / 2
    tot += w
    ops[m.group(2)] += w
    if w >= lo:
        print(f'{w:9.0f}  {a:x}: {m.group(2)} {m.group(3)}')
        folded = False
    elif not folded:
        print('      ...')
        folded = True
print(f'total {tot:.0f}')
print(' '.join(f'{k}={v:.0f}' for k, v in ops.most_common(30)))
