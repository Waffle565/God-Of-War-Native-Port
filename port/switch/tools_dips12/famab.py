# Per-family VU1 native us/run, A vs B, all windows after the warp; mix vs speed decomposition.
import re,sys,os,collections
def load(path):
    acc=collections.Counter(); cnt=collections.Counter(); started=False; runs=0; ms=0
    for l in open(path,errors='replace'):
        if '[gow-warp] arrived' in l: started=True
        if not started or not l.startswith('[vu1nat-run] window'): continue
        m=re.search(r'native (\d+) runs ([\d.]+) ms',l); runs+=int(m.group(1)); ms+=float(m.group(2))
        main,_,misc=l.partition('misc by entry:')
        for name,c,us in re.findall(r'(\w+) nat (\d+) x ([\d.]+) us',main):
            if name=='misc': continue
            acc[name]+=int(c)*float(us); cnt[name]+=int(c)
        for name,c,us in re.findall(r'(part|kind\d+) (\d+) x ([\d.]+) us',misc):
            acc[name]+=int(c)*float(us); cnt[name]+=int(c)
    return acc,cnt,runs,ms
A,B=sys.argv[1],sys.argv[2]
for z in sys.argv[3:]:
    a,ca,ra,ma=load(os.path.join(A,z,'log.txt')); b,cb,rb,mb=load(os.path.join(B,z,'log.txt'))
    if not ra or not rb: print(z,'no data'); continue
    print('== %s  us/run A %.2f B %.2f  (runs A %d B %d)'%(z,1000*ma/ra,1000*mb/rb,ra,rb))
    speed=0; mix=0
    for k in sorted(set(ca)|set(cb), key=lambda k:-(a[k]+b[k])):
        ua=a[k]/ca[k] if ca[k] else 0; ub=b[k]/cb[k] if cb[k] else 0
        sa=ca[k]/ra; sb=cb[k]/rb
        if ca[k] and cb[k]: speed+=sb*(ua-ub)   # at B's mix
        mix+=(sa-sb)*(ua if ca[k] else ub)
        print('  %-9s A %7.2fus share %5.3f | B %7.2fus share %5.3f | d_us %+6.2f contrib(us/run) speed %+5.2f mix %+5.2f'%(k,ua,sa,ub,sb,ua-ub,sb*(ua-ub) if ca[k] and cb[k] else 0,(sa-sb)*(ua if ca[k] else ub)))
    print('  total speed %+.2f mix %+.2f'%(speed,mix))
