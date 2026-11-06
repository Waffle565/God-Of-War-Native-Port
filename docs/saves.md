# Saves on the Switch: one memory card per user

## Where the saves are
- The game saves to a PS2 memory card image (8 MB, PCSX2 `.ps2` layout with ECC; docs/memory-card.md), read and
  written by the native card service through `GOW_MC0`.
- Switch build (from update_dips40s, sw-saves 2026-11-06): **`sdmc:/switch/gow/users/<uid>/play.mcd`**, one card per
  Switch account. `<uid>` = the account's AccountUid, its two 64-bit words as 32 lower-case hex digits
  (`uid[0]` then `uid[1]`). Next to it `user.txt` holds the account's nickname (rewritten at every start), so a look at
  the SD card tells which folder is whose.
- Before update_dips40s every account used the same `sdmc:/switch/gow/play.mcd`, so all profiles shared the four save
  slots. That file is no longer read or written; it stays on the SD card as it was (the pre-split backup).

## How the account is chosen (ps2_switch_runtime.cpp `pickSaveCard`, called from `userAppInit`)
1. `GOW_MC0` set in env.txt: that file, no account lookup (test runs: the boss cards, `boss.mcd`).
2. The account the system picked at launch: the NACP's StartupUserAccount makes HOME ask "who uses the software?",
   and `accountGetPreselectedUser` returns that user.
3. No preselected user (an NRO started from hbmenu): the user selector applet (`pselShowUserSelector`).
4. Nobody picked (selector cancelled or failed): `users/none/play.mcd`, never the old shared file.
- The log lists every account (`[switch] account <uid> '<nickname>'`) and the card chosen (`[switch] saves: ...`).
- A new account has no card file: the load screen says "no God of War saved data in save slot 1" (checked on the
  console as Claude, 2026-11-06); the card is formatted and the file created at the first save (memory_card.cpp
  creates the file at the first write). The folder is created at start; if that fails the log says
  so and saving fails with the game's card error message instead of writing anywhere else.
- Write errors: the card image stays whole in memory; a failed open or write of the file is logged
  (`[SIO2] cannot open the memory card ... / write ... failed`), the file keeps its previous bytes for those pages.

## Save slots inside the card (read from the cards; tools/mcd_slots.py)
- Directory `BASCUS-97399GodOfWar/` (every language uses the USA name, docs/settings.md): `data0.bin` .. `data4.bin`
  (81920 bytes each, all five created with the first save), `static.ico`, `icon.sys` and the 68-byte index file
  `BASCUS-97399GodOfWar`.
- Index: four 16-byte slot entries (slot k at +16*(k-1)): u32 data file number, u32 magic 0xCAFEBAD1, f32 play time in
  seconds, u16 (progress-like, grows with play time), u16 1. An empty slot is `ff ff ff ff` + 12 zero bytes. A u32 at
  +64 of unknown meaning (0 or 1 on the cards seen; not the newest save's data file), kept as it is by the tool. One data file more than slots: a save is written
  to a free data file and then the index switches over.
- Data file: magic 0xCAFEBAD1, zone name at +8, previous zone at +0x13, Kratos's transform at +0x1E.
- The load screen lists the saves newest first; the save screen shows them by slot number.

## Moving existing saves to accounts (one time, on the PC)
`python -I tools/mcd_slots.py list CARD` shows the slots (data file, zone, play time, save date).
`python -I tools/mcd_slots.py keep CARD OUT --slots 1,2 [--front]` writes a copy whose index lists only those slots
(the others empty, as the game writes them; `--front` moves them up to slot 1, 2, ...). Only the index file's page
changes (ECC recomputed and checked against the card's own ECC first); the data files stay, unreferenced ones are free
space the game reuses. It refuses a card with an interrupted write pending (backup block markers set). The input
card is never written. The copy is then put in the account's folder (only where no card exists yet).

## The split of 2026-11-06 (sw-saves, user's ownership)
- Backup of the shared card before anything else: port/switch/saves_backup/2026-11-06/play.mcd (local, not in git;
  downloaded twice, both md5 794fd2cf...). Slots: 1 Athn02A 0:36, 2 Athn02C 0:57, 3 empty, 4 Athn01EE 0:21.
- Waffle565 (uid 10000000227b614bd74882e2ec7dd5ae): slots 1 + 2 (md5 060118ba); piggy (100004a3d752c34bd74882e2ec7dd5ae):
  slot 4 moved to slot 1 (`--front`, md5 b27a38cd); copies in saves_backup/2026-11-06/waffle565 and piggy. Roberta,
  spirixe and Claude start empty. Uploaded only where no card existed, md5 checked by download.
- Console check (update_dips40s, logs and shots in port/switch/logs/saves_test): Claude's load screen "no saved data",
  Waffle565's lists its two saves, piggy's its one save in slot 1; no save made. Afterwards the old play.mcd and both
  cards had their md5 unchanged.
