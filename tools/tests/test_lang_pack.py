"""Checks for tools/lang_pack.py. Run: python3 -I -m unittest discover -s tools/tests"""
import io
import os
import struct
import sys
import unittest

# appended, not prepended: tools/ holds scripts named like standard modules (dis.py)
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import lang_pack  # noqa: E402


class FoldForUsaFont(unittest.TestCase):
    """The USA executable's fonts have no accented letters: European text must be folded to plain ASCII."""

    def test_spanish_accents_and_marks(self):
        text = 'Ascensión ¿Mantén? ¡Arpón de Orión! Niño ÁÉÍÓÚÑ'.encode('cp1252')
        self.assertEqual(lang_pack.fold_for_usa_font(text), b'Ascension Manten? Arpon de Orion! Nino AEIOUN')

    def test_german_umlauts_and_sharp_s(self):
        text = 'Rücktaste Größe Äpfel'.encode('cp1252')
        self.assertEqual(lang_pack.fold_for_usa_font(text), b'Ruecktaste Groesse Aepfel')

    def test_french_italian_and_cp1252_punctuation(self):
        text = 'élève château cœur Perché \u2013 l\u2019arc \u201cdit\u201d\u2026 10\u00b0'.encode('cp1252')
        self.assertEqual(lang_pack.fold_for_usa_font(text), b'eleve chateau coeur Perche - l\'arc "dit"... 10o')

    def test_ascii_and_tokens_unchanged(self):
        text = b'*702*\nPress [TriangleButton] to launch Kratos.\n\n'
        self.assertEqual(lang_pack.fold_for_usa_font(text), text)


def stream_header(kind, interleave, size):
    """PSS audio stream header: SShd {type, rate, channels, interleave, -1, -1} + SSbd {size} (0x28 bytes)"""
    return b'SShd' + struct.pack('<7I', 0x18, kind, 48000, 2, interleave, 0xFFFFFFFF, 0xFFFFFFFF) + b'SSbd' + struct.pack('<I', size)


def pes(stream_id, payload):
    """MPEG-2 PES packet with a 5-byte PTS header"""
    header = b'\x81\x80\x05' + b'\x21\x00\x01\x00\x01'
    return b'\0\0\1' + bytes([stream_id]) + struct.pack('>H', len(header) + len(payload)) + header + payload


def pss(*packets):
    pack = b'\0\0\1\xba' + bytes([0x44, 0, 4, 0, 4, 1, 0, 0, 3, 0xf8])  # MPEG-2 pack header, no stuffing
    return b''.join(pack + p for p in packets) + b'\0\0\1\xb9'


