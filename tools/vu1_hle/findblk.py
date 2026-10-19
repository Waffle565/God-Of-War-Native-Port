import sys, struct, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vudis
rs = 33448; st = 664
def fnv(b):
    h = 1469598103934665603
    for x in b: h = ((h ^ x) * 1099511628211) & 0xffffffffffffffff
    return h
prof, rec, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
want = {}
for l in open(prof):
    if l.startswith('k '):
        r = l.split(); 
        if int(r[5]) > 0.005 * 3e6: want[int(r[1], 16)] = (int(r[2], 16), int(r[3]))
data = open(rec, 'rb').read()
os.makedirs(outdir, exist_ok=True)
for i in range(len(data) // rs):
    code = data[i*rs+16+st:i*rs+16+st+16384]
    for k, (pc, pairs) in list(want.items()):
        if fnv(code[pc:pc+pairs*8]) == k:
            lo = max(0, pc - 8*40)
            with open(f'{outdir}/{k:016x}_{pc:04x}.dis', 'w') as f:
                f.write(f'# record {i}, block {pc:04x} pairs {pairs}\n')
                f.write('\n'.join(vudis.listing(code, lo, 40 + pairs + 40)))
            del want[k]
print('not found', [hex(k) for k in want])
