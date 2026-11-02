import re,sys,statistics as st,os
base=sys.argv[1]
def num(s,k):
    m=re.search(r'(?:^|\s)'+re.escape(k)+r'=([\d.]+)',s); return float(m.group(1)) if m else None
def med(a): a=[x for x in a if x is not None]; return round(st.median(a),1) if a else None
print("zone fps glbusy gl_clut gl_q gswidle job vu vif ee vu_ms gs_ms iop gsq dkwait dkwaitms stagefull draws")
for z in sys.argv[2:]:
    L=open(os.path.join(base,z,'log.txt'),errors='replace').read().splitlines()
    # skip first third (boot / warp)
    rows={k:[] for k in 'fps glbusy clut q idle job vu vif ee vums gsms iop gsq dkw dkwms full draws'.split()}
    started=False
    for l in L:
        if l.startswith('[gow-warp]'): started=True
        if not started: continue
        if l.startswith('[gow-perf]'):
            rows['fps'].append(num(l,'guest_flip_hz')); rows['ee'].append(num(l,'ee_ms')); rows['vums'].append(num(l,'vu_ms')); rows['gsms'].append(num(l,'gs_ms')); rows['iop'].append(num(l,'iop_ms')); rows['gsq'].append(num(l,'gsq_wait_ms'))
        elif l.startswith('[gs-gl]'):
            rows['glbusy'].append(num(l,'busy_ms'))
            m=re.search(r'clut=([\d.]+)ms',l); rows['clut'].append(float(m.group(1)) if m else None)
            m=re.search(r'\?=([\d.]+)ms',l); rows['q'].append(float(m.group(1)) if m else None)
        elif l.startswith('[gsw-idle]'): rows['idle'].append(num(l,'idle_ms'))
        elif l.startswith('[gsw-split]'): rows['job'].append(num(l,'job_ms')); rows['vu'].append(num(l,'vu_ms')); rows['vif'].append(num(l,'vif_ms'))
        elif l.startswith('[gs-dk]') and 'waits=' in l: rows['dkw'].append(num(l,'waits')); rows['dkwms'].append(num(l,'wait_ms')); rows['draws'].append(num(l,'draws'))
        elif l.startswith('[gs-stage]'): rows['full'].append(num(l,'full_ms'))
    print(z,*[med(rows[k][-12:]) for k in 'fps glbusy clut q idle job vu vif ee vums gsms iop gsq dkw dkwms full draws'.split()])
