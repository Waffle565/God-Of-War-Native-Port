#!/usr/bin/env python3
"""989snd (989NOMID.IRX) RPC front end: command -> wrapper -> what it calls (docs/sound.md "989snd RPC interface").

The main server (sid 0x123456, 989NOMID 0x1878) calls handler[fn](buffer) from the table at .data 0x1de88 and then
writes -1 to reply word 1; each handler is a short wrapper that unpacks the arguments and calls one 989snd function
(most of them exported as snd989 #n, the export table SMPD_IOP links to). Prints one line per command: fn, wrapper
offset, its instruction count, the calls it makes (#n = snd989 export, lib:n = import, hex = internal function).

  python -I tools/iop/snd989_rpcmap.py [disc-usa/IOP_MOD/989NOMID.IRX]
"""
import os
import struct
import sys

import capstone

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "disc-usa", "IOP_MOD", "989NOMID.IRX")
    d = open(path, "rb").read()
    phoff = struct.unpack_from("<I", d, 0x1C)[0]
    phnum = struct.unpack_from("<H", d, 0x2C)[0]
    seg = None
    for i in range(phnum):
        kind, off, va, pa, fsize, msize = struct.unpack_from("<6I", d, phoff + i * 32)
        if kind == 1:
            seg = (off, fsize)
    img = d[seg[0]: seg[0] + seg[1]]  # unrelocated: addresses are module offsets

    def w(o):
        return struct.unpack_from("<I", img, o)[0]

    exports, imports, text_end = {}, {}, None
    for off in range(0, len(img) - 24, 4):
        if w(off) == 0x41C00000 and img[off + 12: off + 18] == b"snd989":
            i, p = 0, off + 20
            while i < 200 and not (w(p) == 0 and (w(p + 4) == 0x41E00000 or i > 100)):
                if w(p):
                    exports.setdefault(w(p), i)
                i, p = i + 1, p + 4
        if w(off) == 0x41E00000:
            name = img[off + 12: off + 20].split(b"\0")[0].decode()
            q = off + 20
            while w(q) or w(q + 4):
                imports[q] = "%s:%d" % (name, w(q + 4) & 0xFFFF)
                q += 8
    md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
    table = 0x1DE88
    print("fn  wrapper insns calls")
    for fn in range(0x6B):
        start = w(table + 4 * fn)
        calls, n = [], 0
        for ins in md.disasm(img[start: start + 0x400], start):
            n += 1
            if ins.mnemonic == "jal":
                t = int(ins.op_str, 16)
                calls.append("#%d" % exports[t] if t in exports else imports.get(t, "%x" % t))
            if ins.mnemonic == "jr" and ins.op_str == "$ra":
                break
        print("%02x %6x %4d  %s" % (fn, start, n, " ".join(calls)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
