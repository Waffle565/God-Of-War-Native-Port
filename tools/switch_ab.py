"""Per-zone A/B summary of sweep logs, windows from the warp's arrival to its end (needs GOW_PERF_DIAG, sweep default).

usage: python -I tools/switch_ab.py <sweep out dir>...   (e.g. port/switch/sweep/base_dips13)
per zone: windows, guest fps min/mean, VU1 thread (GSWorker) busy ms per 5 s and us per VU1 program run ([gsw-split] runs), VIF ms per 5 s,
EE ms (guest_wait: EE waiting) and IOP ms per 5 s, GS GL busy ms per 5 s."""
import re, sys, glob


def num(key, line):
    m = re.search(key + r'=([\d.]+)', line)
    return float(m.group(1)) if m else 0.0


for d in sys.argv[1:]:
    for L in sorted(glob.glob(d + '/*/*/log.txt')):
        arrived = False
        perf, split, nat, gl = [], [], [], []
        for l in open(L, encoding='utf-8', errors='replace'):
            if '[gow-warp] arrived' in l:
                arrived = True
            if '[gow-warp] done' in l:
                break
            if not arrived:
                continue
            if '[gow-perf]' in l:
                perf.append((num('guest_flip_hz', l), num('ee_ms', l), num('iop_ms', l), num('guest_wait_ms', l)))
            elif '[gsw-split]' in l:
                split.append((num('job_ms', l), num('vif_ms', l), num('runs', l)))
            elif '[vu1nat-run]' in l:
                m = re.search(r'native (\d+) runs', l)
                nat.append(int(m.group(1)) if m else 0)
            elif '[gs-gl]' in l:
                gl.append(num('busy_ms', l))
        perf, split, nat, gl = perf[1:], split[1:], nat[1:], gl[1:]  # the warp window itself is partly loading
        if not perf or not split:
            print(L, 'no windows after the warp')
            continue
        avg = lambda xs: sum(xs) / len(xs) if xs else 0.0
        fps = [p[0] for p in perf]
        job, vif = avg([s[0] for s in split]), avg([s[1] for s in split])
        runs = avg([s[2] for s in split]) or avg(nat)  # VU1 programs run by the worker ([gsw-split] runs)
        ee, iop, gw = avg([p[1] for p in perf]), avg([p[2] for p in perf]), avg([p[3] for p in perf])
        name = L.replace('\\', '/').split('/')
        print('%-24s %-8s n=%2d fps %4.1f/%4.1f  vu1thr %6.0f ms %5.2f us/run (vif %4.0f)  ee %5.0f (wait %4.0f) iop %4.0f  gl %5.0f'
              % (name[-4][:24], name[-2], len(fps), min(fps), avg(fps), job, job * 1000.0 / runs if runs else 0, vif,
                 ee, gw, iop, avg(gl)))
