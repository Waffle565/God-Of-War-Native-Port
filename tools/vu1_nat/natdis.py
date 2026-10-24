"""Disassembly with the upper special ops decoded correctly (vudis.py prints the ACC forms SUBAz / MADDAw / ... with a
bogus vf destination). usage: natdis.py <16K code .bin> <vudis.py --bin output for it> <start-end hex> ...
(docs/native-render-study.md, nat-vu1b)"""
import struct,sys
c=open(sys.argv[1],'rb').read()
T={0:'ADDAx',1:'ADDAy',2:'ADDAz',3:'ADDAw',4:'SUBAx',5:'SUBAy',6:'SUBAz',7:'SUBAw',8:'MADDAx',9:'MADDAy',10:'MADDAz',11:'MADDAw',12:'MSUBAx',13:'MSUBAy',14:'MSUBAz',15:'MSUBAw',0x10:'ITOF0',0x11:'ITOF4',0x12:'ITOF12',0x13:'ITOF15',0x14:'FTOI0',0x15:'FTOI4',0x16:'FTOI12',0x17:'FTOI15',0x18:'MULAx',0x19:'MULAy',0x1a:'MULAz',0x1b:'MULAw',0x1c:'MULAq',0x1d:'ABS',0x1e:'MULAi',0x1f:'CLIP',0x20:'ADDAq',0x21:'MADDAq',0x22:'ADDAi',0x23:'MADDAi',0x24:'SUBAq',0x25:'MSUBAq',0x26:'SUBAi',0x27:'MSUBAi',0x28:'ADDA',0x29:'MADDA',0x2a:'MULA',0x2c:'SUBA',0x2d:'MSUBA',0x2e:'OPMULA',0x2f:'NOP'}
D=lambda u:''.join(ch for ch,b in zip('xyzw',(8,4,2,1)) if (u>>21)&b)
dis={}
for l in open(sys.argv[2],encoding='utf-8-sig',errors='replace'):
    if len(l)>5 and l[:4].strip():
        try: pc=int(l[:4],16)
        except: continue
        dis[pc]=(l[13:50].strip(),l[50:].strip())
for r in sys.argv[3:]:
    a,b=[int(x,16) for x in r.split('-')]
    for pc in range(a,b+8,8):
        lo,up=struct.unpack('<2I',c[pc:pc+8])
        u,l=dis.get(pc,('?','?'))
        if (up&63)>=0x3c:
            n=T.get((((up>>6)&31)<<2)|(up&3),'?')
            fs,ft=(up>>11)&31,(up>>16)&31
            u='nop' if n=='NOP' else '%s.%s [fs vf%d ft vf%d]'%(n.lower(),D(up),fs,ft)
        if l.startswith('move. vf0, vf0'): l=l[14:].strip()
        if u=='nop' and not l: continue
        print('%04x %-40s %s'%(pc,u,l))
    print('--')
