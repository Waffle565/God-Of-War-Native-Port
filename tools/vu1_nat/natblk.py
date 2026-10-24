# Block-level view of a GOW_VU1NAT_TRACEPC trace: usage natblk.py <trace with [pc] lines> (module pairs collapsed)
import sys
lines=[l.split() for l in open(sys.argv[1]) if l.startswith('[pc]')]
segs=[]; prev=None
for p in lines:
    pc=int(p[1],16)
    if pc>=0x2828:
        if segs and segs[-1][0]=='M': segs[-1][1]+=1
        else: segs.append(['M',1,p[2:]])
    elif prev is None or pc!=prev+8 or segs[-1][0]=='M':
        segs.append([pc,pc,p[2:]])
    else: segs[-1][1]=pc
    prev=pc
for a,b,vi in segs:
    if a=='M': print('module %d pairs   | %s'%(b,' '.join(vi)))
    else: print('%04x-%04x | %s'%(a,b,' '.join(vi)))
