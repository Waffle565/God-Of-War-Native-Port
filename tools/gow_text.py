"""Menu and HUD wording of God of War: read and rewrite the text inside the game's Flash chunks (docs/settings.md).

The menus (R_SHELL.WAD chunk FLP_Shell) and in-game screens (R_PERM.WAD chunk FLP_HUD) are Flash-like movies. Their
text is static: SWF-style text records, each a style byte 0x80|flags followed by
  flags 0x08: u16 font, u16 height; 0x04: RGBA color; 0x01: s16 x; 0x02: s16 y   (in this order)
and a glyph count byte with that many {u16 glyph, u16 advance}. Glyph numbers are the font's: per font a fixed
offset from the character for letters (lowercase right after 'Z'), 0 = space, punctuation and digits measured from
known strings (FONTS below). Lines are centered by their x; a slot number ("Slot 1") is a separate field drawn over
the spaces after "Slot", so lines with it keep those spaces where they were.

Rewriting keeps every record's size (the chunk is a serialized structure with pointers): a new line has at most the
old glyph count; the rest become zero-width spaces, and x moves so the line stays centered.

usage: python -I tools/gow_text.py <WAD file> [chunk]   lists the text lines
"""
import collections
import struct
import sys

# per chunk: glyph of 'A' and the measured punctuation/digit glyphs
FONTS = {
    'FLP_HUD': (26, {'!': 1, '"': 2, "'": 3, '(': 4, ')': 5, ',': 8, '-': 9, '.': 10, ':': 22, '?': 25,
                     **{str(i): 12 + i for i in range(10)}}),
    'FLP_Shell': (25, {'"': 2, "'": 3, '(': 4, ')': 5, ',': 7, '.': 9, ':': 21, '?': 24,
                       **{str(i): 11 + i for i in range(10)}}),
}
# the European chunks (R_SHELL<l>/R_PERM<l>.WAD of the European disc, used for its languages, see tools/lang_pack.py)
# number letters, digits and punctuation the same way and add accented glyphs above 'z', numbered per language's
# chunk (the same accent has different numbers in the French and the Italian HUD). Per language and chunk, the glyphs
# named from that chunk's own words (2026-10-19); only these can be written.
EUROPEAN = {
    'it': {
        'FLP_Shell': {'!': 1, '-': 8, '/': 10, 'à': 105, 'è': 109, 'ì': 113},
        'FLP_HUD': {'/': 11, '°': 81, 'à': 106, 'è': 110, 'ì': 114, 'ò': 119, 'ù': 123},
    },
    'fr': {  # Shell: N°1, Écran, fichier à, Colère, Réinsérez, être; HUD: N°1, Écraser, Pâques, Colère, été, Œil
        'FLP_Shell': {'-': 8, '/': 10, '°': 80, 'É': 88, 'à': 105, 'è': 109, 'é': 110, 'ê': 111},
        'FLP_HUD': {'/': 11, 'Œ': 79, '°': 81, 'É': 89, 'à': 106, 'â': 108, 'è': 111, 'é': 112},
    },
    'de': {  # Shell: CARD-Steckplatz, Schätze, Götter, Zurück; HUD: GÖTTERZORN, Größeres, bewältigt, erhöhen, für
        'FLP_Shell': {'-': 8, '/': 10, 'ä': 107, 'ö': 121, 'ü': 125},
        'FLP_HUD': {'/': 11, '°': 81, 'Ö': 100, 'ß': 105, 'ä': 108, 'ö': 122, 'ü': 126},
    },
    'es': {  # Shell: ¿Quieres, Atrás, Héroe, difícil, analógico, menú; HUD: ¿Seguro, ÁREA, más, éxito, Sí, daño, sólo,
        # último
        'FLP_Shell': {'/': 10, '¿': 81, 'á': 106, 'é': 110, 'í': 114, 'ó': 119, 'ú': 123},
        'FLP_HUD': {'/': 11, '°': 81, '¿': 82, 'Á': 84, 'á': 107, 'é': 111, 'í': 115, 'ñ': 118, 'ó': 120, 'ú': 124},
    },
}
SLOT = '{SLOT}'  # in a new line: the old line's spaces after the anchor ("Slot"), in place (the slot number is drawn
# over them)


class Font:
    def __init__(self, a, specials, extra=None):
        self.code = {' ': 0, **specials, **(extra or {})}
        for i in range(26):
            self.code[chr(65 + i)] = a + i
            self.code[chr(97 + i)] = a + 26 + i
        self.char = {g: c for c, g in self.code.items()}

    def decode(self, glyphs):
        return ''.join(self.char.get(g, '{%d}' % g) for g, _ in glyphs)


def style_length(flags):
    return 1 + (4 if flags & 8 else 0) + (4 if flags & 4 else 0) + (2 if flags & 1 else 0) + (2 if flags & 2 else 0)


