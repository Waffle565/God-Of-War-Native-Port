#!/usr/bin/env python3
"""Full listing of an IRX (base 0): every instruction, jal targets named (lib:ordinal for import stubs),
lui/addiu|ori pairs resolved to module offsets. usage: irxlist.py IRX > out.lst"""
import capstone, struct, sys
m = open(sys.argv[1], 'rb').read()
shoff = struct.unpack_from('<I', m, 32)[0]; shnum = struct.unpack_from('<H', m, 48)[0]
shstr = struct.unpack_from('<H', m, 50)[0]
so = struct.unpack_from('<10I', m, shoff + shstr * 40)[4]
secs = {}
for i in range(shnum):
    n, t, f, a, o, s = struct.unpack_from('<6I', m, shoff + i * 40)
    secs[m[so + n:m.index(b'\0', so + n)].decode()] = (t, a, o, s)
img = bytearray(max(a + s for t, a, o, s in secs.values()))
for name, (t, a, o, s) in secs.items():
    if t == 1 and s:
        img[a:a + s] = m[o:o + s]
w = lambda o: struct.unpack_from('<I', img, o)[0]
names = {}
for off in range(0, secs['.text'][3] - 24, 4):
    if w(off) == 0x41E00000:
        lib = img[off + 12:off + 20].split(b'\0')[0].decode(); q = off + 20
        while w(q) or w(q + 4):
            names[q] = '%s:%d' % (lib, w(q + 4) & 0xFFFF); q += 8
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 | capstone.CS_MODE_LITTLE_ENDIAN)
ta, ts = secs['.text'][1], secs['.text'][3]
hi = {}
for i in md.disasm(bytes(img[ta:ta + ts]), ta):
    c = ''
    if i.mnemonic == 'jal':
        t = int(i.op_str, 16); c = names.get(t, '')
    elif i.mnemonic == 'lui':
        r, v = i.op_str.split(', '); hi[r] = int(v, 0) << 16
    elif i.mnemonic in ('addiu', 'ori', 'lw', 'sw', 'lh', 'lhu', 'lb', 'lbu', 'sh', 'sb'):
        ops = i.op_str.replace('(', ', ').replace(')', '').split(', ')
        if len(ops) == 3 and ops[1 if i.mnemonic in ('addiu', 'ori') else 2] in hi:
            base = ops[1] if i.mnemonic in ('addiu', 'ori') else ops[2]
            v = int((ops[2] if i.mnemonic in ('addiu', 'ori') else ops[1]) or '0', 0)
            if i.mnemonic != 'ori' and v >= 0x8000: v -= 0x10000
            c = '=%x' % ((hi[base] + v) & 0xffffffff)
    print('%05x: %-7s %s%s' % (i.address, i.mnemonic, i.op_str, ('  ; ' + c) if c else ''))
