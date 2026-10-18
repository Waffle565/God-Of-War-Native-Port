"""dumpdiff.py a.dump b.dump [max]: compare two GOW_VU1N_REPLAY_DUMP files record by record."""
import struct
import sys
import collections

# VU1State layout (ps2_vu1.h): vf[32][4] f32, vi[16] i32, acc[4], q, p, i, r, pc, mac, clip, status, (pad) cycles u64, 6 bools,
# (pad) top, itop, branchPending, branchTarget, branchDelay
STATE = 32 * 16 + 64 + 16 + 4 * 8
def state_size():
    off = STATE
    off = (off + 7) & ~7
    off += 8          # cycles
    off += 6          # bools
    off = (off + 3) & ~3
    off += 8          # top, itop
    off += 1          # branchPending
    off = (off + 3) & ~3
    off += 8
    return (off + 7) & ~7
SS = state_size()

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
            yield st, data, pk, (h, rh, ended)

def fields(st):
    w = struct.unpack_from('<128I16i4I8I', st, 0)
    out = {}
    for r in range(32):
        for c in range(4):
            out[f'vf{r}.{"xyzw"[c]}'] = w[r * 4 + c]
    for r in range(16):
        out[f'vi{r}'] = w[128 + r] & 0xFFFFFFFF
    for c in range(4):
        out[f'acc.{"xyzw"[c]}'] = w[144 + c]
    for k, name in enumerate(['q', 'p', 'i', 'r', 'pc', 'mac', 'clip', 'status']):
        out[name] = w[148 + k]
    return out

def main():
    a, b = sys.argv[1], sys.argv[2]
    mx = int(sys.argv[3]) if len(sys.argv) > 3 else 15
    kinds = collections.Counter()
    shown = 0
    for k, (ra, rb) in enumerate(zip(records(a), records(b))):
        if ra[3][0] != ra[3][1] or rb[3][0] != rb[3][1]:
            kinds['hash a/b vs recorded %d/%d' % (ra[3][0] != ra[3][1], rb[3][0] != rb[3][1])] += 1
        if ra[3][2] != rb[3][2]:
            kinds['ended'] += 1
        if ra[:3] == rb[:3]:
            continue
        fa, fb = fields(ra[0]), fields(rb[0])
        d = [f"{n} {fa[n]:08x}/{fb[n]:08x}" for n in fa if fa[n] != fb[n]]
        md = [o for o in range(0, 16384, 4) if ra[1][o:o + 4] != rb[1][o:o + 4]]
        pd = len(ra[2]) != len(rb[2])
        po = [o for o in range(0, min(len(ra[2]), len(rb[2])), 4) if ra[2][o:o + 4] != rb[2][o:o + 4]]
        for x in d:
            kinds[x.split()[0].split('.')[0]] += 1
        if md: kinds['mem'] += 1
        if pd or po: kinds['pkt'] += 1
        if shown < mx:
            shown += 1
            mem = [f"mem[{o:x}] {struct.unpack_from('<I', ra[1], o)[0]:08x}/{struct.unpack_from('<I', rb[1], o)[0]:08x}" for o in md[:3]]
            pk = [f"pkt[{o:x}] {struct.unpack_from('<I', ra[2], o)[0]:08x}/{struct.unpack_from('<I', rb[2], o)[0]:08x}" for o in po[:3]]
            print(f"rec {k + 1}: " + "; ".join(d[:6] + mem + pk) + (f" pktlen {len(ra[2])}/{len(rb[2])}" if pd else "") +
                  f" (state {len(d)}, mem {len(md)}, pkt {len(po)})")
    print("kinds:", dict(kinds.most_common(20)))

main()
