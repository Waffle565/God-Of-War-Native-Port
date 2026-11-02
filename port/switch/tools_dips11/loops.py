import re, sys
# loops.py <objdump lines file>: backward branches (loop back-edges) and the instruction count of their span
L = [l for l in open(sys.argv[1]) if re.match(r'\s*[0-9a-f]+:', l)]
addr = [int(l.split(':')[0], 16) for l in L]
for i, l in enumerate(L):
    m = re.search(r'\t(b\.\w+|cbn?z|tbn?z|b)\s.*?([0-9a-f]{6,})\s<', l)
    if m:
        t = int(m.group(2), 16)
        if t < addr[i]:
            j = addr.index(t) if t in addr else None
            if j is not None:
                print('%x -> %x  %s  span %d instr' % (addr[i], t, m.group(1), i - j + 1))
