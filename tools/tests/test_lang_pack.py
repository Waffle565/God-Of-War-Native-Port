"""Checks for tools/lang_pack.py. Run: python3 -I -m unittest discover -s tools/tests"""
import os
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


if __name__ == '__main__':
    unittest.main()
