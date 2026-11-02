# A/B summary: per zone fps (report), median VU1 native ms per 5 s window after the warp
import re, sys, os, glob, statistics as st
for d in sys.argv[1:]:
    run = sorted(glob.glob(os.path.join(d, '*/')))[-1]
    for z in sorted(os.listdir(run)):
        lp = os.path.join(run, z, 'log.txt')
        if not os.path.exists(lp):
            continue
        on, ms = False, []
        for l in open(lp, errors='replace'):
            if '[gow-warp] arrived' in l: on = True
            m = re.match(r'\[vu1nat-run\] window=[\d.]+ native \d+ runs ([\d.]+) ms', l)
            if on and m: ms.append(float(m.group(1)))
        print(d.split('/')[-1], z, 'vu1 ms/5s med %.0f' % st.median(ms) if ms else '-')
