"""Console sweep: test every movie, level and boss fight of the game on the Switch and write one report.

usage: python -I tools/switch_sweep.py [--movies] [--levels] [--fights] [--bonus] [--only A,B] [--stay 40] [--resume <dir>]
                                       [--out port/switch/sweep] [--movie-batch-s 900] [--no-default]
  (no --movies/--levels/--fights = all three; --only filters by movie / zone name)

Uses only options the console build already has (docs/switch-sweep.md), all off unless env.txt sets them:
  movies  one boot per batch: GOW_MOVIE_OVERRIDE=<list> + GOW_BONUS_MOVIE (the title menu starts the next movie each
          time the player is idle again, tools/movies.py), GOW_MPEG_DIAG picture/sync lines
  levels  one boot per zone (chained warps are not reliable, docs/game-internals.md): GOW_SKIP_FMV, new game by the
          runner's pad script (GOW_PAD_TEST + GOW_PAD_GUION_FILE), GOW_WARP=<zone> 0.5 s after gameplay starts,
          GOW_GOD_MODE, walking in a square and attacking for --stay seconds
  fights  as levels, longer stay (--fight-stay) and QTE mashing (circle) on top of the attacks
Per item: push env.txt (+ pad script), start the game as user "claude" (tools/switch_lib.sh), bot screenshots every
--shot-every s, live log pulls over FTP, close, pull log + Atmosphere crash report + the warp capture (PPM), judge:
  FAIL  crash (new crash report / game gone), hang (log or game clock stopped), warp not arrived, movie not played or
        cut short, black capture, frozen screen (3 identical bot screenshots), error lines
  SLOW  guest fps mean < 25, game speed outside 0.95-1.05, movie pictures/s < 28
  PASS  otherwise
Writes <out>/<date>/report.md + results.json + per item log/shots; --resume <dir> skips the items done there.
The console default (tools/switch_default.sh) is restored after every item and at the end.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import levels  # noqa: E402  (toc_zones)
import movies  # noqa: E402  (movie_list, LOAD_LINE)
import zones   # noqa: E402  (ERROR_PATTERNS)

GAME_TID = '0100D0D0600A0000'
# Git bash (a plain "bash" on Windows can be WSL's)
BASH = os.environ.get('SWEEP_BASH') or next((p for p in (r'C:/Program Files/Git/bin/bash.exe',) if os.path.exists(p)), 'bash')
DEFAULT_ENV = os.path.join(ROOT, 'tools', 'switch', 'default_env.txt')
# boss and main fights (zone, note), from the boss resource WADs each zone loads (GODOFWAR.TOC: zone WAD followed by
# its R_* WADs; the R_ WAD's strings name the creature, docs/switch-sweep.md "Fights"). The warp point is the zone
# start, the fight may need a walk to trigger.
FIGHTS = [('ATHN01D', 'Hydra 1, ship deck (R_AHA/R_AH2)'), ('ATHN01EE', 'Hydra 2 (R_AH3)'),
          ('ATHN01F', "ship end, captain's cabin"), ('PAND03H', "Pandora's Guardian? (R_AB3/R_AB4)"),
          ('PAND04H', 'Cerberus (R_CEP/R_CEA)'), ('ARENA27', 'Challenge of the Gods, Cerberus (R_CEP)'),
          ('ATHN08B', 'Ares, phase 1 (R_A1A-C)'), ('ATHN08C', 'Kratos clones / family (R_AEV)'),
          ('ATHN07B', 'Ares, phase 2 (R_A2A-C)')]
# the 43 movies with speech (European disc: five language tracks), dubbed from the lang pack (docs/settings.md
# "Movies in other languages"): each must log `[disc] dub read: .../voice_<SWEEP_LANG>/<MOVIE>.DUB` (the intro
# SEMO01: `overlay read: .../SEMO01.PSW`, its re-encoded card)
SPEAKING = set('A01MP01 A01MP02 A01MP03 A01MP04 A02MP02 A03MP01 A03MP02 A04MP01 A04MP04 A06MP01 A06MP02 A06MP03 '
               'A06MP04 A06MP05 A07MP01 A07MP04 A07MP05 D01MP02 MA360 MAGICBZK MAGICRTD MATBOLT MEWINTRO P00MP02 '
               'P06MP01 SEMO01 SEMO02 SEMO03 SEMO04 SEMO05 SEMO06 SEMO07 SEMO08 SEMO09 SEMO10 SEMO13 BONUS03 BONUS06 '
               'BONUS10 BONUS12 BONUS14 BONUS16 BONUS17 BONUS18'.split())
SWEEP_LANG = os.environ.get('SWEEP_LANG', 'it')
ERRORS = zones.ERROR_PATTERNS + [(r'missing-target', 'guest crash (missing-target)'), (r'\[gow-heapguard\]', 'heap guard'),
                                 (r'\babort\b|fatal error|Fatal', 'abort/fatal line'), (r'null JALR', 'null JALR')]


def sh(cmd, timeout=900):
    """bash with tools/switch_lib.sh loaded (FTP transfer)"""
    full = f'cd "{ROOT.replace(os.sep, "/")}"; . tools/switch_lib.sh; SW_T=ftp; {cmd}'
    try:
        r = subprocess.run([BASH, '-c', full], capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return 124, 'timeout'


def ftp(*args, timeout=600):
    try:
        r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'switch_ftp.py'), *args],
                           capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout
    except subprocess.TimeoutExpired:
        return 124, ''


def bot(*args, timeout=60):
    for _ in range(3):
        try:
            r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'switch_bot.py'), *args],
                               capture_output=True, text=True, timeout=timeout)
            if r.returncode == 0:
                return r.stdout.strip()
        except subprocess.TimeoutExpired:
            pass
        time.sleep(2)
    return None


def put(local, path):
    # right after a close the game may still hold its files open for a few seconds: retry
    for _ in range(4):
        if ftp('put', local, path)[0] == 0:
            return True
        time.sleep(15)
    return False


def ls_names(path):
    rc, out = ftp('ls', path)
    return [l.split()[-1] for l in out.splitlines() if l.strip()] if rc == 0 else []


EXTRA_ENV = []  # --env


def env_text(extra):
    base = [l for l in open(DEFAULT_ENV, encoding='utf-8').read().splitlines() if l.strip() and not l.startswith('#')]
    return '\n'.join(base + ['GOW_PERF_DIAG=1', 'GOW_DELTA_DIAG=1'] + EXTRA_ENV + extra) + '\n'


def level_pad(total, mash):
    # new game from the title (wall seconds, as tools/bench_script.py), then from gameplay start (g = wall seconds
    # after gameplay starts) a square walk with attacks; fights add circle mashing for the QTEs
    keys = [f'{30 + d:g}:{k}' for d, k in ((0, 'start'), (9, 'x'), (15, 'x'), (21, 'x'), (27, 'x'))]
    t, i = 4.0, 0
    sticks = ['lup', 'lright', 'ldown', 'lleft']
    while t < total:
        keys += [f'g{t:.2f}:{sticks[i % 4]}*1.5', f'g{t + 0.3:.2f}:cuadrado*0.2', f'g{t + 0.9:.2f}:triangulo*0.2',
                 f'g{t + 1.6:.2f}:cuadrado*0.2']
        if mash:
            keys += [f'g{t + d:.2f}:circulo*0.08' for d in (0.15, 0.6, 1.2, 1.8)]
        t += 2.0
        i += 1
    return '\n'.join(keys) + '\n'


def items(args):
    out = []
    only = {x.strip().upper() for x in args.only.split(',')} if args.only else None
    if args.movies:
        sizes = movies.movie_list()
        # the bonus movies (BONUS*) only with --bonus (user 2026-10-30: only the story movies matter)
        names = [m for m in sizes if (not only or m in only) and (args.bonus or not m.startswith('BONUS'))]
        batch, secs = [], 0
        for m in names:
            s = sizes[m] / movies.BYTES_PER_SECOND
            if batch and secs + s > args.movie_batch_s and len(batch) >= 2:
                out.append({'kind': 'movies', 'name': 'movies_' + batch[0], 'list': batch, 'secs': secs})
                batch, secs = [], 0
            batch.append(m)
            secs += s
        if batch:
            out.append({'kind': 'movies', 'name': 'movies_' + batch[0], 'list': batch, 'secs': secs})
    if args.levels:
        out += [{'kind': 'level', 'name': z, 'zone': z, 'stay': args.stay} for z in levels.toc_zones() if not only or z in only]
    if args.fights:
        out += [{'kind': 'fight', 'name': 'fight_' + z, 'zone': z, 'stay': args.fight_stay, 'note': n}
                for z, n in FIGHTS if not only or z in only]
    return out


def judge_common(text, r):
    for pat, label in ERRORS:
        n = len(re.findall(pat, text))
        if n:
            r['fail' if label not in ('heap guard',) else 'warn'].append(f'{label} x{n}')


def perf_of(seg):
    fps = [float(x) for x in re.findall(r'\[gow-perf\].*?guest_flip_hz=([0-9.]+)', seg)]
    speed = [float(x) for x in re.findall(r'\[gow-speed\] .*?speed=([0-9.]+)', seg)]
    return fps, speed


def fps_without_captures(seg):
    """guest fps per [gow-perf] window, without the windows of the sweep's own warp captures: each PPM capture stops
    the game ~3.5 s (readback), so its 5 s window reads 12-15 fps and the stall can reach into the next one. The capture
    line is logged next to those windows' [gow-perf] lines: drop both neighbours."""
    perf = [(m.start(), float(m.group(1))) for m in re.finditer(r'\[gow-perf\].*?guest_flip_hz=([0-9.]+)', seg)]
    drop = set()
    for c in re.finditer(r'\[gow-warp\] capture ', seg):
        near = [k for k in range(len(perf)) if perf[k][0] < c.start()][-1:] + \
               [k for k in range(len(perf)) if perf[k][0] > c.start()][:1]
        drop.update(near)
    return [h for k, (_, h) in enumerate(perf) if k not in drop]


