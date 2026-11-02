# Robust A/B per family: median of per-window us/run (spike windows, e.g. warp captures / hitches, drop out),
# and us/run of the zone at B's mix with those medians.
import re,sys,os,collections,statistics as st
def load(path):
    per=collections.defaultdict(list); cnt=collections.Counter(); started=False
    for l in open(path,errors='replace'):
        if '[gow-warp] arrived' in l: started=True
        if not started or not l.startswith('[vu1nat-run] window'): continue
        main,_,misc=l.partition('misc by entry:')
        items=[(n,int(c),float(u)) for n,c,u in re.findall(r'(\w+) nat (\d+) x ([\d.]+) us',main) if n!='misc']
        items+=[(n,int(c),float(u)) for n,c,u in re.findall(r'(part|kind\d+) (\d+) x ([\d.]+) us',misc)]
        for n,c,u in items:
            if c>=200: per[n].append(u)
            cnt[n]+=c
    return {k:st.median(v) for k,v in per.items() if v},cnt
A,B=sys.argv[1],sys.argv[2]
for z in sys.argv[3:]:
    a,ca=load(os.path.join(A,z,'log.txt')); b,cb=load(os.path.join(B,z,'log.txt'))
    tb=sum(cb.values()); ua=ub=0; rows=[]
    for k in cb:
        if k in a and k in b:
            s=cb[k]/tb; ua+=s*a[k]; ub+=s*b[k]; rows.append((s*(a[k]-b[k]),k,a[k],b[k],s))
    rows.sort()
    print('== %s at B mix: A %.2f B %.2f us/run (%+.1f%%) | '%(z,ua,ub,100*(ua-ub)/ub)+' '.join('%s %.2f/%.2f(%+.1f%%,s%.2f)'%(k,x,y,100*(x-y)/max(y,1e-9),s) for d,k,x,y,s in rows if abs(d)>0.005))
