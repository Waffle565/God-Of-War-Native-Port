"""List the placed objects ("go..." records) of a zone WAD on the disc with their positions.

usage: python -I tools/gow_wadobjs.py <ZONE> [regex]      e.g. ATHN01E 'hydra|boss|check'

Zone WAD records (tools/lang_pack.py wad_records) of tag 393246 and size 92 named go<name> hold the placement of a
script group / entity: floats 12..14 rotation (radians), 15 scale, 16..18 translation. For most zones the translation is
the world position GOW_TELEPORT takes (checked: ATHN01A/01D/01E gostart = their warp points, ATHN01D gosavegamezone1 =
the altar Kratos's save loads at); moving zones (the ATHN01C ships) have their own frame. Strings of the WAD that name
sensors (trigger volumes, `-EntrySensor`) say which group starts a fight (docs/switch-sweep.md "Boss fights").
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lang_pack  # noqa: E402

DISC = os.path.join(os.path.dirname(HERE), 'disc-usa')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    zone = sys.argv[1].upper()
    pat = re.compile(sys.argv[2], re.I) if len(sys.argv) > 2 else None
    _, entries = lang_pack.read_toc(DISC)
    data = lang_pack.read_file(DISC, entries, zone if zone.endswith('.WAD') else zone + '.WAD')
    for pos, tag, size, name, _ in lang_pack.wad_records(data):
        nm = name.split(b'\0')[0].decode('latin1')
        if tag != 393246 or size != 92 or not nm.startswith('go') or (pat and not pat.search(nm)):
            continue
        f = struct.unpack_from('<19f', data, pos + 32)
        print(f'{nm:32s} pos {f[16]:8.0f} {f[17]:8.0f} {f[18]:8.0f}  scale {f[15]:g}')


if __name__ == '__main__':
    main()
