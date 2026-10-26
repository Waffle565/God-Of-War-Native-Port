"""Nintendo Switch button icons for God of War's menus and HUD (docs/settings.md "Button icons").

The game draws its button prompts (menu legends, tutorial text, the QTE prompts in fights) as Flash sprites inside the
Flash chunks FLP_HUD (R_PERM.WAD) and FLP_Shell (R_SHELL.WAD): XButton, CircleButton, SquareButton, TriangleButton
and their "Greek" variants (the QTE coin). Each is a body (a dark disc, or the stone coin) plus a symbol: an
untextured vector mesh with one flat colour (the cross, the circle, ...). L1/R1/L2/R2 (and L3/R3 on the controls
screen) are a body plus a text label.

This tool rewrites a WAD so the face buttons show the Switch letters (Cross = A, Circle = B, Square = X,
Triangle = Y, the layout of docs/switch.md "Controls") and the shoulder labels read L, R, ZL, ZR (stick clicks LS, RS):
- four 64x64 letter textures (drawn here: round strokes, white with a dark outline, no outside artwork) are added as
  GFX_/TXR_PAD_<letter> chunks plus one shared PAL_PAD right before the Flash chunk's mesh group;
- each symbol's mesh reference (FLP array 2) gets a full-UV quad part of the same mesh (a centred +-960 quad, the
  QTE coin's) and the letter texture as its material;
- every key frame that draws a symbol gets a new transformation (appended to the FLP's table): its old one times
  the fit of the quad onto the button body, measured from the named sprite (body part bounds + key frames);
- the label lines are rewritten with tools/gow_text.py (same glyph count, centred).
Nothing else moves inside the FLP except the transformation table's tail, the blend colours and the string table
(the game parses the chunk in order; names are offsets into the string table, which only grows at its end).

FLP layout: from mogaika/god_of_war_browser (pack/wad/flp). Mesh: GoW1 MESH chunk, VIF UNPACK V4-16 positions
(cmd 0x6d) and V2-16 UVs (cmd 0x65, 4.12 fixed point).

usage: python -I tools/gow_pad_icons.py <WAD in> <WAD out>    (R_PERM.WAD or R_SHELL.WAD, also a lang overlay .bin)
       python -I tools/gow_pad_icons.py --preview <out.png>    (the four letters on grey)
"""
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # tools/ (python -I leaves it out)
import gow_text  # noqa: E402

LETTER = {'XButton': 'A', 'CircleButton': 'B', 'SquareButton': 'X', 'TriangleButton': 'Y'}
LABELS = [('L1', 'L'), ('R1', 'R'), ('L2', 'ZL'), ('R2', 'ZR'), ('L3', 'LS'), ('R3', 'RS')]
FLP_GROWTH_MAX = 608  # bytes the FLP may grow: the Italian FLP_HUD loads at +608, breaks the game at +640 (docs/settings.md)
SIZE = 64          # texture size (the Switch renders the HUD at the PS2 resolution, a button is ~54 pixels)
GLYPH = 0.58       # letter box / texture (the PS2 symbol covers ~60 % of the disc, ~56 % of the coin)
STROKE = 0.18      # stroke width / letter box
OUTLINE = 0.06     # dark outline / letter box

U16 = lambda d, o: struct.unpack_from('<H', d, o)[0]  # noqa: E731
U32 = lambda d, o: struct.unpack_from('<I', d, o)[0]  # noqa: E731


# ---- letters ----------------------------------------------------------------------------------------------------
def _arc(cx, cy, r, a0, a1, n=10):
    pts = [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
            cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]
    return list(zip(pts, pts[1:]))


