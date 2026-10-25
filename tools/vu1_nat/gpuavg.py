import re, sys
# gpuavg.py [--skip N] <log.txt>...: GPU time per 5 s window (GOW_DK_GPU_TIME=1, docs/switch-deko3d.md), averaged over
# the [gs-dk-gpu] windows after the first N (default 8: boot, menu, load): busy, each family, and the top programs of
# [gs-dk-gpu-prog] (ms per window, Mfrag when the log has them). nat-vu1n.
args = sys.argv[1:]
skip = 8
if args and args[0] == '--skip':
    skip = int(args[1]); args = args[2:]
for path in args:
    fam, prog, frag = {}, {}, {}
    n = 0
    lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
    gpu = [k for k, l in enumerate(lines) if l.startswith('[gs-dk-gpu]')][skip:]
    for k in gpu:
        n += 1
        for m in re.finditer(r'(\w+)=([\d.]+)', lines[k]):
            fam[m.group(1)] = fam.get(m.group(1), 0.0) + float(m.group(2))
        # the program line follows its window's family line
        for j in range(k + 1, min(k + 4, len(lines))):
            if lines[j].startswith('[gs-dk-gpu-prog]'):
                for m in re.finditer(r'p\d+:(\w+:[0-9a-f]+)=([\d.]+)(?:/([\d.]+))?', lines[j]):
                    prog[m.group(1)] = prog.get(m.group(1), 0.0) + float(m.group(2))
                    if m.group(3):
                        frag[m.group(1)] = frag.get(m.group(1), 0.0) + float(m.group(3))
                break
    if not n:
        print(path, 'no [gs-dk-gpu] windows'); continue
    print('%s: %d windows | %s' % (path, n, ' '.join('%s %.1f' % (f, v / n) for f, v in fam.items())))
    top = sorted(prog.items(), key=lambda kv: -kv[1])[:14]
    print('  programs (ms / 5 s, sum of the per-window top 8): ' +
          ', '.join('%s %.1f%s' % (p, v / n, (' / %.1f Mfrag' % (frag[p] / n)) if p in frag else '') for p, v in top))
