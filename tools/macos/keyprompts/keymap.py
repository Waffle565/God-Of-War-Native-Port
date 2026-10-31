"""Key names for the game's button tokens, per text language (odd/tasks/macos-port.md S5 layout, S11).

The message table (R_PERM.WAD msgs_en.txt) writes buttons as tokens such as [XButton], which the game draws as PS2
icons. The keyboard overlays replace them with the key that the port's keyboard layout presses
(ps2xRuntime ps2_pad_keyboard.h): WASD left stick, arrows right stick, left click Square, right click Triangle,
Space Cross, E Circle, Q/R L1/R1, F/G L2/R2, 1-4 d-pad up/right/down/left, Enter Start, Backspace Select.
[L3] and [R3] have no key and stay as they are.

The translated tables use [Joystick] as the noun "stick" ("[Joystick] izquierdo" = left stick), so the sticks
are replaced as whole phrases first (STICKS); a [Joystick] that is left over becomes WASD, except in messages about
L3/R3, which keep the icon.

Key words are plain ASCII: the Power Up / Moves font has no accented letters (it drops the é of "Entrée"), so
French and German write them without accents (checked by make_kb_overlays.py).
"""

# token -> key text; the same keys in every language, only the words differ
KEYS = {
    'en': {'Enter': 'Enter', 'Backspace': 'Backspace', 'Arrows': 'arrow keys'},
    'es': {'Enter': 'Intro', 'Backspace': 'Retroceso', 'Arrows': 'flechas'},
    'fr': {'Enter': 'Entree', 'Backspace': 'Retour', 'Arrows': 'fleches'},
    'de': {'Enter': 'Eingabe', 'Backspace': 'Ruecktaste', 'Arrows': 'Pfeiltasten'},
    'it': {'Enter': 'Invio', 'Backspace': 'Backspace', 'Arrows': 'frecce'},
}

# (left mouse button, right mouse button, space bar), longest first: a line that would grow past its room uses the
# next, shorter words (make_kb_overlays.py: the move list's two-column rows and the hint boxes)
SHORT = {
    'en': [('left click', 'right click', 'Space'), ('L-click', 'R-click', 'Space'), ('LMB', 'RMB', 'Space')],
    'es': [('clic izquierdo', 'clic derecho', 'Espacio'), ('clic izq.', 'clic der.', 'Espacio'), ('LMB', 'RMB', 'Esp.')],
    'fr': [('clic gauche', 'clic droit', 'Espace'), ('clic G', 'clic D', 'Espace'), ('LMB', 'RMB', 'Esp.')],
    'de': [('Linksklick', 'Rechtsklick', 'Leertaste'), ('LMT', 'RMT', 'Leertaste'), ('LMT', 'RMT', 'Leer')],
    'it': [('clic sinistro', 'clic destro', 'Spazio'), ('clic sx', 'clic dx', 'Spazio'), ('LMB', 'RMB', 'Spazio')],
}


def tokens(code, tier=0):
    """[token] -> key text for a language; `tier`: which SHORT words (clamped to the shortest)"""
    k = KEYS[code]
    left, right, space = SHORT[code][min(tier, len(SHORT[code]) - 1)]
    return {
        '[XButton]': space, '[CircleButton]': 'E', '[SquareButton]': left, '[TriangleButton]': right,
        '[L1Button]': 'Q', '[R1Button]': 'R', '[L2Button]': 'F', '[R2Button]': 'G', '[StartButton]': k['Enter'],
        '[SelectButton]': k['Backspace'], '[UpButton]': '1', '[RightButton]': '2', '[DownButton]': '3',
        '[LeftButton]': '4',
    }


# stick phrases (regular expressions, case-insensitive) -> key text, tried in this order (the right stick first where
# the left-stick pattern would also match it). Messages that mention L3/R3 (pressing the sticks) keep them.
STICKS = {
    'en': [(r'(the )?left analog stick ?\[Joystick\]', 'WASD'),
           (r'right analog stick ?\[Joystick\]', '{Arrows}'),
           (r'right analog stick(?= \+)', '{Arrows}')],
    'es': [(r'(el +)?\[Joystick\] izquierdo', 'WASD'),
           (r'\[Joystick\] derecho', '{Arrows}')],
    'fr': [(r'(le +)?\[Joystick\] gauche', 'WASD'),
           (r'\[Joystick\] droit', '{Arrows}')],
    'de': [(r'(mit )?rechtem Analog-Stick ?\[Joystick\]', 'mit {Arrows}'),
           (r'rechter Analog-Stick(?= \+)', '{Arrows}'),
           (r'(linke[rn]? )?Analog-Stick ?\[Joystick\]', 'WASD')],
    'it': [(r'(la +)?\[Joystick\] sinistra', 'WASD'),
           (r'\[Joystick\] destra', '{Arrows}')],
}
