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
  - movie narration: the intro (SEMO01) and four bonus movies have per-language audio-only streams (`SEMO01A-E.PSS`,
    `BONUS01B-E`, `BONUS19B-E`, `BONUS20A-E`, `BONUS24B-E`, 5-6 MB each) that the European executable plays next to
    the picture. The USA executable cannot, so those movies stay English.
- **Subtitles**: no disc has subtitles for spoken lines or movies. The message table holds only text the game always
  shows (hints, NPC lines like id 410 "I know who you are Spartan!"), so there is no subtitle option to offer;
  a real one would need new transcriptions, timing and a text renderer.

## How a language reaches the USA game: disc overlays
`tools/lang_pack.py <usa disc> [<eu disc>]` writes `<usa disc>/lang/` (make_pc_folder.ps1 runs it on `data\`):
- `text_<code>.bin`: the USA `R_PERM.WAD` with its message table replaced. Both discs use the same message ids (USA has
  396, the European tables 393: ids 1, 8, 9 are USA-only and stay English), so each USA message takes the European
  translation of the same id. The WAD is a list of records `{u32 tag, u32 size, char name[24], data padded to 16}`
  (tag 24 = a value record without data); only the `msgs_en.txt` record changes size.
- `voice_<code>.bin`: the dubbed files, each under the USA name it replaces.
- `shell.bin`: the USA `R_SHELL.WAD` with the save wording (below).
- `<text>_<voice>.toc` + `.layout`: GODOFWAR.TOC with the replaced names pointing at those files, placed after the
  last disc sector (TOC entries: part 1 = PART2.PAK, sector offset relative to its LSN).

The runtime part is generic (ps2xIOP `disc_source.cpp`, extracted discs only): `PS2X_DISC_OVERLAY=<layout>` (absolute
or relative to the disc folder) is read after `.disc_layout`; its `file` lines replace the entry at the same LSN or
add one, `sectors` sets the disc size, paths are relative to the overlay's folder. The first read of each overlay
file is logged (`[disc] overlay read: ...`). The game itself is unchanged: smpd reads the TOC and the files by sector
as before.

Accents: the European tables are cp1252 text, and the USA executable's fonts have no accented letters (a letter
without a glyph is skipped: "Ascensin"). lang_pack.py folds the translated messages to plain ASCII
(`fold_for_usa_font`: accents dropped, umlauts/ß/œ spelled out, ¿ ¡ removed, cp1252 dashes and quotes made plain);
check: `python3 -I -m unittest discover -s tools/tests`.

Movie voices: the European movies carry 5 ADPCM tracks (private stream 1, sub-stream `FF A1 00 <track>`, SShd type
16, 48 kHz stereo, interleave 32; tracks 0 en, 1 fr, 2 de, 3 it, 4 es, the European executable's table at 0x29CA18).
Their video is PAL, so the USA picture is kept: `lang_pack.py` writes `lang/movies_<code>.bin` (44 dubbed movies, keyed
by the USA movie's audio fingerprint; the intro = the European SEMO01's first 36.2 s + the per-language tail
SEMO01A-E from USA sample 1741265) and the runtime (`MovieVoice`, `PS2X_MOVIE_VOICES=lang/movies_<code>.bin`) decodes
the track and replaces the USA PCM byte for byte while the movie is demuxed; timing is untouched. The macOS launcher
sets it when Voice is not English.

Pre-intro voice: Kratos's line before the intro movie ("The gods of Olympus have abandoned me.") is in the sound
bank `SBK_Title01` of `R_SHELL.WAD`; each European `R_SHELL<letter>.WAD` has it dubbed, so lang_pack writes
`shell_<voice>.bin` (the USA shell with the European bank; menus stay USA) and the overlays for a dubbed voice use it.

Not localized: the menus and the in-game message screens (save, death, pause, upgrade texts) keep the USA art and
English text: their text lives in the Flash chunks with the USA font (below), and the European versions of those
chunks expect the European executable (swapping whole European WADs leaves the title menu empty after Start). 

## Save wording
The port saves to a file (`saves\GodOfWar.sav`), so the PS2 texts about a "MEMORY CARD (8MB)(for PlayStation 2)",
card slots, formatting and the DUALSHOCK 2 are reworded: `tools/gow_wording.py` (the table) through
`tools/gow_text.py` (the format), applied by lang_pack.py to every overlay, English included. Example: "Saving Game
to / MEMORY CARD (8MB)(for PlayStation 2) / in MEMORY CARD Slot 1. / Do not remove / MEMORY CARD (8MB)(for
PlayStation 2). / Do not turn power off or reset." becomes "Saving game / in save game slot 1. / Do not quit / the
game while saving.".

Text format (FLP_HUD in R_PERM.WAD, FLP_Shell in R_SHELL.WAD; `python -I tools/gow_text.py <wad>` lists it):
- static text as SWF-style text records: style byte `0x80|flags`, then (flags 0x08) u16 font + u16 height,
  (0x04) RGBA color, (0x01) s16 x, (0x02) s16 y; a glyph count byte; per glyph `{u16 glyph, u16 advance}`.
- glyphs: letters are linear per font (HUD: 'A' = 26, Shell: 'A' = 25, lowercase right after 'Z', drawn as small
  caps), 0 = space, punctuation and digits measured (`FONTS` in gow_text.py).
- lines are centered through their x; advances depend on the height (measured per height from the chunk itself).
- the slot number is a separate field drawn over the spaces after "Slot": reworded slot lines keep that tail in
  place ("in save game slot" has about the width of "in MEMORY CARD Slot", so the line stays nearly centered).
- the chunk is a serialized structure with pointers, so nothing moves: a new line has at most the old glyph count,
  the rest are zero-width spaces, and x is recomputed to keep it centered.

## Tests (2026-10-18, ext agent/lang Release, muted)
- `PS2X_DISC_OVERLAY=lang/en_en.layout`: savetest saves (129 pages) with the new overwrite/saving/complete screens
  (port/lang/st_en); load screen "Select a saved game / Save game slot 1"; zones 1 and 3 ok at 56-60 fps.
- `fr_fr`: savetest saves; load screen shows the level name from the French table ("L'OLYMPE"); zones 2 and 4 ok at
  60 fps; the log shows `fr_fr.toc`, `text_fr.bin` and `shell.bin` read from the overlay.
- Not seen in a run yet: a dubbed line playing (none of the test walks reaches one; the dubbed files are the same
  VAG format and go through the same TOC mechanism as the text, and `voice_<code>.bin` shows up in the log when one
  is streamed). The intro movie narration stays English (above).
- `tools/savetest.py --elf <disc>/SCUS_973.99` runs on another disc folder (used for test discs).

## Switch
The overlay is in ps2xIOP, shared by both builds. The Switch build needs: the ext change merged into its branch, the
`lang/` folder in the RomFS next to the disc files (port/switch/nsp, make_romfs_links.sh), and
`PS2X_DISC_OVERLAY=lang/<text>_<voice>.layout` in its defaults (`sdmc:/switch/gow/env.txt` overrides). A language
choice on the console would be an `env.txt` line until there is an in-game setting.
