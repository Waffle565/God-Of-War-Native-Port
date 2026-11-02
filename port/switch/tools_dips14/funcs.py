import sys, bisect, subprocess, collections, re
# funcs.py <elf> <pcs file> [lo_s-hi_s] [filter]: per-function share of the samples (windows whose end lies in lo..hi s);
# unknown addresses ('?') are attributed to the nearest symbol below them as '~name'
elf, pcs = sys.argv[1], sys.argv[2]
lo, hi = 0, 1e9
if len(sys.argv) > 3 and '-' in sys.argv[3]:
    lo, hi = (float(x) * 1000 for x in sys.argv[3].split('-'))
flt = sys.argv[4] if len(sys.argv) > 4 else ''
nm = subprocess.run(['/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm', '-C', '-S', '--defined-only', elf],
                    capture_output=True, text=True).stdout
syms = []
nosize = []
for l in nm.splitlines():
    p = l.split(' ', 3)
    if len(p) == 4 and p[2] in 'tTwW':
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    elif len(p) == 3 and p[1] in 'tTwW':
        nosize.append((int(p[0], 16), p[2]))
syms.sort()
nosize.sort()
starts = [s[0] for s in syms]
ns = [s[0] for s in nosize]
def strip(n):
    # drop the argument list only (keep '(anonymous namespace)')
    d=0
    for i in range(len(n)-1,-1,-1):
        if n[i]==')': d+=1
        elif n[i]=='(':
            d-=1
            if d==0: return n[:i] if not n[i:].startswith('(anonymous') else n
    return n
c = collections.Counter()
tot = 0
win = None
# a window's samples follow its 'w' line? or precede it: detect by the first line
lines = [l.split() for l in open(pcs)]
blocks = []
cur = []
first_is_w = lines and lines[0] and lines[0][0] == 'w'
for p in lines:
    if not p:
        continue
    if p[0] == 'w':
        if first_is_w:
            blocks.append([float(p[1]), []])
        else:
            blocks.append([float(p[1]), cur])
            cur = []
    elif len(p) == 2:
        if first_is_w:
            blocks[-1][1].append(p)
        else:
            cur.append(p)
for t, ent in blocks:
    if not (lo <= t <= hi):
        continue
    for a, n in ent:
        a = int(a, 16)
        n = int(n)
        tot += n
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and a < syms[i][0] + syms[i][1]:
            name = syms[i][2]
        else:
            j = bisect.bisect_right(ns, a) - 1
            k = i
            cand = []
            if j >= 0:
                cand.append((nosize[j][0], nosize[j][1]))
            if k >= 0:
                cand.append((syms[k][0], syms[k][2]))
            name = '~' + max(cand)[1] if cand else '?'
        c[strip(name)] += n
print('samples', tot)
for k, v in c.most_common(int(__import__('os').environ.get('TOPN','45'))):
    if flt in k:
        print('%5.2f%% %s' % (100 * v / tot, k[:150]))
