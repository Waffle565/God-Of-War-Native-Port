#!/bin/bash
# inl.sh <pcs file> <elf> <mangled-substr> [n]: top sample addresses in the function, with addr2line -i chains
P=$1; E=$2; F=$3; N=${4:-12}
A2L=aarch64-linux-gnu-addr2line
NM=aarch64-linux-gnu-nm

read st sz <<< $($NM -S $E | grep " [tTW] " | grep -m1 "$F" | awk '{print $1, $2}')
python3 - "$P" $st $sz $N > /tmp/inl_addrs.txt <<'PY'
import sys
p, st, sz, n = sys.argv[1], int(sys.argv[2],16), int(sys.argv[3],16), int(sys.argv[4])
c = {}
for l in open(p):
    f = l.split()
    if len(f) != 2: continue
    try: a = int(f[0],16); k = int(f[1])
    except: continue
    if st <= a < st+sz: c[a] = c.get(a,0)+k
tot = sum(c.values())
print(tot)
for a,k in sorted(c.items(), key=lambda x:-x[1])[:n]: print('%x %d' % (a,k))
PY
head -1 /tmp/inl_addrs.txt
# one addr2line call for all addresses (DWARF loaded once); "== addr count" then the inline chain
tail -n +2 /tmp/inl_addrs.txt > /tmp/inl_top.txt
$A2L -a -i -f -C -e $E $(awk '{print $1}' /tmp/inl_top.txt) | python3 -c '
import sys
cnt = dict(l.split() for l in open("/tmp/inl_top.txt"))
lines = [l.rstrip() for l in sys.stdin]
i = 0
while i < len(lines):
    if lines[i].startswith("0x"):
        a = lines[i][2:].lstrip("0"); print("== %s %s" % (a, cnt.get(a, "?"))); i += 1; continue
    fn = lines[i].split("(")[0]; loc = lines[i + 1] if i + 1 < len(lines) else ""
    print("   %s  %s" % (fn, loc.split("/")[-1])); i += 2
'
