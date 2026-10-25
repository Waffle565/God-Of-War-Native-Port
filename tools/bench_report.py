#!/usr/bin/env python3
"""Report of a deterministic benchmark run (GOW_BENCH=1, docs/benchmark.md).

usage: bench_report.py <log.txt> [--ref <other log.txt>] [--scenes a-b,c-d,...] [--windows]

- Scenes are ranges of game seconds (frame index / 60 since the gameplay start); per scene: fps over exactly those
  frames (from the `[bench]` lines' wall times) and the per-thread busy of the 5 s windows that lie inside it, in ms per
  5 s: ee (EE game code, core 1), iop (inline IOP, core 1), core1 = ee + iop, vu1 (VU1 thread job_ms), idle (VU1 thread
  waits), chain (EE chain walk + copy), gsgl (GS stage busy), pres (deko3d present_submit), gsq (EE waits for the GS queue),
  gpu (GPU busy, `[gs-dk-gpu]`: GOW_PERF_DIAG=1 GOW_DK_GPU_TIME=1, docs/switch-deko3d.md), acq (of it, the GPU's wait for a
  swapchain image). Columns without their log lines show -1.
- --ref: compares the `[bench]` signatures (Kratos position, health, rand seed every 60 frames) with another run and
  prints the first frame that differs: equal signatures = the same frames; for differing seeds also how many rand calls
  one run is ahead of the other (a few, positions equal: rand calls shifted across a frame boundary by timing).
- --windows: also prints every 5 s window with the frame range it covered.
"""
import re
import sys

DEFAULT_SCENES = '0-30,30-60,60-90,90-120,120-150,150-180,180-210,210-240'


