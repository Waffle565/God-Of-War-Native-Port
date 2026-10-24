"""icfun.py <harness> [top]: per-function warm-pass instruction counts from icount.sh's PCS=1 files (pcs_<h>_1/3.txt),
without re-running qemu: every function (generated blocks one by one), the harness's own silentNativeHash left out,
plus category totals (generated blocks and traces, kernels vu1hle::, everything else)."""
import bisect
import re
import sys

h = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
D = '/root/vu1h'
re_f = re.compile(r'^([0-9a-f]+) <(.*)>:$')
starts, names = [], []
for line in open(f'{D}/{h}.dis', errors='replace'):
    m = re_f.match(line)
    if m:
        starts.append(int(m.group(1), 16))
        names.append(m.group(2))


def counts(r):
    c = {}
    for line in open(f'{D}/pcs_{h}_{r}.txt'):
        a, n = line.split()
        c[int(a, 16)] = int(n)
    return c


c1, c3 = counts(1), counts(3)
by = {}
for a in set(c1) | set(c3):
    w = (c3.get(a, 0) - c1.get(a, 0)) / 2
    i = bisect.bisect_right(starts, a) - 1
    f = names[i] if i >= 0 else '?'
    by[f] = by.get(f, 0) + w
by.pop('VU1NativeAccess::silentNativeHash(VU1Interpreter&, void*)', None)
tot = sum(by.values())
cat = {'blocks': 0, 'kernels': 0, 'other': 0}
for f, w in by.items():
    if 'VU1NativeCtx&)' in f and f[:1] in 'btr' and re.match(r'^[btr][0-9a-f]{4}_', f):
        cat['blocks'] += w
    elif f.startswith(('vu1hle::', 'unsigned int vu1hle::')):
        cat['kernels'] += w
    else:
        cat['other'] += w
print(f'{h}: {tot / 1e6:.3f} M insns per warm pass (without the harness hash); ' +
      ' '.join(f'{k} {v / 1e6:.3f} M ({100 * v / tot:.1f} %)' for k, v in cat.items()))
for f, w in sorted(by.items(), key=lambda x: -x[1])[:top]:
    print(f'  {w / 1e6:8.4f}M {100 * w / tot:5.1f}%  {f[:110]}')
