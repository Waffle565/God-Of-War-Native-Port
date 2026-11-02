# insn.py <elf> <pcs> <lo_ms> <hi_ms>: samples by instruction class (msr/mrs fpcr, fdiv, fsqrt, dc zva, ldar/stlr) across the
# whole thread, plus samples on the instruction right after an msr fpcr (the stall usually lands there)
import sys, struct, collections
elf, pcs, lo, hi = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
d = open(elf, 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20); phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
segs = []
for i in range(phnum):
    t, f, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
    if t == 1: segs.append((va, off, fs))
def word(a):
    for va, off, fs in segs:
        if va <= a < va + fs: return struct.unpack_from('<I', d, off + a - va)[0]
    return None
cnt = collections.Counter(); t = 0; tot = 0
for l in open(pcs):
    p = l.split()
    if not p: continue
    if p[0] == 'w': t = float(p[1]); continue
    if len(p) == 2 and lo <= t <= hi:
        a = int(p[0], 16); n = int(p[1]); tot += n
        w = word(a); pw = word(a - 4)
        k = 'other'
        if w is None: k = 'none'
        elif w & 0xFFFFFFE0 == 0xD51B4400: k = 'msr fpcr'
        elif w & 0xFFFFFFE0 == 0xD53B4400: k = 'mrs fpcr'
        elif w & 0xFF20FC00 == 0x1E201800: k = 'fdiv'
        elif w & 0xFFFFFC00 == 0x1E21C000: k = 'fsqrt s'
        elif w & 0xFFFFFFE0 == 0xD50B7420: k = 'dc zva'
        elif w & 0x3FFFFC00 == 0x08DFFC00: k = 'ldar'
        if k == 'other' and pw is not None and pw & 0xFFFFFFE0 == 0xD51B4400: k = 'after msr fpcr'
        cnt[k] += n
print('samples', tot)
for k, v in cnt.most_common(): print('%6.2f%% %s' % (100.0 * v / tot, k))
