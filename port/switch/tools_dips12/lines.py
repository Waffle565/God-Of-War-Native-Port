import sys, bisect, subprocess, collections
# lines.py <elf> <pcs> <lo-hi s> <symbol substring> [top]: samples inside one function, per source line (addr2line -i: innermost
# inline frame and the outermost caller line inside the function)
elf, pcs, rng, sym = sys.argv[1:5]; top = int(sys.argv[5]) if len(sys.argv) > 5 else 40
B = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-'
lo, hi = (float(x) * 1000 for x in rng.split('-'))
nm = subprocess.run([B + 'nm', '-C', '-S', '--defined-only', elf], capture_output=True, text=True).stdout
import re; f = [l.split(' ', 3) for l in nm.splitlines() if l.split(' ', 3)[-1].startswith(sym)]
f = max([p for p in f if len(p) == 4 and p[2] in 'tTwW' and 'guard' not in p[3]], key=lambda p: int(p[1], 16)); s0, sz = int(f[0], 16), int(f[1], 16)
print('#', f[3][:120], hex(s0), sz)
cnt = collections.Counter(); tot = 0; ftot = 0
lines = [l.split() for l in open(pcs)]
first_is_w = lines[0][0] == 'w'
blocks = []; cur = []
for p in lines:
    if not p: continue
    if p[0] == 'w':
        if first_is_w: blocks.append([float(p[1]), []])
        else: blocks.append([float(p[1]), cur]); cur = []
    elif len(p) == 2:
        (blocks[-1][1] if first_is_w else cur).append(p)
for t, ent in blocks:
    if not lo <= t <= hi: continue
    for a, n in ent:
        a = int(a, 16); n = int(n); tot += n
        if s0 <= a < s0 + sz: cnt[a] += n; ftot += n
addrs = sorted(cnt)
out = subprocess.run([B + 'addr2line', '-e', elf, '-i', '-f', '-C'] + [hex(a) for a in addrs], capture_output=True, text=True).stdout
# addr2line -i prints a variable number of (func, file:line) pairs per address: rerun per address is slow; use -a marker
out = subprocess.run([B + 'addr2line', '-e', elf, '-a', '-i'] + [hex(a) for a in addrs], capture_output=True, text=True).stdout.splitlines()
per = {}; curA = None
for l in out:
    if l.startswith('0x'): curA = int(l, 16); per[curA] = []
    else: per[curA].append(l.split('/')[-1].split(' ')[0])
inner = collections.Counter(); outer = collections.Counter()
for a in addrs:
    fr = per.get(a) or ['?']
    inner[fr[0]] += cnt[a]; outer[fr[-1]] += cnt[a]
print('window samples %d, in function %d (%.1f%%)' % (tot, ftot, 100 * ftot / max(tot, 1)))
print('-- innermost line'); [print('%5.1f%% %s' % (100 * v / ftot, k)) for k, v in inner.most_common(top)]
print('-- line in the function (outermost)'); [print('%5.1f%% %s' % (100 * v / ftot, k)) for k, v in outer.most_common(top)]
