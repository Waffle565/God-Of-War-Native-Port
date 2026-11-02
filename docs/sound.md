# Sound: the native sound engine

The game's sound engine is its own IOP code: 989snd (`989NOMID.IRX`, sequencer, voice allocation, streaming) with the
SMPD streamer plugin, compiled ahead of time like every IRX (docs/native-port.md). Below it there is no sound hardware
model: the SPU2 register-level model and the IOP's SPU DMA channels were removed on 2026-10-11.

## Handoff (nat-iop9, 2026-10-26) - latest (stopped early: out of tokens)
- ext `agent/natiop2` **4848b52** (on 713f418): grain **1** native (0x4a0c key on a tone): voice allocation 0x1a058
  (free voice of the group range 0x1f014, else steal lowest priority / oldest), noise voices 0x19e6c, 0x19d60 (VOL 0,
  NON), key on 0x1a374 (VOL, VMIXL/R, NON, PITCH, SSA or noise clock, ADSR, VMIXEL word, pending KON 0x1f1ac),
  playing list 0x1b718 / 0x1b7d4, voice-end callback 0x6cb4 (mask part; the "sound ends" part declines, `L1952`).
  sceSdGetSwitch is modelled (`getSwitch`: last value set in this call, else SoundEngine); LIBSD getters (#6 / #8 /
  #10) are no longer recorded for the shadows (pure). No new stub (grain 1 runs inside 0x4748).
- Verified only partly: shadow slots 0 + 3 (port/natiop9_gate.sh, port/natiop9/s1): **0 mismatches**, 60 fps,
  speed 1.00; slot 0 block declines 531 -> 40 (L100005 30, L1952 9, L100007 1). NOT yet run: shadow slots 1 / 2 / 4,
  native gate n1, sound correlation, aarch64 compile (/root/iop_a64.sh). Do these first.
- Next: then grain 5 (0x5dbc), the stop 0xf1dc (+ 0x6cb4's free path 0x2374 / 0xe888), and the rest of nat-iop8's list.

## Handoff (nat-iop8, 2026-10-25)
- ext `agent/natiop2` **713f418** (on de98c7b): more of 989snd's tick native (see "989snd's tick"): the LFOs 0x2938 /
  0x24d8 (all six waveforms and kinds; was idle-only), ducking 0x1c8e4 / 0x1c6e8, pitch 0x6928 / 0x6a20 (bend,
  Note2Pitch, noise clock), grains 25 (random branch) and 27 (random pitch). New stub `989nomid_irx 1c8e4` in
  tools/iop/native_overrides.txt (master): regenerate iopn_989nomid_irx_25e00.cpp in any other worktree (`python -I
  tools/iop/iopn_gen.py port/iop_mods <tmp>`, copy that file only). register_functions.cpp there: never commit.
  New tick pieces: one `case` in iopSnd989TickStep + `isTickStep` (iop_native_snd989.cpp) + a stub line.
- Goal 0 (the 2 fx mismatches in slot 1): not a native error. An interrupt handler (thread 0) ran 989snd's effect
  volume setter 0xe23c (sceSdSetParam EVOL L / R core 1) while the tick's 0xe2e8 (thread 3) was interrupted; the
  shadow counted those calls as the tick's. The native call's result (idle at its entry) is a valid order. LIBSD
  recordings now carry the thread and the shadows compare only the checked thread's calls; the others are printed.
- Verified (PC, port/natiop8_gate.sh, results port/natiop8/): shadow slots 0-4 (`GOW_NATIVE_SND989=0
  GOW_SND989_SHADOW=1`, p1 + g1 for grain 25) **0 mismatches**; slot 0: LFOs / duck / kon / koff ~49k / 12k, 0
  declined; block declines now grains 1 / 5 / 7 / 8 only. Native gate (n1) **5/5 ok, 60 fps, game speed 1.00**;
  sound native vs shadow slots 1-4: correlation 0.9991-0.9999, mean relative difference 0.003-0.006, identical first
  1.41 s. aarch64 (Switch flags, devkitA64 g++ -Wall, private copy /root/src-iop, script /root/iop_a64.sh):
  all 41 ps2xIOP sources compile, no warnings in the hand-written native files (compile only, no console).
- Next: (a) grains 1 (0x4a0c key on a tone, voice allocation) and 5 (0x5dbc start a sound), the stop 0xf1dc (calls
  0x4814 block end, 0xe888 free (44 blocks), 0x12924 stream stop, 0x1af04 / 0x1ad54 voices), 0x12e90 (type-4 /
  stream sounds), volume ramp records 0x32b0 / 0x35dc / 0x3e18 (never seen in the gate slots), the non-idle effect
  path 0xe2e8, then the sound list step 0xf5b0 and the tick loop 0x10a58; (b) unchanged (kernel semaphores, loaders,
  #129, RPC server).
- Switch: no console run; IOP-side C++ only.

## Handoff (nat-iop7, 2026-10-25)
- ext `agent/natiop2` **de98c7b** (e4607ea + de98c7b on 65223ba): pieces of 989snd's tick native (see "989snd's tick"):
  ramp records 0xf690 with pan ramps 0x2e88, the block-sound step 0x4748 (grain timer and the register / random /
  goto / loop / wait / key-off grains), key-on / key-off flush 0x1a878 / 0x1a990 (sceSdSetSwitch), group fades 0x2938
  and effect changes 0xe2e8 when idle. Six new stubs in tools/iop/native_overrides.txt (master): regenerate
  iopn_989nomid_irx_25e00.cpp in any other worktree (`python -I tools/iop/iopn_gen.py port/iop_mods <tmp>`, copy that
  file only). register_functions.cpp there: never commit. Build out/build-pc current (cmake: docs/build-and-run.md).
- Verified (PC, port/natiop7_gate.sh, results port/natiop7/): shadow slots 0-4 0 mismatches for every new piece
  except 2 fx calls in slot 1 where another thread changed the effect state inside an interrupted call (the IRX then
  made 2 LIBSD calls; the native call runs atomically at the entry); slot 0 (fight): block 650k-1.6M calls, ~0.03-0.06 %
  declined (grains 1 / 5 / 7 / 8 / 25 / 27), kon / koff / fades / fx ~49k each, all served; menus decline fades
  while a fade runs. Native gate **5/5 ok, 60 fps, game speed 1.00**; sound native vs shadow slots 1-4: correlation
  0.9996-0.9999, mean relative difference 0.004-0.005, identical first 1.41 s.
- Next: (a) the remaining tick: grain 1 (0x4a0c key on a tone: voice allocation 0x19..., 46 blocks) and 5 (0x5dbc start a
  sound), 0x12e90 (type-4 / stream sounds, 47 blocks), 0x1c8e4 (every 4th sub-tick: voice end detection, 34 blocks),
  0xf1dc (stop), the volume ramp records 0x32b0 / 0x35dc / 0x3e18, fades 0x24d8, then the tick loop 0x10a58 itself;
  (b) native kernel semaphore services for native code, then the loaders fn 06 0x6900, fn 0f 0x6890 -> 0x5f5c, fn 0d
  0x4260, bank load #129 0x83dc; then the RPC server (unchanged leads below).
- Switch: no console run; IOP-side C++ only.

## Handoff (nat-iop6, 2026-10-25)
- ext `agent/natiop2` **65223ba** (1be2974 + decline statistics, on 0a2f982): the 989snd setters #117 / #41 / #32 / #122 and SMPD's stream method
  0x13c0 -> 0x2448 -> 0x1784 native, so SMPD fn 0b is native for every call seen except flags 0x10 / 0x8 (never in
  the traces), lock contention and ending streams (see "989snd setters and the SMPD stream method"). New file
  ps2xIOP/src/native/iop_native_snd989_set.cpp; IopNativeWrites now holds any number of stores + LIBSD calls
  (iopNativeApplyWrites / iopNativeCheckWrites); the shadows also compare a 989snd data snapshot and the LIBSD calls the
  IRX made (iopLibsdRecorded). Build out/build-pc current. register_functions.cpp there: never commit (as before); the
  generated iopn_989nomid_irx_25e00.cpp is unchanged (no new stubs: fn 0b's override already covers these paths).
- Verified (PC, gate script port/natiop6_gate.sh, results port/natiop6/): shadow slots 0-4 (`GOW_NATIVE_SMPD=0
  GOW_SMPD_SHADOW=1 GOW_NATIVE_SND989=0 GOW_SND989_SHADOW=1`) **0 mismatches**, cycles exact for every setter / stream
  call (`soundSet+0`, `m6f20Set+0`, `stream+0`); one interrupted stream call in slot 1 (an interrupt stored into
  989snd's data: no longer counted as a mismatch); native gate **5/5 ok, 60 fps, game speed 1.00**; sound native vs
  shadow slots 1-4: RMS correlation 0.9984-1.0000, mean relative difference 0.002-0.010, identical first 1.41 s.
- fn 0b paths in a fight (slot 0, 230k calls): ~85 % nothing to set, ~4 % setters, ~9 % streams, declined ~0.1 %:
  dSetters ~170, all of them 989snd's lock held by another thread (`declined at L<line>:n` in the shadow line points
  at the source line: the lock's), and the method 0x3d5c (71).
- Next: (3) loaders fn 0d / 0f / 06 and the bank load #129 (file reads + sound-RAM transfers); (4) the RPC server
  (0x1878 / batch 0x177c) and 989snd's tick (it runs the ramp records: function 0x2e88 per record), then 989NOMID /
  SMPD_IOP need not be loaded. The engine class in iop_native_snd989_set.cpp (overlay, locks, sound lookup, setters)
  is the base to build them on: the ramp record function 0x2e88 (29 blocks) calls only #32 0xcef8, 0xd244 and 0xf86c,
  all transcribed, so the tick's record step is the cheapest next piece. Loader leads (SMPD table .data 0x8218):
  fn 06 0x6900 = stream refill request (+0x188 / +0x18c = min(request, free), SignalSema [0x989c] wakes the stream
  thread, optional WaitSema [0x98a0], status 0x2c88(1)): needs kernel semaphore services in IopNativeServices;
  fn 0f 0x6890 -> 0x5f5c (stream open, 27 blocks), fn 0d 0x4260 (-> 989snd #77 / #15 / #9); #129 = 0x83dc (98 blocks,
  file reads through jalr callbacks).
- Switch: no console run; IOP-side C++ only.

## Handoff (nat-iop5, 2026-10-25)
- ext `agent/natiop2` **0a2f982** (on 14d5fd7): SMPD fn 0b native for the paths that set nothing in 989snd, cmd 0x68
  wrapper native when it reaches them (see "SMPD fn 0b and cmd 0x68"). Build out/build-pc current. The generated
  iopn_989nomid_irx_25e00.cpp is git-ignored in ext: regenerate it in any other worktree (`python -I tools/iop/iopn_gen.py
  port/iop_mods <tmp>`, copy only that file; now 6 stub entries incl. 0x1410). register_functions.cpp there: never commit.
- Verified (PC, port/natiop5, gate script port/natiop5_gate.sh): shadow gate slots 0-4 ok (slot 2 one 49.5 fps window
  while another agent's runner loaded the PC), fn 0b **0 mismatches** (196k calls slot 0), cmd 0x68 0 mismatches (195k
  slot 0), all other shadowed commands 0; native gate **5/5 ok, 60 fps, game speed 1.00**; sound native vs shadow
  slots 1-4: RMS correlation 0.9993-1.0000, mean relative difference 0.002-0.008, identical first 1.41 s.
- Native share of fn 0b: slot 0 (fight) ~86 % of calls, slots 1-4 ~45-55 % (the rest: SMPD stream objects, class
  method 0x13c0 -> 0x2448, and calls with setter flags).
- Next: (1) the 989snd setters, which every remaining fn 0b path needs (0x359c, 0x6f20 and the stream path 0x2448 ->
  0x1784 all call them): #41 (0x2b48, pan ramp: lock 0x105cc / 0x10750, automation records 0xf770 / 0xf7a4 / 0xf7dc /
  0xf824, #32), #32 (0xcef8 -> 0x650c type 5 / 0x18e6c type 4: voice volumes, 0x1bf48 volume math, 0x19a64 voice lock,
  LIBSD SetParam under CpuSuspendIntr), #117 (0xd4fc -> 0x46cc), #36 (0xd330 -> 0x6928 -> 0x6a20), #38 (0xd394 -> 0x69a4,
  0x19500), #122 (0x1784); call tree: `tools/iop/irxblocks.py` per function (blocks with instruction counts and
  the calls in each block); the shadow then also has to compare the LIBSD calls made (record them
  in `iopNativeLibsdImport` while a call is checked); (2) then the stream method 0x2448 (accumulates N parameter sets
  at +0x164..+0x178, then 0x1784); (3) loaders fn 0d / 0f / 06 and bank load #129; (4) the RPC server and the tick.
- Leads for (1): the fn 0b flag words in the slot 0-4 traces are only 0, 0x801 / 0xc01 / 0xc04 / 0xc05 (0x800: #41 + #32)
  and 0x8000 / 0x8801 / 0x8c05 (+ #117); 0x10 (#36) and 0x8 (#38) never occur, so #41 + #32 first. #41's ramp records
  (checked): 0xf7dc allocates from the pool at 0x24514 (64 x 0x24 bytes, word 0 bit 0 = in use; sets word 0 = 1,
  +0xc = 0); stagista leads (verify): 0xf7a4(list, mask) = first node of the list at list+8 with `node[0] & mask`,
  0xf770(list, mask) = find + unlink (0xf86c, under CpuSuspendIntr), 0xf824(list, node) = append.
- Switch: no console run; IOP-side C++ only.

## Handoff (nat-iop4, 2026-10-25)
- ext `agent/natiop2` (worktree ext/PS2Recomp-kl-natiop2): 1217389 (SMPD fn 02 / 1a native) + 41012d8 (989snd cmds
  41 / 42 / 4a / 4b native, shadow; SMPD shadow counts interrupted calls; GOW_WINDOW_BACKGROUND) + **14d5fd7** (cmd 4c
  native for native SMPD functions; window mode 2). Build out/build-pc current. Generated iopn_989nomid_irx_25e00.cpp
  in that worktree was regenerated from port/iop_mods with the new stubs (tools/iop/native_overrides.txt, master): any
  other worktree must regenerate it too (only 5 stub entries differ; without the C++ overrides they just interpret one
  instruction). register_functions.cpp in that worktree is a locally regenerated (shorter) file: never commit it.
- Verified: see "989snd status commands" (native gate 5/5 at 60 fps, shadow 0 mismatches, sound equal).
- Native now on the per-frame sound path: LIBSD (all), SMPD fn 02 / 1a, 989snd cmds 41 / 42 / 4a / 4b / 4c. Still IRX
  per frame: the RPC server / batch loop itself, cmd 0x68 -> SMPD **fn 0b** (many per frame in fights) and 989snd's
  tick.
- Next: (1) SMPD fn 0b (0x364c): needs 989snd #25 (0x848 -> 0xcdd4, handle -> sound), #117 (-> 0xd4fc), #36 (->
  0xd330), #38 (-> 0xd394), #41 (-> 0x2b48), #32 (-> 0xcef8) natively, i.e. 989snd's per-sound parameter setters
  (they reach LIBSD, already native); same pattern: wrapper stub + C++ + shadow; (2) cmd 0x68 wrapper (0x1410 -> 0x250)
  once fn 0b is native; (3) the loaders fn 0d / 0f / 06 and bank loading #129; (4) eventually the RPC server and the
  tick, then 989NOMID / SMPD_IOP need not be loaded.
- Test runs: tools/zones.py starts the runner with GOW_WINDOW_BACKGROUND=1 (never takes the focus; user rule). A gate
  run while the PC was busy had GL uploads of 1-4.6 s per 5 s and failed in both the native and the oracle run alike:
  re-run before blaming a change. Give zones.py an absolute `--out`.
- Switch: no console run; IOP-side C++ only.

## Boundary: the LIBSD API
- 989snd reaches the sound unit **only** through LIBSD.IRX's exports (it never touches SPU2 registers; only LIBSD did).
- Those exports are native C++ (`ps2xIOP/src/native/iop_native_overrides.cpp`, listed in tools/iop/native_overrides.txt):
  sceSdInit #4 0x456c, SetParam #5 0x3670, GetParam #6 0x36cc, SetSwitch #7 0x372c, GetSwitch #8 0x3764, SetAddr #9
  0x379c, GetAddr #10 0x3800, SetCoreAttr #11 0x3cbc, VoiceTrans #17 0x18a0, BlockTrans #18 0x1c30, VoiceTransStatus
  #19 0x1af0, BlockTransStatus #20 0x1e4c, SetEffectAttr #23 0x0c3c, SetTransIntrHandler #26 0x3e3c,
  SetEffectWorkArea #31 0x0984 (addresses checked against LIBSD's export table). Note2Pitch #13 stays game code (math).
- Entry codes as libsd: `(id << 8) | (voice << 1) | core`, master/effect/stream volumes with bit 7 (LIBSD's register
  table at 0x4930 gives the id order: 0 VOLL, 1 VOLR, 2 PITCH, 3 ADSR1, 4 ADSR2, 5 ENVX, 6/7 VOLX, 8 MMIX, 9/A MVOL,
  B/C EVOL, D/E AVOL, F/10 BVOL, 11/12 MVOLX, 13 PMON, 14 NON, 15 KON, 16 KOFF, 17 ENDX, 18-1B VMIX L/EL/R/ER,
  1C ESA, 1D EEA, 1E TSA, 1F IRQA, 20 SSA, 21 LSA, 22 NAX). Addresses are byte addresses (16-byte aligned).

## LIBSD: a native import library (agent nat-iop2, 2026-10-24, ext agent/natiop2)
- LIBSD.IRX is no longer loaded. `IopSubsystem::loadModule` records it as served natively (its IOP RAM range stays
  reserved, so 989NOMID / SMPD_IOP still load at 0x25e00 / 0x4c900 where their compiled code was generated), and
  989snd's `libsd` imports - the only module that imports libsd: ordinals 4-11, 13, 17-20, 23, 26, 31 - are served by
  `iopNativeLibsdImport` (ps2xIOP/src/native/iop_native_overrides.cpp) from `IopEmulator`'s import dispatch when no
  loaded module exports "libsd". The handlers are the ones listed above (the same C++ that served LIBSD's export
  addresses before) plus **sceSdNote2Pitch #13**, transcribed from LIBSD 0x38a0 with its two tables (.data 0x5150:
  12 semitone factors, 0x5168: 128 fine steps) and its quirk for a negative fine (the 0x3940 block indexes the
  tables by the fine's octave). LIBSD's module start (RegisterLibraryEntries, the bus setup no-op, zeroing its
  transfer state) has no observable effect left.
- IOP clock: a native call costs what it cost with the IRX (one dispatcher step for the overridden exports; for
  Note2Pitch the instructions LIBSD's code runs on that input, 52-69, counted per basic block), so the IOP clock and
  everything timed by it (989snd's tick, the sample clock) advance as before.
- `GOW_NATIVE_LIBSD=0` loads LIBSD.IRX again (the oracle: its exports then run the same overrides, Note2Pitch as
  LIBSD's compiled code). With `GOW_SND_LIBSDLOG` set, the oracle checks every LIBSD Note2Pitch result against the
  native function on return (`[libsd] Note2Pitch checked against LIBSD.IRX: n, mismatches m, outside tables k`).
- Note2Pitch offline check: the native function against LIBSD's code run by a small R3000 interpreter (Python, the
  IRX's own bytes) over 1.29 M inputs (center note 0-127 / 3, center fine 0-127 / 31, note 0-127, fine -300..300 / 13):
  0 mismatches in result and instruction count wherever the tables are indexed in range. Out of range (|fine| beyond
  an octave below the center, 17 % of that grid) LIBSD reads its own variables after / before the tables; the native
  function returns 0 there and counts it (`outside tables`); 989snd's calls never get there (see Verification).
- IOP heap: the IOP heap cursor only grows, and loading an IRX allocates and frees its start's argv block (path +
  arguments, 0x20 bytes for LIBSD), so skipping the load moved every later allocation (989snd's buffers, the
  addresses in its VoiceTrans / GetOtherData calls) 0x20 lower. `reserveModuleSpace` now allocates and frees the
  same block for every natively served module (pad / card too), so the IOP RAM layout is the all-IRX one.
- Verification (PC, ext agent/natiop2 569d093, RelWithDebInfo, logs off; port/natiop2):
  - Zone gate slots 0-4, native and oracle (`GOW_NATIVE_LIBSD=0`) run side by side, traced and recorded
    (gate-native / gate-oracle 20261024_225407, build before the heap fix): **5/5 ok at 60 fps both**.
  - Note2Pitch: every call in the 10 traces (2638, 1835 distinct inputs, fine 0..127) equals LIBSD's code
    (offline interpreter), and the oracle's on-return check reported no mismatch.
  - Sound (GOW_AUDIO_RAW, oracle vs native): slots 1-4 RMS profile correlation 0.9996-1.0000, mean relative
    difference 0.002-0.006, same length, silences and peaks (samples identical for the first 1.41 s, then the
    usual run-to-run timing). Slot 0 (new game, unscripted fight) diverges in gameplay from the first capture on;
    in that oracle run all groups fade out at ~113 s (cmd 0x09 volume ramps in every batch, the game's own fade:
    the GS draw count drops to a menu-like 3k, Kratos apparently died), not a sound difference.
  - LIBSD call log (slot 1): same calls per ordinal within timing noise (#9/#13/#17/#26 and all one-off calls equal
    in count); the rest of the differences were the 0x20 address shift (VoiceTrans / GetOtherData addresses) plus
    timing. After the heap fix the oracle and native traces use the same addresses (VoiceTrans source 0x18ba5c,
    GetOtherData 0x18ba58 in both). A strict call-for-call identity cannot be shown on PC: two runs of the same
    build part after ~2.7k trace lines (EE timing decides when the per-frame batches arrive between sound ticks).
  - The second side-by-side gate (after the heap fix) ran while the PC was loaded by other builds: both runs fell
    to 1-20 fps and failed alike (not usable); the native gate was repeated alone: gate-native3 (569d093, heap
    fix): **5/5 ok, 60 fps, game speed 1.00**; sound vs the oracle recording of slots 1-4: RMS correlation
    0.9994-1.0000, mean relative difference 0.001-0.004.

## Sound trace (oracle tooling for the native sound driver)
`GOW_SND_LIBSDLOG=<file>` writes everything the sound modules exchange with the rest of the system, in order:
- `sd <ordinal> <args> [-> <result>] [data=<fnv64> | attr=...] @<iop cycle>`: every LIBSD call of 989snd (VoiceTrans
  writes with the FNV-1a 64 of the bytes uploaded to sample RAM, SetEffectAttr with its attribute block);
- `rpc <sid> <fn> <mode> <size> <fnv64> : <words>` / `rep ...`: every RPC to 989snd (sids 0x123456 main, 0x123457
  loader) with the first 1 KB of the payload in words and the reply after the call;
- `dma <iop src> <ee dst> <size> <fnv64>`: IOP -> EE SIF DMA (989snd's status blocks, SMPD's file data);
  `god <ee src> <iop dst> <size> <fnv64>`: sceSifGetOtherData (banks pulled from EE RAM).
`tools/sndtrace_cmp.py A B [--strict]` compares two traces (counts per kind and ordinal, identical prefix, LIBSD calls
between aligned RPCs; mixer-timed results of GetParam / GetSwitch / GetAddr / BlockTransStatus only with --strict).
`tools/zones.py` replaces `{zone}` in environment values with the zone's folder, so one gate writes one trace per
zone (`GOW_SND_LIBSDLOG={zone}/snd.trace`, `GOW_AUDIO_RAW={zone}/mix.raw`); `tools/audio_cmp.py A.raw B.raw`
compares two recordings (identical prefix, SHA-1, RMS profile per second).

## 989snd: what a native driver has to replace (map, 2026-10-24)
- 989NOMID.IRX is 989snd "(c)2000-2003 SCEA, by Buzz Burrowes, built Dec 21 2004", the **NO MIDI** build: "banks
  and streams only" (handler types BLOCK and VAG_STREAM are the ones that can occur; MIDI / AME / basic VAG are not
  in this build). Threads: sound tick, main RPC, loading RPC, 4 streaming (module args `rpc_priority=64`).
- RPC servers (989NOMID 0x18e0 / 0x1a60): **0x123456** main (server 0x1878: `handler[fn](buffer)` from the table at
  .data 0x1de88, 0x6b commands, then reply word 1 = -1; the handler writes its result through the reply pointer
  0x43c80 = reply buffer + 4) and **0x123457** loader (server 0x1964: fn 2 -> #9, 3 -> #10, 0x57 -> #111, 0x59 -> #113,
  0x6a -> #129; the bank loads, on their own thread). EE side: docs/game-internals.md "989snd / SMPD protocol" (direct
  calls, batched cmd 0x4D, plugin cmd 0x68 = SMPD).
- Main commands -> the 989snd function each wrapper calls (`#n` = snd989 export n, the library SMPD_IOP links to; hex
  = internal function; `tools/iop/snd989_rpcmap.py` prints it with wrapper sizes):
  00 #4, 01 #5, 02 #9, 03 #10, 06 #11, 08-10 #15-#23, 11/12 #24, 13-15 #27-#29, 16 #67 (pause group), 17 #68 (continue),
  18 #31, 19 #25, 1a #26, 1b #32, 1c #34, 1d #35, 1e #33, 1f #36, 20 #38, 21 #126, 22-25 #40-#43, 26 d838, 27 d814,
  2a #48, 2b 2028, 2c #49, 2d #50, 2e #52, 2f #55, 30 #53, 31-33 #58-#60, 34 #57, 35 #56, 36 #61, 37 #65, 38 #63,
  3b afd8, 3c b66c, 3d b750, 3e b898, 3f c52c, 40 c4d4 (status: BlockTransStatus, docs/fmv.md), 41 #70, 42 #69, 43-46
  #71-#74, 47 fc90, 48 10050, 49 fed0, 4a 103d8, 4b 10444, 4c 18c, 4d batch (runs a list of commands), 4e #96, 4f #99,
  50-52 #101-#103, 53 c820, 56 #106 (+ SIF DMA), 57 #111, 58 #112, 59 #113, 5a b4a4, 5b b52c, 5c-5f #114-#117, 60 #119,
  61 ef6c, 62 #121, 63 #122, 64 #123, 65 cdvdman import #28 directly, 66 #124, 67 #125, 68 plugin call (SMPD), 69
  #127 (+ SIF DMA), 6a #129 (bank load from EE memory); 04/05/07/28/29/39/3a/54/55 are stubs.
- #129 (0x83dc) is the bank loader state machine used for every bank the game loads from EE RAM (SMPD fn 0xD and
  main / loader fn 0x6a): sceSifGetOtherData chunks, the "SBlk" block header (magic 0x6b6c4253), sample data to
  sound RAM through VoiceTrans, the bank linked into 989snd's bank list (0x45174 state block).
- Reference for formats and names: OpenGOAL's re-creation of 989snd (github open-goal/jak-project, game/sound/989snd:
  SBlk banks, block sounds, voice allocator) is for Jak's MIDI build and not byte-exact; the driver here must be
  reproduced from this IRX's code, routine by routine, and checked against the sound trace (as MC2_D was).

## What the EE asks of 989snd (tools/sndtrace_stats.py, slot 1 / title, 2026-10-24)
Per frame the game sends one batch (cmd 0x4D) of: 4c SMPD fn 1a (x2), 42, 4c SMPD fn 02, 41, 4a, 4b, and 68 SMPD fn 0b
(many per frame in the fight); 09 (group volume: the fades) as needed; loads: 4c SMPD fn 13 / 06 / 07 / 0c, 68 SMPD fn
0d / 0f / 0a, 4e, 0e / 0f; once at boot 00, 64, 0b, 68 SMPD fn 01. So the busiest path is SMPD's plugin functions
(1a, 02, 0b) and 989snd's per-frame status commands 41/42/4a/4b: the natural first native piece.

## Per-frame commands decoded (nat-iop3, 2026-10-25)
- 989snd's two plugin paths: cmd **0x4c** (0x13c0 -> 0x18c) = `plugin(id)->fn[w1](w2, w3, w4, w5, w6)` with the
  result in the reply (the per-frame SMPD fn 1a / 02 calls); cmd **0x68** (0x1410 -> 0x250) = `fn[w1](w2 = size,
  &w3)` (the argument block in the RPC buffer). The plugin is found by id (0x148, 'SMPD' = 0x534D5044); a function
  index >= descriptor+4 -> 989snd error 4.
- SMPD's descriptor is static data (.data **0x8208**, relocated at load, not built at run time): 'SMPD', 0x1d, two
  words, then the table at +0x10 (module offsets): 00 2de4, 01 3250 (init: checks the status block size 0x280,
  stores the EE addresses 0x81f4 / 0x81f8), 02 0018, 03 7094, 04 7180, 05 67ec, 06 6900, 07 6994, 08 69cc, 09 69fc,
  0a 30a8, 0b 364c, 0c 3510, 0d 4260, 0e 4404, 0f 6890, 10 66f4 (pause / continue groups), 11 0000 (returns 0),
  12 0008 (returns 0), 13 2a74, 14 0f34, 15 0f44, 16 2fe8, 17 43fc, 18 36e0, 19 449c, 1a 3778, 1b 675c, 1c 6a60.
  (The older note "descriptor at 0x8748" was a .bss address; 989snd keeps the pointer it was given by #93.)
- SMPD **fn 1a** (0x3778): returns its argument (the game's per-frame round-trip check, sent twice per batch).
- SMPD **fn 02** (0x18): stores its argument at 0x80e0, then 0x2c88(1) builds the 0x280-byte status block at 0x91a0
  and sends it to the EE (EE address from 0x81f4, the 0x455640 block of game-internals.md) with one SIF DMA, waiting
  for it: six stream entries of 0x30 bytes at +0x12c from the stream objects (`[0x82e0] + i * [0x82e4]`: +0 = s+0x84,
  +4 = s+0x18c, +8 = s+0x108, +0xc = s+0x104, +0x10 = s+4, +0x14 = s+0x30, +0x18 = (s16) s+0x130, +0x1c = s+0x184,
  +0x20 = strcpy of the name at s+0x114), word 0 and +0x27c = the sequence (incremented), +0x26c = [0x82b4], +0x270 /
  +0x274 = sysmem free sizes. The sound-entry slots (+4..+0x12c, written by fn 0x13 and the loaders) are sent as
  they are. 0x2c88 is also called from nine other SMPD functions (after opens, loads...).
- SMPD **fn 0b** (0x364c, many per frame in fights): `args[0]` = handle; an SMPD stream handle (0x4720 searches the
  stream pools) goes to the stream object's virtual method, else 989snd #25 resolves the sound handle and 0x359c
  applies the fields selected by the flag word args[5]: 0xf000 -> #117(&args[1]), 0x10 -> #36(s16 args+0x12), 0x8 ->
  #38(s16 args+0x10), 0x800 -> #41(s16 args+0xc, 0, 0x1e, 4) then #32(s16 args+8, -2).
- 989snd **41** (#70, 0x10cac) = the word at 0x1ee68; **42** (#69) = bit mask of the 24 voices of core `w0` whose
  voice record (0x26004 + v * 56) has a non-zero +8 (in use); **4a** (0x103d8) / **4b** (0x10444) = sum / maximum
  of +4 over the used entries (+8 != 0) of the 129-entry table at 0x24e14 (stride 0x18), under 989snd's lock
  (0xfbb0 / 0xfc0c): the sound-RAM allocations (total and largest).
- Listing: `tools/iop/irxlist.py <IRX>` prints a whole IRX with import names (lib:ordinal) and resolved lui pairs.
  `tools/iop/irxblocks.py <listing> <function offset>` prints a function's basic blocks with their instruction counts
  and calls (the cycle accounting of native overrides).

## SMPD fn 0b and cmd 0x68 (native paths, nat-iop5, iop_native_smpd.cpp / iop_native_snd989.cpp)
- SMPD **fn 0b** (0x364c, `fn(size, args)`: +0 handle, +8 / +0xc / +0x10 / +0x12 s16 values, +0x14 flag word). An SMPD
  handle (pool `(h >> 28) & 7` in the table .data 0x82d4: base, stride; pool 0 searched linearly over 32 objects, the
  others indexed by `h & 0xff`; an object holds its handle at +4) goes to the object's virtual method (vtable at +8,
  slot +8: s16 this-delta, +0xc function); any other handle goes through 989snd **#25** (0xcdd4 -> 0xec34: type
  `(h >> 24) & 0x1f` 5 = the 64 block sounds at 0x1fc14 (0x124 bytes), 4 = [0x1ef40] entries at [0x1ef3c] (0x78 bytes),
  live when the entry's word 0 is the handle; returns the handle or 0) and 0x359c, which applies the fields chosen by
  the flag word: 0xf000 #117(&args[1]), 0x10 #36, 0x8 #38, 0x800 #41(+0xc, 0, 0x1e, 4) + #32(+8, -2).
- Native (`computeFn0b`, a prediction without side effects + its stores / LIBSD calls): handle -1; 989snd handles
  (with the setters, nat-iop6: see "989snd setters"); the methods 0x4938 (base class, returns 0), 0x2928 (table
  0x93ec[handle & 0xff] +4 = s16 +8, returns the handle), 0x6f20 (a 989snd sound in an SMPD object: +0xc =
  #25(+0xc); gone -> +4 = 0, returns 0; live -> the setters) and the stream class 0x13c0 (-> 0x2448). Declined (the
  IRX runs): flags 0x10 / 0x8 (#36 / #38), a 989snd lock held by another thread, a stream whose sound is gone (0x19b4),
  0x3d5c, handle 0, pools >= 4, #25's error paths (bad slot: printf).
- IOP clock: the instructions of SMPD's and 989snd's code per basic block (tools/iop/irxblocks.py prints them), the
  import stub of #25 one cycle: e.g. a live 989snd sound with flag word 0 and no pool-0 match = 391.
- Cmd **0x68** (wrapper 0x1410 -> 0x250 -> plugin fn[w1](w2, &w3), lookup 0x148) is native when the target is fn 0b
  and fn 0b serves the call: the C++ applies fn 0b's writes and stores the result in the reply. Cycles 62 + 8 per
  plugin before the match + fn 0b's. Not reproduced: 0x250's dead store of the 5th argument slot below $sp.
- Shadow (`GOW_NATIVE_SMPD=0 GOW_SMPD_SHADOW=1`): every fn 0b entry is predicted and its return checked (v0, the words
  written, cycles); every 4000 checked calls a `[smpd-shadow] fn0b` line with the count per path and the declined
  methods (`m13c0:n`). `GOW_SND989_SHADOW=1` checks cmd 0x68 (reply, plugin writes, cycles) as the other commands.
- Verification: see the nat-iop5 handoff (0 mismatches, 5/5 at 60 fps, sound equal).

## 989snd setters and the SMPD stream method (native, nat-iop6, ps2xIOP/src/native/iop_native_snd989_set.cpp)
- What fn 0b sets in 989snd, transcribed from the IRX (exports from 989NOMID's table at .text 0x1cc98):
  #117 0xd4fc -> 0x46cc (4 bytes at the block sound's +0x4c), #41 0x2b48 (pan ramp), #32 0xcef8 (volume / pan:
  0x650c block sounds, 0x18e6c type-4 sounds), #122 0x18c78 (a type-4 sound's channel, used by stereo streams).
  #36 (0xd330) / #38 (0xd394) (flags 0x10 / 0x8) are not transcribed: never seen in the traces, such calls decline.
- 989snd's lock 0x105cc(id) / 0x10750: recursive, semaphore [0x1f184], owner thread [0x1eef4], depth [0x1ee6c], id
  stack [0x1ee70 + 4 * depth] (32 entries), last id [0x1eef0]; free: 56 + 42 instructions, held by the caller: 36 + 37.
  The voice lock 0x19a64(1, id) / 0x19bbc: semaphore [0x1effc], owner [0x1eff8], depth [0x1f008], id [0x1f00c]; free
  48 + 28, held 26 + 20. A lock held by another thread (or a busy semaphore) declines (the IRX code runs and waits).
- #41(handle, pan, 0, 0x1e, 4): distance = pan - current pan (0xd244: +0x3e block / +0x12 type 4), shortest way round
  (+-360); time = 0x1e * |distance| / 180; time 0 -> no ramp, #32(handle, 0x7fffffff, pan) at once; otherwise a ramp
  record on the sound's list (+8, next +0xc; mask 4): an existing one is updated (bit 31 held while it is written),
  else one is taken from the pool 0x24514 (64 x 0x24, word 0 bit 0 = in use) with function 0x2e88 at +0x10; fields +4 /
  +0x1c step period, +8 period mode, +0x18 step (+-1 or distance * 32 / time), +0x20 target pan. Pool full -> #32 at once.
- #32 block sounds (0x650c(handle, vol, pan, 0)): +0x3a volume (a new volume removes the volume ramp, mask 2), +0x3e
  pan (-1 = the bank's default at template +2, -2 = keep), +0x10 = clamp(+0x38 + 0x7f), +0x12 = (pan + +0x3c) mod 360;
  nothing changed -> return. Children (+0x2c list, next +0x30) get volume * +0xc / 127. Then for each voice in the
  sound's mask (+0x18, 48 bits, 0xf538) under the voice lock: angle (0x6e08 when the template's +6 bit 2: distance
  panning), L / R into the voice record (0x26004 + 56 * v, +0x20 / +0x22) by 0x1bf48 (volume, pan table [0x1e828], pan
  mode [0x1f174]: 1 mono, 2 no phase inversion; sign rules keep a voice's phase), sceSdSetParam VOLL / VOLR
  (`(v / 24) | (v % 24) << 1`, values through the curve 0x1c634, halved) unless +0x16 bit 1, and +0x1a = 0x6e9c.
  Type-4 sounds (0x18e6c) do the same over their voice-record list (+4, next +0xc8; +0x7c voice, +5 channel) with
  0x18be4 / 0x18c28 for volume / pan, and mirror L / R to the paired voice [+0x38] (swapped).
- Cycles: the IRX's instructions per basic block (`tools/iop/irxblocks.py`), one per kernel import stub, two per
  sceSdSetParam (stub + the native import), one per SMPD -> 989snd stub.
- SMPD side: 0x359c (fn 0b with a 989snd handle: #117, then #41 + #32 with the s16 at args+8) and the method 0x6f20
  (the same, #32's volume = (args+8 * object +0x14) >> 7). The stream method 0x13c0 -> 0x2448(stream, args): with
  N = [+0x15c] >= 2 the calls are summed (+0x164.. volumes, +0x16c.. pans, +0x174 / +0x178, flags ORed into +0x13c)
  and every N-th call they become the parameter block (volumes clamped to 0x400, the rest averaged); N = 1 copies the
  arguments; then #25 checks the stream's sound (+0x128) and 0x1784 applies the block: flags & 5 with a stereo stream
  (+0x7c & 0xf == 3) -> #122 per channel (bits 0x800 / 0x400), else 0x800 -> #41 + #32; 0xf000 -> #117; +0x13c = 0.
  Volumes there: (+0x130 * +0x140 / 127 * +0x184) >> 16. A gone sound (0x19b4 closes the stream) declines.
- All of it runs on an overlay of IOP RAM (reads see its own stores) and returns the stores and LIBSD calls
  (IopNativeWrites); the native fn 0b performs them (iopNativeApplyWrites), the shadow compares them: final value of
  every stored byte, 989snd's data (module offsets 0x1d300..0x26b00) against a snapshot for stores the prediction
  missed, the LIBSD calls the IRX made while the call ran (recorded in iopNativeLibsdImport) and the cycles. The
  `[smpd-shadow] fn0b` line counts the paths `soundSet` / `m6f20Set` / `stream` and `<path>+<d>:n` = calls that took
  d more cycles than predicted (0 = exact; more = an interrupt inside the call).

## 989snd's tick (native pieces, nat-iop7, iop_native_snd989_set.cpp / iop_native_snd989.cpp)
- The tick (989NOMID 0x10a58.., sound thread, 989snd's lock 0x105cc held for the whole tick) per sub-tick: counter
  [0x1ee68] += 1, 0xf5b0 (the sound list [0x1ee34], link +0x24), 0x1a990 key-off flush, 0x1a878 key-on flush, 0xe2e8
  effect changes, 0x2938 LFOs, every 4th 0x1c8e4 (ducking). 0xf5b0 per sound: type 5 (block) with +0x16 bit 1 clear ->
  0x4748; type 4 -> 0x12e90 (streams, not transcribed); then, if the sound has ramp records (+8), 0xf690; a non-zero
  result -> 0xf1dc (stop the sound).
- Native (stubs in the compiled 989snd code, tools/iop/native_overrides.txt; each entry reaches the dispatcher, the C++
  serves it or declines and the IRX code runs):
  - **0xf690(record, sound)**: the record list from its last record (recursion first), a record whose word 0 has bit 31
    set (being written) is skipped, mode +8 == 1 counts +4 down, at 0 the record's function [+0x10] runs. Only 0x2e88
    (pan ramp, #41) is transcribed: pan = current (0xd244) + step (+0x18: |step| < 2 -> +-1 per period, else
    `((pan * 32 + step) << 11) >> 16`), wrapped to 0..359 towards the target +0x20 (clamped to it when passed), #32
    (handle, 0x7fffffff, pan); at the target the record is unlinked (0xf86c), else mode 1 reloads +4 = +0x1c.
    Other record functions (volume ramps 0x32b0 / 0x35dc / 0x3e18) decline.
  - **0x4748(block sound)**: +0x4a = current grain (-1: done -> 1 when no voices +0x18 / +0x1c and no children +0x2c),
    +0x48 grain timer (decremented, +0x16 bit 6 cleared); at <= 0 the grains due run through 0x6398 (handler table
    .data 0x1e47c[type], template +8 grains of 8 bytes, +3 type, +4 delay; loop counter +0x50 / +0x52 / +0x54; past
    the last grain +0x4a = -1). Grain types native: 0 / 2 / 3 / 20 / 21 / 35 (null), 26 (random delay), 30 / 31 / 32 / 33
    (register = byte / random / ++ / --; s8 index >= 0 the sound's +0x4c.., < 0 the globals 0x1e45c[~i]), 34
    (test a register: false skips a grain), 36 (goto label grain 35), 22 (loop back to the previous loop start, type 21;
    +0x16 bit 6 limits it to one pass per tick), 38 (wait one tick while voices play), 41 (0x1ad54: key off the sound's
    voices).
    989snd's random numbers 0xc770: XOR of two halfwords of the 249-entry table 0x1e630 (index [0x1e62c], lag 0x67).
    Also native: grains 25 (0x51e4, random branch: m = rand() % byte 0, never the last choice byte 2 twice; +0x4a += m x byte 1, the loop
    counters +0x50 = 1 / +0x52 = byte 1 + 1 / +0x54 = (byte 0 - 1 - m) x byte 1) and 27 (random pitch bend, see
    "Pitch" below). Not transcribed (decline, `L1000<type>` in the decline summary): 1 (key on a tone), 5 (start a
    sound), 7 / 8 (plugin calls 0x18c) and the rest; in a fight ~0.03 % of the block calls decline.
  - **0x1a990 / 0x1a878** key-off / key-on flush: pending masks [0x1f1b4] / [0x1f1ac] (two words, 24 voices per core)
    -> sceSdSetSwitch KOFF (0x16) / VMIXEL + VMIXER (0x19 / 0x1b from 0x1f1a4) + KON (0x15), cleared; the voices move
    between the masks 0x1f1bc (keyed on) and 0x1f1c4 (keyed off).
  - **0x2938: the LFOs** (every other sub-tick: the counter [0x1ee68] even; list [0x1e43c], next +0x2c). An LFO
    record (0x30 bytes): +0 waveform (table .data 0x1e444: 0 none 0x20e0, 1 sine from the 513 halfwords 0x1e038
    quarter by quarter, 2 square with duty +0x18, 3 triangle, 4 saw, 5 random: a new value from 0xc770 at each half
    period, +0x1c = which half), +2 kind, +3 target LFO index, +4 bit 0 invert, +6 depth, +0xc phase (+= 2 x rate +0x10,
    wrapped below 0x8000000; the waveform sees the old phase >> 16), +0x20 scale, +0x28 the sound. 0x24d8 per LFO by
    kind (jump table 0x1d618): 1 volume (sound +0x38 = scale x (wave - 0x7fff) >> 16), 2 pan (+0x3c), 3 / 4 pitch
    (+0x44 / +0x40, scale x wave >> 15), 5 the rate and 6 the depth of another LFO of the sound (+0x5c + index x 0x30;
    6 then recomputes its scale, 0x29d4); result bit 0 (volume / pan changed) -> 0x650c(handle, 0x7fffffff, -2, 1),
    bit 1 (pitch) -> 0x6a20.
  - **Pitch** 0x6928(handle, bend) (the sound and its children +0x2c / +0x30 get bend +0x42) -> 0x6a20(sound): +0x36 =
    clamp(+0x40 + +0x42), +0x14 = +0x44 + +0x46, copied to each voice (+0x24 / +0x26); unless the sound is paused
    (+0x16 bit 1) each voice gets sceSdSetParam PITCH (0x1b67c: note x 128 + fine with the bend scaled by the template's
    range +8 / +9; 0x1b5dc: sceSdNote2Pitch, a non-negative center note scaled by 44100 / 48000) or, for a noise voice
    (template +0xe bit 3), sceSdSetCoreAttr noise clock (core | 8, 0..63). Note2Pitch (#13) is pure: native code
    computes it without the call and the shadow does not record it. Grain **27** (0x5340: random bend =
    (rand() % 32767 -> -0x8000..0x7fff) x byte 0 / 100) is native through it.
  - **0x1c8e4: ducking** (every 4th sub-tick): a group (voice +0x1b) is active while one of its voices plays (state
    +8 == 1 / 4, sound not paused); each of the 4 duck records 0x26a84 (0x18 bytes: +0 group or -1, +4 mask of the
    groups it ducks, +8 target, +0xc down step, +0x10 up step, +0x14 level) moves its level down towards the target
    while its group is active, else up to 0x10000; the 15 group levels 0x1f0f4 = the minimum level of the records
    ducking the group; changed groups -> 0x1c6e8(mask): new L/R volumes (0x1c634) for their playing voices.
  - **0xe2e8** (effect changes per core, then 0x392c effect depth ramps) when idle: [0x1ee00] / [0x1ee04] == 0 and
    the ramp steps [0x1f2fc + 0x48c] / [0x1f320 + 0x48c] == 0. Anything else declines. The 2 "mismatches" nat-iop7 saw
    in slot 1 were not native errors: an interrupt handler (thread 0) called 989snd's effect setter 0xe23c while the
    tick's 0xe2e8 (thread 3) was interrupted, and its two sceSdSetParam (EVOL L / R of core 1, 0x0b81 / 0x0c81) were
    counted as the tick's. The LIBSD recording now keeps each call's thread (IopNativeWrites::Sd::thread) and the
    shadows compare only the calls of the checked thread (iopLibsdRecordedEqual); other threads' calls are printed
    (`w<offset> thread 3 interrupted by thread 0: LIBSD #5(...)`).
- Shadow (`GOW_NATIVE_SND989=0 GOW_SND989_SHADOW=1`): every entry predicted and checked like the commands (v0, the
  stores, a 989snd data snapshot, the LIBSD calls of the checked thread, cycles); a nested entry (0xf690's recursion,
  another thread while a call is checked) is not checked. The `[snd989-shadow]` line has columns ramp / block / kon /
  koff / fades (= the LFOs) / fx / duck and ends with the decline summary (`declines L<line>:n`, shared with the
  setters).
- New tick pieces go through `iopSnd989TickStep(offset)` (iop_native_snd989_set.cpp) and `isTickStep` in
  iop_native_snd989.cpp, plus their stub line in tools/iop/native_overrides.txt.
- Not native yet: 0x12e90 (type-4 / stream sounds), grains 1 / 5 / 7 / 8 / 25..., 0xf1dc (stop), the volume ramp
  records 0x32b0 / 0x35dc / 0x3e18, the tick loop itself.

## SMPD front end (native, nat-iop3, ps2xIOP/src/native/iop_native_smpd.cpp)
- SMPD plugin fn 02 (status block) and fn 1a (echo) are served by native C++ (`iopNativeSmpdOverrides`, registered
  when SMPD_IOP is attached). 989snd calls them with jalr from its own code, so they always enter through the IOP
  dispatcher and the compiled SMPD code needs no stub (not in tools/iop/native_overrides.txt). The native fn 02
  writes the same bytes into SMPD's own block in IOP RAM (state for the IRX code that still runs) and sends it with
  `IopNativeServices::sifSendToEe` (= sceSifSetDma with one descriptor: copy, `dma` sound-trace line, DMA id).
- IOP clock: a native call charges the instructions SMPD's code runs (`IopNativeCall::cycles`, new): fn 02 364,
  fn 1a 2.
- `GOW_NATIVE_SMPD=0` runs SMPD's code (the oracle). With `GOW_SMPD_SHADOW=1` as well, every fn 02 / fn 1a call is
  predicted natively at its entry and checked at its return (same run): result, the status block in IOP RAM (and the
  0x40 bytes after it), the copy in EE RAM, the stored argument and the IOP cycles. `[smpd-shadow]` lines on stderr:
  a total every 2000 calls, the first 20 mismatches.
- A call that took more cycles than SMPD's code with everything else equal is counted as `interrupted` (an interrupt
  ran inside it: seen once in ~11k fn 02 calls), not as a mismatch.

## 989snd status commands (native, nat-iop4, ps2xIOP/src/native/iop_native_snd989.cpp)
- The per-frame commands 0x41 / 0x42 / 0x4a / 0x4b (decoded above) are native: the C++ replaces each command's
  wrapper (989NOMID 0x118c / 0x11b4 / 0x1370 / 0x1398, reached with jalr from the main server 0x1878 and the batch
  0x177c through the handler table .data 0x1de88) and writes the result through the reply pointer [0x1de80], as the
  wrapper does. The wrappers are stubs in the compiled 989snd code (tools/iop/native_overrides.txt, regenerated with
  tools/iop/iopn_gen.py: only those entries change), so every entry reaches the IOP dispatcher.
- Cmd **0x4c** (plugin call, wrapper 0x13c0 -> 0x18c, lookup 0x148 in the descriptor list [0x1de84]) is native when
  it reaches a native SMPD function (fn 02 / fn 1a, the per-frame ones): the C++ finds the plugin, runs the SMPD
  function natively (`iopNativeSmpdRun`, its own writes and SIF transfer) and stores its result. Any other target
  (the loads' fn 13 / 06 / 07 / 0c, unknown plugin, bad index) or `GOW_NATIVE_SMPD=0` declines. Cycles 69 + 8 per
  plugin before the match + the SMPD function's. Not reproduced: the wrapper's dead stack stores below $sp.
  Cmd 0x68 (plugin call with an argument block: the per-frame fn 0b) is native when fn 0b's native code serves the call
  (see "SMPD fn 0b and cmd 0x68").
- 0x4a / 0x4b run under 989snd's recursive lock (0xfbb0 / 0xfc0c: semaphore [0x1ee5c], owner thread [0x1ee60],
  depth [0x1ee64]). The native call serves the two cases without a wait and without a net change: lock free (depth 0,
  owner not the caller, semaphore count > 0: owner ends -1) or held by the calling thread (depth > 0). Otherwise it
  sets `IopNativeCall::declined` (new) and the dispatcher runs the IRX code. The kernel state comes from two new
  services (`currentThreadId` = GetThreadId's result, `semaphoreCount`).
- IOP clock: the instructions the IRX code runs, per path: 0x41 14; 0x42 430 + 4 per voice in use; 0x4a / 0x4b 27 +
  the loop (129 entries: 7 unused, 10 used for 0x4a; 12 / 13 for 0x4b when not / when a new maximum) + lock 56 (free)
  or 42 (held by the caller); one cycle per kernel import.
- `GOW_NATIVE_SND989=0` runs the IRX (the oracle); with `GOW_SND989_SHADOW=1` every call is predicted at the
  wrapper's entry and checked at its return: v0, the reply word, the lock words, the cycles (`[snd989-shadow]`
  lines: calls / mismatches / interrupted / declined per command every 4000 calls, the first 20 mismatches). While a
  call is checked the dispatcher interprets it (the wrapper returns inside 989snd, where compiled code would not come
  back to the dispatcher), so the return is seen.
- Verification (PC, ext agent/natiop2 14d5fd7, RelWithDebInfo; port/natiop4, runs in the background window):
  - Native gate (gate2-native 20261025_164024, SMPD fn 02 / 1a + cmds 41 / 42 / 4a / 4b / 4c native, traced and
    recorded): **slots 0-4 ok, 60 fps, game speed 1.00**.
  - Shadow gate (gate2-shadow 20261025_165712, `GOW_NATIVE_SMPD=0 GOW_SMPD_SHADOW=1 GOW_NATIVE_SND989=0
    GOW_SND989_SHADOW=1`): slots 0-4 ok at 60 fps; per slot 11.5k calls of 41 / 4a / 4b, 23k of 42, 34.5k of 4c and
    11.3k SMPD fn 02 / 22.7k fn 1a: **0 mismatches** (result, reply, lock, status block, EE copy, cycles); 1 interrupted
    call (slot 1); 4c declined 521-752 times per slot (the loads' SMPD functions, by design).
  - Sound (tools/audio_cmp.py native vs shadow mix.raw, slots 1-4): RMS profile correlation 0.9993-0.9999, mean
    relative difference 0.002-0.005, same length / silences / peaks, identical first 1.41 s (as for LIBSD).
    tools/sndtrace_cmp.py slot 1: identical first 2687 lines, then the usual run-to-run timing parting (80 % of the
    aligned RPC segments with equal LIBSD calls, differences in the polled GetAddr / GetParam calls).
  - Captures: mean absolute pixel difference native vs oracle per capture is in the same range as two oracle runs of
    different builds (run-to-run timing); sheets look normal.

## SoundEngine (`ps2xIOP/src/emulator/core/sound_engine.{h,cpp}`)
- Typed state: per voice volume L/R, pitch, ADSR1/2, start/loop/next addresses, envelope; per core VMIX/NON/PMON masks,
  MMIX, ATTR, master/effect/aux/stream volumes, ESA/EEA. 2 MB sample RAM (the game's sample data).
- DSP (unchanged from the former model, PCSX2 / psx-spx algorithms): ADPCM decode, pitch with linear interpolation,
  ADSR, dry mix per voice, master volume; NAX reported inside the playing block like PCSX2's NextA (989snd's polled
  streams abort when NAX leaves the ring halves: that was the 2026-10-09 music-restart bug).
- Core input stream (BlockTrans, "ADMA"): 1 KB blocks of 256 left + 256 right 16-bit samples read from the IOP ring at
  48 kHz, mixed at BVOL while MMIX's sound-input bits (0xC0) are set; mode 0x13 loops, mode 2 stops. 989snd clears its
  transfer handler before starting it and polls the position. (Not implemented in the former model.)
  BVOL is a plain 16-bit signed level (0x7FFF = 1.0), unlike voice volumes; `sceSdBlockTransStatus` returns the IOP
  address being read (bits 0-23) and the ring half (bit 24), as libsd does. Used by the movie sound (docs/fmv.md).
- Transfers are copies; VoiceTrans returns `(size + 63) & ~63` and, for mode 0, schedules the driver's completion
  handler `handler(channel, arg)` (989snd 0x78bc: releases its transfer lock and wakes the waiting thread) 512 IOP cycles
  later through `IopNativeServices::callGuestLater`, as the DMA interrupt did. VoiceTransStatus returns 1.
- One stereo sample per 768 IOP cycles (`advanceTo`), drained by the host audio thread (`drainOutput`).
- Mixer thread (2026-10-19, for the Switch's core 1): `advanceTo` (called every IOP scheduler loop, ~140000 times a
  second, mostly with no whole sample due) only records the IOP cycle to mix up to; once a batch of 128 samples
  (~2.7 ms, `GOW_SOUND_BATCH=<samples>`) is due it hands it to the `SoundMix` thread, which runs the per-sample DSP.
  Every other call (set/get param, switch, address, core attribute, sample RAM transfers, streams, `streamPosition`,
  reset) first calls `sync()`: it takes back a batch the thread has not started, waits only for a batch being mixed,
  and mixes the rest itself. So each register write or read happens after exactly the samples it did with inline
  mixing (the output is the same sample for sample) and the thread mixes in parallel with the EE between the driver's
  calls. While a core input stream (a movie) plays, the IOP fills its ring outside these calls, so `advanceTo` mixes
  inline then. `GOW_SOUND_THREAD=0` mixes inline always. On the Switch the thread runs on core 0
  (`GOW_CORE_SoundMix=<0-3>` moves it) at the default priority (`GOW_PRIO_SoundMix=<0-9>` steps above it). At the
  default priority it waits behind the deko3d thread (GSThread) and the EE mixes much of the samples itself, mostly for
  the driver's ENVX reads (`GOW_SND_SYNC_DIAG=1` prints `[snd-sync]`); one step above (dips38's default) it took core 0
  from the deko3d thread and cost ~3.5 fps where that thread bounds the frame, so dips39 went back to 0
  (docs/performance.md sw-dips15 / sw-dips16).
  - Why batches and take-back: handing every `advanceTo` to the thread cost more than the mixing (a lock and a wake
    per call: `sound=` 94 ms per 5 s); waiting for a woken but not yet scheduled thread cost ~1 ms per wait on a busy
    PC (92 ms per 5 s in one zone). With both, waits are 0-2 ms per 5 s.
  - The hand-off is a state word (idle / pending / running / running+pending, CAS on both sides), not a mutex
    (2026-10-23): on Horizon a condition-variable signal makes the woken worker the owner of the mutex right away, and
    the worker on core 0 waits behind the deko3d / GS stage threads, so the EE's next `advanceTo` / `sync` blocked in
    `svcArbitrateLock` until the worker got the core: ~15 % of the EE thread in a console fight (`GOW_PC_SAMPLE`).
    The mutex and condition variables now only let the worker sleep (it re-checks every 2 ms, covering a signal sent
    between its check and its wait) and `sync()` sleep while a batch is mixed.
  - PC, `PS2X_IOP_NATIVE_STATS=1` (`[IOP:native] ... sound= (mixer thread, waits, worker samples)`), zones gate per
    5 s, inline vs thread: IOP time on the EE thread 168 -> 141 ms mean over the five zones (slot 2: 160 -> 118-124);
    `sound=` 51-107 -> 22 ms (what is left is the per-call bookkeeping and the stats clock itself); the thread
    mixes ~64 % of the samples (14-56 ms), the rest is mixed inline at the driver's calls (`cpu=` +5-15 ms). Batch
    32 moves 96 % of the samples but the wakes cost more than they save on the PC (slot 2: IOP 118 -> 129 ms).
  - Checked 2026-10-19: zones gate 5/5 ok, savetest 129 pages, directory intact, audio stats (missing/dropped only in
    the load windows, same as inline), tools/movies.py SEMO01 identical to inline (1410 pictures at 30/s, 46.0 s audio, A/V drift 0.05 s, no
    silence, clipping, repeats or drops).
  - Console (2026-10-19, ext agent/sw-vu1-par 97a7cfa = sw-ee5 merged, `PLAY=1` bot fight, logs
    `port/switch/logs/20261019_par_snd*`, `PS2X_IOP_NATIVE_STATS=1`, mean of 20 gameplay windows, ms per 5 s on the
    EE thread / core 1):

    | | IOP total | cpu | sound | mixer thread (core 0) | waits | samples on the thread | fps (16 windows) |
    |---|---|---|---|---|---|---|---|
    | `GOW_SOUND_THREAD=0` (inline) | 1183 | 355 | 634 | 0 | 0 | 0 | 22.5 |
    | batch 128 (default) | 1178 | 955 | 33 | 256 | 169 | ~148k of 240k | 22.5 |
    | batch 64 | 1159 | 689 | 291 | 296 | 99 | ~189k | 22.3 |
    | batch 32 | 1097 | 583 | 343 | 335 | 67 | ~206k | 24.0 |

    On the A57 the IOP time on core 1 barely moves (-0 to -7 %): what the thread takes is replaced by inline mixing
    at the driver's sync() calls (`cpu=`) and by waits for a batch being mixed (~700-950 waits per 5 s at batch 128). Batch 32 is the best of the three (-86 ms per 5 s); fps differences are within the run-to-run noise
    (the bot fight varies by +-2 fps).
- Sample clock = the IOP cycles the EE granted (`IopEmulator::runCycles`), each call clamped to its grant: IOP work
  outside runCycles (EE-side RPC calls into IOP servers; a dbcman RPC at 0x12BCC costs ~10 M IOP cycles/s on the
  title screen and menus) and overshoot inside it move the IOP clock ahead of the EE but produce no samples.
- Output rate adaptation (`drainOutput`): the latency target is 100 ms (`kSpu2MaxLatencyFrames`). Above it the
  buffer is played up to 5 % faster with linear interpolation; at or below it the samples are copied unchanged. A
  buffer more than 4 targets above (after a stall) is cut back once. Measured 2026-10-13 (port/menuclock): title screen,
  main menu, pause menu and gameplay play at rate <= 1.002 with a steady buffer (IOP clock 1.000 +- 0.0004 of wall time;
  the ~3.7 % of 2026-10-12 no longer occurs); the Load Game menu at <= 1.004 (IOP clock 1.002).
- Former behaviour (until 2026-10-12): the output cut blocks of samples whenever the buffer exceeded the target.
  Combined with the clock above it dropped ~30 % of the samples on the menus: the audible crackle ("clipping").
  `GOW_AUDIO_STATS=1` prints requested/missing/dropped frames, the highest rate and the buffer level every ~5 s;
  `GOW_IOP_DRIFT=1` prints granted vs advanced IOP cycles and the top outside calls; `GOW_SND_REGLOG=1` logs the
  core register writes (MMIX, MVOL, EVOL, AVOL, BVOL).
- Not mixed yet: reverb (parameters stored), volume sweeps (target applied at once), gaussian interpolation.

## Verified (2026-10-11)
Desert zone with `GOW_AUDIO_PCM`: 158.9 s of audio in a 160 s run (former model 156.5 s), same level profile, same 2
short silences (port/audio_native1 vs audio_native2); zone ok at 60 fps; save test ok.

## Verified (2026-10-12, crackle fix)
Desert run (port/aud9): 0 dropped frames, 0 underruns, 0 saturated samples (was 2.5/s), 0 sample jumps > 12000 (was
1.6/s); per-10 s RMS matches the former recording where both cover the same part (2791/3160/3621 vs 2808/3163/3597).

## Output device
The sound engine plays on the system default output (raylib audio) unless a device is named: `GOW_AUDIO_DEVICE=<part of
its name>` or the first line of `%LOCALAPPDATA%\GodOfWarNativeudio_device.txt` (per user, so every instance and
worktree build follows it; e.g. `Oculus` for "Cuffie (Oculus Virtual Audio Device)"). The stream is then played on
that device through miniaudio directly (ps2xRuntime/src/lib/ps2_audio_device.cpp, raylib's miniaudio build: its
configuration macros must match raudio.c). No match: the log lists the devices and the default output is used.
`play.cmd` sets `GOW_AUDIO_DEVICE=default` (the system default output), so the per-user file only routes test runs
(user 2026-10-13: tests on the Oculus output, playing on the main output). `GOW_AUDIO=0` (zones.py --silent) still mutes;
a muted recording (`GOW_AUDIO_PCM`) never uses the chosen device.

## Sound groups and movies (2026-10-13)
- 989snd keeps every sound handle in a group (handle +0x17). The game pauses and continues groups through SMPD fn 0x10
  (`0x175148(pause, mask)` on the EE -> 989snd plugin call -> SMPD 0x66F4 -> 989snd export #67 pause / #68 continue),
  e.g. all groups while loading (mask 0x7FFF) and, when a movie starts, a per-movie mask (docs/fmv.md). 989snd pauses a
  sound by setting its voices' pitch and volume to 0 and a stream (type 4, 0x12398) by zeroing the voices of its
  channels, all through the LIBSD calls the engine serves.
- Checked with `GOW_SND_CALLLOG`, `GOW_SND_VOICEDUMP` and `GOW_AUDIO_RAW` (port/movies4): the movie stream is mixed
  sample-exact (gain 1.001 against the PSS track); under A01MP05 the voices of the group-1 stream keep playing, as the
  movie's mask asks.
- Recording without hearing it: `GOW_AUDIO=0` with `GOW_AUDIO_PCM` runs the output at volume 0; `GOW_AUDIO_RAW` writes
  the mix before the output's rate adaptation (the PCM file is after it, so it drifts up to 5 % against a source).
- A movie track mixed against the wrong L/R interleave sounds like a buzz but has the track's energy, so level and
  clipping checks miss it. `tools/movie_align.py <movie> <GOW_AUDIO_RAW file> [--ext PSW]` measures the byte offset of
  the PCM against the core input's 512-byte L/R blocks (0 = correct; A01MP02 after A01MP05 measured 676 until
  2026-10-16, docs/fmv.md).
