"""Keyboard button prompts: disc overlays whose texts name the keyboard keys instead of the PS2 buttons.

For every language overlay of tools/lang_pack.py (<disc>/lang/<text>_<voice>.layout) this writes
<disc>/lang/kb_<text>_<voice>.layout: the same overlay, with R_PERM.WAD replaced by kb_text_<text>.bin, whose
message table names the keys (keymap.py: "[XButton]" -> "Space", "[CircleButton]" -> "E", ...). The launcher picks
kb_<text>_<voice>.layout when "Button prompts" is Keyboard.

- Message table (msgs_en.txt): rebuilt as lang_pack.py rebuilds it (the record's size changes, nothing points into
  it); the message after the table's last blank line is English in every language and gets the English keys.
- FLP_HUD (the Power Up move list strings, English in every language): the strings are ActionScript push records
  inside a structure with pointers, so they are rewritten in place at the same length, padded with spaces.
- Not changed: the menu and HUD legends drawn as Flash clips (the PS2 symbols, e.g. "X = Select"), which are shapes,
  not text (odd/tasks/macos-port.md T17).
The kb_text_<code>.bin files go past the end of the language overlays; the .layout files are written last.

usage: python3 -I tools/macos/keyprompts/make_kb_overlays.py <disc folder> [--show <id>[,<id>...]]
       (--show prints those messages in every language as written, to review the wording)
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# next to this file inside the app (Contents/Resources/keyprompts), else the repository's tools/
sys.path[:0] = [HERE, os.path.normpath(os.path.join(HERE, '..', '..'))]
import keymap  # noqa: E402
import lang_pack as lp  # noqa: E402

PREFIX = 'kb_'
MARK = '\x01'  # in front of an inserted key while a message is converted: a key at a line start is capitalised


MOVE_ROW = 40   # move list row (name left, keys right): characters it holds (measured at ~21 px each in 915 px)
HINT_LINE = 53  # hint box line: the longest lines the game's own tables use (it 432, es 126)


def convert(text, code, tier=0, sticks=True):
    """a line (str, latin-1) with keys in place of the button tokens; (new text, tokens replaced).
    `tier`: the keymap.SHORT words; `sticks`: False keeps [Joystick] (messages about pressing the sticks, L3/R3)."""
    count = 0
    words = keymap.KEYS[code]
    text = re.sub(r'(?<=\w)(?=\[[A-Za-z]+Button\])', ' ', text)  # "button[DownButton]" -> "button 3"
    if sticks:
        for pattern, key in keymap.STICKS[code]:
            text, n = re.subn(pattern, MARK + key.format(**words), text, flags=re.I)
            count += n
        n = text.count('[Joystick]')
        text = text.replace('[Joystick]', MARK + 'WASD')
        count += n
    for token, key in keymap.tokens(code, tier).items():
        if key.endswith('.'):
            text = text.replace(token + '.', token)  # "clic der." ends the sentence too
        n = text.count(token)
        text = text.replace(token, MARK + key)
        count += n
    text = re.sub('(^|\n)' + MARK + '(.)', lambda m: m.group(1) + m.group(2).upper(), text)
    return text.replace(MARK, ''), count


def width(text):
    """drawn width in characters: a token's icon is about two"""
    return len(re.sub(r'\[[A-Za-z0-9*]+\]', 'xx', text))


def fitted(line, code, room, sticks):
    """the line with the longest key words that keep it within `room` characters (else the shortest)"""
    for tier in range(len(keymap.SHORT[code])):
        new, n = convert(line, code, tier, sticks)
        if len(new) <= room:
            break
    return new, n


def convert_message(text, code):
    """a message: (new text, tokens replaced, lines still wider than their room)"""
    lines = text.split('\n')
    sticks = 'L3' not in text and 'R3' not in text
    # move list entries: name, keys, level code ("W1"), description; name and keys share one row
    move = len(lines) > 2 and re.fullmatch(r'[A-Z]\d ?', lines[2]) is not None
    out, total, over = [], 0, 0
    for i, line in enumerate(lines):
        if move and i == 1:
            room = max(MOVE_ROW, width(lines[0]) + width(line)) - len(out[0])
        else:
            room = max(HINT_LINE, width(line))
        new, n = fitted(line, code, room, sticks)
        out.append(new)
        total += n
        over += new != line and len(new) > room
    return '\n'.join(out), total, over


def convert_table(table, code):
    """the msgs_en.txt record body; (new body, report)"""
    out, report = [], {'tokens': 0, 'over': [], 'left': []}
    for key, value in lp.parse_messages(table):
        text, n, over = convert_message(value.decode('latin1'), code)
        out.append(b'*%s*\n%s\n\n' % (key, text.encode('latin1')))
        report['tokens'] += n
        if over:
            report['over'].append(key.decode())
    tail, n, _ = convert_message(table[table.rindex(b'\n\n') + 2:].decode('latin1'), 'en')
    body = b''.join(out) + tail.encode('latin1')
    report['tokens'] += n
    report['left'] = sorted(set(re.findall(rb'\[[A-Za-z0-9]+\]', body)) - {b'[L3]', b'[R3]'})
    return body, report


