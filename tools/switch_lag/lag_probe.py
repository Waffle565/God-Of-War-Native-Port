#!/usr/bin/env python3
"""Input-lag probe (docs/performance.md "Input lag"): in gameplay, hold START for d ms then pixelPeek, release,
unpause; repeated for several d. Prints per trial: d, sent->reply window of the capture, shot file.
usage: lag_probe.py <out dir> [delays ms, comma list]"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import switch_bot as sb

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
delays = [int(x) for x in (sys.argv[2] if len(sys.argv) > 2 else '0,33,67,100,133,167,200,250,300,400,600,1000').split(',')]
bot = sb.Bot(sb.address(['x']))
def probe(tag, button, d):
    bot.shot(f'{out}/{tag}{d}_ref.jpg')
    bot.send(f'press {button}'); t0 = time.perf_counter()
    time.sleep(d / 1000.0)
    t1 = time.perf_counter(); data = bytes.fromhex(bot.send('pixelPeek')); t2 = time.perf_counter()
    bot.send(f'release {button}')
    open(f'{out}/{tag}{d}.jpg', 'wb').write(data)
    print(f'{tag} {button} d={d} cap_window={1000*(t1-t0):.0f}..{1000*(t2-t0):.0f} ms', flush=True)
    time.sleep(1.5)

# the first START opens the upgrade menu with an info popup: close it (A = PS2 cross)
for b in ('PLUS', 'A'):
    bot.click(b); time.sleep(1.5)
# menu probe: D-pad down / up moves the selected item
for i, d in enumerate(delays):
    probe('m', 'DDOWN' if i % 2 == 0 else 'DUP', d)
bot.click('PLUS'); time.sleep(2)
bot.shot(f'{out}/menu_closed.jpg')
# gameplay probe: jump (A)
for d in delays:
    probe('j', 'A', d)
