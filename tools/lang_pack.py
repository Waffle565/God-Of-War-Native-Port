"""Disc overlays of the PC/Switch port: the port's save wording and, from the European disc, other languages
(docs/settings.md). The runtime reads one overlay through PS2X_DISC_OVERLAY (ps2xIOP disc_source.cpp), so the extracted
USA disc stays untouched.

- Wording (always): the menus and in-game screens name the save file instead of the PS2 memory card
  (tools/gow_wording.py rewrites the text inside R_SHELL.WAD and R_PERM.WAD).
- Languages (with the European disc, SCES-53133, God of War (Europe, Australia) (En,Fr,De,Es,It)): the USA disc is
  English only; the European one has the menus (R_SHELL<letter>.WAD), the in-game screens and text (R_PERM<letter>.WAD)
  in five languages and the voices dubbed in four (X.VA1-4 / X.VP1-4 next to the English X.VAG / X.VPK: VA1 French,
  VA2 German, VA3 Italian, VA4 Spanish, identified by speech recognition). The European WADs work in the USA game
  once their objects lose the language letter the European executable adds (R_ShellI -> R_Shell, FLP_HUDI -> FLP_HUD,
  shelli -> shell, msgsI.txt -> msgs_en.txt, save folder BESCES-53133 -> BASCUS-97399: european_wad()); their
  message table keeps the USA-only ids in English.
  The dubbed voice files take the place of the English ones.

Output in <usa disc>/lang/:
  text_<code>.bin       R_PERM.WAD in <code> (USA for en, else the European one): wording + message table
  shell_<code>.bin      R_SHELL.WAD in <code>: menus + wording
  voice_<code>/<NAME>   the dubbed files of <code> under the USA names (not for en)
  <text>_<voice>.toc    GODOFWAR.TOC with those files at their new places
  text_<code>_nx.bin, shell_<code>_nx.bin, <text>_<voice>_nx.toc/.layout
                        only with --nx (Switch only, the PC folder never gets them): the same with the Nintendo
                        Switch button icons (tools/gow_pad_icons.py; the Switch build picks the _nx layout unless
                        env.txt sets GOW_PAD_ICONS=ps)
  <text>_<voice>.layout the overlay: the patched TOC + each replacement file at its disc place. The game addresses
                        PART1/PART2.PAK with 32-bit byte offsets, so a file must start within 4 GiB of its PAK: the
                        replacements take the space of the English files they replace (both PAKs are nearly full;
                        ps2xIOP disc_source.cpp lets an overlay file lie inside a PAK).
- Japanese voices (with the Japanese disc, SLPM-67010): a full Japanese dub (identified by speech recognition) under
  the USA file names, so every VAG/VPK/PSS/PSW whose content differs from the USA disc is the Japanese one (voices,
  the movies with their Japanese audio and subtitles; a movie's .PSS reads its widescreen .PSW, see main()).
  Japanese and Korean text are not offered: those
  discs draw them with their own fonts (GOWJ_*/GOWK_*, HUDJ*/HUDK*) and text code the USA executable does not have.
The USA executable keeps its file names (R_PERM.WAD, X.VAG); only the data behind them changes.

usage: python -I tools/lang_pack.py <usa disc folder> [<eu disc folder>] [--ja <japanese disc folder>] [--out <dir>] [--nx]
       (--out: write the overlays there instead of <usa disc>/lang)
       (folders from tools/iso_extract.py --layout)
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # tools/ (python -I leaves it out)
import gow_movie  # noqa: E402
import gow_pad_icons  # noqa: E402
import gow_wording  # noqa: E402

SECTOR = 2048
TEXT_LETTER = {'fr': 'F', 'de': 'G', 'it': 'I', 'es': 'S'}
VOICE_NUMBER = {'fr': 1, 'de': 2, 'it': 3, 'es': 4}
# file kinds that carry speech on the Japanese disc (voices, cutscene audio streams, movies)
JAPANESE_DUB = ('.VAG', '.VPK', '.PSS', '.PSW')


def read_layout(disc):
    files, sectors = {}, 0
    for line in open(os.path.join(disc, '.disc_layout')):
        f = line.split()
        if f and f[0] == 'sectors':
            sectors = int(f[1])
        elif f and f[0] == 'file':
            files[' '.join(f[3:])] = (int(f[1]), int(f[2]))
    return files, sectors


def read_toc(disc):
    raw = open(os.path.join(disc, 'GODOFWAR.TOC'), 'rb').read()
    entries = []
    for i in range(0, len(raw) - 23, 24):
        name = raw[i:i + 12].split(b'\0')[0].decode('latin1')
        part, size, offset = struct.unpack_from('<III', raw, i + 12)
        entries.append((i, name, part, size, offset))
    return raw, entries


def read_file(disc, entries, name):
    for _, n, part, size, offset in entries:
        if n == name:
            with open(os.path.join(disc, f'PART{part + 1}.PAK'), 'rb') as f:
                f.seek(offset * SECTOR)
                return f.read(size)
    raise KeyError(f'{name} not in {disc}')


def wad_records(data):
    """WAD = records {u32 tag, u32 size, char name[24], data[size] padded to 16}; tag 24 = value, no data."""
    pos, records = 0, []
    while pos + 32 <= len(data):
        tag, size = struct.unpack_from('<II', data, pos)
        body = 0 if tag == 24 else size
        records.append((pos, tag, size, data[pos + 8:pos + 32], body))
        pos += 32 + ((body + 15) & ~15)
    if pos != len(data):
        raise ValueError('not a WAD')
    return records


def parse_messages(table):
    return re.findall(rb'\*(\d+)\*\n(.*?)\n\n', table, re.S)


def european_wad(wad, letter):
    """a European language WAD (R_SHELL<letter>/R_PERM<letter>.WAD) under the USA names: the European executable
    looks its objects up with the language letter appended (R_ShellX, FLP_%s with ShellX/HUDX, msgs%s.txt), the USA
    one without it, so every name field naming R_Shell<l>, R_Perm<l>, Shell<l>, HUD<l> (any case, followed by NUL,
    a digit or '_') loses the letter, and msgs<l>.txt becomes msgs_en.txt. Names are NUL-padded fixed fields: the
    rest of the name moves left and the field keeps its size."""
    out = bytearray(wad)
    pattern = re.compile(rb'((?i:r_shell|r_perm|shell|hud))[' + letter.upper().encode() + letter.lower().encode()
                         + rb'](?=[\x000-9_])')
    for m in reversed(list(pattern.finditer(out))):
        end = out.index(b'\0', m.end())
        name = out[m.start():end]
        out[m.start():end + 1] = name[:len(m.group(1))] + name[len(m.group(1)) + 1:] + b'\0\0'
    table = b'msgs' + letter.upper().encode() + b'.txt\0'
    at = out.find(table)
    while at >= 0:
        out[at:at + len(table) + 2] = b'msgs_en.txt\0'
        at = out.find(table, at + 1)
    # the save folder (R_PERM): the European game's, BESCES-53133GodOfWar, becomes the USA one, so every language
    # reads and writes the same saves
    return bytes(out).replace(b'BESCES-53133GodOfWar', b'BASCUS-97399GodOfWar')


def with_usa_messages(eu_perm, usa_perm):
    """a renamed European R_PERM.WAD whose message table also has the USA-only ids (1, 8, 9) in English, in the USA
    table's order (the other ids are shared)"""
    usa = {}
    for pos, tag, size, name, body in wad_records(usa_perm):
        if name.startswith(b'msgs'):
            usa = parse_messages(usa_perm[pos + 32:pos + 32 + size])
    out = bytearray()
    for pos, tag, size, name, body in wad_records(eu_perm):
        if name.startswith(b'msgs'):
            table = eu_perm[pos + 32:pos + 32 + size]
            foreign = dict(parse_messages(table))
            text = b''.join(b'*%s*\n%s\n\n' % (k, foreign.get(k, v)) for k, v in usa)
            text += table[table.rindex(b'\n\n') + 2:]  # the table's own end (a terminating NUL)
            out += struct.pack('<II', tag, len(text)) + name + text + b'\0' * (-len(text) % 16)
        else:
            out += eu_perm[pos:pos + 32 + ((body + 15) & ~15)]
    return bytes(out)


