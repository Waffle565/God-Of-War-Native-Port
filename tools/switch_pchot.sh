# hot addresses inside one function of a console PC-sample file (GOW_PC_SAMPLE), with source lines. WSL:
#   [LO=<address hex, picks one of several clones>] bash tools/switch_pchot.sh <pcs_<thread>.txt> <function name substring> <first_ms> <last_ms>  (ELF: $ELF, default /root/elf_dips7.elf). Run with `wsl --exec bash` (plain `wsl -- bash -c` expands $VAR on the Windows side)
cd "/mnt/c/Users/lovas/Claude/God Of War"
B=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
line=$($B-nm -S -C ${ELF:-/root/elf_dips7.elf} | grep -F "$2" | grep ' [TtWw] ' | grep -F "${LO:-}" | head -1)
lo=$(echo "$line" | awk '{print $1}'); sz=$(echo "$line" | awk '{print $2}')
echo "func $lo size $sz"
python3 - "$1" $lo $sz $3 $4 > /tmp/hot.txt <<'PY'
import sys
f,lo,sz,a,b=sys.argv[1],int(sys.argv[2],16),int(sys.argv[3],16),int(sys.argv[4]),int(sys.argv[5])
cnt={};cur=None
for l in open(f):
    p=l.split()
    if not p: continue
    if p[0]=='w':
        if cur is not None and a<=wend<=b:
            for k,v in cur.items(): cnt[k]=cnt.get(k,0)+v
        wend=int(p[1]); cur={}; continue
    if p[0]=='c': continue
    try: off=int(p[0],16); n=int(p[1])
    except: continue
    if lo<=off<lo+sz: cur[off]=cur.get(off,0)+n
if cur is not None and a<=wend<=b:
    for k,v in cur.items(): cnt[k]=cnt.get(k,0)+v
tot=sum(cnt.values()); print('total',tot)
for k,v in sorted(cnt.items(),key=lambda x:-x[1])[:30]: print('%x %d'%(k,v))
PY
head -1 /tmp/hot.txt
tail -n +2 /tmp/hot.txt > /tmp/hot2.txt
$B-addr2line -i -a -e ${ELF:-/root/elf_dips7.elf} $(awk '{print "0x"$1}' /tmp/hot2.txt) | awk '/^0x/{if(s)print s; s=$1; next}{sub(/.*\//,""); s=s" "$0} END{print s}' > /tmp/hot3.txt
paste -d' ' <(awk '{print $2}' /tmp/hot2.txt) /tmp/hot3.txt
