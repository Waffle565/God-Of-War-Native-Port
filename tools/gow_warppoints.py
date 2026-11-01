"""List the warp points of the loaded zones in an EE RAM dump (GOW_RAM_DUMP), with world positions for GOW_TELEPORT.

usage: python -I tools/gow_warppoints.py <ram.bin> [...]

The warp-point lookup 0x1ADFF8 walks the list at 0x302600 (sentinel; node +0 next, +8 object). Per object: +0x46 kind
(1 = the zone's default warp point, 2 = entry from a named zone: +0xB0 offset of the name in the string block at
*(+0x58), resolved pointer cached at +0xB4), local transform +0x00..+0x3F (rows, translation +0x30), node index +0x40
(u16), owner *(+0x54); 0x1A95D8 multiplies the local transform by the owner's node matrix (*(*(owner+0x104)+0x8C) +
index*0x40), so world = x*N0 + y*N1 + z*N2 + N3. Also prints Kratos's position (player object *(*0x29CD58) +0x80).
"""
import struct
import sys


def u32(m, a):
    return struct.unpack_from('<I', m, a)[0] & 0x1FFFFFF


def cstr(m, a):
    if not 0 < a < len(m):
        return '?'
    e = m.find(b'\0', a, a + 40)
    s = m[a:e if e > 0 else a + 40]
    return s.decode('latin1') if s and all(32 <= c < 127 for c in s) else '?'


def main():
    for path in sys.argv[1:]:
        m = open(path, 'rb').read()
        holder = u32(m, 0x29CD58)
        player = u32(m, holder)
        kx = struct.unpack_from('<3f', m, player + 0x80)
        print(f'== {path}: Kratos {kx[0]:.1f},{kx[1]:.1f},{kx[2]:.1f}')
        head = 0x302600
        node = u32(m, head)
        seen = 0
        while node != head and 0x100000 <= node < len(m) and seen < 2000:
            seen += 1
            o = u32(m, node + 8)
            if 0x100000 <= o < len(m) - 0x100:
                kind = struct.unpack_from('<H', m, o + 0x46)[0]
                idx = struct.unpack_from('<H', m, o + 0x40)[0]
                local = struct.unpack_from('<16f', m, o)
                owner = u32(m, o + 0x54)
                s0 = u32(m, owner + 0x104) if 0x100000 <= owner < len(m) else 0
                arr = u32(m, s0 + 0x8C) if 0x100000 <= s0 < len(m) else 0
                world = None
                if 0x100000 <= arr < len(m) - 0x40 * (idx + 1):
                    n = struct.unpack_from('<16f', m, arr + idx * 0x40)
                    x, y, z = local[12:15]
                    world = [x * n[c] + y * n[4 + c] + z * n[8 + c] + n[12 + c] for c in range(3)]
                name = ''
                if kind == 2:
                    off = struct.unpack_from('<i', m, o + 0xB0)[0]
                    name = cstr(m, u32(m, o + 0xB4)) if u32(m, o + 0xB4) else '?'
                    if name == '?' and off != -1:
                        name = cstr(m, u32(m, o + 0x58) + off)
                w = f'{world[0]:.1f},{world[1]:.1f},{world[2]:.1f}' if world else '?'
                print(f'obj {o:x} kind {kind} from {name or "-"} node {idx} owner {owner:x} world {w} '
                      f'local {local[12]:.1f},{local[13]:.1f},{local[14]:.1f}')
            node = u32(m, node)


if __name__ == '__main__':
    main()