def judge_level(text, item, r):
    m = re.search(r'\[gow-warp\] arrived (\S+) at ([0-9.]+) s \(([0-9.]+) s\)', text)
    if not m:
        t = re.search(r'\[gow-warp\] (timeout[^\n]*)', text)
        r['fail'].append('warp ' + (t.group(1) if t else ('not requested' if '[gow-warp] request' not in text
                                                          else 'not arrived')))
        return
    r['load_s'] = float(m.group(3))
    seg = text[m.end():]
    fps, speed = perf_of(seg)
    fps = fps_without_captures(seg)[1:] or fps
    if fps:
        r['fps'] = [round(min(fps), 1), round(sum(fps) / len(fps), 1)]
        if r['fps'][1] < 25:
            r['slow'].append(f"fps mean {r['fps'][1]}")
        elif r['fps'][0] < 25:
            r['warn'].append(f"fps min {r['fps'][0]}")
    sp = speed[2:] or speed
    if sp:
        r['speed'] = round(sum(sp) / len(sp), 3)
        if not 0.95 <= r['speed'] <= 1.05:
            r['slow'].append(f"game speed {r['speed']}")
        tail = speed[-5:]
        if len(tail) == 5 and max(tail) < 0.05:
            r['fail'].append('hang: game clock stopped (last 5 s speed 0)')
    expect = item['stay'] / 5 * 0.6
    if len(re.findall(r'\[gow-perf\]', seg)) < expect:
        r['fail'].append(f"hang: log stops {len(re.findall(r'.gow-perf.', seg)) * 5} s after arrival")
    # a warp point over a pit (ATHN01EE, ...): Kratos falls and dies even with GOW_GOD_MODE; the death screen is
    # dark, so a black capture there is the test, not the port
    if re.search(r'\[gow-player\][^\n]* hp=0 state=14', seg):
        r['warn'].append('Kratos died after the warp (fall from the warp point)')
        r['fail'] = [f for f in r['fail'] if not f.startswith('black capture')]
    zs = sorted(set(re.findall(r'\[gow-player\].*? zone=(\S+)', seg)))
    if zs:
        r['zones_seen'] = zs[:6]