def lcg_steps(a, b, limit=4096):
    """Steps of the game's rand LCG from seed a to seed b (None beyond limit)."""
    for n in range(limit + 1):
        if a == b:
            return n
        a = (a * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
    return None


def parse(path):
    lines = open(path, 'rb').read().decode('utf-8', 'replace').splitlines()
    windows, bench, cur = [], [], {}
    pending = {'frames': []}

    def kv(line):
        return {k: float(v) for k, v in re.findall(r'(\w+)=(-?[0-9.]+)', line)}

    gsw, idle, chain, gl, dk, gpu = [], [], [], [], [], []
    for line in lines:
        if line.startswith('[bench] f='):
            m = re.match(r'\[bench\] f=(\d+) wall=([0-9.]+) (.*)', line)
            f, wall, sig = int(m.group(1)), float(m.group(2)), m.group(3)
            bench.append((f, wall, sig))
            pending['frames'].append(f)
        elif line.startswith('[gow-perf]'):
            d = kv(line)
            d['frames'] = pending['frames']
            windows.append(d)
            pending = {'frames': []}
        elif line.startswith('[gsw-split]'):
            gsw.append(kv(line))
        elif line.startswith('[gsw-idle]'):
            idle.append(kv(line.split('|')[0]))
        elif line.startswith('[gsw-chain]'):
            chain.append(kv(line))
        elif line.startswith('[gs-gl]'):
            gl.append(kv(line))
        elif line.startswith('[gs-dk-time]'):
            dk.append(kv(line))
        elif line.startswith('[gs-dk-gpu]'):
            gpu.append(kv(line))
    for i, w in enumerate(windows):
        w['vu1'] = gsw[i].get('job_ms', -1) if i < len(gsw) else -1
        w['idle'] = idle[i].get('idle_ms', -1) if i < len(idle) else -1
        w['chain'] = chain[i].get('chain_ms', -1) if i < len(chain) else -1
        w['gsgl'] = gl[i].get('busy_ms', -1) if i < len(gl) else -1
        w['pres'] = dk[i].get('present_submit_ms', -1) if i < len(dk) else -1
        w['gpu'] = gpu[i].get('busy_ms', -1) if i < len(gpu) else -1
        w['acq'] = gpu[i].get('acquire', -1) if i < len(gpu) else -1
    return windows, bench


KEYS = [('ee', 'ee_ms'), ('iop', 'iop_ms'), ('core1', None), ('vu1', 'vu1'), ('idle', 'idle'), ('chain', 'chain'),
        ('gsgl', 'gsgl'), ('pres', 'pres'), ('gsq', 'gsq_wait_ms'), ('gpu', 'gpu'), ('acq', 'acq')]


def busy(w, key, src):
    if key == 'core1':
        return w.get('ee_ms', 0) + w.get('iop_ms', 0)
    return w.get(src, -1)


def main():
    args = sys.argv[1:]
    path = args[0]
    ref = args[args.index('--ref') + 1] if '--ref' in args else None
    scenes = args[args.index('--scenes') + 1] if '--scenes' in args else DEFAULT_SCENES
    windows, bench = parse(path)
    if not bench:
        print('no [bench] lines (GOW_BENCH=1 GOW_PAD_TEST=1 set? gameplay reached?)')
        return
    wall = {f: t for f, t, _ in bench}
    print(f'{path}: {len(bench)} bench points, last frame {bench[-1][0]} (game {bench[-1][0] / 60:.1f} s), '
          f'wall {bench[-1][1]:.1f} s')
    print('scene(game s)  fps   ' + ' '.join(f'{k:>6}' for k, _ in KEYS) + '  windows')
    for sc in scenes.split(','):
        a, b = (float(x) for x in sc.split('-'))
        fa, fb = int(a * 60), int(b * 60)
        fa = max(fa, bench[0][0])
        if fa not in wall or fb not in wall:
            print(f'{sc:>12}   (not reached)')
            continue
        fps = (fb - fa) / (wall[fb] - wall[fa])
        inside = [w for w in windows if w['frames'] and w['frames'][0] >= fa and w['frames'][-1] <= fb]
        cols = []
        for k, src in KEYS:
            vals = [busy(w, k, src) for w in inside]
            cols.append(f'{sum(vals) / len(vals):6.0f}' if vals else '     -')
        print(f'{sc:>12} {fps:5.1f}  ' + ' '.join(cols) + f'  {len(inside)}')
    if '--windows' in args:
        print('\nwindow t   frames(game s)  fps   ' + ' '.join(f'{k:>6}' for k, _ in KEYS))
        for w in windows:
            fr = f"{w['frames'][0] / 60:6.1f}-{w['frames'][-1] / 60:6.1f}" if w['frames'] else '      -       '
            print(f"{w.get('t', 0):8.0f} {fr} {w.get('guest_flip_hz', 0):5.1f}  " +
                  ' '.join(f'{busy(w, k, s):6.0f}' for k, s in KEYS))
    if ref:
        _, rb = parse(ref)
        rs = {f: s for f, _, s in rb}
        same = diff = 0
        first = None
        for f, _, s in bench:
            if f in rs:
                if rs[f] == s:
                    same += 1
                else:
                    diff += 1
                    first = first or (f, s, rs[f])
        print(f'\nsignature vs {ref}: {same} equal, {diff} differ' +
              (f'; first difference at frame {first[0]} (game {first[0] / 60:.1f} s):\n  this {first[1]}\n  ref  {first[2]}'
               if first else ''))
        # rand calls between the two seeds (the game's LCG, gowRand): a few calls with equal positions = the same game
        # state with some rand calls landing on the other side of a frame boundary (timing), not a divergence
        lag = []
        for f, _, s in bench:
            if f in rs and rs[f] != s:
                a, b = (int(x.split('seed=')[1][:8], 16) for x in (s, rs[f]))
                lag.append((f, lcg_steps(b, a), lcg_steps(a, b), s.split(' seed=')[0] == rs[f].split(' seed=')[0]))
        if lag:
            print('  rand calls ahead of ref (frame: this ahead / ref ahead, None = over 4096): ' +
                  ', '.join(f'{f}: {x}/{y}{"" if p else " (pos differ)"}' for f, x, y, p in lag[:8]))


if __name__ == '__main__':
    main()
