# God of War internals (PAL SCES-53133)

Addresses are EE virtual addresses in SCES_531.33 unless prefixed `iop:`. "gp-relative" globals are written as
absolute addresses (e.g. `-0x2BF0($v1)` with `lui v1,0x2A` = 0x29D410).

## Boot flow
- crt0 0x100008: SetupThread syscall 0x3C (gp 0x2D1070, stack 0x1FF8000 size 0x8000, **args buffer $a3 = 0x2C9080**),
  SetupHeap 0x3D (0x35E3A8, 0), _InitSys 0x29A0E0, 0x294AE0, heap init 0x138CB0, static ctors, main 0x138D48.
- Static ctor table 0x2A7F2C: count 2 → 0x26C248 (calls 0x235180, builds tables incl. IRX module table) and 0x29C7B8.
- 0x17AB60(argc, argv): argv[0] starts with "cdrom" → boot flag 0x29D124 = 0 (disc); else 1 (host); argc 0 → stays
  −1 (treated as host). Disc mode: IOP reboot with `cdrom0:\IOPRP300.IMG;1`, modules from `cdrom0:\`, retries on
  failure. Host mode: `IOP_MOD/<name>`, no reboot, no retry.
- IOP init 0x17BD58: sceSifInitRpc, sceCdInit, sceCdMmode(2), reboot IOP (0x299D60/0x299D10), sceCdReadDvdDualInfo
  (0x27B710), sceFsReset, sceSifLoadFileReset, sceSifInitIopHeap, then loads the 8-entry module table at 0x2FF9F0
  (name, args): sio2man, dbcman, sio2d, mc2_d, ds2u_d, libsd, 989nomid (args "rpc_priority=64"), smpd_iop; then
  sceDbcInit 0x27B910, 0x27CD10, sceChk01 0x2841E0.

## Memory
- Game heap (allocator create 0x13D758 from 0x138CB0): all RAM from .bss end 0x35E3A8 to **0x1FF7FF8**.
  Heap stack at 0x2CD9C0; alloc 0x13DCC0 (walks a free list; corrupted list = infinite loop at 0x13DD20).
- Main stack 0x1FF8000–0x2000000. Fixed-block pool "RB NODE Heap" at 0x4558E0 (16-byte nodes, 16-bit indices).
- The game reads IOP-written status through the uncached mirror (e.g. 0x20455640).
- **Runtime-owned guest data must avoid 0x35E3A8–0x2000000** → override moves it into kernel RAM 0x80000–0x100000
  (see runtime-changes.md, GuestReservedLayout).

## Known SDK function addresses (bound or relevant)
sceCdSync 0x27B1B8, sceCdSyncS 0x27B258, _sceCd_scmd_prechk 0x27B2C8, sceCdInit 0x27B420,
sceCdReadDvdDualInfo 0x27B710 (bound by override), sceCdMmode 0x27B7E0, sceDbcInit 0x27B910, scePad2Init 0x27C3F8,
scePad2CreateSocket 0x27C4A0, scePad2Read 0x27C600, scePad2GetState 0x27C7A8, sceSifInitRpc 0x297888,
sceSifBindRpc 0x297ED0, sceSifCallRpc 0x2980B0, sceSifCheckStatRpc 0x2982B0, sceOpen 0x298CD0, sceRead 0x2990E0,
sceSifAllocIopHeap 0x299438, _sceSifLoadModuleBuffer 0x299718, sceSifLoadStartModuleBuffer 0x299928 (bound by
override), _sceSifLoadModule 0x299948, sceSifRebootIop 0x299D60, sceChk01 0x2841E0, sceScfGetLanguage 0x283C98.

## Waits/halts found (each was a hang until fixed)
| Address | What waits | Released by |
|---|---|---|
| 0x13DD20 | heap alloc loop | runtime data out of the game heap |
| 0x27B378 | libcdvd bind loop (IOP RPC 0x80000593) | runtime handler for sceCdReadDvdDualInfo |
| 0x27B978 | libdbc bind loop (RPC 0x80001300) | modules actually loaded (boot argv fix) |
| 0x186080 | deliberate halt if sceChk01 fails | ck01 load path fixed |
| 0x176B58 | RB-tree walk with NULL object 0x29DA60 | resource load succeeding |
| 0x186160–0x186180 | loader pump loop (0x185B10 returns 1) | 989snd batch callbacks (NOP patch removal) |
| 0x183968 | `while (*0x29D410 == 1)` "Immediate Upload" wait | GS FINISH interrupt → handler 0x182EC8 |

## sceChk01 (0x2841E0)
Uploads embedded IRX "ck01" (.data 0x2A5410, 857 bytes, imports cdvdman) with sceSifAllocIopHeap + DMA +
sceSifLoadStartModuleBuffer; ck01's start() calls cdvdman sceCdSC(−9) and returns 5 if it returned 1, else 1.
Game continues only if load ≥ 0 and (result & ~3) == 0; otherwise spins at 0x186080. Results stored at
0x29D11C (ret) / 0x29D120 (result).

## Resource loading (EE ↔ IOP)
- Main game object `*0x337BA8`; loader 0x186018(obj, name, flags 0x10000210): zeroes obj+0x54, starts load
  0x17AE48, pump loop {0x26C4B0, 0x1730A8, 0x1733F0} until 0x185B10 returns 0. 0x1BB6E0 then copies obj->0x54 to
  global 0x29DA60. Resource name from 0x1BB390: "R_Perm" + language letter (byte at 0x29DAA6) → "R_PermU".
- Loader state machine 0x185B10 (object at 0x5CCAE8, state at +0) uses `mfc0 Count` timeouts (needs a running Count).
  Global pause counter 0x29D444 (0x172CE0 increments, 0x172D20 decrements); 0x185B10 returns 1 early while non-zero.
- Sound operations list head 0x2FF948; node state +0x34: 1 = requested, 2 = waiting for bank flag, 3 = registering
  entries (RPC 'SMPD' fn 0x13, 8 per pump). 1→2 by batch callback **0x172D78** (registered via 0x26D670 →
  0x26CCE8 with the request); 2→3 when status entry (slot = node+0x24 & 0xF) at `*0x29CA70 + slot*32 + 0x48` has
  bit 0x2000.

## 989snd / SMPD protocol
- EE 989snd library: RPC client 0x307748 to IOP sid **0x123456** (989snd server).
  - Direct calls 0x26CAC0(cmd, size, buf): sceSifCallRpc NOWAIT, wait 0x26C730, result = reply word 1 (0x307784).
  - Batched calls 0x26CCE8 → flushed by 0x26D050 as cmd **0x4D** (results array count*4+8 bytes); per-command
    callbacks run from 0x26C4B0 when the pending pointer 0x2A30C8 (set at 0x26C808, cleared in 0x26C730) completes.
    Completion = sentinels −1 in result words 0 and count+1.
  - cmd 0x68 = plugin call: payload `'SMPD'(0x534D5044), fn, size, args…`.
- SMPD (game IRX "smpd file streamer", plugin of 989snd; descriptor .data 0x8208: 'SMPD', 0x1d, table at +0x10, decoded in docs/sound.md):
  - fn 1 init (reads PVD, root dir, GODOFWAR.TOC; builds per-extension tables at iop 0x82A40 WAD, 0x85B68 VAG, …:
    8-char name, size, location; extensions VAG VPK WAD M2V PSS M2W PSW PAK at iop 0x5BB50),
  - fn 0xF open resource (name + flags) → handle, fn 6 fill 128 KB EE buffer (0x532840/0x542840/0x552840/0x562840,
    DMA in 64 KB pieces), fn 0xD load sound bank from EE memory (e.g. "GENERAL" EE 0x1EDE520 size 0x10F1E0) via
    sceSifGetOtherData 8 KB chunks + snd989 #129, fn 0x13 register sound entries.
  - Status block 0x280 bytes DMA'd to EE 0x455640 (word 0 = sequence counter; entries 32 bytes; flags at +0x1C).
- IOP 989snd module args: "rpc_priority=64" (parsed with strncmp chain; unknown → "989snd Error: cause 7").

## GS
- GS interrupt handler 0x182EC8 (INTC cause 0): reads CSR 0x12001000; FINISH bit → sets 0x29D410 = 2.
- Upload helper 0x13F778(n): sceDmaSend(*0x3331A0 or *0x3331A4, *0x33319C), packet ends with FINISH.

## Controller (working since 2026-10-09; see iop-changes.md for the fixes)
- (Before the fix the title showed "PLEASE INSERT AN ANALOG CONTROLLER ... PORT 1".) EE socket buffer for port 1: 0x2FFB00 (+0x1C buttons/sticks), DBCMAN status block EE 0x30F0C0.
- Stack: EE libpad2 (scePad2*) + libdbc (sceDbc*, RPC 0x80001300) → IOP DBCMAN.IRX ↔ DS2U_D.IRX ↔ SIO2D.IRX ↔
  SIO2MAN.IRX → SIO2 hardware. Also MC2_D (memory card) uses SIO2.
- IRX imports: SIO2MAN: intrman[4,5,6,7,17,18] dmacman[28,32,33,34,35] thevent thsemap sysclib;
  SIO2D: sio2man[51,61,62,63] secrman[6] vblank[5]; DS2U_D: sio2man[51,61,62] sio2d[6,9,10] dbcman[4,6,7,8];
  DBCMAN: sifcmd/sifman RPC server; MC2_D: sio2man, sio2d, dbcman, secrman[4,5], cdvdman[24].
- SIO2MAN code (module offsets): register helpers at 0x0–0x188 (0xBF808268 CTRL, 0x826C RECV1, 0x8270 RECV2,
  0x8274 RECV3, 0x8278/0x827C, 0x8280 ISTAT, SEND3 at 0x8200+4i, SEND1/2 at 0x8240+8i, FIFO in sb 0x8260,
  FIFO out lbu 0x8264); transfer 0x1D4: set SEND1/2 (4), SEND3 (16), push in-bytes to FIFO, then
  dmacman#28 dmac_request(ch 11, …)+#32 dmac_transfer(11) and #28(ch 12, …)+#32(12), then CTRL|=1 (0x2FC).
  Init 0x77C: RegisterIntrHandler(0x11, handler 0x584), EnableIntr(0x11), dmacman#35(11,3)/(12,3).

## UI timeline player: combat freeze (use-after-free), 2026-10-13

### Crashes analysed

- **crash1.log (ship, after the 2 Hydra fights)**
  - Error: `JALR 0x23b700 -> 0x3f`.
  - 0x23B6A0 is the destructor of a timeline node (vtable 0x2BF4C0, 0x58 bytes, operator new class 144). It walks the
    node's child entries `{node, key}` (8 bytes each) at `*(node+0x28)`; the count is the u16 at `*(node+0x1C)+2`.
    For each child it calls the virtual destructor (vtable at +4, entry +8/+0xC) and then operator delete 0x14C2F0.
  - Child 0x780bb0 (operator new class 32) had already been deleted. Its block was then reused for the string
    "CheckForGlow2", from `_level0/:CheckForGlow2` (strdup 0x143C58), and freed again: free-list link at +0, "kForGlow2"
    at +4 where the vtable was.
- **crash2.log (Cliffs combat)**
  - Error: `JALR 0x149390 -> 0`.
  - 0x149340 empties a node's duplicated-clip list (first index +0x18, count +0x1C, entries `{node, key}` at +0x20).
    It has no NULL check, and the list's entries had been zeroed: the storage was reused.

### Which system it is

The strings belong to the Flash-style HUD movie of the boss/enemy meter: HydraMeter1/2, EnemySetMeter, EnemyEyeGlow,
SimKeyEvent, `_level0/:CheckForGlow1/2`. This movie is in every level WAD.

| Function | Role |
|---|---|
| 0x147520 | Evaluates a node's channels (keyframe search). When the key changes it deletes and recreates the channel's node. |
| 0x148458 | Property/event keys: string values, script calls. |
| 0x1493E8 | Parses "name[...]" and builds the clone list. |
| 0x1491C8 | Creates the clones. |
| 0x14C278 / 0x14C2F0 | operator new/delete on 5 fixed-block pools. The class table is at 0x29C030: 32x500, 64x100, 144x220, 288x20, 512x6. A small request falls back to a bigger class when its own class is full. |

### What was ruled out

| Candidate | How it was checked | Result |
|---|---|---|
| Recompiler miscompile in the involved functions | EE differential tester, 150 calls each (port/shipcrash/diff1) | Only 1-ulp float noise and VU0 flag noise. Covered: 0x147520, 0x146DE8, 0x146FB8, 0x148458, 0x149B10, 0x23B6A0, 0x1493E8, 0x149340, 0x149958, 0x149FA0, 0x23B778, 0x14A560, 0x1491C8, 0x13DA10, 0x13DB18, 0x14C278, 0x14C2F0. |
| Thread races | Thread log | The game has only main (priority 2) and libdbc (priority 1). The runtime's same-priority time slice never applies. |
| Pool exhaustion or a leak | Pool usage log | Class 32 holds about 200 of 500 blocks in ship fights; usage is stable. |
| Double frees in ordinary combat | Pool checker, 5-minute ship fights | None. |

**Not reproduced.**
- The saves on the card are all at quiet save altars, and scripted exploration found no enemies.
- The ship start (new game) has fights but no enemy meter.
- The trigger needs a fight that shows the enemy meter, such as the Hydra fights or the Cliffs combat the user played.

### Fix

The fix is an object guard in gow_overrides.cpp (heap_guard), and it is always on:

- operator new/delete track every block they hand out.
- A second delete of a block that is already free is ignored. Without the guard it links the block into the free list
  twice, which later gives it to two owners.
- Before 0x147520, 0x23B6A0 and 0x149340 run, a child that was deleted is dropped:
  - in 0x147520 and 0x23B6A0 its entry is zeroed;
  - in 0x149340 the entry is removed and the rest move up, since that function has no NULL check.
- A child counts as deleted if its block is free, or its vtable does not point to game code.
- Each case logs `[gow-heapguard] <what> object= at= freed-by=`. The first three also dump the last guest branches, so
  a user log names the delete site that freed the child too early. That site is the remaining root cause.

**Self-test (temporary build).** A node still referenced by a channel was freed every 5000 calls. The guard caught
every case: 11 frees, 11 reports. The game kept running (port/shipcrash/selftest).

### Diagnostics

`gow_overrides_diag.cpp` is the investigation version and is not for the merge. It adds:

- `GOW_POOL_CHECK=1`: a pool-level double-free and live-block checker, plus a class usage log;
- `GOW_THREAD_LOG=1`.
- Status: contained by the guard; root cause (the early delete site) still open, waiting for a user log with
  `[gow-heapguard]` lines from a fight that shows the enemy meter.

## Node pool and the 0x1D6D44 crash (sw-crash2, 2026-11-02; open)

### The crash
- Console only, twice in ~210 sweep new-game boots (port/switch/sweep/ab2_dips5/20261031_1208/ATHN02A,
  ab_dips32b/20261102_0155/ATHN03D). Both logs show `JALR source=0x1d6d44 target=0` with byte-identical registers and
  call traces.
- 0x1D6CD0 is the player controller's per-frame update. The caller 0x21FDA0 runs it every frame in state 4
  (loading), 0xB-0xE and 0x12, on `*0x29CD58`.
- The controller is P+0x10 of a 0x230-byte object (ctor 0x244F18, controller ctor 0x1D5DA0 / dtor 0x1D5DF0). Its +0
  is the player object, vtable at +4. 0x1D6CD0 calls the virtual at vtable+0x68/+0x6C.
- In the crash, controller+0 was 0x30dc0051. Those are the bytes of two pool-node pointers (0x0051xxxx, 0x005130dc)
  shifted by 2: a node had been built at controller-2.
- A second node of the same kind sat at EE 0xC8 (low RAM: next 0x5161cc, parent 0x5130dc, type 9).

### The pool
- 0x24-byte blocks, header `*0x29C708` (PC: 0x5044A0, magic 0x31415927, block 0x24, align 0x10, no growth, blocks
  from 0x5044C0).
- Allocation is 0x13DA10 (head = pool+0x10, link at block+0); free is 0x13DB18. The size-class getter is 0x17E9B8,
  and each class's operator new caches the pool (0x1D40B0 for 0x20 bytes, 0x228528 / 0x1A1448 for 0x1C bytes).
- Nodes have: +0 next sibling, +4 parent, +8 type, +0xC vtable (base 0x2C0458, ctor 0x180A80).
- When a load ends, the loaded data is read back into a tree. The parent comes from 0x1D40B0 (PC: block 0x5130DC). The
  children (type 9) come from 0x228B18 through 0x228528, about 23 of them.
- On the console, two of those allocations returned bogus blocks (0xC8 and controller-2). So the free list was
  already corrupted before the load ended.
- On the PC the list stays valid (1047 free blocks in state 4, full walks clean).

### Facts and what was ruled out
- In both crashing runs the load was slow: levelReady=0 at 50 and 55 s, and the IOP did only ~330 ms of work in the
  45-50 s window against ~520 normally. The other ~210 boots all had levelReady=1 at 50 s.
- The node creation, and the crash in the same frame, came at the scripted X press at 57 s.
- PC slow loads (GOW_CD_READ_DELAY_MS 40-300 ms per read), X presses during the load, the Italian overlay, a failed
  CD read during the load (GOW_CD_FAIL: the driver retries) and x#2 timing 43.0-43.9 s: no fault.
- EE threads during the load: main (priority 2) and libdbc (priority 1), so the equal-priority time slice cannot
  apply.
- The Switch-only overrides differ from the PC only in the frame clock (0x17D778).

### Next
Run console boots with the diagnostic package (port/switch/crash2_diag.patch, update_crash2c): GOW_POOL_CHECK=1
plus GOW_CRASH_DUMP. A pool fault then names the corrupted link (`link-at`) and gives the branch ring and the EE and
IOP RAM.

## Zones and the debug level warp
- A level is a chain of zones, one WAD each (GODOFWAR.TOC: the zone WAD followed by its resource WADs `R_*`;
  108 zone WADs: ATHN01-08, SEWR, DEST, PAND, ARENA, OLYMP, ATRET, HDES). Two zones are loaded at a time (slot table
  0x335280, flags +0x68: 1 in use, 4 loaded). The current zone object is 0x29CAC8; its name (`Athn01A`, the WAD name
  without `WAD_`) comes from 0x1A97C8.
- The ship (new game) is ATHN01A-F: start in 01A/01B, 01C stairs below deck, 01D deck with the save altar (resource
  WADs R_AHA/R_AH2: Hydra), 01E deck outside the captain's cabin, 01EE (R_AH3; its warp point is far below the deck,
  Kratos falls), 01F captain's cabin. ATHN02A-D is the start of Athens, ATHN03A Ares in Athens, DEST00 the desert.
- Level scripts call builtins through the script VM dispatcher 0x1AFAD0 (a1 = id, 59 ids, jump table 0x2B46D0,
  arguments popped from the VM stack: VM +0x140 depth, entries at VM +0): 2 `Load(zone, zone)`, 3 `LoadWad(zone)`,
  4 `LoadForWarp(zone)`, 5 `Warp()`, 6 `LoadCheck(zone)`. Each prints a debug line ("======== LoadForWarp ('%s')").
- Warp: LoadForWarp loads the zone into the slot that does NOT hold the current zone (0x1BB7E8(zone, current)) and
  resets the warp state (0x29CAB0 slot or -1, 0x29CAB4 zone loaded, 0x29CAB8 warp requested, 0x29CAFC warp due).
  Warp() sets 0x29CAB8 and, if the zone is loaded, 0x29CAFC (0x1B25B8); the executor 0x1B25C8 then makes the new zone
  current and moves Kratos to the zone's warp point (0x1B26F0, "Jumping to warp point") and clears the state. A
  Warp() before the load completes has to be repeated.
- Limit: the current zone (0x29CAC8) follows Kratos with a delay. A warp issued while he already stands in the
  neighbour zone evicts the zone he is in: its objects stay linked in the render lists (0x1309C0 dispatcher, crash
  JALR 0x130CAC -> 1) or the game stops presenting. Warping 0.5 s after gameplay starts (Kratos still in the start
  zone, the current one) works for every zone tried, including other areas (PAND04G, HDES01A, DEST00, ATHN03A).
- Save restore 0x22CB18 (stream magic 0xCAFEBAD1) reads the two saved zone names (10 chars each) and loads them with
  0x1B3E08 (names to 0x335288, then 0x1BB7E8): the path a clean multi-zone warp would have to use.

### Moving Kratos (warp-point jump, warp points)
- Player object `*(*0x29CD58)` (class vtable 0x2BA528, shared by the enemies' creature objects): vtable pointer at +4,
  new transform +0x10..+0x4F, current transform +0x50..+0x8F (four rows, translation +0x80, y is up).
- 0x1B26F0 (warp-point jump, a0 = warp point): new matrix from the warp point (0x1A95D8), written to +0x10..+0x4F, then
  the virtual at vtable+0x2C (this + int16 vtable[+0x28], a1 = the new transform; creatures 0x1E6E18 -> 0x1DFE58
  copy it into their node) applies it; with a camera (`*0x29E2A4`) 0x1EF0E0(old, new)
  moves the camera; then 0x1A8718, 0x2247E8(1), 0x1A5FC0, 0x1A9520. `GOW_TELEPORT` (docs/diagnostics.md) repeats the
  first three steps with a given translation.
- Warp points: list at 0x302600 (node +0 next, +8 object); 0x1ADFF8(new zone, old zone) picks the object whose owner
  zone is the new zone: kind (+0x46) 2 with the old zone's name (+0xB0 offset into the string block *(+0x58)) first,
  else kind 1. Local transform +0x00..+0x3F, node index +0x40, owner +0x54; world = local x node matrix
  (`*(*(owner+0x104)+0x8C)` + index*0x40). Only the zones' entry points, no checkpoints (tools/gow_warppoints.py).

### Flash UI action queue (the heap guard's double delete)
- Each Flash UI player has an action queue (player +0xCD8 head, +0xCDC tail; nodes {item, owner, next}, 12 bytes from
  operator new). 0x1460C0 appends at the tail (a2 != 0, from 0x146FB8) or the head (a2 = 0, from 0x149958 and
  0x146FB8). 0x146158 drains it at the end of each player update (0x14A560): for each node the action runs
  (0x1452F8(item, owner)), the next pointer is read, the node is deleted (call at 0x1461A4, return 0x1461AC); after the
  walk head and tail are cleared; with +0xCFC set the queue is dropped without deleting the nodes.
- `[gow-heapguard] second delete ignored ... at=0x1461ac freed-by=0x1461ac` reproduces with the warp
  (`GOW_WARP=ATHN01EE`, Kratos falls and the death HUD runs; node 0x780AD0 in our runs): after a drain deleted the
  node, the next append found it still as head and tail of player 0x7A40D0's queue, so it was in the list of the next
  drain and deleted again. A nested drain of the same player is not the cause (none occurs). The queue head/tail are
  written only by 0x1460C0, the drain's clears (0x1461B8/0x1461BC, 0x146178 for +0xCFC) and the player init 0x14A290.
  Open: which sequence leaves the freed node as head; the guard keeps it harmless.
- Note for overrides: recompiled functions return to the dispatcher early at EE checkpoints (ctx->pc = the resume
  point; resume addresses have their own table slots that point to the original function, not to a replacement). A
  C++ wrapper that calls the original and then does more work runs that work before the original has finished, and
  a guest function called on a copied context must be dispatched again until it returns (gow_overrides.cpp
  gowRunGuest).

#### Resolved: the ATHN01EE double delete was the heap guard (2026-10-13, agent/warp2)
The "second delete ... at=0x1461ac" reports in the ATHN01EE warp run were not a game bug but a heap-guard error.
`gowOperatorNew` read the new block from v0 right after calling 0x14C278. A recompiled function hands control
back early at an EE checkpoint (ctx->pc = resume point; the call is finished later through the original's own
slots), so after an early return v0 was not the new block yet. The reallocated block stayed marked free, and its
next, legitimate delete was reported and ignored (a leak). The same stale state also made the guard drop live
timeline children. Fix (port/warp2/gow_overrides.cpp): the wrapper finishes the allocator before recording the
block. 0x14C278 -> 0x13DA10 -> 0x13DC78 -> strcmp 0x2897C4 never waits, so re-dispatching it to ra is safe.

A/B on the same build, `GOW_WARP=ATHN01EE`, 60 s:
- old guard: "second delete ignored" for 0x780AD0 and 0x7805B0 (port/warp2/r0_oldguard);
- fixed guard: 0 reports, 60 fps, speed 1.00 (port/warp2/r1).

The user's own crashes (ship, Cliffs) happened before the guard existed, so their cause is still open. With the
fixed guard, the guard's reports in a fight that shows the enemy meter are now trustworthy and name the delete site.

Rule for overrides (docs/game-internals.md note on early returns): never use a guest call's results (v0, memory it
writes) right after calling it from a wrapper; either finish the call first (only for functions that never wait) or
do the work before the call.
