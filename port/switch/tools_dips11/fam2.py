import re,sys,os,collections
base=sys.argv[1]
for z in sys.argv[2:]:
    acc=collections.Counter(); cnt=collections.Counter(); tot=0; n=0; job=0; started=False
    for l in open(os.path.join(base,z,'log.txt'),errors='replace'):
        if '[gow-warp] arrived' in l: started=True
        if not started: continue
        if l.startswith('[gsw-split]'): job=float(re.search(r'job_ms=([\d.]+)',l).group(1))
        if l.startswith('[vu1nat-run]') and job>4000:
            m=re.search(r'native (\d+) runs ([\d.]+) ms',l); tot+=float(m.group(2)); n+=1
            for name,c,us in re.findall(r'(\w+) nat (\d+) x ([\d.]+) us',l): acc[name]+=int(c)*float(us)/1000; cnt[name]+=int(c)
            for name,c,us in re.findall(r'(part|kind\d+) (\d+) x ([\d.]+) us',l): acc['m.'+name]+=int(c)*float(us)/1000; cnt['m.'+name]+=int(c)
    if n: print(z,'n=%d native=%.0f'%(n,tot/n),' '.join('%s=%.0f(%.1fus)'%(k,v/n,1000*v/cnt[k]) for k,v in acc.most_common(8)))
