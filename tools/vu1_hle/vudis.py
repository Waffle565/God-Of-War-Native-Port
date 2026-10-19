"""VU micro disassembler (tables parsed from PCSX2's DisVUmicro.h); generic operand print.
usage: vudis.py <record file> <record index> [start pc hex] [pairs]   or   vudis.py --bin <16K code file> <pc> [pairs]"""
import re
import struct
import sys

SRC = r'C:\Users\lovas\Claude\God Of War\ref\pcsx2-src\pcsx2\DebugTools\DisVUmicro.h'
text = open(SRC).read().replace('dis##VU##', '').replace('\\\n', '\n')
tables = {}
for m in re.finditer(r'TdisR5900F\s+(\w+)\[\d*\]\s*=\s*\{(.*?)\};', text, re.S):
    body = re.sub(r'/\*.*?\*/', '', m.group(2))
    tables[m.group(1)] = [x.strip() for x in body.split(',') if x.strip()]

DEST = lambda w: ''.join(c for c, b in zip('xyzw', (8, 4, 2, 1)) if (w >> 21) & b)
BC = 'xyzw'


def upper(u):
    op = u & 0x3F
    name = tables['MicroU'][op]
    if name.startswith('_UPPER_FD_'):
        name = tables[name + '_TABLE'][(u >> 6) & 0x1F]
    name = name.replace('MI_', '')
    fd, fs, ft = (u >> 6) & 31, (u >> 11) & 31, (u >> 16) & 31
    flags = ''.join(f for f, b in (('I', 31), ('E', 30), ('M', 29), ('D', 28), ('T', 27)) if (u >> b) & 1)
    d = DEST(u)
    if name == 'NOP':
        s = 'nop'
    elif name == 'CLIP':
        s = f'clipw.xyz vf{fs}, vf{ft}w'
    elif name.startswith(('ITOF', 'FTOI', 'ABS')) or name in ('MOVE', 'MR32'):
        s = f'{name.lower()}.{d} vf{ft}, vf{fs}'
    elif name.endswith(('ADDA', 'SUBA', 'MULA', 'MADDA', 'MSUBA')) or 'OPMULA' in name or re.match(r'(ADDA|SUBA|MULA|MADDA|MSUBA)[xyzwqi]$', name):
        tail = name[-1] if name[-1] in 'xyzwqi' and not name.endswith(('ADDA', 'MULA', 'SUBA')) else ''
        ftp = (f'vf{ft}{tail}' if tail in 'xyzw' and tail else ('Q' if tail == 'q' else 'I' if tail == 'i' else f'vf{ft}'))
        s = f'{name.lower()}.{d} ACC, vf{fs}, {ftp}'
    else:
        tail = name[-1] if name[-1] in 'xyzwqi' and name not in ('MAX',) else ''
        ftp = f'vf{ft}{tail}' if tail in 'xyzw' else ('Q' if tail == 'q' else 'I' if tail == 'i' else f'vf{ft}')
        s = f'{name.lower()}.{d} vf{fd}, vf{fs}, {ftp}'
    return s, flags


