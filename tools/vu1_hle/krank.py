import sys
for fn in sys.argv[1:]:
    rows = [l.split() for l in open(fn) if l.startswith('k ')]
    rows = [(r[1], int(r[2], 16), int(r[3]), int(r[4]), int(r[5])) for r in rows]
    tot = sum(r[4] for r in rows); acc = 0
    print(fn, 'blocks', len(rows), 'cycles', tot)
    for i, r in enumerate(sorted(rows, key=lambda r: -r[4])[:25]):
        acc += r[4]; print(f'  {r[0]} pc={r[1]:04x} pairs={r[2]:3d} runs={r[3]:7d} cyc={r[4]:9d} {r[4]/tot:6.1%} cum {acc/tot:6.1%} cyc/run {r[4]/r[3]:.0f}')
