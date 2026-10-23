#!/usr/bin/env python3
"""Map the console's GOW_PC_SAMPLE histograms to functions (docs/diagnostics.md "PC sampling").

usage: switch_pcprof.py <pcs_<thread>.txt> <ps2EntryRunner.elf> [first_ms last_ms] [--top N] [--lines FUNC] [--blocks]

Sums the windows whose end time (ms since the sampler started) lies in [first_ms, last_ms] (default: all), then prints
the share of samples per function (generated VU1 blocks summed as <vu1 block>, kept apart with --blocks). --lines FUNC
breaks one function down by source line (needs a build with debug info). Run in WSL: uses aarch64-linux-gnu-nm and
aarch64-linux-gnu-addr2line (or the devkitA64 ones, $NM / $ADDR2LINE)."""
import bisect
import collections
import os
import re
import subprocess
import sys


def main():
    args = [a for a in sys.argv[1:]]
    top = 50
    lines_fn = None
    blocks = False
    if '--top' in args:
        i = args.index('--top'); top = int(args[i + 1]); del args[i:i + 2]
    if '--lines' in args:
        i = args.index('--lines'); lines_fn = args[i + 1]; del args[i:i + 2]
    base = 0
    if '--base' in args:  # logs written before the sampler subtracted the module base (pcs1): its low 32 bits
        i = args.index('--base'); base = int(args[i + 1], 16); del args[i:i + 2]
    if '--blocks' in args:
        args.remove('--blocks'); blocks = True
    path, elf = args[0], args[1]
    lo, hi = (int(args[2]), int(args[3])) if len(args) >= 4 else (0, 1 << 62)
    hist = collections.Counter()
    samples = failed = windows = 0
    use = False
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == 'w':
            use = lo <= int(p[1]) <= hi
            if use:
                windows += 1; samples += int(p[2]); failed += int(p[3])
            continue
        if use:
            hist[(int(p[0], 16) - base) & 0xFFFFFFFF] += int(p[1])
    nm = os.environ.get('NM', 'aarch64-linux-gnu-nm')
    syms = []
    out = subprocess.run([nm, '-C', '--defined-only', '-n', elf], capture_output=True, text=True).stdout
    for l in out.splitlines():
        p = l.split(' ', 2)
        if len(p) == 3 and p[1] in 'tTwW':
            syms.append((int(p[0], 16), p[2]))
    addrs = [s[0] for s in syms]
    by_fn = collections.Counter()
    in_fn = collections.Counter()
    for off, n in hist.items():
        i = bisect.bisect_right(addrs, off) - 1
        name = syms[i][1] if i >= 0 else '?'
        if not blocks and re.match(r'^[tb][0-9a-f]+_[0-9a-f]+(_x)?\(', name):
            name = '<vu1 block>'
        by_fn[name] += n
        if lines_fn and lines_fn in name:
            in_fn[off] += n
    tot = sum(hist.values()) or 1
    print(f'windows={windows} samples={samples} failed={failed} counted={tot}')
    for name, n in by_fn.most_common(top):
        print(f'{100 * n / tot:6.2f}% {n:7d} {name[:140]}')
    if lines_fn:
        a2l = os.environ.get('ADDR2LINE', 'aarch64-linux-gnu-addr2line')
        offs = [o for o, _ in in_fn.most_common(300)]
        res = subprocess.run([a2l, '-e', elf] + [hex(o) for o in offs], capture_output=True, text=True).stdout.split('\n')
        by_line = collections.Counter()
        for o, r in zip(offs, res):
            by_line[r.split('/')[-1].split(' ')[0]] += in_fn[o]
        ftot = sum(in_fn.values()) or 1
        print(f'--- {lines_fn}: {ftot} samples by line')
        for k, n in by_line.most_common(40):
            print(f'{100 * n / ftot:6.2f}% {k}')


if __name__ == '__main__':
    main()
