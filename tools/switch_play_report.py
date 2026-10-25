"""Normal-play report of a console log (NOBENCH=1 runs with GOW_DELTA_DIAG=1, GOW_PERF_DIAG=1; sw-stall).

usage: python -I tools/switch_play_report.py <log.txt> [...]
Per log: fps / speed in the 1 s [gow-speed] windows grouped by fps (40-52, 52-57, >= 57), long frames
([gow-fclk-long] count, per minute of gameplay, mean / max wall fields), GS-thread blocks ([gs-dk-long] by kind) and
the [gs-dk-stall] present / frames totals.
"""
import re
import sys


def report(path):
    speeds = {'40-52': [], '52-57': [], '>=57': []}
    longs, dklong, present, frames = [], {}, [0.0, 0], [0.0, 0]
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('[gow-speed] '):
            m = re.search(r'frames=(\d+) wall=([\d.]+) game=([\d.]+) speed=([\d.]+)', line)
            if not m:
                continue
            fps = int(m.group(1)) / float(m.group(2))
            key = '40-52' if 40 <= fps < 52 else '52-57' if 52 <= fps < 57 else '>=57' if fps >= 57 else None
            if key:
                speeds[key].append((fps, float(m.group(4))))
        elif line.startswith('[gow-fclk-long] '):
            m = re.search(r'wall_fields=([\d.]+)', line)
            if m:
                longs.append(float(m.group(1)))
        elif line.startswith('[gs-dk-long] '):
            k = line.split()[1]
            dklong[k] = dklong.get(k, 0) + 1
        elif line.startswith('[gs-dk-stall] '):
            for name, acc in (('present', present), ('frames', frames)):
                m = re.search(name + r'=(\d+)/([\d.]+)ms/>8:(\d+)/>25:(\d+)', line)
                if m:
                    acc[0] += float(m.group(2))
                    acc[1] += int(m.group(4))
    print(path)
    for k, v in speeds.items():
        if v:
            print(f'  {k:>6} fps: {len(v):4d} windows, fps {sum(f for f, _ in v) / len(v):5.1f}, '
                  f'speed {sum(s for _, s in v) / len(v):.3f}')
    play_s = sum(len(v) for v in speeds.values())
    if longs:
        print(f'  long frames: {len(longs)} ({len(longs) * 60.0 / max(play_s, 1):.1f}/min of 40+ fps play), '
              f'wall fields mean {sum(longs) / len(longs):.1f} max {max(longs):.1f}')
    else:
        print('  long frames: 0')
    print(f'  gs-dk-long: {dklong}  present blocked {present[0]:.0f} ms (>25 ms: {present[1]}), '
          f'frames wait {frames[0]:.0f} ms (>25 ms: {frames[1]})')


for p in sys.argv[1:]:
    report(p)
