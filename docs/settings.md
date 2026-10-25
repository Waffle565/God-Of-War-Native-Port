# Player settings: languages and save wording

The PC launcher (`God of War.exe`, tools/pc_release/launcher.cpp, docs/build-and-run.md "PC release folder") stores
the player's choices in `settings.ini` next to it: `[Video]` (resolution, fullscreen) and `[Game]`:

| Key | Values | Effect |
|---|---|---|
| `Text` | en fr de it es | language of the in-game text (hints, item and ability descriptions, books, treasures, difficulty names) |
| `Voice` | en fr de it es | language of the dubbed voice lines |

The launcher passes the pair to the game as `PS2X_DISC_OVERLAY=lang\<text>_<voice>.layout` (relative to `data\`).
Other languages are offered only when the overlays exist (they need the European disc, see below); without them
the two boxes are greyed out and English is used.

## What the discs contain (measured 2026-10-18)
- **USA disc** (SCUS-97399, the one the port runs): English only. No language selection in the executable
  (`R_Perm`/`Shell`/`HUD` names without a language letter, one message table `msgs_en.txt`).
- **European disc** (SCES-53133, `God of War (Europe, Australia) (En,Fr,De,Es,It)`, extracted in `disc/`):
  - text in English, French, German, Italian and Spanish: `R_PERM<letter>.WAD` and `R_SHELL<letter>.WAD`, letters
    U F G I S. The executable keeps a language index at 0x29DADC (table `_en F G I J K S U`, message table
    `msgs%s.txt`) and sends it to SMPD_IOP (RPC fn 0x1D).
  - voices dubbed in four languages: next to each dubbed English `X.VAG`/`X.VPK` the disc has `X.VA1-4`/`X.VP1-4`
    (102 VAG and 7 VPK names; the other 176 VAG are effects/grunts and stay English). SMPD_IOP's file-type table
    (VAG VPK WAD M2V PSS M2W PSW PAK TOC TXT VA1 VP1 ... VA4 VP4) picks them. Languages identified by speech
    recognition (faster-whisper, P00MI01 narration): **VA1 French, VA2 German, VA3 Italian, VA4 Spanish**.
  - intro narration: the European intro movie `SEMO01.PSS` (4:3, PS2 ADPCM audio) has five audio tracks, one per
    language (payload header `ff a1 00 <track>`: 0 en, 1 fr, 2 de, 3 it, 4 es; packets in any order), 36.2 s each;
    its last sentence ("But it had not always been this way...") is a separate PCM clip per language
    (`SEMO01A-E.PSS`, A = en, at 36.28 s of the USA audio). The European bonus movies with letters
    (`BONUS01/19/20/24 B-E`) keep the English speech (subtitled pictures) and run at PAL 25 pictures/s, which the USA
    player does not finish: not used.
- **Subtitles**: no disc has subtitles for spoken lines or movies. The message table holds only text the game always
  shows (hints, NPC lines like id 410 "I know who you are Spartan!"), so there is no subtitle option to offer;
  a real one would need new transcriptions, timing and a text renderer.

## Other regional discs (measured 2026-10-19)
- **Asia** (SCUS-97399 too): byte for byte the USA disc, nothing to take.
- **Japan** (SLPM-67010, `disc-jpn/`): a full **Japanese dub** (speech recognition: narrator P00MI01 and NPC lines
  Japanese, p 0.96-0.99) under the USA file names: 93 voice VAGs, the movies (PSS/PSW, Japanese audio + burned-in
  subtitles; same MPEG layout and 640x480 video as the USA ones, longer pictures). Text in `R_PERMJ`/`R_SHELLJ`.
- **Korea** (SCKA-30002, `disc-kor/`): Korean text (`msgsK.txt`, UTF-8), English voices (9 VAGs differ), subtitled
  movies.
- Japanese and Korean **text** are not offered: those discs replace the font and HUD art (`GOWJ_*`/`HUDJ*`,
  `GOWK_*`/`HUDK*` instead of `GodOfWar_0`/`HUD*`) and draw multi-byte text with code the USA executable lacks.
  The Japanese dub is offered as Voices "Japanese" (lang_pack.py `--ja`).

## How a language reaches the USA game: disc overlays
`tools/lang_pack.py <usa disc> [<eu disc>] [--ja <jpn disc>]` writes `<usa disc>/lang/` (make_pc_folder.ps1 runs it
on `data\`, with `disc\` and `disc-jpn\` when extracted):
- `text_<code>.bin`: `R_PERM.WAD` (in-game screens and HUD, message table): the USA one for `en`, else the European
  `R_PERM<letter>.WAD` renamed (below). Its message table gets the USA order and the USA-only ids (USA has 396, the
  European tables 393: ids 1, 8, 9 stay English). The WAD is a list of records `{u32 tag, u32 size, char name[24],
  data padded to 16}` (tag 24 = a value record without data); only the `msgs_en.txt` record changes size.
- `shell_<code>.bin`: `R_SHELL.WAD` (the menus): the USA one for `en`, else the European `R_SHELL<letter>.WAD`
  renamed. Both with the save wording (below).
- `voice_<code>/<NAME>`: the dubbed files, each under the USA name it replaces. For `ja` a movie's `.PSS` also
  reads its Japanese widescreen `.PSW` (the port is always 16:9, so only the `.PSW` is played).
- `<text>_<voice>.toc` + `.layout`: GODOFWAR.TOC with the replaced names at their new places (TOC entries: part
  0/1 = PART1/PART2.PAK, sector offset relative to its LSN) and one `file` line per replacement.

Where the replacements go (`place()`): the game addresses a PAK with **32-bit byte offsets**, so a file must start
within 4 GiB of its PAK, and both PAKs nearly fill that (PART2 ends 16k sectors below the limit). Until 2026-10-19
the overlays appended the files after the last disc sector, which left the Spanish voices (and any larger pack)
unreachable. Now each combination puts its replacements into the space of the English files they replace (plus
files it no longer uses, the `.PSS` of aliased movies) and the unused room, best fit, adjacent slots merged.
Measured with movies (tools/movies.py SEMO01): a movie moved inside its own PAK plays; moved past the end of the
original disc it does not. So streamed files (PSS PSW VAG VPK) keep their PAK and stay inside the disc; text WADs may
go anywhere below the limit. Measured 2026-10-19: intro movie SEMO01 plays with en_en, fr_es and en_ja (Japanese
narration confirmed by speech recognition). A movie read from a freshly written overlay file often crashed the game at
the movie start (JALR to 0 at 0x161c68, a virtual call in the main loop): the file's first open was slow (likely the
antivirus scan of a new file) and fell in the movie start. disc_source.cpp now opens every overlay file when it
loads the overlay, at boot; with fresh files 6 of 6 movie runs played since.

The runtime part is generic (ps2xIOP `disc_source.cpp`, extracted discs only): `PS2X_DISC_OVERLAY=<layout>` (absolute
or relative to the disc folder) is read after `.disc_layout`; its `file` lines replace the entry at the same LSN when
they cover it (the TOC) or are added, `sectors` sets the disc size, paths are relative to the overlay's folder. An
overlay file may lie inside a disc file (a PAK): the lookup takes the innermost file holding the sector, entries at
the same LSN keep their order (stable sort), and open files are cached by path (an overlay file at a PAK's first
sector shares its LSN). The first read of each overlay
file is logged (`[disc] overlay read: ...`). The game itself is unchanged: smpd reads the TOC and the files by sector
as before.

Intro narration (`tools/gow_movie.py`, used by lang_pack.py for fr de it es): the USA widescreen intro
`SEMO01.PSW` keeps its picture, packets, sizes and timestamps; only its PCM samples change, to the European
language's track (ADPCM decoded) from the start plus its clip where the English clip lies in the USA audio
(cross-correlation). Its `.PSS` reads the same file. Checked 2026-10-19 with tools/movies.py + speech recognition:
it_it Italian, de_de German, fr_fr French narration, pictures and sound ok.

Menus and in-game screens (2026-10-19): the European `R_SHELL<l>`/`R_PERM<l>.WAD` hold the menus (Flash chunk
`FLP_Shell<l>`), the in-game screens and HUD (`FLP_HUD<l>`) with their own font (`GFX_GOWE`, accented glyphs) in
each language. The European executable looks their objects up with the language letter (strings `R_ShellX`,
`ShellX`, `HUDX`, `msgs%s.txt`; the USA one has `R_Shell`, `Shell`, `HUD`, `msgs_en.txt`), which is why the whole
European WAD left the title menu empty under the USA game. `european_wad()` in lang_pack.py renames every name field
(fixed 24-byte, NUL-padded) of `R_Shell<l>`, `R_Perm<l>`, `Shell<l>`, `HUD<l>` (any case, also the lowercase
`shell<l>` and `Shell<l>_0`) to the USA name and `msgs<l>.txt` to `msgs_en.txt`; it also turns the European save folder
`BESCES-53133GodOfWar` (in R_PERM) into the USA `BASCUS-97399GodOfWar`, so every language shares the saves. With
that the menus, options, load/save screens, pause and HUD are in the chosen language (Italian checked screen by
screen, French, German, Spanish title and load screens). Not localized: the difficulty and date shown for a save
written by the USA game stay English (the level name follows the language), Japanese/Korean (above).

## Save wording
The port saves to a file (`saves\GodOfWar.sav`), so the PS2 texts about a "MEMORY CARD (8MB)(for PlayStation 2)",
card slots, formatting and the DUALSHOCK 2 are reworded: `tools/gow_wording.py` (the table) through
`tools/gow_text.py` (the format), applied by lang_pack.py to every overlay. Example: "Saving Game
to / MEMORY CARD (8MB)(for PlayStation 2) / in MEMORY CARD Slot 1. / Do not remove / MEMORY CARD (8MB)(for
PlayStation 2). / Do not turn power off or reset." becomes "Saving game / in save game slot 1. / Do not quit / the
game while saving.". Tables per text language (`TABLES`: lines, anchor word of the slot gap, loose matching):
- English (USA chunks), exact lines, gap after "Slot".
- Italian: "file di salvataggio", "slot di salvataggio", "Collega un controller"; gap after "CARD".
- French: "sauvegarde", "emplacement N°1", "Ne pas quitter la partie.", formatting = "Création"; gap after "N°".
- German: "Spielstand", "Slot 1" (the line before keeps its preposition: "Speichert Spiel auf / Slot 1."),
  formatting = "anlegen"; gap after "Steckplatz".
- Spanish: "partida guardada", "la ranura 1", "Conecta un mando", formatting = "preparar"; gap after "CARD".

The French, German and Spanish tables match lines without their trailing spaces (`loose`): their chunks repeat a line
with different padding. A new line may only use glyphs the chunk has drawn somewhere (their advance is measured from
the chunk): no â/ê in French (each is in one chunk only), no "j" in the French menu font ("la partie", not "le jeu"),
no ß in German, no ñ in Spanish. Checked 2026-10-19: every table entry matches, no line of the built `text_<code>.bin`
/ `shell_<code>.bin` names a memory card, DUALSHOCK, PS2 or the console (en, it, fr, de, es); English and Italian
files byte-identical to before the French/German/Spanish tables.

Text format (FLP_HUD in R_PERM.WAD, FLP_Shell in R_SHELL.WAD; `python -I tools/gow_text.py <wad>` lists it):
- static text as SWF-style text records: style byte `0x80|flags`, then (flags 0x08) u16 font + u16 height,
  (0x04) RGBA color, (0x01) s16 x, (0x02) s16 y; a glyph count byte; per glyph `{u16 glyph, u16 advance}`.
- glyphs: letters are linear per font (HUD: 'A' = 26, Shell: 'A' = 25, lowercase right after 'Z', drawn as small
  caps), 0 = space, punctuation and digits measured (`FONTS` in gow_text.py). The European chunks number letters,
  digits and punctuation the same and add accented glyphs above 'z', numbered per language and chunk (Italian
  Shell: à 105, è 109, ì 113; Italian HUD: / 11, à 106, è 110, ì 114, ò 119, ù 123; French HUD: è 111, é 112;
  German HUD: ä 108, ö 122, ü 126, ß 105; Spanish HUD: á 107, é 111, í 115, ñ 118, ó 120, ú 124, ¿ 82):
  `EUROPEAN[language][chunk]`, named from each chunk's own words. Only named glyphs can be written.
- lines are centered through their x; advances depend on the height (measured per height from the chunk itself).
- the slot number is a separate field drawn over the spaces after "Slot" (USA), "MEMORY CARD" (Italian,
  Spanish), "N°" (French) or "Steckplatz" (German):
  reworded slot lines keep that tail in place (`{SLOT}`, anchor word per table).
- the chunk is a serialized structure with pointers, so nothing moves: a new line has at most the old glyph count,
  the rest are zero-width spaces, and x is recomputed to keep it centered.

## Tests (2026-10-18, ext agent/lang Release, muted; overlay placement 2026-10-19 above)
- `PS2X_DISC_OVERLAY=lang/en_en.layout`: savetest saves (129 pages) with the new overwrite/saving/complete screens
  (port/lang/st_en); load screen "Select a saved game / Save game slot 1"; zones 1 and 3 ok at 56-60 fps.
- `fr_fr`: savetest saves; load screen shows the level name from the French table ("L'OLYMPE"); zones 2 and 4 ok at
  60 fps; the log shows `fr_fr.toc`, `text_fr.bin` and `shell.bin` read from the overlay.
- Not seen in a run yet: a dubbed line playing (none of the test walks reaches one; the dubbed files are the same
  VAG format and go through the same TOC mechanism as the text, and `voice_<code>.bin` shows up in the log when one
  is streamed).
- `tools/savetest.py --elf <disc>/SCUS_973.99` runs on another disc folder (used for test discs).

## Button icons (tools/gow_pad_icons.py, 2026-10-26, sw-icons; WIP)
Where the PS2 symbols live: the button prompts are Flash sprites in FLP_HUD (R_PERM.WAD) and FLP_Shell (R_SHELL.WAD):
XButton, CircleButton, SquareButton, TriangleButton, their *Greek variants (the QTE coin above enemies), XButtonPress,
L1/L2/R1/R2Button, Start/SelectButton, Joystick, Dpad. The message table writes them as `[XButton]` tags, and the
menu legends are the sprite followed by the text "= Select" (the font has no button glyph). A face button is a body
(dark disc: mesh part with TXR_HUD006, or the coin TXR_HUD183) plus an untextured flat-colour vector mesh (the
symbol). L1/R1/L2/R2/L3/R3 are a body plus a text label. So texture hashing at GS upload cannot work (the symbols have
no texture).
Swap (data only, no runtime cost): `gow_pad_icons.apply(wad)` adds GFX_/PAL_/TXR_PAD_A/B/X/Y (64x64 8-bit, drawn by
the tool: round strokes, white with a dark outline) in front of the Flash chunk's mesh group, points each symbol's mesh
reference at a centred full-UV quad part with that texture, appends one transformation per (old transformation,
symbol) so the quad covers the button body, and rewrites the labels L1->L, R1->R, L2->ZL, R2->ZR, L3->LS, R3->RS.
Symbols are found by name (Cross = A, Circle = B, Square = X, Triangle = Y, docs/switch.md "Controls"), then by
colour in every other sprite with a body (menu legends ButtonBack, press animations, controls screen). FLP format from
mogaika/god_of_war_browser (pack/wad/flp); the parser reaches the string table exactly (checked in all five
languages, HUD and Shell). tools/lang_pack.py now also writes `text_<code>_nx.bin`, `shell_<code>_nx.bin` and
`<text>_<voice>_nx.toc/.layout` (the old files stay byte-identical); `--out <dir>` writes elsewhere.
Checked on the PC (dist GodOfWar.exe, en_en_nx overlay, background window): title "A = Select", Power Up screen
"Press A", "A = Continue", "A = Power Up", "Y = Back", L/R labels (port/switch/shots/pad_icons/). Not seen yet: a QTE
coin in a fight (alignment checked numerically: the quad box equals the coin box), the other languages in game.
Not done: Start/Select bodies (no label to change), message text "L3 button and R3 button", FMV pictures.
Next steps: (1) Switch: in ps2_switch_runtime.cpp pickLanguage() try `lang/<text>_<voice>_nx.layout` first unless
env.txt sets `GOW_PAD_ICONS=ps` (private tree /root/src-icons = copy of /root/src-nv2 with both patches), build,
make_update_to.sh; (2) put the _nx files on the console (sdmc:/switch/gow/lang or a romfs rebuild, LANG_SRC); (3) ask
main for console time, screenshot a menu + a QTE prompt, bench the ship fight (baseline update_swfix3 ~43.5 fps),
install as default only if fps is not lower; (4) refresh dist/data/lang with make_pc_folder.ps1.

## In-game settings panel

F1, the pad's guide button or Select+Start open a settings panel over the running game (ext
`ps2xRuntime/src/lib/settings_menu.cpp`, drawn by the runner with raylib after the game picture). While it is open
the game reads a released pad (`ps2_pad.cpp`), and after it closes the pad stays released until the closing key is let
go, so Esc/Enter (also the game's circle and Start) do not reach the game.

| Row | Effect |
|---|---|
| Resolution | 16:9 sizes up to the monitor, the monitor's own and the current one; applied at once (in fullscreen the monitor mode follows) |
| Fullscreen | on/off at once (same as F11) |
| Text, Voices | the languages of the folder's overlays; take effect after "Restart" |
| Restart to apply languages | only when a language changed: the runner exits with code 3 and the launcher starts it again with the new settings (unsaved progress is lost) |
| Resume, Quit game | close the panel; quit the game |

Every change is written to `settings.ini` at once. The launcher tells the runner where through the environment:
`GOW_SETTINGS` (the ini), `GOW_TEXT`/`GOW_VOICE` (this run's languages), `GOW_TEXTS`/`GOW_VOICES` (codes with overlays, comma
separated). Without `GOW_SETTINGS` (play.cmd, test runs) the panel works but saves nothing. Keys are read as press
edges of the held state once per host frame: raylib's `IsKeyPressed` missed short presses there.
Tested 2026-10-18 (dist folder, muted): panel opens, Text English -> Français + Restart starts the game again with
`Text=fr`, Esc closes without reaching the game's menu.

## Switch
The overlay code is ps2xIOP's (`disc_source.cpp`, ext e53cfc1), shared by both builds; the Switch build tree
`/root/src-nv2` carries it plus two Switch fixes (`port/switch/lang_switch.patch`, originals there as `*.pre_lang`):
- a device path (`sdmc:/...`, `romfs:/...`) counts as absolute (it has no root directory, so `std::filesystem`'s
  `operator/` appended it to the disc folder), both for `PS2X_DISC_OVERLAY` and for the overlay's file entries;
- the language choice at start (`ps2_switch_runtime.cpp` `pickLanguage`, after `env.txt`): `GOW_TEXT` / `GOW_VOICE`
  from `sdmc:/switch/gow/env.txt` (en fr de it es; voice also ja; voice defaults to the text language), else the
  console's system language (`setGetSystemLanguage` + `setMakeLanguage`: Italian, French incl. Canadian, German,
  Spanish incl. Latin American -> that language, anything else English). It takes `lang/<text>_<voice>.layout`
  next to the boot disc (`romfs:/disc-usa/lang/`, linked by make_romfs_links.sh), else
  `sdmc:/switch/gow/lang/` (`tools/switch_push_lang.sh` copies it there, for an NSP installed before the packs; in
  use since 2026-10-25). Source: the PC folder's current pack (`dist/God of War (PC)/data/lang`, 2026-10-19) without
  the Japanese dub (3.4 GB; `*_ja` combinations are missing, so voice ja falls back to the plain disc): ~200 MB,
  500 files. `disc-usa/lang` is an older pack (10-18: one `shell.bin`, voices in one `voice_<l>.bin`; its menus
  stayed English on the console), and logs `[switch] language: system it, text it, voice it -> ...`. Without a layout: plain USA disc
  (English). Console bench 2026-10-25 (ship fight, default clocks): it_it 43.5 fps = English reference the same
  hour (spd5, 43.5); the overlay costs nothing measurable. `PS2X_DISC_OVERLAY=` in env.txt overrides everything.
- 2026-10-25 (sw-fix): the user saw English on the console default: the console did not hold the default at all
  (an unknown exefs without the language code, no env.txt, left by a bench / test run). The language code itself
  works (`[switch] language: system it, text it, voice it -> sdmc:/switch/gow/lang/it_it.layout`, Italian title,
  port/switch/shots/swfix/t1/b_01.jpg). `tools/switch_bench.sh` now ends with `tools/switch_default.sh`
  (`KEEP_BENCH=1` to keep the bench package).

Menus checked 2026-10-19 (dist runner, muted, it_it): title menu "Nuova partita"/"Carica", difficulty "Seleziona
difficoltà", load screen "Nessun dato salvato di God of War / nello slot di salvataggio 1." (before the save folder
rename) and the save list; tools/savetest.py in Italian: 129 pages written into BASCUS-97399GodOfWar, screens
"Selezionare file su cui salvare / nello slot di salvataggio 1.", "Sovrascrivere il salvataggio di God of War nello
slot di salvataggio 1?", "Salvataggio in corso / nello slot 1... / Non chiudere / il gioco.", "Salvataggio
completato.". fr_fr, de_de, es_es: title, load and save list in their language. English output is byte-identical to
before (shell_en.bin = the former shell.bin).