def judge_movies(text, item, r, sizes):
    starts = [mm for mm in re.finditer(movies.LOAD_LINE, text)]
    rows = []
    for i, name in enumerate(item['list']):
        row = {'movie': name, 'fail': [], 'slow': []}
        rows.append(row)
        if i >= len(starts):
            row['fail'].append('not played')
            continue
        seg = text[starts[i].start():starts[i + 1].start() if i + 1 < len(starts) else len(text)]
        sync = [(int(f), float(w)) for f, w in re.findall(r'\[MPEG:sync\] frame=(\d+) pts=\d+ wall_ms=([0-9.]+)', seg)]
        exp = sizes.get(name, 0) / movies.BYTES_PER_SECOND * 30
        if len(sync) < 2:
            row['fail'].append('no pictures')
            continue
        frames = sync[-1][0]
        row['pictures'] = frames
        pps = (sync[-1][0] - sync[0][0]) / max(0.001, (sync[-1][1] - sync[0][1]) / 1000)
        row['pps'] = round(pps, 1)
        # played to the end: the decoder saw the sequence end code after the stream was read to its size
        if not re.search(r'\[MPEG:wait\].*?ended=1.*?seqEnd=1', seg):
            row['fail'].append(f'did not end (no sequence end, {frames} of ~{exp:.0f} pictures)')
        if pps < 28:
            row['slow'].append(f'{pps:.1f} pictures/s')
        if name in SPEAKING:
            row['dub'] = bool(re.search(rf'\[disc\] (?:dub|overlay) read: \S*voice_{SWEEP_LANG}/{name}\.(?:DUB|PSW|PSS)', text, re.I))
            if not row['dub']:
                row['fail'].append(f'no {SWEEP_LANG} dub read')
    r['movies'] = rows