# files the game streams while playing (movies, voices): they must keep their PAK and stay inside the disc (place())
STREAMED = ('.PSS', '.PSW', '.VAG', '.VPK')
PAK_LIMIT = (1 << 32) // SECTOR  # the game addresses a PAK with 32-bit byte offsets: sectors reachable from its start


def sectors_of(size):
    return (size + SECTOR - 1) // SECTOR


def read_meta(disc):
    """the metadata sectors of tools/iso_extract.py --layout (.disc_meta: u32 lsn + 2048 bytes each)"""
    raw = open(os.path.join(disc, '.disc_meta'), 'rb').read()
    return [struct.unpack_from('<I', raw, i)[0] for i in range(0, len(raw), 4 + SECTOR)]


class NoRoom(Exception):
    """place(): no room for this file"""


def place(files, meta, disc_end, entries, replaced, freed, sizes):
    """Disc places for the replacement files of one overlay: {usa name: (part, offset in sectors)}.
    The game addresses a PAK with 32-bit byte offsets, so a file must lie within 4 GiB of its PAK's start, and both
    PAKs nearly fill that. Measured 2026-10-19 (tools/movies.py, SEMO01): a movie plays when moved inside its own PAK,
    not when moved to the other PAK or past the end of the original disc; text WADs work from past the disc end in the
    other PAK (French text, 2026-10-18). So streamed files (STREAMED) stay in the PAK of the English file and inside
    the disc, the others may go anywhere below the limit. Replacements take the space of the English files they
    replace (freed: those plus files the overlay no longer uses) and the unused room; adjacent freed files merge into
    one extent. Largest first, best fit."""
    starts = [files['PART1.PAK'][0], files['PART2.PAK'][0]]
    used_end = [sectors_of(files['PART1.PAK'][1]), sectors_of(files['PART2.PAK'][1])]
    for _, name, part, size, offset in entries:
        used_end[part] = max(used_end[part], offset + sectors_of(size))
    # the room after a PAK's last file up to the limit, around what else is on the disc there: the other files
    # (PART1 is followed by PART2) and the metadata sectors (.disc_meta, e.g. the descriptors at the end of the disc)
    occupied = sorted([(lsn, lsn + sectors_of(size)) for lsn, size in files.values()] + [(lsn, lsn + 1) for lsn in meta]
                      + [(disc_end, disc_end)])  # splits the room at the disc end
    extents = []  # (part, offset, sectors)
    for part in (0, 1):
        start, limit = starts[part] + used_end[part], starts[part] + PAK_LIMIT
        for begin, end in occupied + [(limit, limit)]:
            if end <= start:
                continue
            if min(begin, limit) > start:
                extents.append((part, start - starts[part], min(begin, limit) - start))
            start = max(start, end)
            if start >= limit:
                break
    for _, name, part, size, offset in entries:
        if name in freed:
            extents.append((part, offset, sectors_of(size)))
    extents.sort()
    merged = []
    for part, offset, length in extents:
        if merged and merged[-1][0] == part and merged[-1][1] + merged[-1][2] == offset:
            merged[-1][2] += length
        else:
            merged.append([part, offset, length])
    part_of = {name: part for _, name, part, _, _ in entries}

    def fits(extent, name, need):
        if extent[2] < need:
            return False
        return not name.endswith(STREAMED) or (extent[0] == part_of[name] and
                                               starts[extent[0]] + extent[1] + need <= disc_end)

    places = {}
    for name in sorted(replaced, key=lambda n: -sizes[n]):
        need = sectors_of(sizes[name])
        extent = min((e for e in merged if fits(e, name, need)), key=lambda e: e[2], default=None)  # best fit
        if extent is None:
            raise NoRoom(name)
        places[name] = (extent[0], extent[1])
        extent[1] += need
        extent[2] -= need
    return places


