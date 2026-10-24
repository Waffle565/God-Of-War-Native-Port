# PS2 memory card: format facts for the native memory-card service

Goal (docs/native-port.md, standing order step 1): replace the hardware path EE libmc2 -> DBCMAN RPC -> MC2_D.IRX ->
SIO2MAN -> SIO2 chip model -> raw card image with a native service that implements the card file system directly on
the same image file (`GOW_MC0`, PCSX2 `.ps2` format).

## How the game reaches the card today
- MC2_D.IRX registers **no RPC server** (no `sifcmd` import). Imports: sysmem, sysclib, thbase, thsemap, vblank, sio2man,
  secrman, cdvdman, dbcman, sio2d. It is a driver under DBCMAN, like the pad driver DS2U_D: the EE talks to both pad and
  card through DBCMAN's RPC (`0x80001300` family).
- So pad + card become native together (the EE pad calls are already native: gow_overrides.cpp scePad2*).

## Card image (`.ps2`), verified in ref/pcsx2-src/pcsx2/SIO/Memcard/MemoryCardFolder.h / MemoryCardFile.cpp
- Page = 512 data bytes + 16 spare (ECC) bytes = 528 raw bytes; cluster = 2 pages (1024 data bytes); erase block =
  16 pages (8 clusters). 8 MB card: 16384 pages, file size 8,650,752 bytes (0x840000). Raw offset of data byte b of
  page p: p * 528 + b.
- ECC: 12 bytes per page (3 per 128-byte quarter, Hamming; `CalculateECC` in MemoryCardFile.cpp / MemoryCardFolder.cpp),
  rest of the spare area padding. Readers may ignore it; writers must keep it valid for the real hardware path and
  for PCSX2 (compute it like `CalculateECC`).
- Superblock (page 0): magic "Sony PS2 Memory Card Format " (0x00, 28), version (0x1C, 12), page_len u16 (0x28),
  pages_per_cluster u16 (0x2A), pages_per_block u16 (0x2C), clusters_per_card u32 (0x30), alloc_offset u32 (0x34),
  alloc_end u32 (0x38), rootdir_cluster u32 (0x3C, relative to alloc_offset), backup_block1/2 u32 (0x40/0x44),
  ifc_list[32] u32 (0x50), bad_block_list[32] u32 (0xD0), card_type u8 (0x150, 2 = PS2), card_flags u8 (0x151).
- FAT: superblock ifc_list -> indirect FAT clusters (256 u32 each) -> FAT clusters (256 u32 entries, one per allocatable
  cluster). Entry: bit 31 = in use, bits 0-30 = next cluster (relative to alloc_offset); 0x7FFFFFFF = end of chain
  (stored 0xFFFFFFFF). Free cluster: bit 31 clear. Data cluster c is physical cluster alloc_offset + c.
