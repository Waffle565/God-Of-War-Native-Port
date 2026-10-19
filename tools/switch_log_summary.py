"""Summary of a Switch console log (log switch/*.txt): speed, GL thread, GPU per draw path, GPU bisect phases.

usage: python -I tools/switch_log_summary.py "log switch/log (3).txt"
Reads the [gow-perf], [gsw-split], [vu1-split], [gs-stage-run], [IOP:thread], [gs-gl], [gs-gpu] and [gpu-bisect] lines (docs/switch.md "Console logs").
"""
import re
import sys
from collections import defaultdict


def fields(line):
    return {k: v for k, v in re.findall(r'(\w+)=([^\s]+)', line)}


def main(path):
    text = open(path, encoding='utf-8', errors='replace').read().splitlines()
    print(f'# {path}')
    for line in text:
        if 'renderer ready' in line or 'host mode' in line or 'shared with the host' in line:
            print(line.strip())

    perf = [fields(l) for l in text if l.startswith('[gow-perf]')]
    if perf:
        print('\n## [gow-perf] (per window)')
        print('t | guest_fps | host_hz | ee_ms | iop_ms | vu_ms | gs_ms | guest_wait_ms | gsq_wait_ms')
        for f in perf:
            print(' | '.join(f.get(k, '-') for k in ('t', 'guest_flip_hz', 'host_hz', 'ee_ms', 'iop_ms', 'vu_ms', 'gs_ms',
                                                    'guest_wait_ms', 'gsq_wait_ms')))

    split = [fields(l) for l in text if l.startswith('[gsw-split]')]
    if split:
        print('\n## [gsw-split] GS worker / VU1 thread (per window, ms; vif_ms = job - vu without PS2X_VU1_ASYNC)')
        print('job_ms | vu_ms | kick_ms | vif_ms | runs | kicks | jobs | async_wait_ms')
        for f in split:
            print(' | '.join(f.get(k, '-') for k in ('job_ms', 'vu_ms', 'kick_ms', 'vif_ms', 'runs', 'kicks', 'jobs',
                                                    'async_wait_ms')))

    vsp = [fields(l) for l in text if l.startswith('[vu1-split] window')]
    if vsp:
        print('\n## [vu1-split] data-parallel vertex loops (GOW_VU1_SPLIT; per window; docs/vu1-hle.md)')
        keys = ('probes', 'plans', 'adopted', 'blocks_saved', 'not_started', 'stall', 'mismatch', 'helper_fail', 'mem',
                'wait_ms', 'helper_ms')
        print(' | '.join(keys))
        for f in vsp:
            print(' | '.join(f.get(k, '-') for k in keys))

    run = [fields(l) for l in text if l.startswith('[gs-stage-run]')]
    if any(int(f.get('kicks', 0)) for f in run):
        print('\n## [gs-stage-run] GS frontend runs (per window; fast = kicks of the PACKED fast path, GOW_GS_FASTPACK)')
        print('runs | prims | kicks | queue_ms | fast | check | diffs')
        for f in run:
            if int(f.get('kicks', 0)) or int(f.get('prims', 0)) > 1000:
                print(' | '.join(f.get(k, '-') for k in ('runs', 'prims', 'kicks', 'queue_ms', 'fast', 'check', 'diffs')))

    iop = [l for l in text if l.startswith('[IOP:thread] window')]
    if iop:
        print('\n## [IOP:thread] IOP host thread (per window; thread_ms = IopRun busy, held/takeback = cost on the EE,')
        print('## lead_waits = EE waits for the thread (ms in guest_wait_ms))')
        print('thread_ms | sleeps | held_ms | lock_wait_ms | takeback_ms | syncs | lead_waits | lead_wait_ms | wakes | effects')
        for l in iop:
            f = fields(l)
            sleeps = re.search(r'(\d+) sleeps', l)
            lw = re.search(r'lead_waits=(\d+) \(([\d.]+) ms\)', l)
            print(' | '.join([f.get('thread_ms', '-'), sleeps.group(1) if sleeps else '-', f.get('held_ms', '-'),
                              f.get('lock_wait_ms', '-'), f.get('takeback_ms', '-'), f.get('syncs', '-'),
                              lw.group(1) if lw else '-', lw.group(2) if lw else '-', f.get('wakes', '-'),
                              f.get('effects', '-')]))

    gl = [l for l in text if l.startswith('[gs-gl] window')]
    if gl:
        print('\n## [gs-gl] GL/deko3d thread busy_ms per window: ' +
              ' '.join(fields(l).get('busy_ms', '-') for l in gl))
        print('\n## [gs-gl] GL thread, ms per op kind summed over the log')
        busy = 0.0
        ops = defaultdict(float)
        for l in gl:
            busy += float(fields(l).get('busy_ms', 0))
            for name, ms in re.findall(r' (\w+)=([\d.]+)ms/\d+', l):
                ops[name] += float(ms)
        print(f'busy {busy:.0f} ms in {len(gl)} windows: ' +
              ', '.join(f'{k} {v:.0f}' for k, v in sorted(ops.items(), key=lambda kv: -kv[1])))

    gpu = [l for l in text if l.startswith('[gs-gpu]')]
    if gpu:
        print('\n## [gs-gpu] GPU ms per draw path summed over the log (T tex, I indexed, R rt source, X reinterpret,')
        print('## Z depth source, D date, B shader blend, A z alias, V barrier runs, F fail pass, L linear)')
        total = defaultdict(lambda: [0.0, 0, 0])
        for l in gpu:
            for name, ms, n, kpx in re.findall(r' ([\w:]+)=([\d.]+)ms/(\d+)/(\d+)Kpx', l):
                t = total[name]
                t[0] += float(ms)
                t[1] += int(n)
                t[2] += int(kpx)
        all_ms = sum(t[0] for t in total.values()) or 1.0
        for name, (ms, n, kpx) in sorted(total.items(), key=lambda kv: -kv[1][0])[:16]:
            print(f'{name:14s} {ms:9.1f} ms {100 * ms / all_ms:5.1f} %  {n:7d} spans  {ms / max(n, 1):7.3f} ms/span  {kpx} Kpx')

    phases = [fields(l) for l in text if l.startswith('[gpu-bisect]')]
    if phases:
        print('\n## [gpu-bisect] presents/s of each "off" phase vs the mean of the baseline phases around it')
        for i, f in enumerate(phases):
            if f.get('mode') == 'base':
                continue
            around = [float(phases[j]['presents_hz']) for j in (i - 1, i + 1)
                      if 0 <= j < len(phases) and phases[j].get('mode') == 'base']
            base = sum(around) / len(around) if around else float('nan')
            off = float(f['presents_hz'])
            print(f"{f['mode']:16s} {off:6.2f}/s  base {base:6.2f}/s  x{off / base if base else float('nan'):.2f}  "
                  f"gpu {f.get('gpu_ms_per_s', '-')} ms/s")


if __name__ == '__main__':
    main(sys.argv[1])
