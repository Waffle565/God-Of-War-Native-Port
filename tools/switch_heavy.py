"""GS-thread cost per vertex in the heavy windows of sweep logs (>15 M vertices / 5 s, flip > 20 Hz).

usage: python tools/switch_heavy.py <sweep out dir>... (e.g. port/switch/sweep/ab3_dips7); per zone log: windows,
mean guest flip Hz, M vertices per 5 s, [gs-gl] busy ms and ns per vertex (needs GOW_PERF_DIAG lines, sweep default)."""
import re,sys,glob
for d in sys.argv[1:]:
  for L in sorted(glob.glob(d+'/*/*/log.txt')):
    flips=[];hcpu=[];gl=[]
    for l in open(L,encoding='utf-8',errors='replace'):
        if '[gow-perf]' in l: flips.append(float(re.search(r'guest_flip_hz=([\d.]+)',l).group(1)))
        elif '[gs-hw-cpu]' in l: hcpu.append((int(re.search(r'vertices=(\d+)',l).group(1)),float(re.search(r'draw_ms=([\d.]+)',l).group(1))))
        elif '[gs-gl]' in l: gl.append(float(re.search(r'busy_ms=([\d.]+)',l).group(1)))
    n=min(len(flips),len(hcpu),len(gl))
    rows=[(flips[i],hcpu[i][0],gl[i]) for i in range(n) if hcpu[i][0]>15e6 and flips[i]>20]
    if rows:
        f=sum(r[0] for r in rows)/len(rows); v=sum(r[1] for r in rows)/len(rows); b=sum(r[2] for r in rows)/len(rows)
        print(L.replace(chr(92),'/').split('/')[0], L.replace(chr(92),'/').split('/')[-2], 'heavy windows',len(rows),'flip %.1f'%f,'Mvert/5s %.1f'%(v/1e6),'gl busy %.0f'%b,'ns/vert %.0f'%(b*1e6/v))
    else: print(L.replace(chr(92),'/').split('/')[0], L.replace(chr(92),'/').split('/')[-2],'no heavy windows; max vert %.1fM'%(max(h[0] for h in hcpu)/1e6))
