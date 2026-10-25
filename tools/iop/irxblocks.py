#!/usr/bin/env python3
"""Basic blocks of one IRX function with their instruction counts (for the IOP-cycle accounting of native overrides:
a native function charges the instructions the IRX code runs, docs/sound.md).

usage: irxblocks.py <listing from irxlist.py> <function offset hex> [end offset hex]

A block ends after a branch / jump and its delay slot (the delay slot is counted in the block that branches) or before
a branch target. Each line: start-end, instructions, how it ends (-> taken target / fall-through, calls)."""
import re, sys

ins = {}
for line in open(sys.argv[1]):
    m = re.match(r'([0-9a-f]{5}): (\S+)\s*(.*)', line)
    if m:
        ins[int(m.group(1), 16)] = (m.group(2), m.group(3).split(';')[0].strip(), m.group(3))
start = int(sys.argv[2], 16)

BR = ('beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'beqz', 'bnez', 'bltzal', 'bgezal', 'b')


def target(op):
    return int(op.split(',')[-1].strip(), 16)


# function end: first "jr $ra" at or after the farthest branch target seen (+ its delay slot)
end = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
if end is None:
    a, far = start, start
    while a in ins:
        mn, op, _ = ins[a]
        if mn in BR or mn == 'j':
            far = max(far, target(op))
        if mn == 'jr' and op == '$ra' and a >= far:
            end = a + 8
            break
        a += 4
leaders = {start}
for a in range(start, end, 4):
    mn, op, _ = ins[a]
    if mn in BR or mn == 'j':
        t = target(op)
        if start <= t < end:
            leaders.add(t)
        leaders.add(a + 8)
    elif mn in ('jr', 'jalr', 'jal'):
        leaders.add(a + 8) if mn == 'jr' else None
leaders = sorted(x for x in leaders if start <= x < end)
for i, b in enumerate(leaders):
    e = leaders[i + 1] if i + 1 < len(leaders) else end
    calls, how = [], 'falls'
    for a in range(b, e, 4):
        mn, op, full = ins[a]
        if mn == 'jal':
            calls.append(full.split(';')[1].strip() if ';' in full else op)
        elif mn == 'jalr':
            calls.append('jalr')
        if mn in BR:
            how = '-> %x / falls' % target(op)
        elif mn == 'j':
            how = '-> %x' % target(op)
        elif mn == 'jr':
            how = 'return' if op == '$ra' else 'jr ' + op
    print('%05x-%05x %3d %s%s' % (b, e - 4, (e - b) // 4, how, ('  calls ' + ' '.join(calls)) if calls else ''))
