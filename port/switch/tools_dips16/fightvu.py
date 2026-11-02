"""fightvu.py <log>...: fight windows as bossx3/fightfps.py (state 11, no warp captures), with the VU1 thread per
window ([gsw-split] of the same window): fps min / mean, job ms per 5 s, us per VU1 program run, job ms per frame."""
import re, sys, os
for log in sys.argv[1:]:
    t = open(log, errors='replace').read()
    m = re.search(r'\[gow-teleport\] to|\[gow-warp\] arrived', t)
    t = t[m.start():] if m else t
    st, rows, drop, split = None, [], False, None
    for l in t.splitlines():
        if l.startswith('[gow-player]'):
            s = re.search(r'state=(\d+)', l); st = int(s.group(1)) if s else st
        elif '[gow-warp] capture' in l:
            drop = True
            if rows and rows[-1][1]: rows[-1] = (rows[-1][0], False, rows[-1][2])
        elif l.startswith('[gsw-split]'):
            split = (float(re.search(r'job_ms=([\d.]+)', l).group(1)), float(re.search(r'runs=(\d+)', l).group(1)))
        elif l.startswith('[gow-perf]'):
            h = float(re.search(r'guest_flip_hz=([\d.]+)', l).group(1))
            rows.append((h, st == 11 and not drop, split)); drop = False; split = None
    f = [(h, s) for h, ok, s in rows[1:] if ok and s]
    name = os.path.basename(os.path.dirname(log))
    if not f:
        print(name, '-'); continue
    fps = [h for h, s in f]; job = sum(s[0] for h, s in f) / len(f); runs = sum(s[1] for h, s in f) / len(f)
    pf = sum(s[0] / (h * 5) for h, s in f) / len(f)
    print('%-12s %3d s fps %.1f / %.1f  vu1 job %4.0f ms/5s  %.2f us/run  %.1f ms/frame  (%s)' % (
        name, 5 * len(f), min(fps), sum(fps) / len(fps), job, job * 1000 / runs, pf, log.replace(chr(92), '/').split('sweep/')[-1][:24]))
