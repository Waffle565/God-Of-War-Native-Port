"""Disc overlays of the PC/Switch port: the port's save wording and, from the European disc, other languages
(docs/settings.md). The runtime reads one overlay through PS2X_DISC_OVERLAY (ps2xIOP disc_source.cpp), so the extracted
USA disc stays untouched.

- Wording (always): the menus and in-game screens name the save file instead of the PS2 memory card
  (tools/gow_wording.py rewrites the text inside R_SHELL.WAD and R_PERM.WAD).
- Languages (with the European disc, SCES-53133, God of War (Europe, Australia) (En,Fr,De,Es,It)): the USA disc is
  English only; the European one has the in-game text (the R_PERM<letter>.WAD message table) in five languages and the
  voices dubbed in four (X.VA1-4 / X.VP1-4 next to the English X.VAG / X.VPK: VA1 French, VA2 German, VA3 Italian,
  VA4 Spanish, identified by speech recognition). The message table uses the same ids on both discs, so the USA
  R_PERM.WAD gets the translated table; the dubbed voice files take the place of the English ones. The menus and HUD
  screens keep the USA (English) art and text.

Output in <usa disc>/lang/:
  text_<code>.bin       R_PERM.WAD: wording + message table in <code>
  shell.bin             R_SHELL.WAD: wording
  shell_<code>.bin      R_SHELL.WAD: wording + the title sequence's sound bank in <code> (not for en)
  voice_<code>.bin      the dubbed voice files of <code>, one after the other (not for en)
  movies_<code>.bin     the dubbed movie sound of <code> (not for en; not part of the overlays: the runtime reads it
                        through PS2X_MOVIE_VOICES and puts it in place of the USA movies' PCM, see movie_voices)
  <text>_<voice>.toc    GODOFWAR.TOC with those files pointing at the .bin data
  <text>_<voice>.layout the overlay: the patched TOC + the .bin files, placed past the end of the disc
The USA executable keeps its file names (R_PERM.WAD, X.VAG); only the data behind them changes.

usage: python -I tools/lang_pack.py <usa disc folder> [<eu disc folder>]   (folders from tools/iso_extract.py --layout)
"""
import os
import re
import struct
import sys
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # tools/ (python -I leaves it out)
import gow_wording  # noqa: E402

SECTOR = 2048
TEXT_LETTER = {'fr': 'F', 'de': 'G', 'it': 'I', 'es': 'S'}
VOICE_NUMBER = {'fr': 1, 'de': 2, 'it': 3, 'es': 4}


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


# The European tables are cp1252 text with accented letters; the USA executable's fonts have none, and a letter
# without a glyph is skipped ("Ascensin"). Each one becomes the plain letters a player would type without accents.
USA_FONT_FOLD = {
    'ä': 'ae', 'ö': 'oe', 'ü': 'ue', 'Ä': 'Ae', 'Ö': 'Oe', 'Ü': 'Ue', 'ß': 'ss',
    'œ': 'oe', 'Œ': 'Oe', 'æ': 'ae', 'Æ': 'Ae',
    '¿': '', '¡': '', '–': '-', '—': '-', '‘': "'", '’': "'", '“': '"', '”': '"',
    '…': '...', '°': 'o', '«': '"', '»': '"',
}


def fold_for_usa_font(data):
    """cp1252 bytes -> ASCII bytes the USA fonts can draw (accents dropped, umlauts spelled out)."""
    out = []
    for ch in data.decode('cp1252', errors='replace'):
        if ord(ch) < 0x80:
            out.append(ch)
        elif ch in USA_FONT_FOLD:
            out.append(USA_FONT_FOLD[ch])
        else:
            base = unicodedata.normalize('NFKD', ch).encode('ascii', 'ignore').decode('ascii')
            out.append(base)
    return ''.join(out).encode('ascii')


def parse_messages(table):
    return re.findall(rb'\*(\d+)\*\n(.*?)\n\n', table, re.S)