def convert_flp(chunk):
    """FLP_HUD body with the move list strings converted in place; (new body, strings changed)"""
    out, changed = bytearray(chunk), 0
    for m in re.finditer(rb'\x96(..)\x00([^\x00]*Button\][^\x00]*)\x00', chunk, re.S):
        old = m.group(2)
        if struct.unpack('<H', m.group(1))[0] != len(old) + 2:
            continue  # not a push of this one string
        new, _ = fitted(old.decode('latin1'), 'en', len(old), True)
        new = new.encode('latin1')
        if len(new) > len(old):
            continue
        out[m.start(2):m.end(2)] = new + b' ' * (len(old) - len(new))
        changed += 1
    return bytes(out), changed


def check_font(code):
    """key words must be printable ASCII: the menu font draws no accented letters (they vanish in the Moves list)"""
    words = [w for tier in keymap.SHORT[code] for w in tier] + list(keymap.KEYS[code].values())
    bad = sorted({c for w in words for c in w if not ' ' <= c <= '~'})
    if bad:
        raise ValueError(f'{code}: key words with characters the game font cannot draw: {bad!r}')


def keyboard_perm(perm, code):
    """R_PERM.WAD (a lang_pack text_<code>.bin) with keys; (new WAD, report)"""
    out, report = bytearray(), {'flp': 0}
    for pos, tag, size, name, body in lp.wad_records(perm):
        if name.startswith(b'msgs'):
            table = perm[pos + 32:pos + 32 + size]
            text, table_report = convert_table(table, code)
            check_font(code)
            report.update(table_report, table=text)
            out += struct.pack('<II', tag, len(text)) + name + text + b'\0' * (-len(text) % 16)
        elif name.split(b'\0')[0] == b'FLP_HUD':
            chunk, report['flp'] = convert_flp(perm[pos + 32:pos + 32 + size])
            out += perm[pos:pos + 32] + chunk + perm[pos + 32 + size:pos + 32 + ((body + 15) & ~15)]
        else:
            out += perm[pos:pos + 32 + ((body + 15) & ~15)]
    return bytes(out), report


def read_overlay(path):
    """lang_pack .layout -> (sectors, [(lsn, size, name)])"""
    sectors, files = 0, []
    for line in open(path):
        f = line.split()
        if f and f[0] == 'sectors':
            sectors = int(f[1])
        elif f and f[0] == 'file':
            files.append((int(f[1]), int(f[2]), ' '.join(f[3:])))
    return sectors, files


def write(path, data, mode='wb'):
    tmp = path + '.tmp'
    with open(tmp, mode) as f:
        f.write(data)
    os.replace(tmp, path)


def main(disc, show=()):
    lang = os.path.join(disc, 'lang')
    files, _ = lp.read_layout(disc)
    part2 = files['PART2.PAK'][0]
    bases = sorted(n[:-7] for n in os.listdir(lang) if n.endswith('.layout') and not n.startswith(PREFIX))
    if not bases:
        sys.exit(f'{lang} has no language overlays: run python3 -I tools/lang_pack.py {disc} [<eu disc>] first')
    overlays = {combo: read_overlay(os.path.join(lang, combo + '.layout')) for combo in bases}
    next_lsn = max(sectors for sectors, _ in overlays.values())
    blobs = {}  # text code -> (lsn, size)
    for code in sorted({combo.split('_')[0] for combo in bases}):
        perm = open(os.path.join(lang, f'text_{code}.bin'), 'rb').read()
        data, report = keyboard_perm(perm, code)
        write(os.path.join(lang, f'{PREFIX}text_{code}.bin'), data)
        blobs[code] = (next_lsn, len(data))
        next_lsn += (len(data) + lp.SECTOR - 1) // lp.SECTOR
        left = ', '.join(t.decode() for t in report['left']) or 'none'
        over = ', '.join(report['over']) or 'none'
        print(f"{code}: {report['tokens']} tokens -> keys, {report['flp']} move list strings, tokens left: {left}; "
              f"messages with a line wider than its room: {over}")
        for key, value in lp.parse_messages(report['table']):
            if key.decode() in show:
                print(f'  {code} {key.decode()}: {value.decode("latin1")!r}')
    for combo, (_, entries) in overlays.items():
        code = combo.split('_')[0]
        lsn, size = blobs[code]
        toc_lsn, toc_size, toc_name = next(e for e in entries if e[2].endswith('.toc'))
        toc = bytearray(open(os.path.join(lang, toc_name), 'rb').read())
        for i in range(0, len(toc) - 23, 24):
            if toc[i:i + 12].split(b'\0')[0] == b'R_PERM.WAD':
                struct.pack_into('<III', toc, i + 12, 1, size, lsn - part2)
        write(os.path.join(lang, f'{PREFIX}{combo}.toc'), bytes(toc))
        lines = [f'# {PREFIX}{combo}: {combo}.layout with keyboard key names (tools/macos/keyprompts)',
                 f'sectors {next_lsn}', f'file {toc_lsn} {len(toc)} {PREFIX}{combo}.toc']
        lines += [f'file {l} {s} {n}' for l, s, n in entries if n not in (toc_name, f'text_{code}.bin')]
        lines.append(f'file {lsn} {size} {PREFIX}text_{code}.bin')
        write(os.path.join(lang, f'{PREFIX}{combo}.layout'), '\n'.join(lines) + '\n', 'w')
    print(f'{lang}: {len(overlays)} keyboard overlays ({PREFIX}<text>_<voice>.layout), disc end -> {next_lsn} sectors')


if __name__ == '__main__':
    args = sys.argv[1:]
    shown = ()
    if '--show' in args:
        i = args.index('--show')
        shown = tuple(args[i + 1].split(','))
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    main(args[0], shown)
