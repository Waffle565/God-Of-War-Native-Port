# God of War save slots inside a PS2 memory card image (PCSX2 .ps2 layout, the Switch build's play.mcd).
#
#   python -I tools/mcd_slots.py list CARD
#   python -I tools/mcd_slots.py keep CARD OUT --slots 1,2 [--front]
#
# The game keeps its saves in BASCUS-97399GodOfWar/ (docs/saves.md): data0.bin .. data4.bin (81920 bytes each, always all
# five) and the 68-byte index file BASCUS-97399GodOfWar: four 16-byte slot entries (slot k at +16*(k-1): u32 data file
# number, u32 magic 0xCAFEBAD1, f32 play time in s, u16 progress?, u16 1; an empty slot is ff ff ff ff + 12 zero bytes,
# as the game writes it) and a u32 at +64 (the data file of the most recent save, as far as seen). One data file more
# than slots: a save goes to a free file, then the index switches over (read from the cards, not from the game's code).
# `keep` writes a copy whose index lists only the given slots (the others empty, as the game writes them); with --front
# the kept slots move up to slot 1, 2, ... in their order. Only the index file's page changes (data recomputed with its
# ECC); the data files stay where they are. CARD is never written.
import argparse, struct, sys, hashlib

RAW, PAGE = 528, 512
DIR = 'BASCUS-97399GodOfWar'
MAGIC = 0xCAFEBAD1
EMPTY = b'\xff\xff\xff\xff' + bytes(12)

PARITY = [bin(b).count('1') & 1 for b in range(256)]
COLMASK = [0, 7, 22, 17, 37, 34, 51, 52, 52, 51, 34, 37, 17, 22, 7, 0, 67, 68, 85, 82, 102, 97, 112, 119, 119, 112, 97,
           102, 82, 85, 68, 67, 82, 85, 68, 67, 119, 112, 97, 102, 102, 97, 112, 119, 67, 68, 85, 82, 17, 22, 7, 0, 52, 51,
           34, 37, 37, 34, 51, 52, 0, 7, 22, 17, 97, 102, 119, 112, 68, 67, 82, 85, 85, 82, 67, 68, 112, 119, 102, 97, 34,
           37, 52, 51, 7, 0, 17, 22, 22, 17, 0, 7, 51, 52, 37, 34, 51, 52, 37, 34, 22, 17, 0, 7, 7, 0, 17, 22, 34, 37, 52,
           51, 112, 119, 102, 97, 85, 82, 67, 68, 68, 67, 82, 85, 97, 102, 119, 112, 112, 119, 102, 97, 85, 82, 67, 68, 68,
           67, 82, 85, 97, 102, 119, 112, 51, 52, 37, 34, 22, 17, 0, 7, 7, 0, 17, 22, 34, 37, 52, 51, 34, 37, 52, 51, 7, 0,
           17, 22, 22, 17, 0, 7, 51, 52, 37, 34, 97, 102, 119, 112, 68, 67, 82, 85, 85, 82, 67, 68, 112, 119, 102, 97, 17,
           22, 7, 0, 52, 51, 34, 37, 37, 34, 51, 52, 0, 7, 22, 17, 82, 85, 68, 67, 119, 112, 97, 102, 102, 97, 112, 119, 67,
           68, 85, 82, 67, 68, 85, 82, 102, 97, 112, 119, 119, 112, 97, 102, 82, 85, 68, 67, 0, 7, 22, 17, 37, 34, 51, 52,
           52, 51, 34, 37, 17, 22, 7, 0]


def ecc128(buf):
    # PCSX2 MemoryCardFile.cpp CalculateECC (= MC2_D's table): 3 bytes per 128 data bytes
    col, lp0, lp1 = 0x77, 0x7F, 0x7F
    for i, b in enumerate(buf):
        col ^= COLMASK[b]
        if PARITY[b]:
            lp0 ^= ~i & 0x7F
            lp1 ^= i
    return bytes((col, lp0, lp1))