def localized_perm(usa_perm, eu_perm):
    """USA R_PERM.WAD with each message the European table has replaced by its translation (the ids are shared;
    USA-only ids keep their English text)."""
    foreign = {}
    for pos, tag, size, name, body in wad_records(eu_perm):
        if name.startswith(b'msgs'):
            foreign = {k: fold_for_usa_font(v) for k, v in parse_messages(eu_perm[pos + 32:pos + 32 + size])}
    out = bytearray()
    for pos, tag, size, name, body in wad_records(usa_perm):
        if name.startswith(b'msgs'):
            table = usa_perm[pos + 32:pos + 32 + size]
            text = b''.join(b'*%s*\n%s\n\n' % (k, foreign.get(k, v)) for k, v in parse_messages(table))
            text += table[table.rindex(b'\n\n') + 2:]  # the table's own end (a terminating NUL)
            out += struct.pack('<II', tag, len(text)) + name + text + b'\0' * (-len(text) % 16)
        else:
            out += usa_perm[pos:pos + 32 + ((body + 15) & ~15)]
    return bytes(out)


def voiced_shell(usa_shell, eu_shell):
    """USA R_SHELL.WAD with its sound banks (SBK_*) taken from a European R_SHELL<letter>.WAD. SBK_Title01 holds the
    line Kratos says before the intro movie ("The gods of Olympus have abandoned me."), dubbed per language there;
    the menus (FLP_Shell) stay USA: the European ones expect the European executable."""
    banks = {name.split(b'\0')[0]: eu_shell[pos + 32:pos + 32 + size]
             for pos, tag, size, name, body in wad_records(eu_shell) if name.startswith(b'SBK')}
    out = bytearray()
    for pos, tag, size, name, body in wad_records(usa_shell):
        bank = banks.get(name.split(b'\0')[0])
        if bank is not None:
            out += struct.pack('<II', tag, len(bank)) + name + bank + b'\0' * (-len(bank) % 16)
        else:
            out += usa_shell[pos:pos + 32 + ((body + 15) & ~15)]
    return bytes(out)


def overlay_files(text, voice):
    """the blobs of overlay <text>_<voice>: the shell with the voice's title line, the text, the dubbed voices"""
    if voice == 'en':
        return ['shell', f'text_{text}']
    return [f'shell_{voice}', f'text_{text}', f'voice_{voice}']


# Movie voices. The European movies are PAL (640x512, 25 fps) and carry five language tracks: private stream 1,
# sub-stream header FF A1 00 <track>, each with its own SShd (type 16 = SPU ADPCM, 48 kHz, 2 channels, interleave
# 32 bytes). The European executable registers the ADPCM callback on channel <track> (table at 0x29CA18 of
# SCES_531.33). The USA executable plays only PCM (sub-stream FF A0, type 1, interleave 512), so the USA movies keep
# their picture and sound, and the runtime swaps in the chosen track while demuxing (PS2X_MOVIE_VOICES, MovieVoice.cpp).
MOVIE_TRACK = {'en': 0, 'fr': 1, 'de': 2, 'it': 3, 'es': 4}
ADPCM_LEAD = 28  # each European track starts with one silent ADPCM block (28 samples) the USA PCM does not have
# SEMO01 (the intro): the European movie stops 36.2 s in; the rest is a short movie per language, SEMO01A-E (letter =
# 'a' + track), whose PCM starts at this USA sample (measured against SEMO01A, the English one).
INTRO_TAIL = ('SEMO01', 1741265)
STREAM_HEADER = 0x28  # SShd (32 bytes) + SSbd (8 bytes) at the start of every PSS audio stream