- Directory entry, 512 bytes (2 per cluster): mode u32 (0x00; 0x10 file, 0x20 dir, 0x8000 exists, 0x0400 PS2 save,
  rwx bits 0x1/0x2/0x4; defaults dir 0x8427, file 0x8497), length u32 (0x04; bytes for a file, entry count incl. "." and
  ".." for a directory), created time (0x08, 8 bytes: unused, sec, min, hour, day, month, year u16), cluster u32 (0x10;
  first cluster, 0xFFFFFFFF for an empty file), dirEntry u32 (0x14; in a subdir's "." = its index in the parent),
  modified time (0x18), attr u32 (0x20), name[32] (0x40). Entry 0 = ".", entry 1 = "..".

## The game's card protocol (captured 2026-10-11, PS2X_TRACE_RPC=800013, boot + save load, port/rpc2)
- **The card file system runs on the EE** (libmc2); the IOP side (DBCMAN + MC2_D) only moves raw pages. A native service
  therefore only has to answer raw page commands from the card image; the game keeps doing its own file system.
- DBCMAN main server 0x80001300 (0x90-byte buffers, synchronous): fn 0x80001363 version check; fn 0x80001304
  (words 0x310, 0x30CF40 = an EE buffer); fn 0x80001301 twice = socket creation, buffer `3 2 {2|3} 0 0 "MC2SOCKET"`
  (memory-card sockets on ports 2 and 3).
- MC2 socket server **0x8000131C** (fn 0x8000131C, mode 1 = asynchronous, send/receive 0x2090 bytes, ~270 calls/s).
  Send words: `0, cmd, 0x2080, page, count, 0x200 (page size), 0x10 (spare size), 0x4000`. Counts seen:
  cmd 1 x31354 (read `count` pages from `page`; pages advance by 2 = one cluster), cmd 4 x975 (about once per frame:
  status poll), cmd 6 x8 and cmd 0xC x8 (around the save load), cmd 0xD x1, cmd 3 x1 (all other words 0: probe/identify).
- Still to find: the reply layout of each command (receive buffer after completion: data + spare, status words) and the
  meaning of cmds 3/4/6/0xC/0xD (MC2_D dispatch), plus the write/erase commands (only seen when saving).

## Native card I/O (2026-10-11, ext 6ef7583)
- MC2_D (cmd dispatch 0x1f74; status block 0x4418 / status word 0x4420 / data buffer 0x4498, sent to the EE by DBCMAN
  signal-data) stays the game's own compiled code. Its six routines that talked to the card over SIO2 are replaced by
  native C++ on the card image (ps2xIOP/src/native/iop_native_overrides.cpp, tools/iop/native_overrides.txt):
  read_page 0x0c8c (port, slot, page, size, stack+0x10 = IOP dst) -> 1/0; write_page 0x0fdc (port, slot, page, IOP src,
  stack+0x10 = size); erase_block 0x0a98 (port, slot, first page); get_specs 0x13a8 (port, slot, u32 out[2] =
  0x00100200, 0x4000); init_card 0x0650 and probe 0x164c (port, slot) -> 1 when a card is present (a real card echoes
  the 0x55 terminator the driver sets, which gives 1). Ports 2/3 = card slots 1/2.
- Verified with tools/savetest.py (load slot 1, R2 at the save point, Yes / slot / overwrite Yes): "Save Complete",
  129 pages written, root directory intact, the saved card loads into gameplay (port/savetest3, port/saveload1).

## Still to find (next)
- Done: SIO2D's probes are native too (pad probe 0x0978 -> none, card probe 0x0a1c -> card image present on port 2/3),
  MC2_D's F3 check 0x08d8 native; no SIO2 transfer exists, the SIO2 model is deleted (ext 2026-10-11).

## Native DBCMAN + MC2_D service (agent nat-iop1, 2026-10-24, ext agent/natiop)
User directive 2026-10-24: nothing emulated or recompiled. The five IRX of the pad / card chain (SIO2MAN, SIO2D,
DBCMAN, DS2U_D, MC2_D: ~86k generated lines) are no longer loaded. `IopSubsystem::loadModule` records them as HLE
modules (log line `[IOP:native] '...MC2_D.IRX' served natively`) and the RPC servers they registered are answered by
hand-written C++ (ps2xIOP/src/modules/dbcman.cpp, `NativeDbcService`), written from the two modules' code
(module offsets in the comments). `GOW_NATIVE_PADMC=0` loads the IRX files again (recompiled modules = the
verification oracle; the old SIO2D / MC2_D / SIO2MAN native overrides stay for that path only).
What the EE (libdbc + libmc2, the game's own code) talks to, read from DBCMAN.IRX / MC2_D.IRX and checked against an
oracle trace (savetest with `PS2X_TRACE_RPC=800013 PS2X_IOP_TRACE_DMA=1`; the first capture, made with sw-core1's exe, is in
D:\Claude Archive\GoW-Archive
atiop-20261024\oracle1-core1-exe-trace):
- **0x80001300** (DBCMAN main, 0x90-byte buffer, synchronous; jump table 0x2560 indexed by fn - 0x80001301):
  0x1363 version -> word0 = 0x310; 0x1304 -> word1 = EE address of a 0x80-byte table (one word per socket, 1 = a device
  answers behind it), SIF-DMA'd at once and on every change, word0 = 1; 0x1301 create socket: words 0..3 descriptor
  (flags 3 = match type + port/slot, type 2 = memory card, port 2/3, slot 0), words 5..7 "MC2SOCKET", word 8 an EE
  pointer (unused by the IOP), words 10/11 the socket's two EE status addresses (0x324840 / 0x3248c0); reply word 9 =
  socket index (0, 1). A socket whose port has no card gets no device (its commands answer -23).
  0x1302 delete, 0x1303 socket -> device (unused by the game).
- **0x8000131C** (DBCMAN socket server, 0x2090 bytes, `sceSifCallRpc` mode 1): word0 socket, word1 MC2_D command,
  word2 reply size (MC2_D writes 0x2080), words 3.. MC2_D's parameters p: +0 page, +4 count, +8 page size 0x200,
  +0xc pages per block 0x10, +0x10 card pages 0x4000, +0x14 / +0x18 backup blocks 1/2 (0x3ff / 0x3fe, from the
  superblock), +0x1c / +0x20 block arguments, +0x24 ECC flag, +0x28 the write data. The command runs to completion
  inside the RPC (MC2_D's server thread did the same); the reply is the buffer with the result at +0x208c.
  0x8000131E/F: the same with 0x90-byte buffers (result +0x8c), unused.
- MC2_D's results reach the EE by SIF DMA, never through the RPC reply: page data to socket address[0] + 0x80 + n *
  0x200 (dbcman#9), then a 0x80-byte status block (MC2_D bss 0x4418: +0 command counter (the EE waits for it to
  change), +4 device, +8 status (1 ok, 0 error, 2 card changed), +0xc.. results). Reads (cmd 1/8) put the status at
  address[0]; every other command alternates address[0] / address[1] (MC2_D's toggle 0x34f0).
- Commands (MC2_D 0x1f74, jump table 0x3460): 1 read pages with ECC check/1-bit correction, 2 write pages, 3 specs,
  4 probe (0/1/2), 5 backup markers, 6 recover an interrupted write, 7 copy block, 8 one raw page + ECC, 9 erase
  block, 10 clock (sceCdReadClock BCD), 11 raw writes, 12 init card, 13 acknowledge a card change. Before any command
  but 4/13, a card flagged "changed" (set when the card appears, cleared by 13) answers status 2.
- Write (cmd 2, power-loss safe): per touched erase block, unless it is a backup block: recover pending markers (cmd
  6), write marker page backup2*16 = block index, copy the block to backup1 (page-verified), marker page backup2*16+1 =
  block index, erase the block, write the new pages (ECC computed, read back and checked), restore the other pages
  from backup1 (verified), erase backup2. The service reproduces this on the card image, including MC2_D's page
  buffers 0x4498 / 0x48b8 whose stale bytes end up in the marker pages, so the card bytes match the IRX path.
- ECC: 3 bytes per 128 (table MC2_D .data 0x34f4, = PCSX2's), page buffer = 0x200 + 12 bytes written per page
  (the last 4 spare bytes stay as they were).
- Seen per savetest (load slot 1, save): cmd 1 x64.7k, 4 x2k (status poll ~once a frame), 6 x16, 12 x15, 2 x14,
  10 x2, 13 x1, 3 x1.
- IOP RAM layout: the five IRX are not loaded, but their IOP RAM range is reserved (`IopEmulator::reserveModuleSpace`,
  the loader's cursor advanced by each IRX's span), so LIBSD / 989NOMID / SMPD_IOP still load at 0x20400 / 0x25e00 /
  0x4c900 where their compiled code was generated (without it they loaded lower and ran on the R3000 interpreter:
  `[IOP] no native code for libsd.irx base=0x10000`). Goes away when those three are native too.

### Verification (PC, ext agent/natiop 7953c07 = sw-vu1gpu 0927485 + this, Release, logs off; port/natiop/v)
- Protocol A/B (`savetest`, IRX path `GOW_NATIVE_PADMC=0` vs native, both traced; comparator scratchpad
  `padmc_cmp.py`: per card command its words, data DMAs (address, size, FNV of the payload), status block and reply):
  64.7k commands identical through boot, title, load of slot 1, gameplay and the save menus; the runs part only where
  the game's own timing differs (an extra probe pair), plus two trace lines cut by another thread's log line. The 16
  write-side commands (14 x cmd 2, 2 x cmd 10) are the same with the same results.
- Card bytes after the save: both paths change the same 129 pages; 124 of them byte-identical, the other 5 differ only
  in the save time (directory entry dates, the data's clock fields and the ECC bytes over them).
- Cross loads (zones.py slot 1, the saved card): native loads the native save, the IRX path loads the native save,
  native loads the IRX save: ok, 60 fps. Saves made before (ref/saves/Mcd002.ps2, slots 1-4) load in the gate.
- Zone gate slots 0-4 native `port/natiop/v/gate-native2/20261024_214229` against the IRX gate
  `port/natiop/v/gate-irx/20261024_210449` as baseline: 5/5 ok, 60 fps, game speed 1.01, no capture changed (pad walk
  in every zone). In the IRX gate slot 3 exited early once (code 1, after gameplay start; not seen natively).
- IOP time (`PS2X_IOP_NATIVE_STATS=1`, mean of the last 8 windows of 5 s, slots 1 / 2 / 4; slot 0 windows differ in
  content, slot 3's IRX run died): IOP total 161 / 133 / 131 ms (IRX) vs 178 / 145 / 128 ms (native), i.e. the same
  within run-to-run noise; IOP instructions 5.41 / 3.21 / 3.43 M vs 5.37 / 3.18 / 3.39 M (-0.7 to -1.0 %: the
  removed modules' idle threads); imports -1.2k per window. In gameplay the card is polled only (cmd 4 about once a
  frame), so nothing measurable was expected; the removed cost is IRX code at load / save (~270 card RPCs a second
  while loading, each running DBCMAN + MC2_D's generated code before).

### Handoff (nat-iop1, 2026-10-24 evening)
- ext branch `agent/natiop` (worktree ext/PS2Recomp-kl-natiop, from sw-vu1gpu 0927485): 501a45b service + module
  bypass + MSVC fix of 0927485 (`__builtin_prefetch` -> `_mm_prefetch` in ps2_vif1_interpreter.cpp, it did not compile
  with MSVC), db14890 traces / port-table bytes, 7953c07 IOP RAM reservation. Build `out/build-pc` (Release, logs off).
  Only ps2xIOP + that one runtime line changed: merges into sw-vu1gpu without conflicts expected.
- ext/PS2Recomp-kl-natiop-pc (branch agent/natiop-pc at sw-core1 7da6cae + the same patch, uncommitted) was a
  side build made while chasing a slowdown that turned out to be the CMake log defaults (docs/build-and-run.md);
  it holds nothing unique and can be removed (`git worktree remove --force`, branch delete).
- Switch: the touched files pass devkitA64 `-fsyntax-only -D__SWITCH__`; no console run (sw-k11 owns it).
- Still compiled and linked: the generated code of the five modules (iopn_sio2man/dbcman/sio2d/mc2_d/ds2u_d,
  ~3.5 MB of C++) and their IRX overrides, for the `GOW_NATIVE_PADMC=0` oracle. Once the native path has run on the
  console, drop them from the registry (regenerate tools/iop/iopn_gen.py output without them) and remove the
  oracle switch.
- Next (docs/native-port.md "IOP side"): LIBSD as a native import library, then the 989snd / SMPD native driver,
  then the IOP OS HLE + guest clock.
