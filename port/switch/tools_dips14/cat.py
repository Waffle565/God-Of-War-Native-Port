import re,collections,sys
# cat.py <funcs.py output>: shares by category (EE game code, IOP, waits, runtime functions)
c=collections.Counter()
for l in open(sys.argv[1]):
    m=re.match(r'\s*([\d.]+)% (.*)',l)
    if not m: continue
    v=float(m.group(1)); n=m.group(2)
    if n.lstrip('~').startswith('sub_'): k='EE game code'
    elif 'iop::' in n or 'SoundEngine' in n: k='IOP'
    elif 'svcWait' in n: k='wait'
    else: k='rt: '+n
    c[k]+=v
for k,v in c.most_common(int(sys.argv[2]) if len(sys.argv)>2 else 40): print('%6.2f %s'%(v,k[:140]))
