# A/B summary per zone: fps (report), EE+IOP ms/frame (median, windows without warp-capture neighbours), VU1 native
# ms/5s (median), [gsw-chain-seg] medians (count/KB per class) -- all after [gow-warp] arrived
import re, sys, os, glob, statistics as st
def med(v): return st.median(v) if v else float('nan')
for d in sys.argv[1:]:
    run = sorted(glob.glob(os.path.join(d, '*/')))[-1]
    rep = open(os.path.join(run, 'report.md'), encoding='utf-8').read()
    for z in sorted(x for x in os.listdir(run) if os.path.isdir(os.path.join(run, x))):
        if not os.path.exists(os.path.join(run, z, 'log.txt')): continue
        t = open(os.path.join(run, z, 'log.txt'), errors='replace').read()
        a = t.find('[gow-warp] arrived'); t = t[a:] if a >= 0 else t
        lines = t.splitlines(); cap = {i for i, l in enumerate(lines) if '[gow-warp] capture' in l}
        perf = [(i, l) for i, l in enumerate(lines) if l.startswith('[gow-perf]')]
        drop = set()
        for c in cap:
            before = [k for k, (i, _) in enumerate(perf) if i < c][-1:]; after = [k for k, (i, _) in enumerate(perf) if i > c][:1]
            drop.update(before + after)
        ms = []
        for k, (i, l) in enumerate(perf):
            if k in drop: continue
            g = dict(re.findall(r'(\w+)=([\d.]+)', l)); fr = float(g['guest_flip_hz']) * float(g['window'])
            if fr > 0: ms.append((float(g['ee_ms']) + float(g['iop_ms'])) / fr)
        vu = [float(x) for x in re.findall(r'\[vu1nat-run\] window=[\d.]+ native \d+ runs ([\d.]+) ms', t)]
        seg = [tuple(map(int, re.findall(r'=(\d+)/(\d+)KB', l)[j])) for l in lines if l.startswith('[gsw-chain-seg]') for j in range(3)]
        seg3 = [seg[i:i + 3] for i in range(0, len(seg), 3)]
        segs = ' '.join('%s %d/%dKB' % (n, med([s[j][0] for s in seg3]), med([s[j][1] for s in seg3])) for j, n in enumerate(('lt1k', 'lt16k', 'ge16k'))) if seg3 else '-'
        m = re.search(r'\| %s \| (\w+) \| [\d.]+ \| ([\d.]+) / ([\d.]+)' % z, rep)
        print('%s %s %s fps %s/%s ee+iop %.1f ms/f vu1 %.0f ms/5s seg %s' % (d.rstrip('/').split('/')[-1], z, m.group(1) if m else '?', m.group(2) if m else '?', m.group(3) if m else '?', med(ms), med(vu), segs))
