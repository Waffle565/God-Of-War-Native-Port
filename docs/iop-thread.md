# The IOP on its own host thread (ext agent/sw-iopthr, 2026-10-19)

Why: on the Switch, core 1 carries the EE game code (~2.9 s per 5 s at ~28 fps gameplay) and, on the same thread, the
IOP (native IRX code, the IOP scheduler, services: ~1.2 s per 5 s). At 60 fps the EE work roughly doubles while the
IOP cost stays (it is paid per wall-clock second), so the IOP has to leave the EE thread
(docs/switch-deko3d.md "The EE thread's core"). Sound mixing already left (docs/sound.md "Mixer thread").

Code: `ps2xRuntime/src/lib/ps2_iop_thread.{h,cpp}` (`PS2IopThread`), hooks in `ps2_runtime.cpp` (every `PS2Runtime`
IOP entry point), `ps2_iop_host.cpp` (deferred EE effects), `Kernel/EeScheduler.cpp` (sync before idling), and
`IopSubsystem::takePendingIopCycles` / `runIopCycles` (ps2xIOP: `runPendingEeCycles` split in two halves).

## Design: the EE keeps the clock, the thread runs the batches

- **Who decides IOP time.** Unchanged: the EE accounts its cycles at checkpoints and the IOP gets 1/8 of them in
  batches of 512 IOP cycles (`accrueEeCycles`). Inline mode runs each due batch at once (`runCycles`, ~72000 calls a
  second). With the thread the EE thread takes the due batch (`takePendingIopCycles`) and queues it; the `IopRun`
  thread runs the queued batches in order, each with the same `runIopCycles(n)` call inline mode would have made at
  that point. So the sequence of IOP runs is identical; only the host thread and the moment differ.
- **Every EE call into the IOP is a sync point.** `PS2Runtime::{handleIopRpc, notifyIopSifTransfer,
  read/write/zero/allocate/freeIopMemory, loadIopModule(Buffer), stopIopModule, selectIopRpcAbi, canBindIopRpc,
  resetIop}` and the ELF-load reset open a `PS2IopThread::Exclusive` (drain): the EE sets a "wanted" flag, takes the
  execution mutex (the thread stops after its current batch), applies the effects the thread queued, and runs every
  batch still queued itself ("take-back", as the sound mixer does). The call then sees the IOP exactly where inline
  mode has it at that EE instant: all granted batches run, the partial batch not yet due not run. RPC server code,
  SIF DMA into IOP RAM, module starts etc. still run on the EE thread, as before. `iopDebugSnapshot` (debug panel,
  other thread) only pauses the thread.
- **IOP effects on EE state are deferred.** While the IOP runs on its thread, `PS2IopHostAdapter::writeGuest` /
  `zeroGuest` (IOP-side `sceSifSetDma`, the extra data of `sceSifSendCmd`) and `sendSifCommand` (IOP -> EE SIF
  command: guest malloc of the packet, `queueInvocation` of the EE handler) are copied into a queue; the EE thread
  applies it in order at its next full checkpoint (`advanceIopEeCycles`, at most one batch of EE time later: the
  checkpoint fast path never skips past the next IOP batch), at every sync point and before it idles. So the EE sees
  IOP writes only at checkpoints, as with inline mode, and SIF commands reach the EE scheduler on the EE thread.
  Reads of EE RAM from the IOP thread (sceSifSetDma's destination check, ELF loads) are direct. Memory card,
  CDVD and host files are IOP-owned (native mc2_d/sio2 overrides, DiscSource has its own lock); `memoryCard` /
  `audioCommand` host calls belong to HLE services reached only through RPC (EE thread).
- **Before the EE idles** (`EeScheduler::run`, no ready thread and no invocation) it drains the IOP (`syncIop`):
  inline mode had run those batches already, and a SIF command they send must be able to wake the EE now, not at the
  next vblank. If the drain queued an invocation, the EE runs it instead of sleeping.
- **Queue.** A single-producer ring of batch sizes (`m_ring`, 1024 entries; batches are >= 512 cycles and the lead is
  clamped, so it never fills). The EE writes batches into it and publishes them in groups of `GOW_IOP_WAKE` IOP
  cycles (default 4096 = 8 batches), at every sync point and before it idles: one cross-core handoff (and at most
  one wake) per group instead of per batch. The consumer is whoever holds the execution mutex (the thread, or an
  owner taking back). Counters written by different threads sit on their own cache lines.
- **Bounded lead.** The EE may run `GOW_IOP_LEAD` IOP cycles (default 65536 = 128 batches, ~1.8 ms of IOP time)
  ahead of the thread; beyond that it waits until the thread is half way back (`lead_waits`, charged to
  `guest_wait_ms`, not `iop_ms`). Most lead waits are the EE's idle catch-up: after an idle wait the EE accounts the
  whole wait as one big batch (inline mode ran it in one go on the EE thread; now the EE thread sleeps while the IOP
  thread runs it). So the IOP's effects reach the EE at most ~lead + one group later than inline (~2 ms of guest
  time); in gameplay there are almost none (`effects=0` per 5 s; 2-3 on the save screens).
