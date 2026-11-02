"""glb.py <sweep dir>...: per run, the windows after the warp: fps, GS GL busy ms, GL ms per frame; mean over the
GL-bound windows (busy >= 4300 ms of 5000) and the others."""
import glob, re, sys
num = lambda k, l: float(re.search(k + r'=([\d.]+)', l).group(1))
for d in sys.argv[1:]:
    for L in sorted(glob.glob(d + '/*/*/log.txt')):
        arrived, perf, gl = False, [], []
        for l in open(L, encoding='utf-8', errors='replace'):
            if '[gow-warp] arrived' in l: arrived = True
            if '[gow-warp] done' in l: break
            if not arrived: continue
            if '[gow-perf]' in l: perf.append(num('guest_flip_hz', l))
            elif '[gs-gl]' in l: gl.append(num('busy_ms', l))
        w = list(zip(perf[1:], gl[1:]))
        b = [x for x in w if x[1] >= 4300]; o = [x for x in w if x[1] < 4300]
        f = lambda xs: '%d win fps %.1f gl %.0f ms (%.1f ms/frame)' % (len(xs), sum(x[0] for x in xs) / len(xs), sum(x[1] for x in xs) / len(xs), sum(x[1] / x[0] / 5 for x in xs) / len(xs)) if xs else '-'
        print('%-40s bound: %s | other: %s' % (L.replace(chr(92), '/').split('sweep/')[1][:40], f(b), f(o)))
