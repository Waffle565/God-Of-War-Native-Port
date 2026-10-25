import re, sys
# famavg.py <log.txt>...: run-weighted us per run per family over all full [vu1nat-run] windows after the first 2
for path in sys.argv[1:]:
    lines = [l for l in open(path, encoding='utf-8', errors='replace') if '[vu1nat-run] window=5.0' in l][2:]
    tot = {}
    for l in lines:
        for m in re.finditer(r'(\w+) nat (\d+) x ([\d.]+) us, rec (\d+) x ([\d.]+) us', l):
            f = m.group(1)
            a = tot.setdefault(f, [0, 0.0, 0, 0.0])
            a[0] += int(m.group(2)); a[1] += int(m.group(2)) * float(m.group(3))
            a[2] += int(m.group(4)); a[3] += int(m.group(4)) * float(m.group(5))
    out = []
    for f, a in tot.items():
        out.append('%s nat %.2f (%d) rec %.2f (%d)' % (f, a[1] / a[0] if a[0] else 0, a[0], a[3] / a[2] if a[2] else 0, a[2]))
    print(path.split('/')[-2] if '/' in path else path, '%d windows |' % len(lines), '; '.join(out))