def ppm_luma(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    vals = [px[o] + px[o + 1] + px[o + 2] for o in range(0, w * h * 3 - 2, 3 * 97)]
    mean = sum(vals) / len(vals) / 3
    sd = (sum((v / 3 - mean) ** 2 for v in vals) / len(vals)) ** 0.5
    return mean, sd


def run_item(item, outdir, args, sizes):
    d = os.path.join(outdir, item['name'])
    sd = os.path.join(d, 'shots').replace(os.sep, '/')
    os.makedirs(sd, exist_ok=True)
    r = {'name': item['name'], 'kind': item['kind'], 'fail': [], 'slow': [], 'warn': [], 'shots': []}
    if item['kind'] == 'movies':
        extra = ['GOW_MOVIE_OVERRIDE=' + ','.join(item['list']), 'GOW_BONUS_MOVIE=24', 'GOW_BONUS_MOVIE_DELAY=8',
                 'GOW_PAD_TEST=1', 'GOW_PAD_TEST_NO_CAPTURE=1', 'GOW_PAD_GUION=99999:select', 'GOW_MPEG_DIAG=1']
        budget = 90 + item['secs'] + 30 * len(item['list'])
    else:
        pad = os.path.join(d, 'sweep_pad.txt')
        open(pad, 'w').write(level_pad(item['stay'] + 20, item['kind'] == 'fight'))
        if not put(pad, 'switch/gow'):
            r['fail'].append('pad script push failed')
            return r
        extra = ['GOW_SKIP_FMV=1', 'GOW_GOD_MODE=1', 'GOW_PLAYER_TRACE=1', 'GOW_PAD_TEST=1', 'GOW_PAD_TEST_NO_CAPTURE=1',
                 'GOW_PAD_GUION_FILE=sdmc:/switch/gow/sweep_pad.txt', 'GOW_WARP=' + item['zone'],
                 f"GOW_WARP_STAY={item['stay']}"]
        budget = 75 + item['stay'] + 25
    os.makedirs(os.path.join(d, 'envdir'), exist_ok=True)
    envf = os.path.join(d, 'envdir', 'env.txt')
    open(envf, 'w', newline='\n').write(env_text(extra))
    if not put(envf, 'switch/gow'):
        r['fail'].append('env push failed')
        return r
    crash0 = set(ls_names('atmosphere/crash_reports'))
    for f in ls_names(''):
        if f.startswith('gow_warp_') and f.endswith('.ppm'):
            ftp('rm', f)
    rc, out = sh(f'sw_ready "{sd}" && sw_launch "{sd}"', timeout=300)
    if rc:
        r['fail'].append('launch failed: ' + out.strip().splitlines()[-1] if out.strip() else 'launch failed')
        sh(f'sw_home_close "{sd}"', timeout=200)
        return r
    t0 = time.time()
    # the log cannot be read while the game runs (sys-ftpd gets no access to a file open for writing): runs end by
    # time budget, hangs are judged from the pulled log
    last_md5, same, last_pull, k = None, 0, time.time(), 0
    gone = False
    while time.time() - t0 < budget:
        time.sleep(max(0, args.shot_every - (time.time() - t0) % args.shot_every))
        k += 1
        shot = os.path.join(sd, f's{k:03d}_{int(time.time() - t0)}.jpg')
        if bot('shot', shot) is not None and os.path.exists(shot):
            h = hashlib.md5(open(shot, 'rb').read()).hexdigest()
            same = same + 1 if h == last_md5 else 0
            last_md5 = h
            el = time.time() - t0
            # not for movies: the title menu between and after them shows still pictures; the decoder lines judge them
            if same == 2 and el > 45 and item['kind'] != 'movies':
                r['fail'].append(f'frozen screen at {int(time.time() - t0)} s (3 identical screenshots)')
        title = bot('title')
        if title is not None and title.upper() != GAME_TID:
            gone = True
            r['fail'].append(f'game gone at {int(time.time() - t0)} s (title "{title}")')
            break
        if time.time() - last_pull >= 120:
            # keep the console awake: Auto-Sleep counts only HID input (the runner's pad script is not), and the game
            # does not report media playback; a right-stick nudge below the roll threshold resets the idle timer
            last_pull = time.time()
            bot('cmd', 'setStick', 'RIGHT', '0x600', '0x0')
            time.sleep(0.3)
            bot('cmd', 'setStick', 'RIGHT', '0x0', '0x0')
    r['run_s'] = int(time.time() - t0)
    if not gone:
        sh(f'sw_home_close "{sd}"', timeout=200)
    else:
        bot('shot', os.path.join(sd, 'gone.jpg'))
        sh(f'sw_home_close "{sd}"', timeout=200)
    ftp('put', os.path.join(ROOT, 'port', 'switch', 'logs', 'default_env', 'env.txt'), 'switch/gow')  # default env
    log = os.path.join(d, 'log.txt')
    if ftp('get', 'switch/gow/log.txt', log, timeout=300)[0]:
        r['fail'].append('log not pulled')
        log = None
    new = [c for c in set(ls_names('atmosphere/crash_reports')) - crash0 if GAME_TID.lower() in c.lower()]
    for c in new:
        ftp('get', 'atmosphere/crash_reports/' + c, os.path.join(d, c))
        r['fail'].insert(0, 'CRASH: atmosphere report ' + c)
    for f in ls_names(''):
        if f.startswith('gow_warp_') and f.endswith('.ppm'):
            p = os.path.join(d, f)
            if ftp('get', f, p)[0] == 0:
                ftp('rm', f)
                mean, sdv = ppm_luma(p)
                png = p[:-4] + '.png'
                subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'ppm2png.py'), p, png], check=False)
                os.remove(p)
                r['shots'].append(os.path.relpath(png, outdir))
                if mean < 6 and sdv < 4:
                    r['fail'].append(f'black capture {f} (mean {mean:.1f})')
    if log:
        text = open(log, encoding='utf-8', errors='replace').read()
        judge_common(text, r)
        if item['kind'] == 'movies':
            judge_movies(text, item, r, sizes)
        else:
            judge_level(text, item, r)
    jp = sorted(f for f in os.listdir(sd) if f.startswith('s') and f.endswith('.jpg'))
    r['shots'] += [os.path.relpath(os.path.join(sd, f), outdir) for f in jp[len(jp) // 3::max(1, len(jp) // 3)][:3]]
    return r


def verdict(r):
    if r['fail'] or any(m['fail'] for m in r.get('movies', [])):
        return 'FAIL'
    if r['slow'] or any(m['slow'] for m in r.get('movies', [])):
        return 'SLOW'
    return 'PASS'


def write_report(outdir, results):
    lines = [f'# Switch sweep {os.path.basename(outdir)}', '',
             'Tool: tools/switch_sweep.py (docs/switch-sweep.md). FAIL = crash / hang / not arrived / black / frozen /'
             ' errors; SLOW = fps < 25 or game speed outside 0.95-1.05 (movies: < 28 pictures/s).', '']
    counts = {}
    mv = [r for r in results if r['kind'] == 'movies']
    if mv:
        lines += ['## Movies', '', '| movie | result | pictures | pictures/s | notes |', '|---|---|---|---|---|']
        for r in mv:
            for m in r.get('movies', [{'movie': x, 'fail': ['batch: ' + '; '.join(r['fail'])], 'slow': []}
                                      for x in r.get('list', [])]):
                v = 'FAIL' if m['fail'] or (r['fail'] and not m.get('pictures')) else 'SLOW' if m['slow'] else 'PASS'
                counts[v] = counts.get(v, 0) + 1
                lines.append(f"| {m['movie']} | {v} | {m.get('pictures', '-')} | {m.get('pps', '-')} | "
                             f"{'; '.join(m['fail'] + m['slow'])} |")
            if r['fail'] or r['warn']:
                lines.append(f"| ({r['name']} batch) | | | | {'; '.join(r['fail'] + r['warn'])} |")
        lines.append('')
    for kind, title in (('level', 'Levels (one warp per boot)'), ('fight', 'Fights')):
        rs = [r for r in results if r['kind'] == kind]
        if not rs:
            continue
        lines += [f'## {title}', '', '| zone | result | load s | fps min / mean | speed | notes | shots |',
                  '|---|---|---|---|---|---|---|']
        for r in rs:
            v = verdict(r)
            counts[v] = counts.get(v, 0) + 1
            fps = f"{r['fps'][0]} / {r['fps'][1]}" if r.get('fps') else '-'
            shots = ' '.join(f'[{i}]({s.replace(os.sep, "/")})' for i, s in enumerate(r['shots']))
            lines.append(f"| {r['name']} | {v} | {r.get('load_s', '-')} | {fps} | {r.get('speed', '-')} | "
                         f"{'; '.join(r['fail'] + r['slow'] + r['warn'])} | {shots} |")
        lines.append('')
    lines.append('Totals: ' + ', '.join(f'{k} {v}' for k, v in sorted(counts.items())))
    open(os.path.join(outdir, 'report.md'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    json.dump(results, open(os.path.join(outdir, 'results.json'), 'w'), indent=1)


def rejudge(outdir, todo):
    rj = os.path.join(outdir, 'results.json')
    byname = {it['name']: it for it in todo}
    sizes = movies.movie_list()
    out = []
    for r in json.load(open(rj)):
        it = byname.get(r['name'])
        log = os.path.join(outdir, r['name'], 'log.txt')
        if it and os.path.exists(log):
            keep = [f for f in r['fail'] if f.startswith(('CRASH', 'game gone', 'launch', 'black', 'env', 'pad', 'log'))]
            for f in r['fail']:
                m = re.match(r'frozen screen at (\d+) s', f)
                if m and it['kind'] != 'movies':
                    keep.append(f)
            r.update({'fail': keep, 'slow': [], 'warn': []})
            r.pop('movies', None)
            text = open(log, encoding='utf-8', errors='replace').read()
            judge_common(text, r)
            (judge_movies(text, it, r, sizes) if it['kind'] == 'movies' else judge_level(text, it, r))
        out.append(r)
    write_report(outdir, out)
    print('rejudged', len(out), '->', os.path.join(outdir, 'report.md'))


def default_package():
    """the console default's package: DEFAULT_PKG, else the one tools/switch_default.sh installs"""
    if os.environ.get('DEFAULT_PKG'):
        return os.environ['DEFAULT_PKG']
    m = re.search(r'PKG=\$\{DEFAULT_PKG:-([^}]+)\}', open(os.path.join(ROOT, 'tools', 'switch_default.sh')).read())
    return m.group(1) if m else 'update_crash1'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--movies', action='store_true')
    ap.add_argument('--levels', action='store_true')
    ap.add_argument('--fights', action='store_true')
    ap.add_argument('--bonus', action='store_true', help='also the bonus movies (BONUS*)')
    ap.add_argument('--only', default='')
    ap.add_argument('--stay', type=int, default=40)
    ap.add_argument('--fight-stay', type=int, default=150)
    ap.add_argument('--shot-every', type=int, default=15)
    ap.add_argument('--movie-batch-s', type=float, default=900)
    ap.add_argument('--out', default=os.path.join(ROOT, 'port', 'switch', 'sweep'))
    ap.add_argument('--resume', default='')
    ap.add_argument('--dry', action='store_true', help='list the items only')
    ap.add_argument('--env', default='', help='extra env.txt lines for every item, comma-separated (A/B tests)')
    ap.add_argument('--package', default='', help='push this update package (port/switch/nsp/out/<name>) before the run')
    ap.add_argument('--rejudge', default='', help='judge the pulled logs of a run dir again (no console)')
    args = ap.parse_args()
    if not (args.movies or args.levels or args.fights):
        args.movies = args.levels = args.fights = True
    todo = items(args)
    if args.dry:
        for it in todo:
            print(it['name'], it.get('list', ''), round(it.get('secs', it.get('stay', 0))))
        return
    if args.rejudge:
        rejudge(args.rejudge, todo)
        return
    outdir = args.resume or os.path.join(args.out, datetime.datetime.now().strftime('%Y%m%d_%H%M'))
    os.makedirs(outdir, exist_ok=True)
    EXTRA_ENV[:] = [e for e in args.env.split(',') if e]
    if args.package:
        for _ in range(3):  # FTP can be briefly unreachable right after a previous run closed the game
            rc, out = sh(f'TRANSFER=ftp sw_transfer && sw_push_update port/switch/nsp/out/{args.package}', timeout=600)
            if rc == 0:
                break
            time.sleep(20)
        if rc:
            print('[sweep] package push failed: ' + out[-300:], flush=True)
            return
        print(f'[sweep] package {args.package}' + (f" env {','.join(EXTRA_ENV)}" if EXTRA_ENV else ''), flush=True)
    rj = os.path.join(outdir, 'results.json')
    results = json.load(open(rj)) if os.path.exists(rj) else []
    done = {r['name'] for r in results}
    sizes = movies.movie_list()
    try:
        for it in todo:
            if it['name'] in done:
                continue
            print(f"[sweep] {datetime.datetime.now():%H:%M:%S} {it['name']}", flush=True)
            r = run_item(it, outdir, args, sizes)
            results.append(r)
            write_report(outdir, results)
            bad = [m['movie'] + ': ' + '; '.join(m['fail'] + m['slow']) for m in r.get('movies', []) if m['fail'] or m['slow']]
            print(f"[sweep] {it['name']} {verdict(r)} {'; '.join(r['fail'] + r['slow'] + r['warn'])} {' | '.join(bad)}",
                  flush=True)
    finally:
        sh(f'sw_ready "{outdir.replace(os.sep, "/")}"', timeout=300)  # home menu, nothing running
        ftp('rm', 'switch/gow/sweep_pad.txt')
        rc, out = sh('TRANSFER=ftp bash tools/switch_default.sh', timeout=600)
        print('[sweep] default restored' if rc == 0 else '[sweep] DEFAULT RESTORE FAILED: ' + out[-300:], flush=True)
        local = os.path.join(ROOT, 'port', 'switch', 'nsp', 'out', default_package(),
                             'atmosphere', 'contents', GAME_TID, 'exefs', 'main')
        got = os.path.join(outdir, 'exefs_main.chk')
        if ftp('get', f'atmosphere/contents/{GAME_TID}/exefs/main', got)[0] == 0:
            a, b = (hashlib.md5(open(x, 'rb').read()).hexdigest() for x in (local, got))
            print(f'[sweep] exefs md5 {b[:8]} ' + ('= default' if a == b else f'!= default {a[:8]}'), flush=True)
            os.remove(got)
        write_report(outdir, results)
        print('[sweep] report', os.path.join(outdir, 'report.md'), flush=True)


if __name__ == '__main__':
    main()
