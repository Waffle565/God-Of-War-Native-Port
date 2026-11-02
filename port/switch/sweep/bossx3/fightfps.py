# fps of the [gow-perf] windows while fighting: after the warp arrival / teleport, last [gow-player] state = 11
# (no death / menu screens), without warp-capture neighbours; seconds of fighting
import re, sys, os
for log in sys.argv[1:]:
    t = open(log, errors='replace').read()
    m = re.search(r'\[gow-teleport\] to|\[gow-warp\] arrived', t)
    t = t[m.start():] if m else t
    st, fps, drop = None, [], False
    for l in t.splitlines():
        if l.startswith('[gow-player]'):
            s = re.search(r'state=(\d+)', l); st = int(s.group(1)) if s else st
        elif '[gow-warp] capture' in l:
            drop = True
            if fps and fps[-1][1]: fps[-1] = (fps[-1][0], False)
        elif l.startswith('[gow-perf]'):
            h = float(re.search(r'guest_flip_hz=([\d.]+)', l).group(1))
            fps.append((h, st == 11 and not drop)); drop = False
    f = [h for h, ok in fps[1:] if ok]
    name = os.path.basename(os.path.dirname(log))
    print('%-14s fight %3d s  fps min %.1f mean %.1f' % (name, 5 * len(f), min(f), sum(f) / len(f)) if f else name + ' -')
