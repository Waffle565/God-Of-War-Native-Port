# Card image diff: python -I tools/cardcmp.py BASE.ps2 A.ps2 B.ps2 (pages changed vs BASE in A and B, identical or not)
import sys
RAW=528
base, a, b = (open(p,'rb').read() for p in sys.argv[1:4])
def changed(x): return [p for p in range(len(base)//RAW) if base[p*RAW:(p+1)*RAW] != x[p*RAW:(p+1)*RAW]]
ca, cb = changed(a), changed(b)
print('pages changed: oracle', len(ca), 'native', len(cb), 'same page set:', ca == cb)
same = [p for p in ca if a[p*RAW:(p+1)*RAW] == b[p*RAW:(p+1)*RAW]]
print('identical changed pages:', len(same), 'of', len(ca))
diff = [p for p in ca if p in cb and a[p*RAW:(p+1)*RAW] != b[p*RAW:(p+1)*RAW]]
for p in diff[:12]:
    x, y = a[p*RAW:(p+1)*RAW], b[p*RAW:(p+1)*RAW]
    d = [i for i in range(RAW) if x[i] != y[i]]
    print(' page', p, 'bytes differ:', len(d), 'first', d[:8])
