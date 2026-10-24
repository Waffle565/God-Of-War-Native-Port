"""Compare the pad/memory-card protocol of two runs (oracle IRX path vs native service).
usage: padmc_cmp.py LOG_A LOG_B  (both with PS2X_TRACE_RPC=800013 PS2X_IOP_TRACE_DMA=1)
Per card command (status polls = cmd 4 skipped): the command words, the data DMAs (dst, size, fnv) and the
status block (dst slot ignored, word 0 = counter ignored), the reply words."""
import re, sys

def parse(path):
    cmds = []; cur = None
    for l in open(path, 'rb').read().decode('latin1').splitlines():
        if '[rpc] sid=0x800013' in l:
            w = l.split('|')[1].split() if '|' in l else []
            sid = re.search(r'sid=0x([0-9a-f]+)', l).group(1); fn = re.search(r'fn=0x([0-9a-f]+)', l).group(1)
            cur = {'k': (sid, fn, tuple(w)), 'dma': [], 'rep': None}
            cmds.append(cur)
        elif '[rpc-reply] sid=0x800013' in l and cur is not None:
            cur['rep'] = tuple(l.split('|')[1].split())
        elif '[IOP:sifdma]' in l and cur is not None:
            src = int(re.search(r'src=0x([0-9a-f]+)', l).group(1), 16)
            if src >= 0x20000:
                continue
            dst = int(re.search(r'dst=0x([0-9a-f]+)', l).group(1), 16); size = int(re.search(r'size=0x([0-9a-f]+)', l).group(1), 16)
            fnv = re.search(r'fnv=([0-9a-f]+)', l); by = l.split('bytes=')[1].split()
            cur['dma'].append((dst, size, fnv.group(1) if fnv else None, tuple(by)))
    return cmds

def norm(c):
    sid, fn, w = c['k']
    if sid == '8000131c' and len(w) > 1 and w[1] == '4':
        return None
    dmas = []
    for dst, size, fnv, by in c['dma']:
        if size == 0x80 and sid == '8000131c':
            dmas.append(('status', by[4:]))  # counter dropped, slot ignored (16 bytes shown)
        else:
            dmas.append((hex(dst), size, fnv))
    return (sid, fn, w, tuple(dmas), c['rep'] if '--replies' in sys.argv else None)

args = [x for x in sys.argv[1:] if not x.startswith('--')]
a = [x for x in map(norm, parse(args[0])) if x]
b = [x for x in map(norm, parse(args[1])) if x]
print('commands (no polls):', len(a), len(b))
n = min(len(a), len(b)); bad = 0
for i in range(n):
    if a[i] != b[i]:
        bad += 1
        if bad <= 10:
            print('DIFF at', i); print('  A', a[i]); print('  B', b[i])
print('equal prefix compared:', n, 'differences:', bad)