def dub_movies(code, usa, entries, usa_names, eu, eu_entries, eu_names, store, alias):
    """The intro SEMO01 narrated in a European language (tools/gow_movie.py): its USA widescreen movie with the
    language's track and closing clip; the .PSS reads it too. Returns the bytes stored.
    The European bonus movies per language (BONUS01/19/20/24 B-E) are not used: they keep the English speech (only
    their pictures differ, by subtitles) and run at the PAL 25 pictures per second, which the USA player does not
    finish (measured 2026-10-19)."""
    total = 0
    clips = [f'SEMO01{x}.PSS' for x in 'ABCDE']
    if 'SEMO01.PSW' in usa_names and all(c in eu_names for c in ['SEMO01.PSS'] + clips):
        movie = gow_movie.dub_intro(read_file(usa, entries, 'SEMO01.PSW'), read_file(eu, eu_entries, 'SEMO01.PSS'),
                                    [read_file(eu, eu_entries, c) for c in clips], VOICE_NUMBER[code])
        store('SEMO01.PSW', movie)
        alias['SEMO01.PSS'] = 'SEMO01.PSW'
        total += len(movie)
    return total


def movie_dubs(usa, entries, usa_names, eu, eu_entries, eu_names, out, voices):
    """Every speaking movie in the European languages, as dub files (tools/gow_movie.py dub_file, played by ps2xIOP
    disc_source.cpp over the USA movie): {voice code: {usa movie name: path relative to lang/}}.
    A movie is dubbed when its European version (same name, or the .PSS of a USA .PSW: the European disc has the
    SEMO movies only as .PSS) has the five language tracks (0 en, 1 fr, 2 de, 3 it, 4 es); the European movies with
    one track have no speech (music and effects, checked by speech recognition 2026-10-30) and stay as they are.
    Only the file the port plays is dubbed: the .PSW where there is one (the port is always 16:9), else the .PSS.
    SEMO01 is not here: dub_movies() replaces it whole (its text card is in the picture)."""
    dubs = {code: {} for code in voices if code in VOICE_NUMBER}
    if not dubs:
        return dubs
    total = 0
    for name in sorted(usa_names):
        base, ext = os.path.splitext(name)
        if ext not in ('.PSS', '.PSW') or base == 'SEMO01' or (ext == '.PSS' and base + '.PSW' in usa_names):
            continue
        source = name if name in eu_names else base + '.PSS'
        if source not in eu_names:
            continue
        european = read_file(eu, eu_entries, source)
        if len(gow_movie.tracks(european)) < 5:
            continue
        movie = read_file(usa, entries, name)
        for code in dubs:
            path = f'voice_{code}/{base}.DUB'
            data = gow_movie.dub_file(movie, european, VOICE_NUMBER[code])
            with open(os.path.join(out, path), 'wb') as f:
                f.write(data)
            dubs[code][name] = path
            total += len(data)
    print(f'movie dubs: {len(next(iter(dubs.values())))} movies x {len(dubs)} languages, {total >> 20} MB')
    return dubs