STROKES = {  # segments in a unit box, y down
    'A': [((0.5, 0.1), (0.17, 0.9)), ((0.5, 0.1), (0.83, 0.9)), ((0.31, 0.62), (0.69, 0.62))],
    'X': [((0.2, 0.1), (0.8, 0.9)), ((0.8, 0.1), (0.2, 0.9))],
    'Y': [((0.2, 0.1), (0.5, 0.5)), ((0.8, 0.1), (0.5, 0.5)), ((0.5, 0.5), (0.5, 0.9))],
    'B': [((0.26, 0.1), (0.26, 0.9)), ((0.26, 0.1), (0.55, 0.1)), ((0.26, 0.5), (0.58, 0.5)),
          ((0.26, 0.9), (0.6, 0.9))] + _arc(0.55, 0.3, 0.2, -90, 90) + _arc(0.6, 0.7, 0.2, -90, 90),
}


def _dist(px, py, seg):
    (ax, ay), (bx, by) = seg
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy or 1)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def letter_pixels(ch, size=SIZE, ss=4):
    """per pixel (fill, outline) coverage in 0..15: 16 x 16 = the 256 palette entries"""
    segs = STROKES[ch]
    box, x0 = GLYPH * size, (1 - GLYPH) * size / 2
    hw, ow = STROKE / 2, STROKE / 2 + OUTLINE
    out = []
    for y in range(size):
        for x in range(size):
            fill = edge = 0
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + 0.5) / ss - x0) / box
                    v = (y + (sy + 0.5) / ss - x0) / box
                    if -0.2 < u < 1.2 and -0.2 < v < 1.2:
                        d = min(_dist(u, v, s) for s in segs)
                        fill += d <= hw
                        edge += d <= ow
            n = ss * ss
            out.append((round(15 * fill / n), round(15 * edge / n)))
    return out


def palette():
    """index = outline coverage * 16 + fill coverage: white letter over a dark outline, alpha = outline coverage"""
    pal = []
    for i in range(256):
        edge, fill = i >> 4, i & 15
        k = fill / edge if edge else 0.0
        g = round(0x18 + (0xf4 - 0x18) * min(1.0, k))
        pal.append((g, g, g, round(0x80 * edge / 15)))
    return pal


def letter_indices(ch, size=SIZE):
    return bytes(min(e, 15) * 16 + min(f, e) for f, e in letter_pixels(ch, size))


def png(path, w, h, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))  # noqa: E731
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


# ---- WAD chunks / textures --------------------------------------------------------------------------------------
def swizzle8(lin, w, h):
    """linear 8-bit indices -> the PS2 PSMT8 block order (GFX format 0, as R_SHELL stores them)"""
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            bl = (y & ~0xf) * w + (x & ~0xf) * 2
            sw = (((y + 2) >> 2) & 1) * 4
            py = (((y & ~3) >> 1) + (y & 1)) & 7
            out[bl + py * w * 2 + ((x + sw) & 7) * 4 + ((y >> 1) & 1) + ((x >> 2) & 2)] = lin[y * w + x]
    return bytes(out)


def chunk(tag, name, body):
    head = struct.pack('<II', tag, len(body)) + name.encode().ljust(24, b'\0')
    return head + body + b'\0' * (-len(body) % 16)


def palette_chunk():
    """PAL_PAD: the one palette all four letters share (each letter used to carry its own copy: 3 KB more, and the
    Italian / French / Spanish R_PERM then outgrew the game's load buffer, docs/settings.md "Button icons")"""
    pal = palette()
    stored = [None] * 256
    for i in range(256):  # CSM1: entries 8-15 and 16-23 of every 32 swap places
        stored[(i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)] = pal[i]
    return chunk(0x3001e, 'PAL_PAD', struct.pack('<6I', 12, 16, 16, 0, 32, 1) + b''.join(bytes(c) for c in stored))


def texture_chunks(ch, gfx_format, txr_tail, size=SIZE):
    name = f'PAD_{ch}'
    idx = letter_indices(ch, size)
    if gfx_format == 0:
        idx = swizzle8(idx, size, size)
    gfx = struct.pack('<6I', 12, size, size, gfx_format, 8, 1) + idx
    txr = struct.pack('<I', 7) + f'GFX_{name}'.encode().ljust(24, b'\0') + b'PAL_PAD'.ljust(24, b'\0')
    txr += txr_tail
    return chunk(0x3001e, f'GFX_{name}', gfx) + chunk(0x1e, f'TXR_{name}', txr)


