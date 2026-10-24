# Sound: the native sound engine

The game's sound engine is its own IOP code: 989snd (`989NOMID.IRX`, sequencer, voice allocation, streaming) with the
SMPD streamer plugin, compiled ahead of time like every IRX (docs/native-port.md). Below it there is no sound hardware
model: the SPU2 register-level model and the IOP's SPU DMA channels were removed on 2026-10-11.

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

### Handoff (nat-iop2, 2026-10-25)
- ext `agent/natiop2` (worktree ext/PS2Recomp-kl-natiop2, from agent/natiop 7953c07): 569d093 LIBSD native import
  library + heap-layout fix + sound trace. Build `out/build-pc` (RelWithDebInfo, logs off). Only ps2xIOP changed.
- Not started: native 989snd / SMPD code. Next, in order: decode the per-frame commands (41/42/4a/4b and SMPD fn
  1a/02/0b: wrappers in `tools/iop/snd989_rpcmap.py`, SMPD's plugin table is built at run time, read it from IOP RAM),
  then the SMPD front end natively with each RPC's reply and DMA checked against the oracle trace in the same run
  (shadow mode: run the IRX path, compare the native result), then bank loading (#129).
- Determinism: compare within one run (shadow) rather than two runs; two runs part after a few thousand trace lines.
- Switch: no console run; the change is IOP-side C++ only (no new platform code).

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
  (`GOW_CORE_SoundMix=<0-2>` moves it).
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
