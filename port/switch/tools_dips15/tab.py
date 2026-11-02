import re,sys
# tab.py log [t_lo t_hi]: one row per 5 s window
lo=float(sys.argv[2]) if len(sys.argv)>2 else 0; hi=float(sys.argv[3]) if len(sys.argv)>3 else 1e9
def kv(l): return dict(re.findall(r'(\w+)=([-\d.]+)',l))
rows=[];cur={}
for l in open(sys.argv[1],errors='replace'):
    if l.startswith('[gow-perf]'):
        d=kv(l); cur={'t':float(d['t']),'fps':float(d['guest_flip_hz']),'ee':float(d['ee_ms']),'iop':float(d['iop_ms']),'gw':float(d['guest_wait_ms']),'gsq':float(d['gsq_wait_ms'])}; rows.append(cur)
    elif not cur: continue
    elif l.startswith('[gsw-split]'): d=kv(l); cur['job']=float(d['job_ms']); cur['vif']=float(d['vif_ms'])
    elif l.startswith('[gsw-idle]'): cur['vidle']=float(kv(l)['idle_ms'])
    elif l.startswith('[gs-gl]'): d=kv(l); cur['gl']=float(d['busy_ms'])
    elif l.startswith('[gs-dk] draws'): d=kv(l); cur['dkw']=float(d['wait_ms'])
    elif l.startswith('[gsw-chain]'): cur['chain']=float(kv(l)['chain_ms'])
    elif l.startswith('[gs-dk-time]'): d=kv(l); cur['acq']=float(d['acquire_ms']); cur['psub']=float(d['present_submit_ms'])
    elif l.startswith('[gow-player]'):
        d=re.search(r'state=(\d+)',l); cur['st']=d.group(1) if d else ''
cols=['t','fps','ee','iop','gw','gsq','chain','job','vif','vidle','gl','dkw','acq','psub','st']
print(' '.join('%6s'%c for c in cols))
for r in rows:
    if lo<=r['t']<=hi: print(' '.join('%6s'%(('%.0f'%r[c]) if isinstance(r.get(c),float) else r.get(c,'-')) for c in cols))