# ---- mesh -------------------------------------------------------------------------------------------------------
class Mesh:
    def __init__(self, m):
        self.m = m
        n, end = U32(m, 8), U32(m, 4)
        self.parts = [U32(m, 0x50 + 4 * i) for i in range(n)] + [end]

    def objects(self, pi):
        m, p, pe = self.m, self.parts[pi], self.parts[pi + 1]
        ng = U16(m, p + 2)
        gs = [p + U32(m, p + 4 + 4 * i) for i in range(ng)] + [pe]
        out = []
        for gi in range(ng):
            g = gs[gi]
            no = U32(m, g + 4)
            os_ = [g + U32(m, g + 0xc + 4 * i) for i in range(no)] + [gs[gi + 1]]
            out += [(os_[i], os_[i + 1]) for i in range(no)]
        return out

    def verts(self, pi):
        """per object: (positions, uvs)"""
        res = []
        for o, oe in self.objects(pi):
            d = self.m[o + 0x20:oe]
            q, pts, uvs = 0, [], []
            while q + 4 <= len(d):
                c = U32(d, q)
                cmd, num = (c >> 24) & 0x7f, (c >> 16) & 0xff
                if cmd == 0x6d and num and q + 4 + num * 8 <= len(d):
                    pts += [struct.unpack_from('<2h', d, q + 4 + 8 * i) for i in range(num)]
                    q += 4 + num * 8
                elif cmd == 0x65 and num and q + 4 + num * 4 <= len(d):
                    uvs += [struct.unpack_from('<2h', d, q + 4 + 4 * i) for i in range(num)]
                    q += 4 + num * 4
                else:
                    q += 4
            res.append((pts, uvs))
        return res

    def quad(self):
        """the largest centred single-object quad with full UVs (HUD: the QTE coin, +-960)"""
        best = None
        for pi in range(len(self.parts) - 1):
            try:
                v = self.verts(pi)
            except (struct.error, IndexError):
                continue
            if len(v) != 1 or len(v[0][0]) != 4 or not v[0][1]:
                continue
            (pts, uvs), = v
            b, ub = bounds(pts), bounds(uvs)
            if ub[0] == 0 and ub[1] == 0 and ub[2] >= 4095 and ub[3] >= 4095 and b[0] == -b[2] and b[1] == -b[3] \
                    and b[2] == b[3] and (best is None or b[2] > best[1]):
                best = (pi, b[2])
        return best


def bounds(pts):
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


