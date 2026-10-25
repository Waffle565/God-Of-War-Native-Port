# usage: pfs0.py <nsp> [outdir]  lists the PFS0 entries; with outdir extracts entries below 64 MB
import struct, sys, os
f = open(sys.argv[1], 'rb')
magic, n, strsz, _ = struct.unpack('<4sIII', f.read(16))
assert magic == b'PFS0', magic
ents = [struct.unpack('<QQII', f.read(24)) for _ in range(n)]
strs = f.read(strsz)
base = 16 + 24 * n + strsz
for off, size, no, _ in ents:
    name = strs[no:strs.index(b'\0', no)].decode()
    print(name, size)
    if len(sys.argv) > 2 and size < 64 << 20:
        os.makedirs(sys.argv[2], exist_ok=True)
        f.seek(base + off)
        open(os.path.join(sys.argv[2], name), 'wb').write(f.read(size))