class MovieVoices(unittest.TestCase):
    """Dubbed movie audio: the European movies carry five language tracks; the USA movies keep their picture and get
    the chosen track through lang/movies_<code>.bin (read by the runtime's PSS demuxer, PS2X_MOVIE_VOICES)."""

    def test_pss_audio_splits_private_stream_tracks(self):
        data = pss(pes(0xE0, b'\0\0\1\xb3video'), pes(0xBD, b'\xff\xa1\x00\x00' + b'en1'),
                   pes(0xBD, b'\xff\xa1\x00\x04' + b'es1'), pes(0xBD, b'\xff\xa1\x00\x04' + b'es2'))
        tracks = lang_pack.pss_audio(io.BytesIO(data), len(data))
        self.assertEqual(tracks, {(0xA1, 0): b'en1', (0xA1, 4): b'es1es2'})

    def test_movie_key_covers_header_and_first_2048_pcm_bytes(self):
        stream = stream_header(1, 512, 4096) + bytes(range(256)) * 16
        key = lang_pack.movie_key(stream)
        self.assertEqual(key, lang_pack.movie_key(stream[:0x28 + 2048] + b'other tail'))
        self.assertNotEqual(key, lang_pack.movie_key(stream[:0x28 + 2047] + b'\x01'))
        self.assertEqual(lang_pack.movie_key(b'a'), 0xAF63DC4C8601EC8C)  # FNV-1a 64 of "a"

    def test_dubbed_movie_takes_the_language_track_without_its_lead_block(self):
        tracks = {(0xA1, n): stream_header(16, 32, 64) + bytes([n]) * 64 for n in range(5)}
        segments = lang_pack.movie_segments('A01MP01', 'es', lambda name: tracks)
        self.assertEqual(segments, [(-28, 0, 32, bytes([4]) * 64)])

    def test_undubbed_movie_has_no_segments(self):
        tracks = {(0xA0, 0): stream_header(1, 512, 1024) + bytes(1024)}
        self.assertIsNone(lang_pack.movie_segments('P01MP01', 'es', lambda name: tracks))

    def test_intro_adds_the_language_tail_movie(self):
        movies = {'SEMO01': {(0xA1, n): stream_header(16, 32, 32) + bytes([n]) * 32 for n in range(5)},
                  'SEMO01E': {(0xA0, 0): stream_header(1, 512, 1024) + b'\x07' * 1024}}
        segments = lang_pack.movie_segments('SEMO01', 'es', movies.__getitem__)
        self.assertEqual(segments, [(-28, 0, 32, bytes([4]) * 32), (1741265, 1, 512, b'\x07' * 1024)])

    def test_pack_layout_read_by_the_runtime(self):
        blob = lang_pack.pack_movies({0x20: [(5, 1, 512, b'abc')], 0x10: [(-28, 0, 32, b'x' * 20), (9, 1, 512, b'y')]})
        self.assertEqual(blob[:16], b'PSMV' + struct.pack('<3I', 1, 2, 0))
        self.assertEqual(struct.unpack_from('<QIIQII', blob, 16), (0x10, 0, 2, 0x20, 2, 1))
        segs = [struct.unpack_from('<iIIIQQ', blob, 48 + 32 * i) for i in range(3)]
        self.assertEqual(segs, [(-28, 0, 32, 0, 144, 20), (9, 1, 512, 0, 176, 1), (5, 1, 512, 0, 192, 3)])
        self.assertEqual((blob[144:164], blob[176:177], blob[192:195], len(blob)), (b'x' * 20, b'y', b'abc', 208))


def wad(*records):
    """WAD bytes from (tag, name, body) records"""
    return b''.join(struct.pack('<II', tag, len(body)) + name.ljust(24, b'\0') + body + b'\0' * (-len(body) % 16)
                    for tag, name, body in records)


class ShellVoice(unittest.TestCase):
    """R_SHELL.WAD holds the title sequence's sound bank (SBK_Title01: Kratos's "The gods of Olympus have abandoned
    me." before the intro movie); the European R_SHELL<letter>.WAD has it dubbed, the rest of the shell stays USA."""

    usa = wad((30, b'WAD_R_Shell', b'w' * 8), (30, b'SBK_Title01', b'E' * 40), (24, b'GroupEnd', b''),
              (30, b'FLP_Shell', b'usa menus'))
    eu = wad((30, b'WAD_R_ShellS', b'w' * 8), (30, b'SBK_Title01', b'S' * 24), (24, b'GroupEnd', b''),
             (30, b'FLP_ShellS', b'european menus'))

    def test_voice_takes_the_european_title_sound_bank(self):
        shell = lang_pack.voiced_shell(self.usa, self.eu)
        self.assertEqual(shell, wad((30, b'WAD_R_Shell', b'w' * 8), (30, b'SBK_Title01', b'S' * 24),
                                    (24, b'GroupEnd', b''), (30, b'FLP_Shell', b'usa menus')))
        self.assertEqual(len(lang_pack.wad_records(shell)), 4)

    def test_shell_without_european_bank_unchanged(self):
        eu = wad((30, b'WAD_R_ShellS', b'w' * 8), (30, b'FLP_ShellS', b'european menus'))
        self.assertEqual(lang_pack.voiced_shell(self.usa, eu), self.usa)

    def test_overlay_uses_the_shell_of_its_voice(self):
        self.assertEqual(lang_pack.overlay_files('fr', 'es'), ['shell_es', 'text_fr', 'voice_es'])
        self.assertEqual(lang_pack.overlay_files('es', 'en'), ['shell', 'text_es'])


if __name__ == '__main__':
    unittest.main()