# ---- FLP --------------------------------------------------------------------------------------------------------
class FLP:
    """GoW1 FLP chunk body, parsed in the game's order (header 0x60, then the arrays one after another)"""

    def __init__(self, d):
        self.d = d
        self.counts = [U32(d, o) for o in (0x0c, 0x14, 0x1c, 0x24, 0x2c, 0x34, 0x3c)]
        self.ntrans, self.strsize = U16(d, 0x48), U32(d, 0x58)
        self.p = 0x60
        self.handlers = [struct.unpack_from('<HH', d, self.p + 4 * i) for i in range(self.counts[0])]
        self.p += 4 * self.counts[0]
        self.meshrefs = self._meshrefs(self.counts[1])
        recs = [self._take(0x24) for _ in range(self.counts[2])]
        for r in recs:  # fonts
            chars, flags = U32(d, r), U16(d, r + 0xc)
            if flags & 6:
                self._meshrefs(chars)
            self._take(2 * chars)
            self._pad()
            self._take(2 * (0x100 if flags & 1 else chars))
            self._pad()
        recs = [self._take(0x24) for _ in range(self.counts[3])]
        for r in recs:  # static labels: render commands
            self._take(U32(d, r + 0x14))
            self._pad()
        self._take(0x20 * self.counts[4])
        self.sprites, self.timelines = [], []
        recs = [self._take(0xc) for _ in range(self.counts[5])]
        for r in recs:
            self._pad()
            self.timelines.append(self._sub1(self._take(0x18)))
            for x in [self._take(0x10) for _ in range(U16(d, r + 8))]:
                self._pad()
                self._take(U32(d, x))
        self._pad()
        self.sprites = [self._sub1(x) for x in [self._take(0x18) for _ in range(self.counts[6])]]
        self._pad()
        self.timelines.append(self._sub1(self._take(0x18)))
        self._pad()
        self.trans = self.p
        self.blend = self.trans + 0x14 * self.ntrans
        self.strs = len(d) - self.strsize
        if self.strs != self.blend + 8 * U16(d, 0x50):
            raise ValueError('FLP layout not understood (string table not where the arrays end)')

    def _take(self, n):
        o = self.p
        self.p += n
        return o

    def _pad(self):
        self.p = (self.p + 3) & ~3

    def _meshrefs(self, n):
        recs = [self._take(8) for _ in range(n)]
        return [(r, [self._take(8) for _ in range(U16(self.d, r + 2))]) for r in recs]

    def _sub1(self, r):
        d = self.d
        self._pad()
        els = []
        for e in [self._take(8) for _ in range(U16(d, r + 2))]:
            self._pad()
            els.append([self._take(0xa) for _ in range(U16(d, e + 2))])
        self._pad()
        for lab in [self._take(0xc) for _ in range(U16(d, r + 4))]:
            self._pad()
            for x in [self._take(8) for _ in range(U16(d, lab + 2))]:
                self._pad()
                self._take(U32(d, x))
        return els

    def name(self, off):
        if off in (0xffff, 0xffffffff):
            return None
        return self.d[self.strs + off:self.d.index(b'\0', self.strs + off)].decode('latin1')

    def keyframes(self):
        for els in self.sprites + self.timelines:
            for ks in els:
                yield from ks

    def target(self, k):
        h = U16(self.d, k + 2)
        return self.handlers[h] if h < len(self.handlers) else None

    def transform(self, i):
        if i >= self.ntrans:
            return (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
        o = self.trans + 0x14 * i
        m = [x / 65536 for x in struct.unpack_from('<4i', self.d, o)]
        ox, oy = struct.unpack_from('<2h', self.d, o + 16)
        return (*m, float(ox), float(oy))  # offsets in mesh units (the file's 1/16 pixel)


def compose(a, b):
    """a after b; matrix (m0 m2; m1 m3), the convention does not matter for the scale/translate cases used here"""
    m0, m1, m2, m3, ox, oy = a
    n0, n1, n2, n3, px, py = b
    return (m0 * n0 + m2 * n1, m1 * n0 + m3 * n1, m0 * n2 + m2 * n3, m1 * n2 + m3 * n3,
            m0 * px + m2 * py + ox, m1 * px + m3 * py + oy)


def inverse(a):
    m0, m1, m2, m3, ox, oy = a
    det = m0 * m3 - m1 * m2
    i0, i1, i2, i3 = m3 / det, -m1 / det, -m2 / det, m0 / det
    return (i0, i1, i2, i3, -(i0 * ox + i2 * oy), -(i1 * ox + i3 * oy))


def pack_transform(t):
    m0, m1, m2, m3, ox, oy = t
    return struct.pack('<4i2h', *[round(x * 65536) for x in (m0, m1, m2, m3)], round(ox), round(oy))


def patch_flp(data, mesh):
    """returns the new FLP body and a report; texture names TXR_PAD_<letter>"""
    f = FLP(data)
    d = data
    quad = mesh.quad()
    if quad is None:
        raise ValueError('no full-UV quad part in the mesh')
    qpart, qhalf = quad
    material = lambda r: [(U32(d, m), U32(d, m + 4)) for m in r[1]]  # noqa: E731
    sprite_of = lambda t: f.sprites[t[1]] if t and t[0] == 7 and t[1] < len(f.sprites) else None  # noqa: E731

    def button(els):
        """(body, symbol) of a button sprite: (meshref, its first key frame) of a textured part and of a one-slot
        untextured part, else None"""
        body = symbol = None
        for ks in els:
            t = f.target(ks[0]) if ks else None
            if not t or t[0] != 1:
                continue
            mats = material(f.meshrefs[t[1]])
            if any(off not in (0xffff, 0xffffffff) for _, off in mats):
                body = (t[1], ks[0])
            elif len(mats) == 1:
                symbol = (t[1], ks[0])
        return (body, symbol) if body and symbol else None

    # the named sprites give each symbol colour its letter; every sprite with a body and a symbol of such a colour
    # is a button too (the menu legends "ButtonBack", the "press" animations, the controls screen)
    colour = {}
    for k in f.keyframes():
        nm = f.name(U16(d, k + 8))
        base = nm[:-5] if nm and nm.endswith('Greek') else nm
        els = sprite_of(f.target(k)) if base in LETTER else None
        while els is not None and len(els) == 1 and els[0] and sprite_of(f.target(els[0][0])) is not None:
            els = sprite_of(f.target(els[0][0]))  # a wrapper sprite around the button
        b = button(els) if els else None
        if b:
            colour[material(f.meshrefs[b[1][0]])[0][0]] = LETTER[base]
    symbols = {}  # meshref index -> (letter, fit transform in symbol space)
    for els in f.sprites:
        b = button(els)
        if not b:
            continue
        body, symbol = b
        ch = colour.get(material(f.meshrefs[symbol[0]])[0][0])
        if ch is None or symbol[0] in symbols:
            continue
        bpart = struct.unpack_from('<h', d, f.meshrefs[body[0]][0])[0]
        textured = [p for p, uv in mesh.verts(bpart) if uv] or [p for p, _ in mesh.verts(bpart)]
        x0, y0, x1, y1 = bounds(textured[0])
        fit = ((x1 - x0) / (2 * qhalf), 0.0, 0.0, (y1 - y0) / (2 * qhalf), (x0 + x1) / 2, (y0 + y1) / 2)
        to_symbol = compose(inverse(f.transform(U16(d, symbol[1] + 4))), f.transform(U16(d, body[1] + 4)))
        symbols[symbol[0]] = (ch, compose(to_symbol, fit))

    out = bytearray(d)
    strings = b''
    names = {}
    for ch in sorted(set(v[0] for v in symbols.values())):
        names[ch] = f.strsize + len(strings)
        strings += f'TXR_PAD_{ch}'.encode() + b'\0'
    for i, (ch, _) in symbols.items():
        r, mats = f.meshrefs[i]
        struct.pack_into('<h', out, r, qpart)
        struct.pack_into('<II', out, mats[0], 0xffffffff, names[ch])
    # every transformation is stored once: a new one that equals an entry of the table (or another new one) reuses
    # it. The FLP must grow as little as possible: the Italian FLP_HUD broke the game (null jump at 0x23a9ac on the
    # legal screen) once it grew by 640 bytes (30 new entries + names), 600 still loaded (docs/settings.md)
    new = []
    index = {}
    by_value = {}
    for i in range(f.ntrans):
        by_value.setdefault(d[f.trans + 0x14 * i:f.trans + 0x14 * (i + 1)], i)
    drawn = 0
    for k in f.keyframes():
        t = f.target(k)
        if not t or t[0] != 1 or t[1] not in symbols:
            continue
        key = (U16(d, k + 4), t[1])
        if key not in index:
            packed = pack_transform(compose(f.transform(key[0]), symbols[t[1]][1]))
            if packed not in by_value:
                by_value[packed] = f.ntrans + len(new)
                new.append(packed)
            index[key] = by_value[packed]
        struct.pack_into('<H', out, k + 4, index[key])
        drawn += 1
    if f.ntrans + len(new) > 0xffff:
        raise ValueError('transformation table full')
    struct.pack_into('<H', out, 0x48, f.ntrans + len(new))
    struct.pack_into('<I', out, 0x58, f.strsize + len(strings))
    body = bytes(out[:f.blend]) + b''.join(new) + bytes(out[f.blend:]) + strings
    report = {'symbols': {i: v[0] for i, v in symbols.items()}, 'quad part': qpart, 'key frames': drawn,
              'transforms': len(new)}
    return body, sorted(names), report


def apply(wad, verbose=False):
    """the WAD (R_PERM / R_SHELL, also a language overlay of them) with Switch button icons"""
    chunks = list(gow_text.wad_chunks(wad))
    flp = next((c for c in chunks if c[3] in ('FLP_HUD', 'FLP_Shell')), None)
    if flp is None:
        return wad
    fi = chunks.index(flp)
    # the Flash chunk's mesh: the last MESH (0x19001e) before it, inside the MDL group in front of it
    mi = max(i for i in range(fi) if chunks[i][1] == 0x19001e)
    gi = max(i for i in range(mi) if chunks[i][1] == 0x28)  # its GroupStart: the textures go in front of it
    mpos, _, msize, _ = chunks[mi]
    mesh = Mesh(wad[mpos + 32:mpos + 32 + msize])
    fpos, ftag, fsize, fname = flp
    body = wad[fpos + 32:fpos + 32 + fsize]
    body, done = gow_text.rewrite(body, fname, LABELS)
    body, letters, report = patch_flp(body, mesh)
    gfx = next(c for c in chunks if c[3].startswith('GFX_'))
    gfx_format = U32(wad, gfx[0] + 32 + 12)
    ref = next(c for c in chunks if c[1] == 0x1e and c[3].startswith('TXR_') and c[2] == 88
               and U32(wad, c[0] + 32) == 7 and U32(wad, c[0] + 32 + 4 + 24 + 24 + 24) & 0xff == 0x81)
    tail = wad[ref[0] + 32 + 52:ref[0] + 32 + 88]  # the LOD fields of a 64x64 texture
    textures = palette_chunk() + b''.join(texture_chunks(ch, gfx_format, tail) for ch in letters)
    gpos = chunks[gi][0]
    flp_chunk = chunk(ftag, fname, body)
    end = fpos + 32 + ((fsize + 15) & ~15)
    out = wad[:gpos] + textures + wad[gpos:fpos] + flp_chunk + wad[end:]
    if len(flp_chunk) - (32 + ((fsize + 15) & ~15)) > FLP_GROWTH_MAX:
        raise ValueError(f'{fname} would grow by {len(flp_chunk) - 32 - ((fsize + 15) & ~15)} bytes (> {FLP_GROWTH_MAX})')
    if verbose:
        print(f'{fname}: labels {dict(done)}, {report}, textures {letters}, {len(wad)} -> {len(out)} bytes')
    return out


if __name__ == '__main__':
    args = sys.argv[1:]
    if args[:1] == ['--preview']:
        pal = palette()
        tiles = [letter_indices(ch) for ch in 'ABXY']
        w, h = SIZE * 4, SIZE
        rgba = bytearray()
        for y in range(h):
            for x in range(w):
                r, g, b, a = pal[tiles[x // SIZE][y * SIZE + x % SIZE]]
                a = min(255, a * 2)
                rgba += bytes(round(c * a / 255 + 0x60 * (255 - a) / 255) for c in (r, g, b)) + b'\xff'
        png(args[1], w, h, rgba)
    elif len(args) == 2:
        open(args[1], 'wb').write(apply(open(args[0], 'rb').read(), verbose=True))
    else:
        sys.exit(__doc__)
