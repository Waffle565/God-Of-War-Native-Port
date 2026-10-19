"""hlecmp.py ref.dump test.dump [show]: compare two GOW_VU1N_REPLAY_DUMP files with tolerances (GIF packets field by
field: XYZ, ST/Q, RGBA, ADC; final VI registers / pc; data memory words), for the non-bit-exact VU1 HLE kernels."""
import struct
import sys
import collections

SS = 664  # sizeof(VU1State) (dumpdiff.py state_size)


def records(path):
    with open(path, 'rb') as f:
        while True:
            st = f.read(SS)
            if len(st) < SS:
                return
            data = f.read(16384)
            n = struct.unpack('<I', f.read(4))[0]
            pk = f.read(n)
            h, rh, ended = struct.unpack('<QQB', f.read(17))
            yield st, data, pk, ended


def f32(w):
    return struct.unpack('<f', struct.pack('<I', w))[0]


def gif(pk):
    """yield (kind, reg, words) per register write; kind 'tag' for tags"""
    q = [struct.unpack_from('<4I', pk, o) for o in range(0, len(pk) - 15, 16)]
    i = 0
    while i < len(q):
        w = q[i]
        lo = w[0] | (w[1] << 32)
        hi = w[2] | (w[3] << 32)
        nloop = lo & 0x7FFF
        flg = (lo >> 58) & 3
        nreg = (lo >> 60) & 0xF or 16
        yield 'tag', -1, w
        i += 1
        if flg == 0:
            for _ in range(nloop):
                for r in range(nreg):
                    if i >= len(q):
                        return
                    yield 'packed', (hi >> (4 * r)) & 0xF, q[i]
                    i += 1
        elif flg == 1:
            n = (nloop * nreg + 1) // 2
            for _ in range(n):
                if i < len(q):
                    yield 'reglist', -1, q[i]
                i += 1
        else:
            for _ in range(nloop):
                if i < len(q):
                    yield 'image', -1, q[i]
                i += 1


def rel(a, b):
    fa, fb = f32(a), f32(b)
    if fa == fb:
        return 0.0
    d = abs(fa - fb)
    m = max(abs(fa), abs(fb))
    return d / m if m > 0 else d


def main():
    a, b = sys.argv[1], sys.argv[2]
    show = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    c = collections.Counter()
    worst = collections.defaultdict(float)
    shown = 0
    nrec = 0
    for k, (ra, rb) in enumerate(zip(records(a), records(b))):
        nrec += 1
        sa = struct.unpack_from('<128I16i4I8I', ra[0], 0)
        sb = struct.unpack_from('<128I16i4I8I', rb[0], 0)
        if sa[128 + 0:128 + 16] != sb[128:128 + 16]:
            c['rec: vi differ'] += 1
            if shown < show:
                shown += 1
                print(f'rec {k}: vi', [(r, sa[128 + r], sb[128 + r]) for r in range(16) if sa[128 + r] != sb[128 + r]])
        if sa[148 + 4] != sb[148 + 4]:
            c['rec: pc differ'] += 1
        if ra[3] != rb[3]:
            c['rec: ended differ'] += 1
        ga, gb = list(gif(ra[2])), list(gif(rb[2]))
        if len(ga) != len(gb):
            c['rec: gif length differ'] += 1
            if shown < show:
                shown += 1
                print(f'rec {k}: gif writes {len(ga)} vs {len(gb)}')
            continue
        bad = False
        for (ka, reg, wa), (kb, _, wb) in zip(ga, gb):
            if wa == wb:
                c['gif exact'] += 1
                continue
            if ka != 'packed' or reg not in (1, 2, 4, 5, 0xC, 0xD):
                c[f'gif {ka} reg {reg:x} differ'] += 1
                bad = True
                continue
            if reg == 2:  # ST, Q
                e = max(rel(wa[0], wb[0]), rel(wa[1], wb[1]), rel(wa[2], wb[2]))
                worst['st'] = max(worst['st'], e)
                c['st ok' if e < 1e-4 else 'st BAD'] += 1
                bad |= e >= 1e-4
            elif reg == 1:  # RGBA (Q from ST)
                e = max(abs((wa[i] & 0xFF) - (wb[i] & 0xFF)) for i in range(4))
                worst['rgba'] = max(worst['rgba'], e)
                c['rgba ok' if e <= 2 else 'rgba BAD'] += 1
                bad |= e > 2
            else:  # XYZ(F)2/3
                ex = max(abs(((wa[i] & 0xFFFF) - (wb[i] & 0xFFFF) + 0x8000) % 0x10000 - 0x8000) for i in (0, 1))
                if reg in (5, 0xD):
                    ez = abs(wa[2] - wb[2])
                    ezr = ez / max(1, wa[2])
                else:
                    ez = abs(((wa[2] >> 4) & 0xFFFFFF) - ((wb[2] >> 4) & 0xFFFFFF))
                    ezr = ez / max(1, (wa[2] >> 4) & 0xFFFFFF)
                adc = ((wa[3] >> 15) & 1) != ((wb[3] >> 15) & 1)
                worst['xy'] = max(worst['xy'], ex)
                worst['z_rel'] = max(worst['z_rel'], ezr)
                if adc:
                    c['xyz ADC differ'] += 1
                elif ex <= 2 and (ez <= 4 or ezr < 1e-5):
                    c['xyz ok'] += 1
                else:
                    c['xyz BAD'] += 1
                    bad = True
            if bad and shown < show:
                shown += 1
                print(f'rec {k}: reg {reg:x} {["%08x" % x for x in wa]} vs {["%08x" % x for x in wb]}')
        dw = sum(1 for o in range(0, 16384, 4) if ra[1][o:o + 4] != rb[1][o:o + 4])
        c['data words differ'] += dw
        if bad:
            c['rec: gif beyond tolerance'] += 1
    print(f'{nrec} records')
    for kk, v in sorted(c.items()):
        print(f'  {kk}: {v}')
    print('  worst:', {k: round(v, 7) for k, v in worst.items()})


if __name__ == '__main__':
    main()
