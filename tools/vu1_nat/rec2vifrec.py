"""rec2vifrec.py <GOW_VU1N_RECORD file> <out vifrec> [max]: VU1 run records as a GOW_VIF1_REC file with runs only
(no VIF stream, VIF registers zero), so the classifier / dispatch oracle of tools/vu1_nat can run on the older
recordings (fight.rec, menu.rec). The decoder check is meaningless on such a file."""
import struct, sys
SS = 664; RS = 16 + SS + 16384 + 16384
src, dst = sys.argv[1], sys.argv[2]
mx = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
with open(src, 'rb') as f, open(dst, 'wb') as o:
    o.write(struct.pack('<4I', 0x52464956, 1, SS, 92))
    n = 0
    while n < mx:
        r = f.read(RS)
        if len(r) < RS:
            break
        st = r[16:16 + SS]
        # VU1State (x86-64 / aarch64 layout): vf 0, vi 512, acc 576, q p i r 592, pc 608, ..., top 640, itop 644
        pc = struct.unpack_from('<I', st, 608)[0]
        top, itop = struct.unpack_from('<II', st, 640)
        o.write(struct.pack('<6I', 2, 16 + SS + 92 + 32768, pc, top, itop, 0))
        o.write(st); o.write(b'\0' * 92); o.write(r[16 + SS:])
        n += 1
print(n, 'runs')