- **Sleeping and waking.** An idle thread looks for work for `GOW_IOP_SPIN` microseconds (default 50, `std::this_thread::yield`
  between looks) and then sleeps on a condition variable; the EE notifies only when it publishes to a sleeping
  thread and clears the flag when it does (a thread slow to wake costs one notify, not one per publish: before that
  fix the PC run paid ~28000 notifies a second).
- **Fallback.** `GOW_IOP_THREAD=0` runs the IOP inline on the EE thread exactly as before (same code path as the
  sw-ee5 base). On the Switch the thread is pinned to core 0 (ThreadNaming default), `GOW_CORE_IopRun=<0-2>` moves it.
- **Statistics.** `GOW_IOP_THREAD_STATS=1` (or `PS2X_IOP_NATIVE_STATS=1`) prints every 5 s
  `[IOP:thread] thread_ms (batches, cycles, sleeps) | ee: held_ms lock_wait_ms takeback_ms (cycles) syncs lead_waits
  (ms) | grants (cycles) wakes effects max_lead`: thread_ms is the IOP thread's busy time, the `ee:` part what the
  IOP still costs the EE thread (exclusive scopes including the calls they guard, waits for the execution mutex,
  batches taken back). `[IOP:native]` keeps reporting the IOP's own time (now mostly on the thread). The `iop_ms` of
  `GOW_PERF_DIAG` remains the EE thread's IOP time (the thread charges no perf bucket); queuing a batch has no perf
  scope (two clock reads would cost more than the queuing), lead waits go to `guest_wait_ms`.

## Why not ...
- **A bigger inline batch:** 2048 IOP cycles per batch cut the call overhead to a third but the memory card load from
  slots 1-4 never finished (sw-ee4): IOP state at the EE's calls changed. The thread keeps the IOP state at every EE
  call identical to inline mode.
