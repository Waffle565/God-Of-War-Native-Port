"""hottb.py <qemu log> <dis> <function substring> [n]: hottest translation blocks inside one function, with their code."""
import collections
import re
import sys

log, dis, fsub = sys.argv[1], sys.argv[2], sys.argv[3]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 12
text = {}
func = None
re_f = re.compile(r'^([0-9a-f]+) <(.*)>:$')
re_i = re.compile(r'^\s+([0-9a-f]+):\s+[0-9a-f]{8}\s+(.*)$')
for line in open(dis, errors='replace'):
    m = re_f.match(line)
    if m:
        func = m.group(2)
        continue
    m = re_i.match(line)
    if m and func and fsub in func:
        text[int(m.group(1), 16)] = m.group(2)
tbs = {}
counts = collections.Counter()
cur = None
re_exec = re.compile(r'^Trace \d+: 0x[0-9a-f]+ \[[0-9a-f]+/([0-9a-f]+)/')
with open(log, errors='replace') as f:
    for line in f:
        if line.startswith('IN:'):
            cur = 'start'
        elif cur == 'start' and line.startswith('0x'):
            cur = int(line.split(':')[0], 16)
            tbs[cur] = 0
        elif isinstance(cur, int) and line.startswith('OBJD-T:'):
            tbs[cur] += len(line.split()[1]) // 2
        elif line.startswith('Trace'):
            cur = None
            m = re_exec.match(line)
            if m:
                pc = int(m.group(1), 16)
                if pc in text:
                    counts[pc] += 1
weighted = sorted(counts.items(), key=lambda kv: -kv[1] * (tbs.get(kv[0], 0) // 4))
total = sum(c * (tbs.get(pc, 0) // 4) for pc, c in counts.items())
print(f"total insns in {fsub}: {total}")
for pc, c in weighted[:top]:
    n = tbs.get(pc, 0) // 4
    print(f"--- tb {pc:x}: {c} x {n} insns = {c * n}")
    for a in range(pc, pc + n * 4, 4):
        print(f"   {a:x}: {text.get(a, '?')}")