def lines(data, font):
    """text lines: dicts with the glyph run position, its glyphs, the style record and the text height in effect"""
    out, p, height = [], 0, None
    limit = max(100, max(font.char) + 1)  # glyph numbers above the font's are not text
    while p < len(data) - 5:
        n = data[p]
        if 1 <= n <= 80 and p + 1 + 4 * n <= len(data):
            glyphs = [struct.unpack_from('<HH', data, p + 1 + 4 * j) for j in range(n)]
            style = None
            for flags in range(0x81, 0x90):
                s = p - style_length(flags)
                if s >= 0 and data[s] == flags:
                    style = (s, flags)
                    break
            if (style and all(g <= limit and a <= 0x400 for g, a in glyphs)
                    and sum(g in font.char and g != 0 for g, _ in glyphs) >= (n + 1) // 2):
                s, flags = style
                q = s + 1
                if flags & 8:
                    height = struct.unpack_from('<H', data, q + 2)[0]
                    q += 4
                if flags & 4:
                    q += 4
                x_at = q if flags & 1 else None
                out.append({'pos': p, 'glyphs': glyphs, 'height': height, 'x_at': x_at, 'text': font.decode(glyphs)})
                p += 1 + 4 * n
                continue
        p += 1
    return out


def advances(all_lines):
    table = collections.defaultdict(collections.Counter)
    for line in all_lines:
        for g, a in line['glyphs']:
            if a:
                table[(line['height'], g)][a] += 1
    return {k: c.most_common(1)[0][0] for k, c in table.items()}


def width(glyphs):
    return sum(a for _, a in glyphs)


def rewrite(data, chunk, table, extra=None, anchor='Slot', loose=False):
    """table: [(old line, new line)]; returns the new data and how many lines each entry changed. extra: more glyphs
    of the chunk's font (a European chunk's accents, EUROPEAN[language][chunk]); anchor: the word after which a
    line's slot number gap starts; loose: lines match without their trailing spaces (the European chunks repeat a
    line with different padding)."""
    font = Font(*FONTS[chunk], extra)
    found = lines(data, font)
    adv = advances(found)
    by_glyph = collections.defaultdict(dict)
    for (h, g), a in adv.items():
        by_glyph[g][h] = a

    def advance(g, height):
        if (height, g) in adv:
            return adv[(height, g)]
        h, a = next(iter(by_glyph[g].items()))  # same glyph at another size, scaled
        return round(a * height / h)

    out = bytearray(data)
    done = collections.Counter()
    for line in found:
        for old, new in table:
            if line['text'] != old and not (loose and line['text'].rstrip() == old.rstrip()):
                continue
            glyphs = line['glyphs']

            def encode(text):
                return [(font.code[c], advance(font.code[c], line['height'])) for c in text]

            gap = []
            prefix, suffix = new, ''
            if SLOT in new:  # keep the spaces after "Slot" (the slot number is drawn over them) where they were
                prefix, suffix = new.split(SLOT)
                cut = old.index(anchor) + len(anchor)
                end = cut
                while end < len(glyphs) and glyphs[end][0] == 0:
                    end += 1
                gap = glyphs[cut:end]
            fresh = encode(prefix)
            body = fresh + gap + encode(suffix)
            if len(body) > len(glyphs):
                raise ValueError(f'{chunk}: {new!r} does not fit in {len(glyphs)} glyphs ({old!r})')
            body += [(0, 0)] * (len(glyphs) - len(body))
            if line['x_at'] is not None:
                x = struct.unpack_from('<h', data, line['x_at'])[0]
                if gap:  # the gap starts where it did: shift by the width difference of the text before it
                    x += width(glyphs[:cut]) - width(fresh)
                else:
                    x += (width(glyphs) - width(fresh)) // 2
                struct.pack_into('<h', out, line['x_at'], x)
            struct.pack_into('<' + 'HH' * len(body), out, line['pos'] + 1, *[v for pair in body for v in pair])
            done[old] += 1
    return bytes(out), done


def wad_chunks(data):
    pos = 0
    while pos + 32 <= len(data):
        tag, size = struct.unpack_from('<II', data, pos)
        body = 0 if tag == 24 else size
        yield pos, tag, size, data[pos + 8:pos + 32].split(b'\0')[0].decode('latin1')
        pos += 32 + ((body + 15) & ~15)


def rewrite_wad(wad, table_by_chunk, extra_by_chunk=None, anchor='Slot', loose=False):
    out = bytearray(wad)
    report = {}
    for pos, tag, size, name in wad_chunks(wad):
        if name in table_by_chunk:
            extra = (extra_by_chunk or {}).get(name)
            new, done = rewrite(wad[pos + 32:pos + 32 + size], name, table_by_chunk[name], extra, anchor, loose)
            out[pos + 32:pos + 32 + size] = new
            report[name] = done
    return bytes(out), report


if __name__ == '__main__':
    # usage: gow_text.py <WAD> [chunk] [--lang it]   (--lang: a European chunk of that language, its accents)
    args = sys.argv[1:]
    language = None
    if '--lang' in args:
        i = args.index('--lang')
        language = args[i + 1]
        del args[i:i + 2]
    wad = open(args[0], 'rb').read()
    for pos, tag, size, name in wad_chunks(wad):
        if name in FONTS and (len(args) < 2 or args[1] == name):
            extra = EUROPEAN.get(language, {}).get(name)
            for line in lines(wad[pos + 32:pos + 32 + size], Font(*FONTS[name], extra)):
                print(f"{name} {line['pos']:#07x} h={line['height']} {len(line['glyphs']):2d} {line['text']}")