- **A producer/consumer split at the RPC level** (RPCs queued to the thread): EE-side `sceSifCallRpc` is synchronous
  in this HLE (the server runs inside the call), so the EE would wait for the thread on every call anyway; the batch
  queue moves the part that is not synchronous (the IOP's own threads, timers, sound driver, idle loops).

## Results (PC, ext agent/sw-iopthr d404373, Release, same exe with and without the thread)
- Zones gate `port/iopthr/gate/20261019_143936` (thread): slots 0-4 ok, 60 fps, game speed 1.01, no capture flagged
  against port/swee5/gate/20261019_111818; slots 1-4 load their saves from the card. Inline control with the same exe
  (`GOW_IOP_THREAD=0`): `port/iopthr/gate-inline/20261019_150207`, also 5/5 ok.
- savetest (`port/iopthr/savetest`): 129 pages changed, card directory intact (same as sw-ee5). The save/load menus
  sync ~2100 times a second (menu RPC polling) against ~120 in gameplay; take-back stays ~10 ms per 5 s there.
- tools/movies.py --only SEMO01 (`port/iopthr/movies/20261019_150025`): 1410 pictures at 30/s, 46.0 s audio, A/V drift
  0.05 s, no silence, clipping, repeats or drops: identical to sw-ee5.
- IOP time on the EE thread (`iop_ms` of GOW_PERF_DIAG, mean of the last 8 windows of 5 s, slots 0-4):
  inline 281 / 265 / 248 / 168 / 237 ms, thread 32 / 27 / 19 / 20 / 23 ms (-90 %). What is left on the EE thread is
  the exclusive scopes (`held_ms` 22-37 ms: the RPC calls themselves, ~600 per 5 s in gameplay, which run the server
  code inline as before), the effects check and a few hundred take-back cycles. The IOP thread is busy
  `thread_ms` 175-320 ms per 5 s (the IOP's own time `[IOP:native] total` 150-295 ms plus the per-batch clock of the
  stats). The game load on this shared PC varied between runs (other agents' builds), so the EE game-code time
  (`ee_ms`) is not comparable run to run; in the back-to-back slot 2 pair `port/iopthr/p6` the EE thread's busy time
  (ee + iop) was 1275 ms with the thread vs 1625 ms inline.
- Per 5 s of gameplay: ~360000 batches granted, ~45000 groups published, 7000-9000 wakes of the thread, 750-1100
  lead waits (nearly all the idle catch-up), `effects` 0.
- devkitA64 `-fsyntax-only -D__SWITCH__` (flags of the Switch build) on every touched file: ok. Not yet run on the
  console.

## For the console
- Build as usual (`switch/build.sh` in WSL); nothing to configure. The thread starts with the first IOP batch,
  pinned to core 0 (`GOW_CORE_IopRun=<0-2>` in env.txt moves it), and `GOW_IOP_THREAD=0` restores inline mode for an
  A/B on the same build. `GOW_IOP_THREAD_STATS=1` prints the `[IOP:thread]` line (tools/switch_log_summary.py does
  not parse it yet).
- Expected on core 1: the ~1.2 s per 5 s of IOP + leftover sound work on the EE thread drops to the RPC calls and
  the sync bookkeeping (PC: -90 % of `iop_ms`), i.e. roughly 1 s per 5 s of core-1 time freed at the current frame
  rate, which the EE and the GS frontend can use. Core 0 takes it instead (with deko3d, SoundMix and the
  presentation): watch the deko3d draw throughput in fights; if core 0 is full, try `GOW_CORE_IopRun=2` (VU1 core)
  and `GOW_IOP_SPIN=0` (no spinning before sleeping: fewer cycles burnt on a shared core, more wakes).

## Console (ext agent/sw-core1, 2026-10-19)
- The Switch default is inline (`GOW_IOP_THREAD=0`, ps2_switch_runtime.cpp). Measured variants and fps:
  docs/switch-deko3d.md "Core 1: GS stage fast path and the IOP thread's priority". In short: `IopRun` on core 0 at
  the default priority waited behind the deko3d thread (Horizon does not time-slice equal priorities), so it now
  runs one step above it (ThreadNaming.h; `GOW_PRIO_IopRun=<0-9>` changes it); with `GOW_IOP_SPIN=0` and
  `GOW_IOP_LEAD=4194304` (the ring has 16384 entries, so the lead clamp is ~8.4 M cycles) the EE's lead waits fall
  to ~0 and its IOP time to 0.1-0.25 s per 5 s, but the frame rate is 1-3 fps below inline: core 1 is not the
  limit any more (the EE waits for VU1) and the deko3d thread slows with `IopRun` beside it. Core 2 is worse.
- tools/switch_log_summary.py prints the `[IOP:thread]` line (thread_ms, sleeps, held/takeback, lead waits).

## Open (handoff)
- The RPC calls (~120 a second in gameplay, ~2100 on the save/load menus) still run their IOP server code on the EE
  thread; an asynchronous RPC (`sceSifCallRpc` with the nowait flag) could go to the thread, a synchronous one
  cannot without the EE waiting anyway.
- Switch core 3 (2026-10-24, docs/vu1-gpu.md sw-vu1k4 / sw-vu1k5 handoffs): the npdm now has `highest_cpu_id` 3, so
  `GOW_CORE_IopRun=3` works (ThreadNaming accepts 0-3); the GS stage sits on core 3 by default. The IOP thread there
  (`GOW_IOP_THREAD=1 GOW_CORE_IopRun=3 GOW_IOP_LEAD=4194304`) is ~1.1-1.2 s busy per 5 s in a fight and takes the EE
  core's iop_ms from ~800 to 150-220 ms, no lead waits, no extra audio underruns (`GOW_AUDIO_STATS`) or hitches; but
  with the fight VU1-bound it measured 0.7-2.4 fps lower in two pairs of bot fights (42.6 vs 40.2, 42.3 vs 41.6), so
  inline stays the default. Turn it on once core 1 limits (60 fps needs the EE's time).
