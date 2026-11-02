import sys, subprocess, collections
# lines3.py <elf> <pcs> <lo-hi s> <symbol prefix> [top] [file]: samples of one function grouped by inlined function name
# (addr2line -f -i chain; the first frame whose function is not a one-line helper) and by the 2nd file frame
elf, pcs, rng, sym = sys.argv[1:5]; top = int(sys.argv[5]) if len(sys.argv) > 5 else 40
MATCH = sys.argv[6] if len(sys.argv) > 6 else 'nat_part.inl'
B = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-'
lo, hi = (float(x) * 1000 for x in rng.split('-'))
nm = subprocess.run([B + 'nm', '-C', '-S', '--defined-only', elf], capture_output=True, text=True).stdout
f = [l.split(' ', 3) for l in nm.splitlines() if l.split(' ', 3)[-1].startswith(sym)]
f = max([p for p in f if len(p) == 4 and p[2] in 'tTwW' and 'guard' not in p[3]], key=lambda p: int(p[1], 16)); s0, sz = int(f[0], 16), int(f[1], 16)
cnt = collections.Counter(); ftot = 0; cur = []; blocks = []
lines = [l.split() for l in open(pcs)]; first_is_w = lines[0][0] == 'w'
for p in lines:
    if not p: continue
    if p[0] == 'w':
        if first_is_w: blocks.append([float(p[1]), []])
        else: blocks.append([float(p[1]), cur]); cur = []
    elif len(p) == 2: (blocks[-1][1] if first_is_w else cur).append(p)
for t, ent in blocks:
    if lo <= t <= hi:
        for a, n in ent:
            a = int(a, 16)
            if s0 <= a < s0 + sz: cnt[a] += int(n); ftot += int(n)
addrs = sorted(cnt)
out = subprocess.run([B + 'addr2line', '-e', elf, '-a', '-i', '-f', '-C'] + [hex(a) for a in addrs], capture_output=True, text=True).stdout.splitlines()
per = {}; curA = None; fn = None
for l in out:
    if l.startswith('0x') and ' ' not in l: curA = int(l, 16); per[curA] = []; fn = None
    elif fn is None: fn = l
    else: per[curA].append((fn.split('(')[0][-60:], l.split('/')[-1].split(' ')[0])); fn = None
helpers = ('::R', '::W', '::WA', '::A', '::Raw', '::add', '::sub', '::mul', '::madd', '::msub', '::lq', '::sq', '::lqi', '::sqi', '::seti', '::vi', '::bc', '::addF', '::clip', '::ilw', '::isw', '::Iv', '::Qv', '::lane', '::yzx', '::zxy', '::mr32', '::mfirv', '::fcand', '::div', '::loi', '::kick', '::xtop', '::sf')
g1 = collections.Counter(); g2 = collections.Counter()
for a in addrs:
    fr = [x for x in per.get(a, []) if MATCH in x[1]]
    k = next((x[0] for x in fr if not x[0].endswith(helpers) and 'vu1n::' not in x[0]), '?'); g1[k] += cnt[a]
    k2 = fr[1][1] if len(fr) > 1 else (fr[0][1] if fr else '?'); g2[k2] += cnt[a]
print('in function %d samples' % ftot)
print('-- inlined function'); [print('%5.1f%% %s' % (100 * v / ftot, k)) for k, v in g1.most_common(top)]
print('-- 2nd frame in file'); [print('%5.1f%% %s' % (100 * v / ftot, k)) for k, v in g2.most_common(top)]