def main(usa, eu=None, jpn=None, out=None, nx=False):
    files, sectors = read_layout(usa)
    meta = read_meta(usa)
    starts = [files['PART1.PAK'][0], files['PART2.PAK'][0]]
    toc_lsn, _ = files['GODOFWAR.TOC']
    raw, entries = read_toc(usa)
    langs = ['en'] + (list(TEXT_LETTER) if eu else [])  # text languages
    voices = langs + (['ja'] if jpn else [])
    out = out or os.path.join(usa, 'lang')
    os.makedirs(out, exist_ok=True)

    # every replacement file once on disk, relative to lang/: {key: {usa name: (path, size)}}
    stored = {}
    # names that read another replacement: {key: {usa name: usa name it reads}}
    aliases = {}

    def store(key, name, data, path):
        os.makedirs(os.path.dirname(os.path.join(out, path)) or out, exist_ok=True)
        with open(os.path.join(out, path), 'wb') as f:
            f.write(data)
        stored.setdefault(key, {})[name] = (path, len(data))

    usa_perm = read_file(usa, entries, 'R_PERM.WAD')
    usa_shell = read_file(usa, entries, 'R_SHELL.WAD')
    if eu:
        _, eu_entries = read_toc(eu)
        eu_names = {e[1] for e in eu_entries}
    if jpn:
        _, jpn_entries = read_toc(jpn)
        jpn_names = {e[1] for e in jpn_entries}
    for code in langs:
        if code == 'en':
            perm, shell = usa_perm, usa_shell
        else:
            letter = TEXT_LETTER[code]
            perm = with_usa_messages(european_wad(read_file(eu, eu_entries, f'R_PERM{letter}.WAD'), letter), usa_perm)
            shell = european_wad(read_file(eu, eu_entries, f'R_SHELL{letter}.WAD'), letter)
        perm, shell = gow_wording.apply(perm, code), gow_wording.apply(shell, code)
        store(f'text_{code}', 'R_PERM.WAD', perm, f'text_{code}.bin')
        store(f'shell_{code}', 'R_SHELL.WAD', shell, f'shell_{code}.bin')
        if not nx:
            continue
        store(f'text_{code}_nx', 'R_PERM.WAD', gow_pad_icons.apply(perm), f'text_{code}_nx.bin')
        store(f'shell_{code}_nx', 'R_SHELL.WAD', gow_pad_icons.apply(shell), f'shell_{code}_nx.bin')
    usa_names = {e[1] for e in entries}
    for code in voices[1:]:
        total = 0
        for name in sorted({e[1] for e in entries}):
            base, ext = os.path.splitext(name)
            if code == 'ja':
                if ext not in JAPANESE_DUB or name not in jpn_names:
                    continue
                # a movie with a widescreen version (.PSW) is only played as that one (the port is always 16:9), so
                # its .PSS reads the Japanese widescreen movie too and its English .PSS space is free for others
                if ext == '.PSS' and base + '.PSW' in usa_names:
                    aliases.setdefault(f'voice_{code}', {})[name] = base + '.PSW'
                    continue
                body = read_file(jpn, jpn_entries, name)
                if body == read_file(usa, entries, name):
                    continue
            else:
                dub = base + {'.VAG': f'.VA{VOICE_NUMBER[code]}', '.VPK': f'.VP{VOICE_NUMBER[code]}'}.get(ext, '?')
                if dub not in eu_names:
                    continue
                body = read_file(eu, eu_entries, dub)
            store(f'voice_{code}', name, body, f'voice_{code}/{name}')
            total += len(body)
        if code in VOICE_NUMBER:
            total += dub_movies(code, usa, entries, usa_names, eu, eu_entries, eu_names,
                                lambda name, body: store(f'voice_{code}', name, body, f'voice_{code}/{name}'),
                                aliases.setdefault(f'voice_{code}', {}))
        print(f'voice {code}: {len(stored[f"voice_{code}"])} files, {total >> 20} MB')
    dubs = movie_dubs(usa, entries, usa_names, eu, eu_entries, eu_names, out, voices) if eu else {}

    for text, voice, icons in [(t, v, i) for t in langs for v in voices for i in (('', '_nx') if nx else ('',))]:
        members = dict(stored[f'shell_{text}{icons}'])
        members.update(stored[f'text_{text}{icons}'])
        alias = {}
        if voice != 'en':
            members.update(stored[f'voice_{voice}'])
            alias = {n: t for n, t in aliases.get(f'voice_{voice}', {}).items() if t in members}
        while True:
            try:
                places = place(files, meta, sectors, entries, set(members), set(members) | set(alias),
                               {n: size for n, (_, size) in members.items()})
                break
            except NoRoom as e:
                # a movie that does not fit stays English (its English file keeps its place); placing again
                # without it, as its space was counted free
                name = e.args[0]
                if not name.endswith(('.PSS', '.PSW')):
                    raise SystemExit(f'{text}_{voice}: no room for {name}')
                print(f'{text}_{voice}: no room for {name}, it stays English')
                del members[name]
                alias = {n: t for n, t in alias.items() if t != name}
        patched = bytearray(raw)
        lines = []
        end = sectors
        for i, name, part, size, offset in entries:
            if name in members or name in alias:
                target = alias.get(name, name)
                path, length = members[target]
                new_part, new_offset = places[target]
                struct.pack_into('<III', patched, i + 12, new_part, length, new_offset)
                if name in members:
                    lsn = starts[new_part] + new_offset
                    lines.append(f'file {lsn} {length} {path}')
                    end = max(end, lsn + sectors_of(length))
            elif name in dubs.get(voice, {}):  # the movie stays in place; its audio comes from the dub file
                lines.append(f'dub {starts[part] + offset} {size} {dubs[voice][name]}')
        combo = f'{text}_{voice}{icons}'
        open(os.path.join(out, combo + '.toc'), 'wb').write(patched)
        head = [f'# {combo}: text {text}, voices {voice}{", Switch button icons" if icons else ""}'
                ' (tools/lang_pack.py)', f'sectors {end}',
                f'file {toc_lsn} {len(patched)} {combo}.toc']
        open(os.path.join(out, combo + '.layout'), 'w', newline='\n').write('\n'.join(head + sorted(lines)) + '\n')
    both = ' x 2 (PS2 / Switch button icons)' if nx else ''
    print(f'{out}: {len(langs) * len(voices)} overlays{both} (text {", ".join(langs)}; voices {", ".join(voices)})')


if __name__ == '__main__':
    args = sys.argv[1:]
    japanese = None
    if '--ja' in args:
        i = args.index('--ja')
        japanese = args[i + 1]
        del args[i:i + 2]
    nx = '--nx' in args
    args = [a for a in args if a != '--nx']
    target = None
    if '--out' in args:
        i = args.index('--out')
        target = args[i + 1]
        del args[i:i + 2]
    if len(args) not in (1, 2):
        sys.exit(__doc__)
    main(*args, jpn=japanese, out=target, nx=nx)