def pss_audio(f, size):
    """{(sub-stream, track): stream bytes} of the private stream 1 audio in a PSS (MPEG-2 program stream) read from f"""
    tracks, buf, pos, left = {}, b'', 0, size
    while True:
        if len(buf) - pos < 0x10000 and left:
            chunk = f.read(min(left, 1 << 22))
            left -= len(chunk)
            buf, pos = buf[pos:] + chunk, 0
        if len(buf) - pos < 6 or buf[pos:pos + 3] != b'\0\0\1' or buf[pos + 3] == 0xB9:
            return {k: bytes(v) for k, v in tracks.items()}
        if buf[pos + 3] == 0xBA:  # pack header + stuffing
            pos += 14 + (buf[pos + 13] & 7)
            continue
        n = struct.unpack_from('>H', buf, pos + 4)[0]
        if buf[pos + 3] == 0xBD:
            payload = buf[pos + 9 + buf[pos + 8]:pos + 6 + n]  # after the PES header: FF Ax 00 <track> + data
            tracks.setdefault((payload[1], payload[3]), bytearray()).extend(payload[4:])
        pos += 6 + n


def movie_key(stream):
    """FNV-1a 64 of an audio stream's header and first 2048 PCM bytes: how the runtime recognises a USA movie (the .PSS
    and .PSW of a movie share their PCM)"""
    h = 0xCBF29CE484222325
    for b in stream[:STREAM_HEADER + 2048]:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def movie_segments(name, code, tracks_of):
    """the dubbed sound of a movie for language <code> as [(USA start sample, kind 0 ADPCM / 1 PCM16, interleave,
    data)], None when the movie has no language tracks; tracks_of(name) gives a European movie's pss_audio"""
    track = MOVIE_TRACK[code]
    stream = tracks_of(name).get((0xA1, track))
    if stream is None:
        return None
    segments = [(-ADPCM_LEAD, 0, struct.unpack_from('<I', stream, 20)[0], stream[STREAM_HEADER:])]
    if name == INTRO_TAIL[0]:
        tail = tracks_of(name + 'ABCDE'[track])[(0xA0, 0)]
        segments.append((INTRO_TAIL[1], 1, struct.unpack_from('<I', tail, 20)[0], tail[STREAM_HEADER:]))
    return segments


