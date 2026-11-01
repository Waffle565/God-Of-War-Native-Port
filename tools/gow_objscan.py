"""List the game objects of an EE RAM dump (GOW_RAM_DUMP) with their world positions, grouped by class (vtable).

usage: python -I tools/gow_objscan.py <ram.bin> [--near X,Y,Z] [--min-count 1] [--top 40]

The objects the warp-point jump moves (0x1B26F0, docs/game-internals.md "Moving Kratos") carry a vtable pointer at +4
(method +0x2C = "apply transform", int16 this-adjust at +0x28) and two 4x4 transforms: the new one at +0x10..+0x4F and
the current one at +0x50..+0x8F (translation row +0x80, w = 1.0 at +0x8C). The player object is *(*0x29CD58). This
scans every 16-byte aligned address for that shape (vtable whose +0x2C is EE code, rows of finite floats, w 1.0) and
prints one line per class: count, the vtable's +0x2C method, and the positions (sorted by distance to --near, default
Kratos). Boss arenas: the classes that occur only in a boss zone's dump, near their creature positions.
"""
import argparse
import math
import struct
import sys


def u32(m, a):
    return struct.unpack_from('<I', m, a)[0]


def f32(m, a, n):
    return struct.unpack_from('<%df' % n, m, a)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--near', default='')
    ap.add_argument('--min-count', type=int, default=1)
    ap.add_argument('--top', type=int, default=60)
    ap.add_argument('--per-class', type=int, default=6)
    a = ap.parse_args()
    m = open(a.dump, 'rb').read()
    size = len(m)
    holder = u32(m, 0x29CD58) & 0x1FFFFFF
    player = u32(m, holder) & 0x1FFFFFF if 0x100000 <= holder < size else 0
    kpos = f32(m, player + 0x80, 3) if player else (0.0, 0.0, 0.0)
    print(f'player object {player:x} vtable {u32(m, player + 4) & 0x1FFFFFF:x} pos {kpos[0]:.1f},{kpos[1]:.1f},{kpos[2]:.1f}')
    near = tuple(float(x) for x in a.near.split(',')) if a.near else kpos
    one = struct.pack('<f', 1.0)
    vt_ok = {}
    classes = {}
    # w of the current translation row is 1.0: find those first (fast), then check the rest
    pos = m.find(one, 0x100000)
    while pos != -1:
        o = pos - 0x8C
        if o >= 0x100000 and o % 16 == 0:
            vt = u32(m, o + 4) & 0x1FFFFFF
            ok = vt_ok.get(vt)
            if ok is None:
                ok = 0x100000 <= vt < size - 0x30 and 0x100000 <= u32(m, vt + 0x2C) < 0x300000 and u32(m, vt + 0x2C) % 4 == 0
                vt_ok[vt] = ok
            if ok:
                rows = f32(m, o + 0x50, 16)
                if all(math.isfinite(v) and abs(v) < 1e6 for v in rows) and rows[3] == 0.0 and rows[7] == 0.0 and rows[11] == 0.0:
                    # rotation rows of unit-ish length
                    if all(0.01 < math.sqrt(sum(r * r for r in rows[k:k + 3])) < 100 for k in (0, 4, 8)):
                        classes.setdefault(vt, []).append((o, rows[12:15]))
        pos = m.find(one, pos + 4)
    out = []
    for vt, objs in classes.items():
        if len(objs) < a.min_count:
            continue
        objs.sort(key=lambda t: math.dist(t[1], near))
        out.append((math.dist(objs[0][1], near), vt, objs))
    out.sort()
    for d, vt, objs in out[:a.top]:
        ps = ' '.join(f'{o:x}@{p[0]:.0f},{p[1]:.0f},{p[2]:.0f}' for o, p in objs[:a.per_class])
        print(f'vt {vt:x} m2c {u32(m, vt + 0x2C):x} n={len(objs)} nearest {d:.0f}: {ps}')


if __name__ == '__main__':
    sys.exit(main())
