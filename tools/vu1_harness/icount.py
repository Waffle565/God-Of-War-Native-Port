"""icount.py <qemu log> <objdump -d output>: dynamic aarch64 instruction profile from `qemu-aarch64 -d in_asm,exec,nochain`.
Counts every executed instruction (TB start + byte length from the log, instruction text from objdump), classifies
Q-register (128-bit NEON) instructions (two micro-ops each on the Cortex-A57's 64-bit FP pipes) and loads/stores, and
splits an A57 cost estimate (instructions + Q instructions) by function."""
import collections
import re
import sys

log, dis = sys.argv[1], sys.argv[2]
KEEP_BLOCKS = '--blocks' in sys.argv
insn = {}      # pc -> (is_q, is_mem, function)
func = '?'
starts = {}
re_f = re.compile(r'^([0-9a-f]+) <(.*)>:$')
re_i = re.compile(r'^\s+([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)$')
for line in open(dis, errors='replace'):
    m = re_i.match(line)
    if m:
        op, args = m.group(2), m.group(3)
        isq = bool(re.search(r'\bv\d+\.(4s|2d|16b|8h)\b|\bq\d+\b', args))
        insn[int(m.group(1), 16)] = (isq, op.startswith(('ld', 'st')), func)
        continue
    m = re_f.match(line)
    if m:
        func = m.group(2)
        starts[int(m.group(1), 16)] = func
        if 'VU1NativeCtx' in func and func[:1] in ('b', 'r') and not KEEP_BLOCKS:
            func = '<generated blocks>'

# A guest pc can be translated more than once with different lengths (the translation cache fills up with large
# binaries and is flushed): each executed TB is identified by its host code pointer (Trace lines), mapped to the length
# of the translation logged just before its first execution; counts are kept per (pc, length).
tbs = {}       # start pc -> byte length of its latest translation
host = {}      # host TB pointer -> byte length of the translation it holds
counts = collections.Counter()
cur = None
fresh = None   # (pc, length) of the translation logged last, not yet executed
re_exec = re.compile(r'^Trace \d+: (0x[0-9a-f]+) \[[0-9a-f]+/([0-9a-f]+)/')
with open(log, errors='replace') as f:
    for line in f:
        if line.startswith('IN:'):
            cur = 'start'
            continue
        if cur == 'start' and line.startswith('0x'):
            cur = int(line.split(':')[0], 16)
            tbs[cur] = 0
            fresh = None
            continue
        if isinstance(cur, int) and line.startswith('OBJD-T:'):
            tbs[cur] += len(line.split()[1]) // 2
            fresh = (cur, tbs[cur])
            continue
        if line.startswith('Trace'):
            cur = None
            m = re_exec.match(line)
            if m:
                pc = int(m.group(2), 16)
                if fresh and fresh[0] == pc:
                    host[m.group(1)] = fresh[1]
                    fresh = None
                counts[(pc, host.get(m.group(1), tbs.get(pc, 0)))] += 1

tot = collections.Counter()
by = collections.Counter()
for (pc, length), c in counts.items():
    for a in range(pc, pc + length, 4):
        i = insn.get(a)
        if not i:
            tot['unknown'] += c
            continue
        tot['insns'] += c
        tot['q'] += c * i[0]
        tot['mem'] += c * i[1]
        by[i[2]] += c * (1 + i[0])
print(" ".join(f"{k}={v}" for k, v in tot.items()) + f" a57_cost={tot['insns'] + tot['q']}")
# --pcs=<file>: executed count of every instruction address (annotate a disassembly with icann.py)
for a in sys.argv[3:]:
    if a.startswith('--pcs='):
        per = collections.Counter()
        for (pc, length), c in counts.items():
            for x in range(pc, pc + length, 4):
                per[x] += c
        with open(a[6:], 'w') as o:
            for x, c in sorted(per.items()):
                o.write(f"{x:x} {c}\n")
entries = collections.Counter()
for (pc, _), c in counts.items():
    if pc in starts:
        f = starts[pc]
        if 'VU1NativeCtx' in f and f[:1] in ('b', 'r') and not KEEP_BLOCKS:
            f = '<generated blocks>'
        entries[f] += c
for name, c in by.most_common(None if '--all' in sys.argv else 25):  # --all: every function (diffs of two runs)
    print(f"{c} {name} [calls {entries.get(name, 0)}]")