class Card:
    def __init__(self, data):
        self.d = bytearray(data)
        if len(self.d) != RAW * 0x4000 or not self.d.startswith(b'Sony PS2 Memory Card Format '):
            raise SystemExit('not an 8 MB PS2 card image with ECC (.ps2 / .mcd)')
        sb = self.page(0)
        self.ppc, = struct.unpack_from('<H', sb, 0x2A)
        self.alloc, = struct.unpack_from('<I', sb, 0x34)
        self.root, = struct.unpack_from('<I', sb, 0x3C)
        self.ifc = struct.unpack_from('<32I', sb, 0x50)

    def page(self, p):
        return bytes(self.d[p * RAW:p * RAW + PAGE])

    def clus(self, c):
        return b''.join(self.page(c * self.ppc + i) for i in range(self.ppc))

    def fat(self, c):
        ind = struct.unpack('<256I', self.clus(self.ifc[c // 256 // 256]))
        return struct.unpack('<256I', self.clus(ind[c // 256 % 256]))[c % 256]

    def chain(self, c):
        out = []
        while c not in (0xFFFFFFFF, 0x7FFFFFFF) and len(out) < 9000:
            out.append(c)
            n = self.fat(c)
            if not n & 0x80000000:
                raise SystemExit(f'FAT: cluster {c:#x} not in use')
            c = n & 0x7FFFFFFF
        return out

    def file_pages(self, c, length):
        # physical page numbers holding the first `length` bytes of the file starting at cluster c
        pages = [(self.alloc + x) * self.ppc + i for x in self.chain(c) for i in range(self.ppc)]
        return pages[:(length + PAGE - 1) // PAGE]

    def read(self, c, length):
        return b''.join(self.page(p) for p in self.file_pages(c, length))[:length]

    def entries(self, c, n):
        data = self.read(c, n * 512)
        for i in range(n):
            e = data[i * 512:(i + 1) * 512]
            mode, length, cl = struct.unpack_from('<III', e, 0)[0], struct.unpack_from('<I', e, 4)[0], struct.unpack_from('<I', e, 0x10)[0]
            sec, mi, hr, dy, mo, yr = struct.unpack_from('<BBBBBH', e, 0x19)
            yield e[0x40:0x60].split(b'\0')[0].decode('latin1'), mode, length, cl, f'{yr:04d}-{mo:02d}-{dy:02d} {hr:02d}:{mi:02d}:{sec:02d}'

    def find(self, path):
        c, n = self.root, struct.unpack_from('<I', self.read(self.root, 512), 4)[0]
        for comp in path.split('/'):
            for name, mode, length, cl, mt in self.entries(c, n):
                if mode & 0x8000 and name == comp:
                    c, n = cl, length
                    found = (cl, length, mt)
                    break
            else:
                return None
        return found

    def write_page(self, p, data):
        assert len(data) == PAGE
        o = p * RAW
        self.d[o:o + PAGE] = data
        for q in range(4):
            self.d[o + PAGE + q * 3:o + PAGE + q * 3 + 3] = ecc128(data[q * 128:(q + 1) * 128])

    def ecc_ok(self, p):
        o = p * RAW
        return all(self.d[o + PAGE + q * 3:o + PAGE + q * 3 + 3] == ecc128(self.d[o + q * 128:o + (q + 1) * 128]) for q in range(4))


def index_of(card):
    f = card.find(f'{DIR}/{DIR}')
    if not f:
        raise SystemExit(f'no {DIR}/{DIR} on the card (no God of War saves)')
    return f


def slots(card):
    cl, length, _ = index_of(card)
    idx = card.read(cl, length)
    out = []
    for k in range(4):
        di, = struct.unpack_from('<I', idx, 16 * k)
        magic, t, a, b = struct.unpack_from('<IfHH', idx, 16 * k + 4)
        info = None
        if di != 0xFFFFFFFF and magic == MAGIC:
            f = card.find(f'{DIR}/data{di}.bin')
            data = card.read(f[0], 0x40) if f else b''
            zone = data[8:0x13].split(b'\0')[0].decode('latin1') if data[:4] == struct.pack('<I', MAGIC) else '?'
            info = dict(data=di, zone=zone, time=t, a=a, b=b, mtime=f[2] if f else '?')
        out.append(info)
    last, = struct.unpack_from('<I', idx, 64)
    return idx, out, last


def cmd_list(a):
    card = Card(open(a.card, 'rb').read())
    idx, out, last = slots(card)
    print(f'{a.card}: md5 {hashlib.md5(card.d).hexdigest()}, index {idx.hex()}')
    for k, s in enumerate(out, 1):
        if s is None:
            print(f'  slot {k}: empty')
        else:
            h, m = int(s["time"]) // 3600, int(s["time"]) % 3600 // 60
            print(f'  slot {k}: data{s["data"]}.bin zone {s["zone"]:9s} play time {h}:{m:02d} ({s["time"]:.0f} s) '
                  f'field {s["a"]} saved {s["mtime"]}')
    print(f'  last saved data file: {last}')


def cmd_keep(a):
    src = open(a.card, 'rb').read()
    card = Card(src)
    keep = [int(x) for x in a.slots.split(',') if x]
    if not keep or any(k < 1 or k > 4 for k in keep) or len(set(keep)) != len(keep):
        raise SystemExit('--slots: 1-4, comma separated, no repeats')
    # an interrupted card write leaves a block index in the backup-2 marker pages, and the card driver would copy the
    # old block back over this edit on the next start: refuse (let the game finish its recovery first)
    b2, = struct.unpack_from('<I', card.page(0), 0x44)
    if any(card.page(b2 * 16 + i) != b'\xff' * PAGE for i in range(2)):
        raise SystemExit('the card has a pending write recovery (backup block markers set): load it in the game once first')
    idx, out, last = slots(card)
    for k in keep:
        if out[k - 1] is None:
            raise SystemExit(f'slot {k} is empty')
    ents = [idx[16 * k:16 * k + 16] for k in range(4)]
    if a.front:
        new = [ents[k - 1] for k in keep] + [EMPTY] * (4 - len(keep))
    else:
        new = [ents[k] if k + 1 in keep else EMPTY for k in range(4)]
    kept = [out[k - 1] for k in keep]
    if last not in [s['data'] for s in kept]:
        last = max(kept, key=lambda s: s['mtime'])['data']  # the newest kept save
    nidx = b''.join(new) + struct.pack('<I', last)
    assert len(nidx) == len(idx) == 68
    cl, length, _ = index_of(card)
    pages = card.file_pages(cl, length)
    assert len(pages) == 1
    p = pages[0]
    if not card.ecc_ok(p):
        raise SystemExit(f'index page {p}: ECC does not match the data (unexpected; nothing written)')
    pg = bytearray(card.page(p))
    pg[:68] = nidx
    card.write_page(p, bytes(pg))
    changed = [q for q in range(0x4000) if card.d[q * RAW:(q + 1) * RAW] != src[q * RAW:(q + 1) * RAW]]
    assert changed in ([p], []), changed  # [] = the index already listed exactly these slots
    open(a.out, 'wb').write(card.d)
    print(f'{a.out}: slots {keep} of {a.card} kept{" (moved to the front)" if a.front else ""}, {f"page {p} rewritten" if changed else "no page changed"}, '
          f'md5 {hashlib.md5(card.d).hexdigest()}')
    a.card = a.out
    cmd_list(a)


ap = argparse.ArgumentParser(description=__doc__)
sp = ap.add_subparsers(dest='cmd', required=True)
l = sp.add_parser('list'); l.add_argument('card'); l.set_defaults(fn=cmd_list)
k = sp.add_parser('keep'); k.add_argument('card'); k.add_argument('out'); k.add_argument('--slots', required=True)
k.add_argument('--front', action='store_true'); k.set_defaults(fn=cmd_keep)
a = ap.parse_args()
a.fn(a)