def pack_movies(movies):
    """lang/movies_<code>.bin: 'PSMV', u32 version 1, u32 movies, u32 0; per movie (by key) {u64 key, u32 first
    segment, u32 segments}; per segment {i32 start sample, u32 kind, u32 interleave, u32 0, u64 data offset, u64 size};
    then the data, 16-byte aligned"""
    keys = sorted(movies)
    nseg = sum(len(movies[k]) for k in keys)
    head, table, data = bytearray(b'PSMV' + struct.pack('<3I', 1, len(keys), 0)), bytearray(), bytearray()
    base = 16 + 16 * len(keys) + 32 * nseg
    for k in keys:
        head += struct.pack('<QII', k, len(table) // 32, len(movies[k]))
        for start, kind, interleave, body in movies[k]:
            table += struct.pack('<iIIIQQ', start, kind, interleave, 0, base + len(data), len(body))
            data += body + b'\0' * (-len(body) % 16)
    return bytes(head + table + data)


def movie_voices(usa, entries, eu, eu_entries, codes):
    """{code: lang/movies_<code>.bin} for the USA movies the European disc dubs"""
    def open_entry(disc, toc, name):
        _, _, part, size, offset = next(e for e in toc if e[1] == name)
        f = open(os.path.join(disc, f'PART{part + 1}.PAK'), 'rb')
        f.seek(offset * SECTOR)
        return f, size

    eu_names = {e[1] for e in eu_entries}
    cache = {}

    def tracks_of(base):
        if base not in cache:
            cache.clear()
            f, size = open_entry(eu, eu_entries, base + '.PSS')
            with f:
                cache[base] = pss_audio(f, size)
        return cache[base]

    keys = {}  # USA base name -> keys of its .PSS/.PSW audio
    for name in sorted({e[1] for e in entries if e[1].endswith(('.PSS', '.PSW'))}):
        f, size = open_entry(usa, entries, name)
        with f:
            stream = pss_audio(f, min(size, 1 << 18)).get((0xA0, 0))
        if stream and len(stream) >= STREAM_HEADER + 2048:
            keys.setdefault(name[:-4], set()).add(movie_key(stream))
    out = {code: {} for code in codes}
    for base in sorted(keys):
        if base + '.PSS' not in eu_names:
            continue
        for code in codes:
            segments = movie_segments(base, code, tracks_of)
            for k in keys[base] if segments else ():
                out[code][k] = segments
    return {code: pack_movies(movies) for code, movies in out.items()}


def main(usa, eu=None):
    files, sectors = read_layout(usa)
    part2 = files['PART2.PAK'][0]
    toc_lsn, _ = files['GODOFWAR.TOC']
    raw, entries = read_toc(usa)
    langs = ['en'] + (list(TEXT_LETTER) if eu else [])
    out = os.path.join(usa, 'lang')
    os.makedirs(out, exist_ok=True)

    # blobs past the end of the disc: name -> (lsn, data, {usa file name: (offset in blob, size)})
    blobs, next_lsn = {}, sectors

    def add(name, data, members):
        nonlocal next_lsn
        blobs[name] = (next_lsn, data, members)
        next_lsn += (len(data) + SECTOR - 1) // SECTOR

    usa_perm = read_file(usa, entries, 'R_PERM.WAD')
    shell = gow_wording.apply(read_file(usa, entries, 'R_SHELL.WAD'))
    add('shell', shell, {'R_SHELL.WAD': (0, len(shell))})
    if eu:
        _, eu_entries = read_toc(eu)
        eu_names = {e[1] for e in eu_entries}
    for code in langs:
        perm = usa_perm if code == 'en' else localized_perm(usa_perm, read_file(eu, eu_entries, f'R_PERM{TEXT_LETTER[code]}.WAD'))
        perm = gow_wording.apply(perm)
        add(f'text_{code}', perm, {'R_PERM.WAD': (0, len(perm))})
    for code in langs[1:]:
        voiced = voiced_shell(shell, read_file(eu, eu_entries, f'R_SHELL{TEXT_LETTER[code]}.WAD'))
        add(f'shell_{code}', voiced, {'R_SHELL.WAD': (0, len(voiced))})
        data, members = bytearray(), {}
        for name in sorted({e[1] for e in entries}):
            base, ext = os.path.splitext(name)
            dub = base + {'.VAG': f'.VA{VOICE_NUMBER[code]}', '.VPK': f'.VP{VOICE_NUMBER[code]}'}.get(ext, '?')
            if dub in eu_names:
                body = read_file(eu, eu_entries, dub)
                members[name] = (len(data), len(body))
                data += body + b'\0' * (-len(body) % SECTOR)
        add(f'voice_{code}', bytes(data), members)
        print(f'voice {code}: {len(members)} files, {len(data) >> 20} MB')
    for name, (_, data, _) in blobs.items():
        with open(os.path.join(out, name + '.bin'), 'wb') as f:
            f.write(data)
    if eu:
        for code, blob in movie_voices(usa, entries, eu, eu_entries, langs[1:]).items():
            with open(os.path.join(out, f'movies_{code}.bin'), 'wb') as f:
                f.write(blob)
            print(f'movies {code}: {struct.unpack_from("<I", blob, 8)[0]} movie sounds, {len(blob) >> 20} MB')

    for text in langs:
        for voice in langs:
            used = overlay_files(text, voice)
            patched = bytearray(raw)
            for blob in used:
                lsn, _, members = blobs[blob]
                for i, name, part, size, offset in entries:
                    if name in members:
                        start, length = members[name]
                        struct.pack_into('<III', patched, i + 12, 1, length, lsn + start // SECTOR - part2)
            combo = f'{text}_{voice}'
            open(os.path.join(out, combo + '.toc'), 'wb').write(patched)
            lines = [f'# {combo}: text {text}, voices {voice} (tools/lang_pack.py)', f'sectors {next_lsn}',
                     f'file {toc_lsn} {len(patched)} {combo}.toc']
            lines += [f'file {blobs[b][0]} {len(blobs[b][1])} {b}.bin' for b in used]
            open(os.path.join(out, combo + '.layout'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    print(f'{out}: {len(langs) ** 2} overlays ({", ".join(langs)}), disc end {sectors} -> {next_lsn} sectors')


if __name__ == '__main__':
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    main(*sys.argv[1:])
