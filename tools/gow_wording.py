"""The port's wording for the save screens and PS2-only prompts (docs/settings.md "Save wording"): the save file
replaces the PS2 memory card, so no line names a MEMORY CARD (8MB)(for PlayStation 2), a card slot or a DUALSHOCK 2.

Each entry is (line on the USA disc, new line); tools/gow_text.py rewrites them in FLP_HUD (R_PERM.WAD) and FLP_Shell
(R_SHELL.WAD). "{SLOT}" keeps the spaces after the old line's "Slot", over which the game draws the save slot number. An empty new
line hides the old one. The card-name lines become empty: each message reads from its first and last line.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # tools/ (python -I leaves it out)
import gow_text  # noqa: E402

SLOT = gow_text.SLOT

LINES = [
    # the card name on its own line in every message
    ('MEMORY CARD (8MB)(for PlayStation 2)', ''),
    (' MEMORY CARD (8MB)(for PlayStation 2)', ''),
    ('MEMORY CARD (8MB)(for PlayStation  2)', ''),
    ('on MEMORY CARD (8MB)(for PlayStation 2)', ''),
    # "... in MEMORY CARD Slot 1." -> "... in save game slot 1."
    ('in MEMORY CARD Slot   .', 'in save game slot' + SLOT + '.'),
    ('in MEMORY CARD Slot   . ', 'in save game slot' + SLOT + '.'),
    (' in MEMORY CARD Slot   .', 'in save game slot' + SLOT + '.'),
    ('in MEMORY CARD Slot   ?', 'in save game slot' + SLOT + '?'),
    (' in MEMORY CARD Slot   ?', 'in save game slot' + SLOT + '?'),
    ('in MEMORY CARD Slot    ', 'in save game slot' + SLOT),
    ('found in MEMORY CARD Slot   .', 'in save game slot' + SLOT + '.'),
    (' found in MEMORY CARD Slot   .', 'in save game slot' + SLOT + '.'),
    ('found in MEMORY CARD Slot   ?', 'in save game slot' + SLOT + '?'),
    ('in MEMORY CARD Slot    is unformatted.', 'Save game slot' + SLOT + 'is not ready.'),
    ('in MEMORY CARD Slot     contains', 'Save game slot' + SLOT + 'contains'),
    ('Memory Card Slot   .', 'Save game slot' + SLOT + '.'),
    ('Memory Card Slot         ', 'Save game slot' + SLOT),
    # first lines of the messages
    ('Select Load file from', 'Select a saved game'),
    ('Select Save file from', 'Select where to save'),
    ('MEMORY CARD', 'SAVE FILE'),
    ('Error Saving to', 'Error saving'),
    ('Saving Game to', 'Saving game'),
    ('Do not remove', 'Do not quit'),
    ('MEMORY CARD (8MB)(for PlayStation 2).', 'the game while saving.'),
    ('Do not turn power off or reset.', ''),
    ('Do not power off or reset.', ''),
    ('Create new God of War file on', 'Create a new save'),
    ('Overwrite God of War file on', 'Overwrite the save'),
    ('No God of War Data found on', 'No saved game found'),
    ('Error loading from', 'Error loading'),
    ('Loading Game from', 'Loading game'),
    ('Load God of War file from', 'Load the saved game'),
    ('Checking for', 'Checking'),
    ('Error formatting', 'Error preparing'),
    ('Formatting Complete.', 'Save file ready.'),
    ('Formatting', 'Preparing'),
    ('Error reading from', 'Error reading'),
    ('Insufficient free space for new save game file', 'Not enough free space for a new save file'),
    ('Do you want to format?', 'Prepare it now?'),
    ('No MEMORY CARD (8MB)(for PlayStation 2)', 'No save file found'),
    ('MEMORY CARD (8mb)(for PlayStation 2)', 'Save file found'),
    # controller prompt (a gamepad or the keyboard on PC)
    ('Please insert a DUALSHOCK   2', 'Please connect a controller'),
    ('analog controller into', 'to continue.'),
    ('controller port 1 to continue.', ''),
    ('The DUALSHOCK   2', 'The controller'),
    ('Please insert a DualShock 2 Controller.  ', 'Please connect a controller.'),
]

# Italian (the European disc's R_SHELLI/R_PERMI chunks, lang_pack.py): the memory card becomes the save file
# ("file di salvataggio") and its slot the save slot ("slot di salvataggio"); the slot number gap follows "CARD".
LINES_IT = [
    # controller prompts
    ('Inserire un controller analogico', 'Collega un controller'),
    ("(DUALSHOCK  2) nell'ingresso", 'per continuare.'),
    ('controller 1 per continuare. ', ''),
    ('Il controller analogico', 'Il controller'),
    ('(DUALSHOCK  2) è stato rimosso.       ', 'è stato scollegato.'),
    ('(DUALSHOCK  2) è stato rimosso.    ', 'è stato scollegato.'),
    ('Inserire un controller', 'Collega un controller.'),
    ('analogico (DUALSHOCK  2).  ', ''),
    # card names and slots
    ('salvataggio sulla memory card (PS2)', 'file di salvataggio.'),
    ("inserita nell'ingresso MEMORY CARD 1 e 2.", ''),
    ("La memory card (PS2) nell'ingresso ", ''),
    ("La memory card (PS2) nell'ingresso", ''),
    ('MEMORY CARD     contiene tesori', 'Lo slot' + SLOT + 'contiene tesori'),
    ('MEMORY CARD    non è formattata.', 'Lo slot' + SLOT + 'non è pronto.'),
    ("inserita nell'ingresso MEMORY CARD   .", 'nello slot di salvataggio' + SLOT + '.'),
    ("inserita nell'ingresso MEMORY CARD   ?", 'nello slot di salvataggio' + SLOT + '?'),
    ("dall'ingresso MEMORY CARD   .         ", 'dallo slot di salvataggio' + SLOT + '.'),
    ("dall'ingresso MEMORY CARD   .", 'dallo slot di salvataggio' + SLOT + '.'),
    ("nell'ingresso MEMORY CARD   .", 'nello slot di salvataggio' + SLOT + '.'),
    ("nell'ingresso MEMORY CARD   . ", 'dallo slot di salvataggio' + SLOT + '.'),
    ("nell'ingresso MEMORY CARD    .", 'nello slot di salvataggio' + SLOT + '.'),
    ("nell'ingresso MEMORY CARD   ?", 'dallo slot di salvataggio' + SLOT + '?'),
    ("nell'ingresso MEMORY CARD    ", 'nello slot' + SLOT),
    ('MEMORY CARD    in corso...', 'nello slot' + SLOT + '...'),
    ('Ingresso', 'Slot di'),
    ('MEMORY CARD 1', 'salvataggio 1'),
    ('MEMORY CARD 2', 'salvataggio 2'),
    ('sulla memory card (PS2) inserita', ''),
    ('dalla memory card (PS2) inserita', ''),
    ('memory card (PS2) inserita', ''),
    ('della memory card (PS2) inserita', 'del file di salvataggio'),
    ("inserita nell'ingresso", ''),
    ('God of War sulla memory card (PS2)', 'God of War'),
    # first lines of the messages
    ('Salvataggio sulla memory card (PS2)', 'Salvataggio in corso'),
    ('Non rimuovere', 'Non chiudere'),
    ('la memory card (PS2).', 'il gioco.'),
    ('la memory card (PS2). ', 'il gioco.'),
    ('Non resettare/spegnere la console!', ''),
    ('Caricamento del salvataggio dalla', 'Caricamento del salvataggio'),
    ('Controllo della memory card (PS2)', 'Controllo in corso'),
    ('Errore durante la formattazione', 'Errore durante la preparazione'),
    ('Formattazione completata.', 'Salvataggio pronto.'),
    ('Formattazione della', 'Preparazione...'),
    ('Formattarla?', 'Prepararlo?'),
    ('Nessuna memory card (PS2)', 'Nessun salvataggio'),
    ('Memory card (PS2) trovata', 'Salvataggio trovato'),
]

# French: the memory card becomes the save ("sauvegarde") and its slot the "emplacement"; the slot number gap follows
# "N°". The tables of fr/de/es match lines without their trailing spaces (gow_text.rewrite loose). New lines use only
# glyphs both chunks have (no â, ê in French; no ß in German; no ñ in Spanish).
LINES_FR = [
    # controller prompts
    ('Insérez une manette analogique', 'Insérez une manette'),
    ('(DUALSHOCK  2) dans le port de', 'pour continuer.'),
    ('manette N°1 pour continuer.', ''),
    ('La manette analogique', 'La manette'),
    (' (DUALSHOCK  2) a été retirée.', 'a été débranchée.'),
    ('analogique (DUALSHOCK  2).', 'compatible.'),
    # card names and slots
    ('sur la memory card (PS2)', ''),
    ('depuis la memory card (PS2)', ''),
    ('de la memory card (PS2)', ''),
    ('dans la fente pour MEMORY CARD N°1 et 2', ''),
    ('dans la fente pour MEMORY CARD N°1 et 2.', 'dans les emplacements N°1 et 2.'),
    ('La memory card (PS2) dans la fente', ''),
    ('pour MEMORY CARD N°    contient des', "L'emplacement N°" + SLOT + 'contient des'),
    ('dans la fente pour MEMORY CARD N°   .', "dans l'emplacement N°" + SLOT + '.'),
    ('dans la fente pour MEMORY CARD N°    ?', "dans l'emplacement N°" + SLOT + '?'),
    ('dans la fente pour MEMORY CARD N°', "dans l'emplacement N°" + SLOT),
    ('fente pour MEMORY CARD N°   .', "l'emplacement N°" + SLOT + '.'),
    ('Sélectionner fichier à charger dans la', 'Sélectionner fichier à charger dans'),
    ('Fente pour', 'Sauvegarde'),
    ('MEMORY CARD N°1', 'N°1'),
    ('MEMORY CARD N°2', 'N°2'),
    ('Memory card (PS2) dans la fente pour', ''),
    ('MEMORY CARD N°    non formatée.', "L'emplacement N°" + SLOT + 'est vide.'),
    ('Formater ?', 'Le créer ?'),
    ('Pas de memory card (PS2)', 'Pas de sauvegarde'),
    ('Memory card (PS2)', 'Sauvegarde'),
    # first lines of the messages
    ('Ne pas retirer', 'Ne pas quitter'),
    ('la memory card (PS2).', 'la partie.'),  # no j in the French menu font
    ('Ne pas éteindre ou redémarrer.', ''),
    ('Erreur lors du formatage', 'Erreur de création'),
    ('Formatage terminé.', 'Création terminée.'),
    ('Formatage', 'Création'),
]

# German: the memory card becomes the save ("Spielstand") and its slot "Slot"; the slot number gap follows
# "Steckplatz". The line before a slot line keeps its preposition ("Speichert Spiel auf / Slot 1.").
LINES_DE = [
    # controller prompts
    ('Analog Controller (DUALSHOCK  2)', 'Controller'),
    ('an Controller-Anschluss 1 anschl.', 'verbinden.'),
    ('Der Analog Controller', 'Der Controller'),
    ('(DUALSHOCK  2) wurde entfernt.', 'wurde getrennt.'),
    ('Analog Controller', 'Controller'),
    ('(DUALSHOCK  2) anschl.', 'verbinden.'),
    # card names and slots
    ('Speicherdatei auf Memory Card (PS2)', 'Speicherdatei auf'),
    ('in MEMORY CARD-Steckplatz 1 und 2.', 'Slot 1 und 2.'),
    ('Auf Memory Card (PS2) in MEMORY CARD-', 'In'),
    ('Steckplatz     sind freigesch. Schätze', 'Slot' + SLOT + 'sind freigesch. Schätze'),
    ('in MEMORY CARD-Steckplatz   .', 'Slot' + SLOT + '.'),
    ('in MEMORY CARD-Steckplatz', 'Slot' + SLOT),
    ('Steckplatz    auswählen.', 'Slot' + SLOT + 'auswählen.'),
    ('Ladedatei von MEMORY CARD-', 'Ladedatei von'),
    ('Speicherdatei von MEMORY CARD-', 'Speicherdatei auf'),
    ('MEMORY CARD-', 'Spielstand'),
    ('Steckplatz 1', 'Slot 1'),
    ('Steckplatz 2', 'Slot 2'),
    ('Memory Card (PS2)', ''),
    ('Memory Card (PS2) in MEMORY CARD-', ''),
    ('Steckplatz    erstellen?', 'Slot' + SLOT + 'erstellen?'),
    ('Steckplatz    überschreiben?', 'Slot' + SLOT + 'überschreiben?'),
    (' Steckplatz    gefunden.', 'Slot' + SLOT + 'gefunden.'),
    ('Steckplatz    gefunden. .', 'Slot' + SLOT + 'gefunden.'),
    ('Steckplatz    laden?', 'Slot' + SLOT + 'laden?'),
    ('Steckplatz    ist nicht formatiert.', 'Slot' + SLOT + 'ist nicht angelegt.'),
    ('Möchten Sie sie formatieren?', 'Möchten Sie ihn anlegen?'),
    ('Keine Memory Card (PS2) in', 'Kein Spielstand in'),
    ('MEMORY CARD-Steckplatz 1 und 2 gefunden.', 'Slot 1 und 2 gefunden.'),
    ('Memory Card (PS2) in', 'Spielstand in'),
    ('MEMORY CARD-Steckplatz    gefunden.', 'Slot' + SLOT + 'gefunden.'),
    # first lines of the messages
    ('nicht entfernen.', 'Nicht beenden.'),
    ('Nicht ausschalten oder zurücksetzen.', ''),
    ('Suche nach', 'Prüfe'),
    ('Fehler beim Formatieren der', 'Fehler beim Anlegen von'),
    ('Formatierung abgeschlossen.', 'Spielstand angelegt.'),
    ('Formatiert', 'Erstelle'),
]

# Spanish: the memory card becomes the saved game ("partida guardada") and its slot the "ranura"; the slot number
# gap follows "CARD".
LINES_ES = [
    # controller prompts
    ('Inserta un mando analógico', 'Conecta un mando'),
    ('(DUALSHOCK  2) en el', 'para continuar.'),
    (' puerto de mando 1 para continuar.', ''),
    (' Se ha extraído', 'Se desconectó'),
    ('el mando analógico (DUALSHOCK  2).', 'el mando.'),
    (' Inserta un mando', 'Conecta un mando'),
    ('Inserta un mando', 'Conecta un mando'),
    ('analógico (DUALSHOCK  2)', 'compatible.'),
    # card names and slots
    ('nuevo archivo en la memory card (PS2)', 'nuevo archivo de guardado en'),
    ('de la ranura para MEMORY CARD 1 y 2.', 'las ranuras 1 y 2.'),
    ('La memory card (PS2) de la ranura para', ''),
    ('MEMORY CARD     contiene tesoros', 'La ranura' + SLOT + 'contiene tesoros'),
    ('de la ranura para MEMORY CARD   .', 'la ranura' + SLOT + '.'),
    ('de la ranura para MEMORY CARD   ?', 'la ranura' + SLOT + '?'),
    ('de la ranura para MEMORY CARD', 'la ranura' + SLOT),
    ('la ranura para MEMORY CARD   .', 'la ranura' + SLOT + '.'),
    ('la ranura para MEMORY CARD   ?', 'la ranura' + SLOT + '?'),
    (' la ranura para MEMORY CARD   .', 'la ranura' + SLOT + '.'),
    (' la ranura para MEMORY CARD   ?', 'la ranura' + SLOT + '?'),
    ('en la ranura para MEMORY CARD   .', 'en la ranura' + SLOT + '.'),
    (' en la ranura para MEMORY CARD   .', 'en la ranura' + SLOT + '.'),
    ('Ranura para', 'Ranura de'),
    ('MEMORY CARD 1', 'guardado 1'),
    ('MEMORY CARD 2', 'guardado 2'),
    ('memory card (PS2) de', ''),
    ('memory card (PS2)', ''),
    ('en la memory card (PS2) de', 'en'),
    ('War en la memory card (PS2) de', 'War en'),
    ('de la memory card (PS2) de', 'de'),
    ('La memory card (PS2)', ''),
    ('de la ranura para', ''),
    ('MEMORY CARD    no tiene formato.', 'La ranura' + SLOT + 'no está preparada.'),
    ('¿Quieres formatearla?', '¿Quieres prepararla?'),
    ('No hay ninguna memory card (PS2)', 'No hay ninguna partida guardada'),
    ('en la ranura para MEMORY CARD 1 ó 2.', 'en las ranuras 1 o 2.'),
    ('Se ha encontrado una memory card (PS2)', 'Se ha encontrado una partida'),
    # first lines of the messages
    ('Error al cargar de la', 'Error al cargar de'),
    ('Error al guardar en la', 'Error al guardar en'),
    ('Guardando partida en la', 'Guardando partida en'),
    ('Error al leer la', 'Error al leer'),
    ('Comprobando la', 'Comprobando'),
    ('Error al formatear la', 'Error al preparar'),
    ('Formateado realizado con éxito.', 'Ranura preparada.'),
    ('Formateando la', 'Preparando'),
    ('No extraigas la', 'No cierres'),
    ('memory card (PS2).', 'el juego.'),
    ('No apagues ni reinicies la consola.', ''),
]

# per text language: (table, word the slot number gap follows, match without trailing spaces); the European
# languages' chunks also bring their accented glyphs (gow_text.EUROPEAN)
TABLES = {'en': (LINES, 'Slot', False), 'it': (LINES_IT, 'CARD', False), 'fr': (LINES_FR, 'N°', True),
          'de': (LINES_DE, 'Steckplatz', True), 'es': (LINES_ES, 'CARD', True)}


def apply(wad, language='en'):
    """the WAD with the port's wording in <language> (R_PERM.WAD and R_SHELL.WAD, the USA ones for en and the renamed
    European ones otherwise; other WADs come back unchanged). A language without a table keeps the card wording."""
    if language not in TABLES:
        return wad
    lines, anchor, loose = TABLES[language]
    new, _ = gow_text.rewrite_wad(wad, {'FLP_HUD': lines, 'FLP_Shell': lines}, gow_text.EUROPEAN.get(language),
                                  anchor, loose)
    return new


if __name__ == '__main__':
    # usage: gow_wording.py [--lang it] <WAD>...   reports the lines each table entry changed
    args = sys.argv[1:]
    language = 'en'
    if args[:1] == ['--lang']:
        language, args = args[1], args[2:]
    lines, anchor, loose = TABLES[language]
    for path in args:
        _, report = gow_text.rewrite_wad(open(path, 'rb').read(), {'FLP_HUD': lines, 'FLP_Shell': lines},
                                         gow_text.EUROPEAN.get(language), anchor, loose)
        for chunk, done in report.items():
            print(path, chunk, sum(done.values()), 'lines')
            for old, _ in lines:
                if old not in done:
                    print('   not found:', repr(old))
