# hot addresses inside one function of a console PC-sample file (GOW_PC_SAMPLE), with source lines. WSL:
#   [LO=<address hex, picks one of several clones>] bash tools/switch_pchot.sh <pcs_<thread>.txt> <function name substring> <first_ms> <last_ms>  (ELF: $ELF, default /root/elf_dips7.elf)
cd "/mnt/c/Users/lovas/Claude/God Of War"
B=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
read lo sz name < <($B-nm -S -C ${ELF:-/root/elf_dips7.elf} | grep -F "$2" | grep ' [TtWw] ' | grep -F "${LO:-}" | head -1 | awk '{print $1, $2, $4}')
echo "func $lo size $sz"
python3 - "$1" $lo $sz $3 $4 > /tmp/hot.txt <<'PY'
import sys
f,lo,sz,a,b=sys.argv[1],int(sys.argv[2],16),int(sys.argv[3],16),int(sys.argv[4]),int(sys.argv[5])
cnt={};cur=None;win=[]
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
for k,v in sorted(cnt.items(),key=lambda x:-x[1])[:25]: print('%x %d'%(k,v))
PY
head -1 /tmp/hot.txt
tail -n +2 /tmp/hot.txt | while read a n; do echo "$n $a $($B-addr2line -i -e ${ELF:-/root/elf_dips7.elf} 0x$a | sed 's#.*/##' | tr '\n' ' ')"; done