def lower(l):
    op = l >> 25
    name = tables['MicroL'][op]
    if name == 'LowerOP':
        name = tables['LowerOP_OPCODE'][l & 0x3F]
        if name.startswith('LowerOP_T3'):
            name = tables[name + '_OPCODE'][(l >> 6) & 0x1F]
    name = name.replace('MI_', '')
    it, is_, id_ = (l >> 16) & 15, (l >> 11) & 15, (l >> 6) & 15
    ft, fs = (l >> 16) & 31, (l >> 11) & 31
    d = DEST(l)
    imm11 = l & 0x7FF
    imm11s = imm11 - 0x800 if imm11 & 0x400 else imm11
    imm15 = (l & 0x7FF) | ((l >> 10) & 0x7800)
    if name == 'disNULL':
        return f'?? {l:08x}'
    if name in ('LQ',):
        return f'lq.{d} vf{ft}, {imm11s}(vi{is_})'
    if name in ('SQ',):
        return f'sq.{d} vf{fs}, {imm11s}(vi{it})'
    if name in ('ILW', 'ISW'):
        return f'{name.lower()}.{d} vi{it}, {imm11s}(vi{is_})'
    if name in ('IADDIU', 'ISUBIU'):
        return f'{name.lower()} vi{it}, vi{is_}, {imm15}'
    if name == 'IADDI':
        i5 = (l >> 6) & 31
        return f'iaddi vi{it}, vi{is_}, {i5 - 32 if i5 & 16 else i5}'
    if name in ('IADD', 'ISUB', 'IAND', 'IOR'):
        return f'{name.lower()} vi{id_}, vi{is_}, vi{it}'
    if name in ('B', 'BAL'):
        return f'{name.lower()} vi{it}, ->{imm11s:+d}'
    if name in ('IBEQ', 'IBNE'):
        return f'{name.lower()} vi{it}, vi{is_}, ->{imm11s:+d}'
    if name.startswith('IB'):
        return f'{name.lower()} vi{is_}, ->{imm11s:+d}'
    if name in ('JR', 'JALR'):
        return f'{name.lower()} vi{it}, vi{is_}'
    if name in ('LQI', 'LQD'):
        return f'{name.lower()}.{d} vf{ft}, (vi{is_}{"++" if name == "LQI" else "--"})'
    if name in ('SQI', 'SQD'):
        return f'{name.lower()}.{d} vf{fs}, (vi{it}{"++" if name == "SQI" else "--"})'
    if name in ('ILWR', 'ISWR'):
        return f'{name.lower()}.{d} vi{it}, (vi{is_})'
    if name in ('DIV', 'RSQRT'):
        return f'{name.lower()} Q, vf{fs}{BC[(l >> 21) & 3]}, vf{ft}{BC[(l >> 23) & 3]}'
    if name == 'SQRT':
        return f'sqrt Q, vf{ft}{BC[(l >> 23) & 3]}'
    if name in ('MOVE', 'MR32'):
        return f'{name.lower()}.{d} vf{ft}, vf{fs}'
    if name == 'MTIR':
        return f'mtir vi{it}, vf{fs}{BC[(l >> 21) & 3]}'
    if name == 'MFIR':
        return f'mfir.{d} vf{ft}, vi{is_}'
    if name == 'XGKICK':
        return f'xgkick vi{is_}'
    if name.startswith('FC') or name.startswith('FM') or name.startswith('FS'):
        return f'{name.lower()} vi{it}, vi{is_}, {l & 0xFFFFFF:#x}'
    if name.startswith('E'):
        return f'{name.lower()} P, vf{fs}{BC[(l >> 21) & 3]}'
    if name == 'MFP':
        return f'mfp.{d} vf{ft}, P'
    return f'{name.lower()} it=vi{it} is=vi{is_} fs={fs} ft={ft} raw={l:08x}'


def listing(code, pc, pairs):
    out = []
    for k in range(pairs):
        p = pc + k * 8
        if p + 8 > len(code):
            break
        l, u = struct.unpack_from('<II', code, p)
        us, flags = upper(u)
        ls = 'loi ' + hex(l) if (u >> 31) & 1 else lower(l)
        out.append(f'{p:04x} [{p // 8:4d}]  {us:<36} {ls:<40} {flags}')
    return out


if __name__ == '__main__':
    if sys.argv[1] == '--bin':
        code = open(sys.argv[2], 'rb').read()
        pc = int(sys.argv[3], 16)
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 200
    else:
        rs, st = 33448, 664
        with open(sys.argv[1], 'rb') as f:
            f.seek(rs * int(sys.argv[2]))
            r = f.read(rs)
        code = r[16 + st:16 + st + 16384]
        pc = int(sys.argv[3], 16) if len(sys.argv) > 3 else struct.unpack_from('<I', r, 16 + 608)[0]
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 200
    print('\n'.join(listing(code, pc, n)))
