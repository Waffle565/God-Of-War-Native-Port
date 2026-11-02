"""Remote steering of a running test game (packages update_boss4+, env GOW_PAD_LIVE=sdmc:/switch/gow/live.txt).

usage: python -I tools/switch_live.py <shot.jpg> <step> [<step> ...]
  step  x,y,s[,buttons[,hold]]  hold the left stick at x,y (-1..1, y up) and the PS2 buttons (hex mask: 4000 cross,
                         8000 square, 1000 triangle, 2000 circle, 0200 R2, 0008 start) for s seconds
        w<s>             wait s seconds
        T<x>,<y>,<z>     teleport Kratos to that world position (GOW_TELEPORT coordinates; packages update_boss7+)
Each step writes live.txt (new sequence number) over FTP; the game polls it every 0.25 s (gow_overrides.cpp
GOW_PAD_LIVE). Ends with a usb-botbase screenshot and prints Kratos's position from live.txt.pos (update_boss7+). usb-botbase's own setStick does not reach the game's stick.
"""
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import switch_bot  # noqa: E402

SEQ = os.path.join(tempfile.gettempdir(), 'gow_live_seq.txt')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    seq = int(open(SEQ).read()) if os.path.exists(SEQ) else int(time.time()) % 100000
    tmp = os.path.join(tempfile.mkdtemp(), 'live.txt')
    for step in sys.argv[2:]:
        if step.startswith('w'):
            time.sleep(float(step[1:]))
            continue
        seq += 1
        if step.startswith('T'):
            x, y, z = (float(v) for v in step[1:].split(','))
            s = 0.6
            open(tmp, 'w').write(f'{seq} T {x} {y} {z}\n')
        else:
            p = step.split(',')
            x, y, s = float(p[0]), float(p[1]), float(p[2])
            b = p[3] if len(p) > 3 else '0'
            hold = float(p[4]) if len(p) > 4 else s  # 5th field: the game holds longer than the step (no gap)
            open(tmp, 'w').write(f'{seq} {x} {y} {hold} {b}\n')
        subprocess.run([sys.executable, '-I', os.path.join(HERE, 'switch_ftp.py'), 'put', tmp, 'switch/gow'],
                       capture_output=True, check=False)
        time.sleep(s + 0.4)  # the game picks the file up within 0.25 s of the upload
    open(SEQ, 'w').write(str(seq))
    bot = switch_bot.Bot(switch_bot.address([]))
    print(bot.shot(sys.argv[1]), 'bytes')
    pos = os.path.join(os.path.dirname(tmp), 'live.txt.pos')
    subprocess.run([sys.executable, '-I', os.path.join(HERE, 'switch_ftp.py'), 'get', 'switch/gow/live.txt.pos', pos],
                   capture_output=True, check=False)
    if os.path.exists(pos):
        print('pos', open(pos).read().strip())
    if os.environ.get('LIVE_HALF'):  # a half-size copy next to it (<shot>_h.jpg), cheaper to look at
        try:
            from PIL import Image
            Image.open(sys.argv[1]).resize((640, 360)).save(sys.argv[1][:-4] + '_h.jpg', quality=80)
        except ImportError:
            pass


if __name__ == '__main__':
    main()
