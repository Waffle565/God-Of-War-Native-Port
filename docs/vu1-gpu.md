# VU1 vertex loops off the exact path: CPU HLE kernels first, then the Switch GPU (ext agent/sw-vu1gpu)

User decision (2026-10-19): move the dominant VU1 vertex program to the Switch GPU (deko3d); not bit-exact, it must
look the same (pixel comparison against the CPU path, tolerances documented). Plan: (1) understand the program,
(2) a plain-float CPU reference (HLE) compared with the exact path on the recordings, which also says whether a fast
CPU HLE alone reaches the target, (3) GPU path, (4) console fps / per-core busy / screenshots.

## Handoff (2026-10-24 night 9, sw-vu1k5: core 3 as a resource; in progress)
- **npdm `highest_cpu_id` 3 is the default now** (port/switch/nsp/gow.json): the application may pin threads to core 3.
- **GS-limited fight phase (165-200 s, gsq_wait ~1100 ms) = the GPU, not a CPU core.** In kd3's log that phase has
  `[gs-dk-time] present_submit_ms` ~0.7-1.1 s per 5 s (`dkQueuePresentImage` blocks, ~4 ms per present; 150-165 s: 60 ms),
  while draws and vertices are *lower* than in the VU1-bound windows. Same package with `GOW_GPU_MHZ=max` (handheld
  307 -> 460.8 MHz, run `s3g`): present_submit 45-53 ms in every window, gsq_wait ~6 ms, **hitches (frames >= 2.3
  fields) 8-15 -> 0-1 per 5 s**, and the fight is VU1-bound throughout (40-44.6 fps, vu 4.1 s). No core can absorb it;
  the GPU clock (or less GPU work) does. Not made default here (handheld battery/heat: the user's call, see
  docs/switch.md clocks).
- **GSStage on core 3** (`GOW_CORE_GSStage=3`, run `s3a`, kd3c3 package): the deko3d thread's busy time 3.7 -> 2.2-2.6 s
  per 5 s (on core 0 the stage, one priority step above, preempted it: deko3d's wall-clock busy included that);
  fight 165-200 s 36-41 -> 38-46 fps (gsq_wait 1100 -> 500-800 at the default GPU clock), 150-165 s unchanged (VU1);
  title 60.0, vu_ms 3635 -> 3718 (+2 %: the stage reads the queue from another core). Made the default (ext ThreadNaming.h).
- **VU1 programs on their own thread with a spinning VIF producer on core 3** (`PS2X_VU1_ASYNC=1`, new
  `PS2X_VU1_ASYNC_WAIT_SPIN_US` = the worker spins for the program's end instead of 4096 pauses + condvar, VU1Run spins
  for the next job, package `update_ka1`): **slower**. Worker alone on core 3 (`as1g`): title 53.4-54.4 fps (60), fight
  35.4-38.2 (40-44); worker + stage on core 3 (`as2g`): title 44, fight 28-35. The VU1 thread's execute time per run is
  unchanged (18.1 us) but a run cycle takes ~27 us instead of ~20: 176838 of 183512 MSCALs wait for the previous program
  (the VIF work between two MSCALs is ~2 us, so the worker is always at the next MSCAL first) and each hand-off costs
  more than the overlap. Dropped (kept as an option).

## Handoff (2026-10-24 night 8, sw-vu1k4 stopped here: ext agent/sw-vu1gpu 19da740 + generator eb1bf2a blocks (`/root/vu1pregen-gen3`), package `update_kd3`)
- **Re-profile (kc3, fight 150-180 s, line level: the Switch ELF has `-g`, so `switch_pcprof.py --lines FUNC` works
  without a special build)**: the dispatch / bookkeeping share was mostly small fixed costs per call, not the loop logic:
  hleChain's samples sat ~45 % on `fetch_add`s of the kernel stats (LDXR/STXR loops on the A57) and the guard checks of
  three function-local statics; finishXgkick ~54 % on the two `gsw` counter adds (those counters shared cache lines
  with the EE thread's `[gsw-chain]` counters: false sharing across cores); kCullClip ~28 % on `macN % 5` ring
  indexing; per XGKICK packet two `std::this_thread::get_id()` (pthread_self -> __syscall_thread_self, ~0.6 %);
  codeChanged compared and copied all 16 KB of micro memory on every code generation.
- **memmove (2.0 %) callers** (new `GOW_PC_SAMPLE_LR`, docs/diagnostics.md): 1.7 of the 2.0 points are
  `ps2_gs_stage::push <- GifArbiter::submitAndDrain`, the copy of each XGKICK packet into the GS stage queue (memcpy and
  memmove are one function in newlib). The copy is the hand-off to the stage thread; not removable without the stage
  reading VU1 memory that the next program overwrites.
- **Changes (ext 19da740, all exact: x86 replay dumps of fight.rec / menu.rec, HLE=1 and HLE=0, DIRECT=1 TRACES=1,
  byte-identical to 529de89 and to a61de30, cycles included)**: stats as relaxed load + store (one writer), no static
  guards in hleRun / vu1hle::report (clock read every 64th call), one `hleStop` byte test per runNative dispatch, perf
  counters on their own cache lines (`ps2_perf::PaddedCounter`), kCullClip rings of 8 (`& 7`), VIF1 diagnostic env
  reads out of the per-UNPACK path, PS2Memory records each VU1 code generation's changed pair range
  (`markVU1CodeModified(offset, size)`, `vu1CodeChangedRange`: MPG passes its range, other writers the whole memory) and
  codeChanged compares / copies only the union since the slots' generation, thread_local role flags for
  `onGsWorkerThread` / `ps2_gs_stage::onStageThread`.
- Console (handheld, bot fight, sampler env `c/env_pcs.txt`; title = 10-20 s at the 60 fps cap, so its vu_ms is the
  stable per-frame cost; fights differ per run):
  | package | change | title fps / vu_ms per 5 s | fight fps | VU1 thread (fight) |
  |---|---|---|---|---|
  | `update_kc3` (before) | a61de30 | 60.0 / 3775-3780 | 38.6-42.8 (150-180 s, avg 40.6) | dispatch 14.0 %, kCullClip 4.9, VIF 8.8, codeChanged 2.05, idle 6.9 |
  | `update_kd1` | stats / guards / padded counters | 60.0 / 3704-3706 | 39.6-43.2 (150-170 s, avg 41.7); died at ~205 s | dispatch 11.7 % (hleChain 4.1 -> 2.1) |
  | `update_kd2` | + kCullClip & 7, hleStop byte, VIF env | 60.0 / 3712-3720 | 39.6-40.8 (155-185 s, avg 40.3) | dispatch 11.4, kCullClip 4.1, VIF 8.3 |
  | `update_kd3` | + code-change ranges, thread_local roles | 60.0 / **3631-3641 (-3.8 % vs kc3)** | **43.0-45.4 (150-165 s, avg 44.2)**, 36-38.6 (165-185 s, gsq_wait ~1100 ms = GS/GL bound), 41.4-46.0 (185-215 s) | dispatch 10.4, codeChanged 1.56, idle 9.3, blocks 35.2, kernels 16.7, VIF 8.4 |
- **VIF unpack on another core (assessed, not built)**: VIF decode + unpack is 8.4 % of the VU1 thread (~0.42 s per
  5 s); that is the most an overlap can remove. The hardware order needs the VIF side to stop at each MSCAL until the
  previous program ended (~50 k runs per second in fights, ~16 us each), so producer and consumer meet ~50 k times a
  second; a condition-variable hand-off costs more than the ~2 us of VIF work per run (as PS2X_VU1_ASYNC=1 measured),
  so both sides would have to spin, and core 0 has ~1.8 s per 5 s free, not a spinning core. Only worth a prototype if
  core 3 is usable (next item).
- **Core 3 is usable** (`update_kd3c3` = kd3's program + an npdm with `highest_cpu_id` 3, scratchpad `c/gow_c3.json`;
  env `c/env_c3.txt`: `GOW_IOP_THREAD=1 GOW_CORE_IopRun=3 GOW_IOP_LEAD=4194304 GOW_PC_SAMPLE_CORE=3`, sampler on IopRun
  too): the log shows `thread pinned to core 3` (no "failed") and the IOP thread runs there ~22 % busy (78 % of its
  samples wait); the EE thread's waits go 19 -> 33 % of its samples, iop_ms on core 1 ~900 -> ~200 ms per 5 s. Title 60.0
  (vu_ms 3635), fight 42.8-43.8 (155-165 s), 33.6-39.4 later (gsq_wait 700-1500): unchanged, the fight stays bound by
  the VU1 thread (vu_ms 4110-4130). Not made default (gow.json still says 2; no gain for the fight yet, and the IOP thread
  changes event timing); it is the place for a spinning VIF producer or a VU1 helper (GOW_VU1_SPLIT measured +24 % with
  its helper on core 0, never on a free core).
- **Next levers** (kd3 fight, VU1 thread): blocks 35 %, kernels 17 % (kCullClip 5.6: its writer stores and blends;
  kUnlitSt / kLitSt / kSkinLitSt are load/store bound), dispatch 10.4 % (runNative's `sl.known` load = the slot's cache
  miss is 13 % of runNative), VIF 8.4 %, per-run entry/exit ~6 % (run / execute / flushPipelines / exportFlags /
  syncInflight / importFlags / the MSCAL lambda and std::function hops), kick hand-off ~4.5 % (memmove 1.7, finishXgkick
  1.7, push / stage); idle 9 %. Structural options with core 3: (1) VIF decode + unpack on a core-3 thread that spins,
  one MSCAL ahead (cap ~8 %); (2) GOW_VU1_SPLIT helper pinned to core 3 (`GOW_CORE_VU1Split=3`), measure; (3) the
  kick hand-off copy done by the stage thread from a double-buffered VU1 output region.
- Console left on DBI's MTP screen with `update_kd3` installed (kd3 npdm, core mask 0-2); env.txt and pcs_*.txt deleted
  from the SD.
- Tools (scratchpad): `ln.sh <pkg> <lo> <hi> FUNC...` (per-line profile), `chk.sh <harness>` (x86 harness from the
  worktree with gen3's blocks, dumps vs h5 = 529de89 and k3g = a61de30), `bk.sh <pkg> <harness>` (package + chk),
  `c/cycle4.sh` (cycle3.sh + a title screenshot at 15 s), `neon.sh` (arm_neon.h line -> intrinsic). WSL: call scripts by
  path (`wsl -- bash -c '... $VAR ...'` loses `$VAR`: the outer shell expands it).

## Handoff (2026-10-23 night 7, sw-vu1k3 stopped here: ext agent/sw-vu1gpu a61de30 + tools/vu1/vu1n_gen.py eb1bf2a, package `update_kc3` installed)
- **Kernels entered from direct chains** (dcbc523): chainStep, at a pc whose `hleStop` byte is set, calls
  `VU1NativeAccess::hleChain`, which runs the kernel there itself (shared `hleRun`, the same code runNative runs between
  two blocks: stats, the one history entry, `arraysReadyBy`, Q validity), then any kernel at the pc it returns, then
  sets the next block up as a chain successor (runNative's order: stop / budget test before each kernel, the slot must
  be set up and hold a function, else a stub slot returns the pc to runNative with kVu1nFinished). A decline at the
  first kernel lets the chain go on into the block (as runNative's fall-through). Bisecting and GOW_VU1_SPLIT keep the
  old hand-over. x86 replay dumps (fight.rec, menu.rec; HLE=1 and HLE=0, DIRECT=1 TRACES=1) **byte-identical to
  529de89, cycles included** (scratchpad `hb5.sh` / `hr5.sh`, baseline harness `h5` built from `git archive 529de89`).
- Console `update_kc1` (bot fight 150-190 s, sampler env): fight 38.4-44.8 fps (avg ~42; this run's fight had other
  enemies than sc5's, which died at 190 s: part of the gain is the scene), title 60.0 (10-20 s) / ~50 (25-65 s, as sc5),
  vu_ms 4160-4210 per 5 s (83-84 %, unchanged); VU1 thread: runNative 13.1 -> 9.0 % + hleChain 3.0 % = dispatch
  12.0 % (-1.1 points); kCullClip 6.5 %.
- **Soft callouts no longer force the memory form** (generator eb1bf2a + ext a61de30): `callout_kind` in
  tools/vu1/vu1n_gen.py: OPMULA / OPMSUB and the EFU ops incl. WAITP (lower specials 0x70-0x7E) are "soft"; a block
  with only soft callouts keeps its registers in locals (and gets fast variants: 704 -> 713). `NATIVE_CALLOUTS`
  (default): OPMULA / OPMSUB = the MUL / MSUB emitters on swizzled operands (`_MM_SHUFFLE(3,0,2,1)` / `(3,1,0,2)`,
  lanes outside DEST zeroed so maddc's all-lane overflow test sees only DEST, as the exact engine computes; a w in
  DEST keeps the callout since the exact engine writes w = 0) followed by `VU1NativeAccess::opmFlags` (the callout's
  other effects: the MAC entry joins the ring at the block's start cycle as importFlags put it, status current +
  sticky with OPMSUB's product bits, the commit of the FDIV / EFU values in flight = `calloutCommit`); EFU / WAITP go
  through `VU1NativeAccess::efuOp` (execLower's EFU cases moved into `VU1Interpreter::execEfu`, run on the local VF
  value, then calloutCommit). `VU1N_NATIVE_CALLOUTS=0`: the callout stays, registers stored / reloaded around it
  (`Regs.callout`, with a `synced` set so a register is stored once). Q is stored / reloaded around the helpers when
  the block wrote it (a callout's commit can put an older FDIV result back into s.q). x86 replay dumps
  byte-identical to 529de89 (fight: b1748 runs 3050 times in the replay).
- Console (bot fight, sampler env; fights vary per run, see the windows):
  | package | change | fight fps | title 10-20 s | VU1 thread |
  |---|---|---|---|---|
  | `update_kc1` | dcbc523 hleChain | 38.4-44.8 (150-190 s) | 60.0 | runNative 9.0 % + hleChain 3.0 %; vu_ms 4160-4210 |
  | `update_kc2` | + spilled soft callouts (gen1) | 37.6-38.6 (150-170 s), 45.6-48.0 (175-200 s) | 60.0 | b1748 2.26 -> 1.70 %, b0250 1.27 % |
  | `update_kc3` | + native soft callouts (gen3) | 38.6-42.8 (150-180 s, avg 40.6), 33.8-41.8 (180-200 s, gsq_wait ~1000) | 60.0 | exact-engine callout cost 6.0 % -> 2.3 % (execEfu 0.9, opmFlags 0.5, efuOp 0.5; normalizeFmacResult / __extendsftf2 / execUpper gone); vu_ms 4060-4100 |
- **Where runNative's time goes (kc3, annotated disassembly, scratchpad `fdis.py`)**: spread over the dispatch loop
  (slot loads `sl.known` 214 samples = cache misses on the 128-byte slots, hist stores, the hle tried / kernel bytes):
  per-dispatch cost, not one hot spot. **Half the dispatches are blocks that return without chaining** (replay
  GOW_VU1N_BLOCKPROF: 49 k of 97 k on fight.rec have 0 cycles at the blockprof point = no chain step): JR exits
  (e.g. 0x530, the subroutine 0x350 calls; the 0x1238 dispatch table). The generator's `--jr` (JR / JALR exits chain
  to their profiled targets, then to any block; off since "measured no gain" in an older state) takes fight.rec's
  runNative dispatches **97201 -> 11649**, dumps byte-identical (gen4 = gen3 + `--jr`, package `update_kc4`).
- **`--jr` is slower on the console, not made default** (`update_kc4` = gen4): VU1 thread categories (fight 150-180 s,
  scratchpad `cat.sh` over pp.sh output) kc3 -> kc4: dispatch (runNative + hleChain) 14.1 -> 6.7 %, **generated
  blocks 33.3 -> 43.7 %**, kernels 16.0 -> 14.5, idle 6.9 -> 6.2; title 10-20 s **59.2-59.4** (kc3 60.0, vu_ms 4100 vs
  3780), fight 34.8-38.0 (bot died at 195 s, as sc5). A chain step inlined in thousands of block functions (timing,
  history, successor set-up, an indirect call) costs more than runNative's one hot loop doing the same per dispatch
  (i-cache / branch-predictor footprint), so removing dispatches by chaining more does not pay; the per-dispatch
  work itself (finishBlockTiming / entry timing / hist / slot loads, cache misses on the 128-byte slots) is what
  costs. `--jr --no-jr-any` (predicted targets only) leaves 33437 dispatches on fight.rec, dumps identical, not
  measured on the console.
- **Console package recipe (the state measured as kc3)**: ext agent/sw-vu1gpu a61de30 + the vu1n blocks of the
  default generator command (`python3 -I tools/vu1/vu1n_gen.py <dir>/vu1n_generado port/vu1_micro --heads
  port/vu1n_heads.txt --parts 6 --locals`, no `--jr`) next to vu1pregen-vgpu's vu1_generado: in WSL
  `/root/vu1pregen-gen3` (scratchpad `gen.sh <name>` + `mkpre.sh <name>` + `PRE=/root/vu1pregen-<name> bp9.sh
  <pkg>`). **The ext runtime a61de30 needs blocks from eb1bf2a's generator or older ones; blocks from eb1bf2a need
  a61de30** (efuOp / opmFlags). `/root/vu1pregen-vgpu` still holds the old blocks (pre-eb1bf2a, also fine with
  a61de30: they only use callUpper / callLower).
- Next levers (VU1 thread kc3, fight): runNative + hleChain 14.1 % = per-dispatch bookkeeping, not the loop: make
  the per-block timing work cheaper (the slots' first cache line, hist ring writes, entry-timing memo) rather than
  chaining more; kCullClip 4.8 %; VIF decode/unpack 8.7 %; codeChanged 2.0 % + slotFor 1.0 % + setupTrace 0.5 %
  (code swaps); memmove 2.2 % (callers unknown: the sampler keeps only the PC; a build with `-g` would allow
  `switch_pcprof.py --lines`); idle 6.9 %.
- Tools (scratchpad): `hb5.sh <name> [src]` (x86 harness, `GEN=` generated dir), `hr5.sh <name> <base>` (dumps
  vs a baseline harness, HLE=1/0, DIRECT=1 TRACES=1; baseline `h5` = 529de89 from `git archive`), `gen.sh` / `gen2.sh`
  (`GENARGS=`) + `gcmp.sh` (generated dirs, CR ignored), `mkpre.sh`, `bp9.sh` (`PRE=` pregen dir), `bpf.sh` + `dsum.sh`
  (replay runNative dispatch counts: a dispatch with 0 cycles at the blockprof point returned without a chain step),
  `fdis.py` (sample counts on a function's disassembly), `cat.sh` (VU1 thread categories), `c/cycle3.sh` (cycle2.sh
  that retries HOME after the fight until the HOME menu screenshot shows).
- Console left on DBI's MTP screen with `update_kc3` installed; env.txt and the pcs_*.txt deleted from the SD.
- Console note: the HOME click after the fight did not register (game kept running ~10 min); `c/recover3.sh <name>`
  (NOHOME=1 when the HOME menu is already up) closes the game, opens DBI's MTP and pulls the log. HOME toggles: check
  the screenshot (hbmenu/HOME menu jpg ~190 KB, game ~95 KB) before pressing again.

## Handoff (2026-10-23 night 6, sw-vu1k2 stopped here: ext agent/sw-vu1gpu 529de89, package `update_sc5` installed)
- New kernels in ps2_vu1_hle.inl (all checked with the x86 harness on fight.rec + menu.rec against HEAD with
  `GOW_VU1N_DIRECT=0`, scratchpad `hx4.sh` = hx3.sh without REBUILD, `cmpx.sh` = cycle sum + hlecmp):
  - `scat_b / scat_bw / scat / scat_w` (5d66edb): the skinned module's batch-front scatter loops (qwords to the
    offsets of packed lists, 2-6 pair blocks). **Byte-identical dumps, cycles included** (each native block's idle
    table: head 4, tail 3/2, pair-4 block 2, reload 12/11; native blocks do not stall across block starts unless
    they read entry flags). `Kernel::keepsQ`: kernels without DIV/EFU leave an FDIV/EFU entry valid.
  - `cull_clip` (52f0fc9, a3d13d6, db7ebfd, 529de89): the vertex module's whole clip-flag culling strip loop
    0cd8..0e10 (B1, B2 with fmand, path A / B with fcget, the accept / reject / partial exits), entries 0cd8 and
    0d00. Values exact (data, GIF, the whole VU1 state incl. MAC / clip flags on all 4000 records); the flag reads use
    the writers the blocks' timing gives them (fmand: B1's addz.w; fcget: B2's second clipw on path A, path B's own
    second clipw). **Not cycle-exact**: +91 k VU cycles over 336 calls on fight.rec (~3 % of the loop, always above
    native): the native traces (TRACES=1) time the later parts on idle tables, which I could not reproduce block by
    block (B2 = 7 and path A = 9 fit best; the rest of the residual is in the trace timing). Uses the generated
    blocks' fast variants where their Bounds checks pass (ORed masks, NEON clip flags: movemask is slow under
    sse2neon), MAC writers kept raw in the ring (flags computed on read).
  - runNative after a kernel (52f0fc9): one history entry stands for the kernel (all lanes ready at its start: same
    timing as the empty history before), so the next block's entry timing can come from the predecessor memo.
- Merged agent/sw-core1 7da6cae (IOP idle-batch deferral, import-stub cache): `idle_defer ... violations=0` in all runs.
- Console (handheld, bot fight, env `c/env_pcs.txt`; fight windows 150-190 s; bot fights vary +-2 fps per run):
  | package | ext | fight fps | other | title | VU1 thread |
  |---|---|---|---|---|---|
  | `update_snd1` (before) | a26a44a | 31-37.4 (35-37 from 165 s) | | 51-60 | blocks 29.6 %, runNative 7.6 % |
  | `update_sc1` | 5d66edb (scatter) | 34.8-36.8 | | 60.0 | scatter blocks 5.6 % -> kernels 0.7 %, runNative 11.0 % |
  | `update_sc2` | 7a78411 (cull_clip exact helpers + core1 merge) | 35.4-37.6 | deck after the fight 41.4-42.8; iop_ms 820-1076 | 60.0 | kCullClip 7.4 % (costlier than the blocks) |
  | `update_sc2` + `GOW_IOP_THREAD=1 GOW_IOP_SPIN=0 GOW_IOP_LEAD=4194304` | | 28.6-35.0 (avg 33.7) | iop_ms (core 1) 160-250 | 60.0 | **slower: not made default** |
  | `update_sc4` | db7ebfd (entry 0d00, raw MAC) | 36.2-37.8 | deck 44.2-46.4 (+7 % vs sc2) | 60.0 | blocks 18.1 %, kCullClip 8.3 % |
  | `update_sc5` | 529de89 (NEON checks) | 35.4-37.2 (avg 36.6) | died at 190 s | 60.0 | kCullClip 4.3 %, runNative 13.1 %, vu_ms 4140-4180 per 5 s (83 %), idle 6 % |
  The fight stays at ~36 fps: the VU1 thread is still ~83 % busy and runNative's own dispatch loop grew with every
  kernel (7.6 -> 13.1 %: each kernel call is one more runNative dispatch where the blocks used to chain directly).
- Not done (next, in order): (1) cheaper kernel dispatch: let chainStep / chainDirect call a kernel at a hleStop pc
  itself instead of returning to runNative (the dispatch loop is now the largest single item, 13 %); (2) native
  codegen in tools/vu1/vu1n_gen.py for the exact-engine callouts: OPMULA / OPMSUB are MUL / MSUB on swizzled operands
  in the exact engine (ps2_vu1_core.cpp: crossLeft / crossRight, w lane 0), so `emit_upper` can map them onto the
  mul / msub emitters with dest xyz (fall back for a w dest); ERLENG / WAITP / EEXP need EFU support in the generator
  (P value + `m_efu` timing entries like the FDIV ones). Blocks with a callout keep the memory form, which is why
  b1748 (2.0 %), b0250 (1.6 %) and b18c0 (0.6 %) are slow; (3) the batch front's L3/L4 loops (0x168 / 0x1e0 with a
  `bal` to the FMAC subroutine at 0x4c8) and L5 (0x2f0, `jalr` per list entry) have no kernel (they write MAC /
  status flags).
- Tools added (scratchpad): `fb2.py` (find a generated block's module in a .rec by its registry key: key = FNV over
  pc + pairs), `gf2.sh <fn>...` (print generated block functions), `gk.sh <key>` (registry line), `pp.sh lo hi
  pat,pat pkg...` (VU1 thread profile lines per package), `fit.sh` / `km.sh` (cycle sums vs HEAD per kernel mask).
- Console left on DBI's MTP screen with `update_sc5`; env.txt deleted from the SD.

## Handoff (2026-10-23 night 5, sw-vu1k stopped here: ext agent/sw-vu1gpu a26a44a, package `update_snd1`)
- State of the console (handheld, bot fight, all runs with the sampler env `c/env_pcs.txt`, ~1-2 % cost; the SD's
  env.txt has been deleted since; the console is on DBI's MTP screen with `update_snd1` installed):
  | package | ext | fight 150-200 s | title |
  |---|---|---|---|
  | `update_con2` (before) | 06b18e2 | 28-29 | 54-56 |
  | `update_pcs1` (sampler only) | + sampler | 27-30 | 55 |
  | `update_pcs2` | b5445ee (dispatch fixes) | 30-37 | 56.2-56.4 |
  | `update_pcs3` | 0183d57 (key table, caller sampling) | 31-35 | 56.2-56.6 |
  | `update_snd1` | a26a44a (sound hand-off) | 31-37.4 (35-37 from 165 s) | 60.0 first windows, then 51-58 |
  Bot fights vary by a few fps per run; the step over the 30 fps line is consistent in all three runs.
- Step order vs the directive: the PC profile showed that the dispatch costs were larger and cheaper to remove than a
  batch-front kernel, so those went first. Not built yet: the batch-front kernel (blocks at pc < 0x460 are **20.5 %**
  of the VU1 thread live), the clip-flag culling loop kernel (0c10-0e30 **13.7 %**, 1728-1808 2.6 %), the batch
  switch (0.8 %: smaller live than in the replay). The culling loop's fmand / fcget read MAC and clip flags with the
  stall-dependent timing the generated blocks take from `n.cyc` (see `b0d00_53ba0530` in vu1n_part3.cpp:
  `I14 = (cyc[3] - cyc[2] >= 4) ? m2 : ...`), and it has two paths per point (w < 0 / >= 0) plus three exits
  (accept, reject: ADC 0x8001 written back, partial: vi1 - 16384 and the 929.z flag); a kernel must reproduce which
  writer each flag read sees on both paths (replay dumps vs HEAD with DIRECT=0 are the check, as for the others).
- Other measured items (pcs3 fight, VU1 thread): exact-engine callouts ~5 % (OPMULA/OPMSUB upper callouts and
  ERLENG/WAITP lower callouts in `b1748_ff5616cb`, an EFU op at 0x288 in `b0250_b0fc2612`: native versions in
  tools/vu1/vu1n_gen.py would remove the callout + commit round trips), VIF decode/unpack ~6.5 %, codeChanged 1.6 %,
  commitReadyPipelines 1.4 %, runNative 7.8 %, kernels 6.7 %, idle 5.4 %.
- EE thread after a26a44a: 32.6 % of its fight samples wait in `EeScheduler::processDueDeadlines` (pacing / idle):
  core 1 has room now; the fight is bound by the VU1 thread (vu_ms ~4.3 s per 5 s).
- Tools: scratchpad `c/cycle2.sh` (cycle.sh + clears/pulls `pcs_*.txt`), `bp8.sh <name>` keeps `/root/elf_<name>.elf`,
  `hx3.sh <name>` (x86 harness exactness: HLE=0 dumps vs HEAD, HLE=1 vs HEAD with DIRECT=0; `/root/vu1h/vghead` =
  HEAD before this round), `k2.sh`-style calls of tools/switch_pcprof.py.

## Handoff (2026-10-23 night 4, sw-vu1k: live PC sampling, VU1 dispatch costs cut; ext agent/sw-vu1gpu b5445ee, `update_pcs2`)
- New tool `GOW_PC_SAMPLE` (docs/diagnostics.md): real A57 wall-time profile of the VU1 thread in the bot fight (pcs1 =
  con2 code + sampler, 145-195 s, 28-29 fps): generated blocks 45.6 % (spread: the top block, culling `b0d00`, 2.9 %),
  **runNative 13.3 % of which 42 % was the kernel matching** (byte-wise FNV of every kernel's region for every pc after
  every code change: `tried[]` was cleared on each of the ~1000 code changes per 5 s), VIF decode/unpack ~7 %,
  code-change handling ~5 % (codeChanged walked all 2048 128-byte slots; slotFor / setupTrace / the timing map),
  exact-engine callouts ~6.5 % (long double = software quad on AArch64 ~2.6 %), kernels 5.5 %, idle 4.6 %, clock
  reads 1.4 %, entry/exit ~2 %.
- Fixes (b5445ee, all exact: x86 replay dumps of fight.rec/menu.rec identical to HEAD with HLE=0, identical to HEAD
  with `GOW_VU1N_DIRECT=0` with HLE=1): kernel matches reset only near changed pairs and memoised by a hash of the pairs
  a match reads (`windowKey`); chains stop at untried/kernel pcs (`hleStopAt`), so the cull loops reached by direct
  chaining get their kernel again (with DIRECT=1 they had run natively: `cull=20/520` cycles instead of `80/104416`);
  codeChanged scans only the slots near the change plus a trace bitmask; `ps2_perf::nowNanos` reads `cntpct_el0`
  (`cntvct_el0` traps at EL0 on Horizon: first try crashed); normalizeFmacResult converts to long double lazily.
- Console (handheld, bot fight, `update_pcs2` + sampler env): **fight 150-200 s 30-37 fps (pcs1 27-30)**, title
  56.2-56.4 (55); runNative 13.3 -> 7.6 % of the VU1 thread. Bot fights differ per run: confirm with a second run.
- Remaining in the pcs2 profile: slot set-up after code changes ~3.8 % (slotFor 1.9, setupTrace 0.8, timing map
  1.1), codeChanged 1.5 %, hleMatch 1.2 %, exact callouts ~6 %, VIF ~7 %, blocks 48 %. EE thread (core 1):
  `svcArbitrateLock` 13 % of its samples = a contended host mutex (not identified yet: needs callers; the sampler only
  keeps the PC).

## Handoff (2026-10-23 night 3: GPU vertex path measured and dropped; no code change, ext agent/sw-vu1gpu 06b18e2, `update_con2`)
- **Measurement** (console, handheld, `update_con2` + env `GOW_VU1_HLE_TIME=1`, same bot fight, log `port/switch/logs/20261023_ht`):
  wall time inside the HLE vertex kernels per 5 s vs the VU1 thread's `vu_ms`:
  | scene | VU1 thread vu_ms | all kernels | per kernel (ms per 5 s) | fps |
  |---|---|---|---|---|
  | fight 150-190 s | 4420 | **~300 (6.7 %)** | unlit_st 107, lit_st 97, **skin_lit_st 91 (2.1 %)**, cull 0, unlit11 1 | 28.7 avg / 27.8 min |
  | title 15-35 s | 3905 | ~590 (15 %) | skin_lit28 326, unlit11 146, unlit_st 113, lit_st 5 | 54.3 |
  (fight: 173 k VU1 runs per 5 s = 25.5 us per run, of which 1.7 us in kernels; skin_lit_st ~80 ns per vertex, 1.13 M vertices per 5 s.)
  The qemu figure "kSkinLitSt 22 % of the VU instructions" was an instruction count: the kernels are dense NEON at high IPC, the rest
  of the VU1 thread is dispatch code (branches, loads, runNative round trips) at low IPC, so their wall share is a third of that.
- **Decision: the GPU vertex path does not pay; not built.** Its ceiling is the kernels' wall time: moving all of it to deko3d at zero
  CPU cost takes the fight's VU1 thread 4.42 -> 4.12 s per 5 s (30.9 -> 28.8 ms per frame; 60 fps needs < 16.7), i.e. below the game's
  30 fps step, no visible fps change; the title is not VU1-bound (core 2 78 %, 54-56 fps). And the cost is not zero (details in
  "GPU path" below): the backend reads CPU-side vertex positions per draw (render-target extents `usedW/usedH` that decide texture
  reinterpretation, `isZInversion`, the shadow-volume wrap groups' per-triangle boxes, Z flips), the VU program's GIF packets would
  need a by-reference vertex block through the GS stage and the frontend, and the CPU would still run the loops' control flow.
- **Where the VU1 thread goes (fight)**: 93 % is outside the vertex loops: the per-batch code (batch front pc 00d8/00e0/0458/0350/0240,
  clip-flag culling loops 0cd8..0df8 / 1728..17d8, batch switch 1258), the runNative dispatch (~20 per run) and the exact-engine
  entry/exit per run (~1200 runs per frame). That is control flow producing GIF tags, flags and XGKICKs: it cannot go to the GPU.
- **Next best levers (for the coordinator to pick)**, by expected size on the fight's VU1 thread (28.8 ms per frame outside kernels):
  1. **Batch-front kernel** (pc 00d8..0398: XTOP, matrix loads, normal length, bounding tests with MAC-flag reads): ~45 % of the
     remaining VU cycles and 4-6 dispatches per batch -> 1 (largest single CPU item; needs the MAC-flag/clip results exact: verify with
     `cmp.sh`/`hlecmp.py` on fight.rec and menu.rec as the other kernels).
  2. Kernels for the clip-flag culling loops (~19 % of the remaining cycles; must push the clip-flag ring at exit) and the batch switch
     (1258, ~9.5 %).
  3. Per-run overhead: exact-engine entry/exit (~6 % of instructions) and runNative's dispatch (~13 %).
  Even all of these together are unlikely to halve the VU1 thread; 60 fps also needs core 1 cut (EE 2.06 s + IOP 1.53 s per 5 s now:
  ~45-50 fps cap with VU1 free). Realistic target with 1+2: VU1 ~20 ms per frame, i.e. still the 30 fps step in fights unless the
  game's own frame pacing is changed (a 60 fps patch also doubles the EE and VU1 work per second).
- Console left on DBI's MTP screen; `sdmc:/switch/gow/env.txt` now holds `GOW_VU1_HLE_TIME=1` (harmless: two clock reads per kernel
  call; remove it or let the next cycle.sh run without an env file delete it).

## Handoff (2026-10-23 night 2: core 0 cut, vsync spin and VU1 helper assessed; ext agent/sw-vu1gpu 06b18e2, `update_con2`)
- **Core 0 (done, default)**: ext 06b18e2 (docs/switch-deko3d.md "Stream-buffer reuse"): the 8 MiB vertex-stream orphan reuses
  retired deko3d memblocks (was a dkMemBlockCreate/Destroy pair ~90 times per 5 s) and a CLUT load that leaves the palette unchanged
  no longer flushes the pending batch. Same bot fight 150-190 s: GL thread 2.57 -> 2.17 s per 5 s (stream 541 -> 255 ms, present
  280 -> 100 ms, draw calls 17.4 k -> 14.0 k), gsq_wait 79 -> 3 ms; core 0 total ~3.2 s per 5 s (~1.8 s free). fps 28.0 avg / 27.2 min,
  6 frames >= 2.3 fields per 5 s, title 55.4 (con1: 29.0 / 28.0, 1.2, 56.0): within run-to-run variance (this run had ~4 % more VU1 runs
  per frame); fps stays bound by the VU1 thread (core 2: gsw job 4.79 s, vu 4.42 s per 5 s = 96 %). Screenshots correct.
- **The `[gs-gl] clut` bucket was mostly draws**: the CLUT load itself is `[gs-hw-cpu] clut_ms` ~190 ms (10 us each); the rest of the
  44 us per op was the batch flush. Only ~15 % of the CLUT loads are unchanged palettes, so most flushes remain (the palette really
  changes per object). The remaining per-draw cost: draw_ms ~760 per 14 k draws (~54 us), Kicks op 0.8 s per 360 k strips.
- **Step 1 (vsync spin) is already in place**: `gowCountdownSkip` (gow_overrides.cpp, the two countdown back edges of 0x17A1D0 /
  0x17A2A0) jumps the countdown to the next scheduler deadline and the EE then sleeps in `processDueDeadlines` (condition variable,
  `guest_wait`). The profile's "39 % in the spin" are the IOP batches run at those checkpoints (iop bucket: real IOP work that has to
  run somewhere) plus the sleep (wait bucket); the spin's own code is ~3 %. Core 1's real slack is `guest_wait` ~1.3 s per 5 s, not 40 %.
  Nothing to change there.
- **Step 2 (coarse VU1 helper) assessed, not built**: earlier measurements already cover it: run-level speculation (`GOW_VU1N_REPLAY_LIVEIN`,
  docs/vu1-hle.md) gives the same output for only 78 % of consecutive run pairs (75 % with memory), so a helper running run k+1 early fails
  ~25-40 %; `PS2X_VU1_ASYNC=1` (VU1 programs on their own thread, VIF decoding overlapped) was slower (2 us VIF per 27 us run, every MSCAL
  waits); `GOW_VU1_SPLIT` (loop split) +24 %. A chain-level split has only ~2 chains per frame (`[gsw-chain]` chains=284 per 5 s) whose
  boundary is a run boundary with the same carried-state problem. Not worth a prototype; if someone wants a number first: per-chain
  vu_ms and whether chain 2's first run's live-ins match last frame's.
- **Next (per user decision 2026-10-19): the GPU vertex path** for the skin/lit kernels (kSkinLitSt is 22 % of the VU instructions; see
  "GPU path" below), judged by pixels. Core 0 now has ~1.8 s per 5 s free for the extra CPU-side work it needs. Core 1 (EE 2.05 s + IOP
  1.6 s + GS-stage share) caps ~45-50 fps even with VU1 free: 60 fps also needs EE/IOP time cut (EE 14 ms per frame, IOP ~5.5 ms at 60 fps).

## Handoff (2026-10-23 evening: log ring + GSStage core 0 = fights at 30 fps; ext agent/sw-vu1gpu ac30584, `update_con1`)
- ext ac30584 (on top of fast1 1b9a06d): GSStage thread on core 0 by default (`GOW_CORE_GSStage=1` restores core 1); stdout/stderr
  through a memory log ring written to the SD by a low-priority core-0 thread (`[switch-log]`); `[gsw-chain]`. Package
  `port/switch/nsp/out/update_con1`.
- Console, handheld, same bot fight: **fight 150-190 s 29.0 avg / 28.0 min** (fast1 24.0 / 22.2), frames >= 2.3 fields 1 per 5 s
  (was ~21); **title 51.5 fps** (47.6). `[switch-log]`: SD writes avg 4.8 ms, max 65 ms each, i.e. every log line used to stall
  the thread that printed it (EE speed lines every second, worker/stage/deko3d lines every 5 s): that was most of the hitches.
  With `GOW_EMC_MHZ=max GOW_GPU_MHZ=max` (opt-in, not default: battery/heat in handheld): title 57.0, fight 28.2 (no gain).
- Fight frames now sit at 1.9-2.1 fields (the game's 30 fps step). Budget per 5 s at 29 fps: VU1 thread (core 2) 4.35-4.44 s busy
  (~30 ms per frame: cap ~33 fps); core 0: deko3d 2.58 s (clut 0.82 s / 18.6 k ops, unnamed draw-op bucket 0.82 s / 360 k,
  texflush 0.32, vrun 0.29, present 0.28) + GS stage ~1.0 s = 3.6 s (72 %: cap ~40 fps); core 1: EE 2.1 s + IOP 1.6 s +
  pacing 1.3 s (a large part of ee+iop is the vsync spin with its IOP checkpoints, 39 % of EE samples in the profile), real
  game EE code ~14 ms per frame. EE chain walk+copy only ~110 ms / 5 s (~0.8 ms per frame): the EE->VU1 overlap prototype was
  dropped (<= 2-3 %).
- Next for 60 fps: VU1 needs ~2x AND core 0 needs ~1.8x. Recommendation: (1) VU1 work onto the core-1 slack (the vsync spin
  and pacing are ~40 % of core 1: make the spin sleep until the vblank event, then a coarse VU1 helper there, or the GPU
  vertex path for the skin/lit kernels if per-run splitting stays as unprofitable as GOW_VU1_SPLIT was); (2) core 0: the CLUT
  op (44 us each, ~128 per frame) and the per-draw op cost in the deko3d backend; the GS stage could share core 1 slack too.

## Handoff (2026-10-23 afternoon: console env tests, console agent)
- Same bot fight each run (cycle.sh PLAY=1 BOOT=45, 150 s), update_ee3, windows 150-190 s (later windows are often the death screen):
  baseline 23.2 fps avg / 22.0 min, 30 frames >= 2.3 fields per 5 s (`[gow-speed-hist]` bucket 23 is the cap);
  `GOW_CORE_GSStage=0` 25.8 / 22.4, 15 hitches; `GOW_IOP_THREAD=1 GOW_IOP_SPIN=0 GOW_IOP_LEAD=4194304` 24.4 / 22.7, 9 hitches;
  both 22.5 / 19.4 (core 0 overloaded: deko3d 2.0 -> 3.0 s / 5 s, gsq_wait doubles); GSStage=0 + IOP thread on core 1
  (`GOW_CORE_IopRun=1`) 24.2 / 17.4; GSStage=0 + `PS2X_GS_QUEUE_CHUNKS=32` 24.2 / 22.6 (gsq_wait 0.36 -> 0.29 s, no fps gain).
  Kept: GSStage on core 0 (now ext agent/sw-vu1gpu ac30584).
- update_fast1 (PIE, VU1 -O3, raw-FPCR div/sqrt) vs update_ee3, no env, console handheld (`[switch] mode handheld`, EMC
  1331 MHz, GPU 307 MHz): title 47.6 vs 47.3 fps, VU1 19.2 vs 21.0 us per run (-9 %); fight 150-190 s 24.0 / 22.2 vs 23.2 / 22.0,
  25.1 vs 25.7 us per run. fast1 + `GOW_EMC_MHZ=max GOW_GPU_MHZ=max` (EMC 1600, GPU 460.8 handheld): title 48.4, fight
  26.2 / 25.2 (150-250 s: 27.1 / 25.2, no death), deko3d 2.09 -> 1.80 s / 5 s, hitches 21 -> 18 per 5 s.
- `[ee-wait]` (update_ee3, `GOW_EE_SAMPLE`): in the fight ALL EE idle is cause 7 = pacing (1.0-1.6 s / 5 s), none is DMAC/vblank
  wait. With the VU1 thread 3.6 s / 5 s busy (~29 ms per frame) the VU1 path is the per-frame limiter; the EE gets ahead and
  sleeps in the pacing wait.
- `PS2X_GS_WORKER_TRACE` on the console is unusable as is: consecutive trace events from three threads are ~56 ms apart
  (only 30 events in 2 s). stderr is unbuffered to the SD file, so each line seems to block the printing thread for tens of
  ms. Fix prepared (85bd55d): stdout/stderr go to a 1 MiB memory ring that a priority-0x3B thread on core 0 writes every
  100 ms; `[switch-log]` every 30 s reports the SD write cost (avg/max ms). If confirmed, every speed line on the EE, stage or
  worker thread was a potential hitch. Also new: `[gsw-chain]` (EE time to walk+copy the DMA source chains before the worker
  job is queued, per 5 s).

## Handoff (2026-10-23 late night: cheap compiler/code fixes, package `update_fast1`; ext agent/sw-vu1gpu 1b9a06d)
Console-free round (the console was asleep): four audit items, each its own ext commit, verified off the console; they
need the console A/B (fight + title menu fps, `gsw-split` vu_ms per run, `[ee-prof]` not needed).
- e9daace + PIE fix **-fPIE instead of -fPIC** for `ps2_runtime`, `ps2_iop`, `libmpeg2` (Switch; plain non-PIC fails the
  `-pie` link: "read-only segment has dynamic relocations", vtables in .rodata): the cause was devkitPro's
  `Platform/NintendoSwitch.cmake:21` (`CMAKE_POSITION_INDEPENDENT_CODE ON`), not raylib (its line 69 is the Android branch).
  build.ninja: 121 `-fPIC` compile lines -> 7 (raylib only); before: `libps2_runtime.a` 3852 `ADR_GOT_PAGE` relocations,
  `libps2_iop.a` 1146, .got 0x13398 bytes; with -fPIE 2240 / 185, .got 0x12d28 (the rest are extern data, which -fPIE
  still reaches through the GOT). VU1 harness with the core files `-fPIC` vs not: +0.4 % instructions only
  (the VU1 core has few interposable calls); the gain, if any, is in the GS stage / IOP / runtime helpers (console only).
- 77976fe **-O3** on `ps2_vu1_core/upper/lower.cpp` and `vu1n_part*.cpp` (`PS2X_SWITCH_VU1_OPT`). aarch64 harness under qemu
  (`icount.sh`, 100 records, HLE+DIRECT+TRACES, minus the harness's own `silentNativeHash`): fight 2.36 -> 2.24 M insns per
  warm pass (-5 %; runNative -12 %, generated blocks -2.4 %), menu 4.17 -> 4.05 M (-3 %). Replay exact (400 records fight +
  menu, HLE off: 0 differ; HLE on: dumps byte-identical to the -O2 build). Generated EE code not tried (no harness for it,
  and 88 MB of text). `-fno-plt` is moot (static link, no PLT); `-fomit-frame-pointer` is already the -O2 default.
  The -O3 vu1n parts need more compiler memory: the 14-job build was OOM-killed once (cc1plus on vu1n_part4 beside the
  unity batches); resumed with `taskset -c 0-7` (scratchpad `bp8.sh`).
- d5c14fa **EE FPU div/sqrt** (`ps2FpuDivideNearest` / `ps2FpuSqrtNearest`, ps2_runtime_macros.h): on AArch64 one `mrs fpcr`
  + two `msr` around an asm `fdiv`/`fsqrt` (sse2neon did fegetround/fesetround calls + extra FPCR reads, 4 FPCR writes per
  divide); no write when the thread already rounds to nearest (only with `GOW_EE_FPU_NEAREST=1`: the EE runs RZ+FZ, so the
  two writes stay). Exactness: 10 M random + edge-case pairs (div and sqrt) under RZ+FZ and RN+FZ, old vs new bit-identical,
  FPCR restored (aarch64 test under qemu, scratchpad `fpt/`). qemu time for 1 M divides 194 -> 46 ms (instruction count
  only; the real cost is the FPCR-write serialisation, console only). `PS2X_FPU_CSR_SSE2NEON` brings back the old code.
- b0d4e2a + 82a4a63 **Clocks**: start-up line `[switch] mode docked|handheld; clocks (Hz, start -> now): cpu .. emc .. gpu ..`.
  `GOW_EMC_MHZ` / `GOW_GPU_MHZ` (MHz or `max`: EMC 1600, GPU 768 docked / 460.8 handheld) set those rates and the 2 s keeper
  re-applies them (a re-apply is logged); default stays the system's rates (not verified on hardware, so not on by default).
- env.txt: no package carries one (make_update.sh writes exefs only). The SD's `sdmc:/switch/gow/env.txt` still holds
  `GOW_EE_SAMPLE=200 GOW_CORE_EeSampler=1 GOW_PRIO_EeSampler=3` from update_ee2: the console agent must replace it
  (scratchpad `c/env_fast1.txt` = defaults, `c/env_fast1_clk.txt` = `GOW_EMC_MHZ=max GOW_GPU_MHZ=max`).
- A/B for the console agent: `update_ee3` (c7337ec, the same code minus these four; sampler off without the env) vs `update_fast1` (exefs main 49.3 MB, built 15:44) with env_fast1.txt, then
  update_fast1 with env_fast1_clk.txt; read the `[switch] mode` line first (docked or handheld decides the GPU max).

## Handoff (2026-10-23 night: EE thread profile, structural-lever study; ext agent/sw-vu1gpu c7337ec)
- New tool (ext c7337ec, `update_ee2`/`update_ee3`): `GOW_EE_SAMPLE=<hz>` starts a sampler thread that every 5 s prints `[ee-prof]` (exclusive guest
  function = the innermost function entered through `dispatchGuestBranch`, or the resume pc when the scheduler re-enters a function after a
  yield, plus the EE thread's perf bucket 0 ee / 3 iop / 4 wait) and `[ee-wait]` (EE idle waits by wake cause). Cost: two relaxed stores per guest call.
  On the Switch use `GOW_EE_SAMPLE=200 GOW_CORE_EeSampler=1 GOW_PRIO_EeSampler=3` (std::this_thread::sleep_for and thread::detach are ENOSYS
  on this libc: svcSleepThread via `ps2xSwitchSleepNs`, the thread is leaked). Read with scratchpad `prof.py <log> <first window> <last window>`.
- Fight profile (bot fight, update_ee2, ~22-24 fps; per 5 s: ee 2.7-3.0 s, iop 0.8-1.0 s, guest_wait 1.1-1.8 s): of the EE thread's samples
  **~39 % are in the frame loop's vsync spin** (resume pc 0x17a32c in sub_0017A2A0: `*(0x29C7D0)==1` polled with 20000-iteration countdown loops,
  every iteration an `eeCheckpointDue`): bucket iop 14.6 % (the IOP batches the checkpoints run) + wait 21.7 % + 3 % own. The game's real EE
  code is a flat tail: 0x17a114 (sub_0017A0E0 after the Flip call) 7.5 %, 0x162f18 3.9 %, 0x27b9f0 3.0 %, 0x254310 2.1 %, 0x27a210 1.7 %,
  0x158020 1.1 %, everything else below 1 % each (60 entries listed): no hot function to rewrite. `guest_wait` is the pacing sleep
  (EeScheduler.cpp:1946, the guest clock may not run ahead of the wall clock) plus `waitForEvent` when every guest thread is blocked.
- `[gow-speed-hist]` in the fight: most frames take 1.6-1.9 fields (27-31 ms), 3-8 frames per second take >= 2.4 fields: the 22-25 fps average is
  a hitch problem as much as a throughput one.
- Arithmetic at 24 fps (41.7 ms/frame): core 1 = EE game code ~20 ms + GS stage ~7.5 ms (it preempts `ee_ms`) + IOP ~8 ms (per wall second, 3 ms at
  60 fps); VU1 thread ~30 ms. 60 fps (16.7 ms) would need EE game code + stage < 13 ms and VU1 < 16 ms: 2x on both, and the EE tail is flat.
- Decision (numbers above): no VU1-on-GPU prototype this round. It removes at most the VU1 thread (3.6 s/5 s) and the GS stage still consumes
  the GIF stream the VU1 programs produce (0.9 s/5 s, core 1) so the stage and the EE (4.3 s/5 s on core 1) stay; without a core-1 cut the
  fps ceiling stays ~30. Cheapest tests first (coordinator audit, 2026-10-23): (1) `[ee-wait]` by cause (update_ee3, built, not yet run) and the
  EE->VU1 chain walk (`PS2X_GS_WORKER_TRACE`); (2) `GOW_CORE_GSStage=0`, then `GOW_IOP_THREAD=1 GOW_IOP_SPIN=0 GOW_IOP_LEAD=4194304` now that deko3d is
  ~1.8 s busy; (3) gsq_wait split; (4) -O3 on the VU1 files; (5) PIC off for ps2_runtime/ps2_iop; (6) FPU divide csr writes; (8) clock/docked logging.
- State: console `update_ee2`'s `env.txt` (the three sampler variables) is still on the SD. At 14:00 the console stopped answering on Wi-Fi
  (<switch-ip>: no ping, probably asleep after the cycle): needs a physical wake before any further bot/MTP use.

## Addendum (2026-10-23 evening: HLE becomes the Switch default, where the rest goes)
- `GOW_VU1_HLE=1` is now a Switch default (ext 70c9fbb, `update_vgpu5`; env.txt `GOW_VU1_HLE=0` turns it off). Console bot fight windows
  (`gsw-split` vu_ms / runs): **21.5 us per VU1 run** with HLE+DIRECT+TRACES vs 27.6-28.2 (DIRECT+TRACES only) and 33.1 (no env); title menu
  44-47 fps (up to 55 in the quiet windows). Bot fight fps still 17-55 (avg ~29, scenes differ; the VU thread stays ~3.6 s / 5 s busy because
  the faster runs let the EE queue more of them: judge by us/run, not by fps). The kernels' per-call clock reads are now opt-in
  (`GOW_VU1_HLE_TIME`; they were on whenever GOW_PERF_DIAG was, which the Switch has by default).
- Where the remaining VU1 time goes (aarch64 harness under qemu, `icount.sh`, fight.rec first 100 records, HLE+DT, excluding the harness's own
  hashing; script: scratchpad `ic.sh`): kernels ~30 % (kSkinLitSt alone 22 %: ~100 NEON ops per vertex, already at the A57's 128-bit throughput),
  generated blocks and traces ~37 %, runNative's dispatch loop ~13 % (~4 k instructions per VU1 run), exact-engine entry/exit ~6 %. Dispatch
  counts (x86 harness, `GOW_VU1N_BLOCKPROF` + `krank.py`): after the kernels the cycles sit in the batch front (pc 00d8/00e0/0458/0350/0240
  ~45 % of the remaining VU cycles), the clip-flag culling loops (0cd8..0df8 and 1728..17d8, ~19 %), and the batch-switch routine (1258, 9.5 %).
  Each of these is a 5-10 % item of the VU thread; no single block or kernel is a big lever any more.
- Thread budget at ~30 fps in the fight (5 s windows): EE 2.7-2.8 s busy (IOP inline ~0.85 s of it) + 1.1-1.4 s `guest_wait`, VU1 thread 3.6 s,
  GS stage 0.7 s, deko3d thread 1.75 s. 60 fps needs EE < 16 ms per frame as well (it is 18.7 ms at 30 fps) and VU1 < 16 ms (24 ms): both
  threads need 1.5-2x, which the remaining micro-optimisations of the CPU path do not give; the structural options are the GPU vertex path
  (step 4) or fewer VU1 runs per frame (840 per frame now).
- `SSE2NEON_PRECISE_MINMAX=0` (harness `PMM=0`) in the VU1 sources: bit-identical replay on 800 records (fight, menu; HLE off) and -4 % A57 cost
  of the VU work, but NOT applied: the precise min/max exists because PS2 data holds NaN patterns (0x7FFFFFFF) that the VU treats as large numbers
  and that the clamps must map to the bound (x86 minps/maxps return the second operand on NaN, NEON fmax propagates the NaN); 4 % is not worth that risk
  without a recording that exercises those patterns.
- Next candidates (each ~5-10 % of the VU thread, in order of certainty): (1) a kernel for the batch front (pc 00d8..0398: XTOP, matrix loads, normal
  length, bounding tests with MAC-flag reads) so that the 4-6 dispatches per batch become one; (2) kernels for the clip-flag culling loops
  (0cd8.. / 1728..): they need the clip flag ring pushed at exit; (3) a cheaper runNative dispatch (hist entry, slot checks) for the ~20 dispatches per run;
  (4) the GPU vertex path or fewer VU1 runs per frame for the real step towards 60 fps.
- Harness: `tools/vu1_harness/build.sh` takes `PMM= PDIV= PSQRT= PDP=` (0/1) for the SSE2NEON_PRECISE_* defines (default 1, as the Switch build).

## Addendum (2026-10-23, step 1 + fight replay A/B)
- Second fight run (DIRECT+TRACES, HLE off, update_vgpu3): menu 40 fps again (31 without), fight 16-26 (scenes vary); not worse than the baseline 15-24 -> `GOW_VU1N_TRACES=1 DIRECT=1` is now the Switch default (ext 3df92c2).
- Deterministic fight replay on the console (`fight.rec`, 6000 warm runs): no env 137.2 ms, DIRECT+TRACES 128.6, DIRECT+TRACES+HLE 108.4 (-21 %), HLE alone 118.1.
- Leftover on fight.rec with HLE on (x86 harness block profile): the batch switch / per-object code (pc 00d8/00e0/0458/1258/1748/0350), ~73 dispatches and ~10 pairs per block per record, ~315 A57 cycles per block: this is the next target (step 3), not another loop kernel.

## Handoff (2026-10-23 later: menu kernels, ext agent/sw-vu1gpu)
- Two new kernels in ps2_vu1_hle.inl: `skin_lit28` (hash 5181a0b08a6d0989, 28-pair self-loop, no perspective divide: STQ.xy, position
  M_B*p + w*(M_A*p) as floats, lit colour min 255.0; entered after the batch-switch jalr) and `unlit11` (a2eaa96b750883ce, 11-pair
  self-loop, Q of the previous pass). Both transcribe the pairs in order (reads before writes inside a pair), cycles = pairs per pass
  (identical to the exact path's total on menu.rec: 4557336 / 1714647). Note: the title menu profile is 5181a0b0 34 %, eae77e32 (the
  batch-switch routine itself, pc 1258, 11 pairs, 23 %), a2eaa96b 13 %, 515a4959 8 %.
- Verification (x86 harness, menu.rec, reference dump with HLE off): GIF 342142 writes exact, 0 beyond tolerance; 482 data words
  differ by 1 ulp (float results in the VU buffers, the exact path rounds wide MADDs differently).
- Console, replay benchmark of menu.rec (6000 warm runs): no env 191.6 ms, DIRECT+TRACES 146.9, + `GOW_VU1_HLE=1` 115.2 (fight.rec before
  this change: 137 / 128.6 / 108.4). Live title menu with DIRECT+TRACES+HLE: **44-46 fps** (38-40 with DIRECT+TRACES only), screen correct;
  the VU thread is still ~3.3 s / 5 s busy: the batch switch (eae77e32 and the pc 00d8/00e0/0458 blocks) is what is left (step 3).
- Switch default DIRECT=1 TRACES=1 is set (ext commit "Switch default"); `GOW_VU1_HLE` is still off by default (flag in env.txt).
- Package: port/switch/nsp/out/update_vgpu4 (local only).

## Handoff (2026-10-23, console measurements of the HLE kernels; ext agent/sw-vu1gpu 556db41 + the diagnostics commit)
- State: `update_vgpu3` (= `update_vgpu1` + the intro-movie fix 3873093 merged as 556db41 + diagnostics) is on the console. Build:
  `bash port/switch/env_sync.sh ... vgpu`, `VU1_PREGEN=/root/vu1pregen-vgpu BUILD_DIR=/root/gow-switch-vgpu ./switch/build.sh` (docs/switch-env-setup.md).
  Console loop = the 4c236500 `cycle.sh` (scratchpad, copy it): bot menu -> New Game -> fight, `PLAY=1 BOOT=45` (BOOT = seconds before the
  first B; 25 was too early now); after a failed cycle the console may sit at the user picker (A) or hbmenu ordering differs: DBI is the 12th tile
  (DDOWN, DRIGHT x5), then A, A, DDOWN x9, A.
- Baseline `update_vgpu1`, no env: title menu 31-33 fps (VU thread 3.6 s / 5 s, EE 1.7 s, guest waits 2.8 s), bot fight 15-24 fps (VU 3.5-3.9 s,
  EE 2.4-2.8 s), movie 53-58 fps. (Old docs: menu 28-29, fight 19-28: the bot fight differs from run to run.)
- `GOW_VU1_HLE=1` changes NOTHING measurable (menu 31, fight 18-25, VU thread still 3.6-3.8 s). Why (measured with the new diagnostics):
  - the kernels themselves are cheap: `[vu1-hle]` now prints calls/VU cycles/wall ms: unlit 135 ms, lit 78, skin 111, cull 22+22 per 5 s
    (~0.35 s for ~110 M VU cycles); native pairs drop 225 M -> 120 M and blocks 28 M -> 22 M (`GOW_VU1N_DIAG`),
  - what is left costs ~3 s: ~22 M small blocks per 5 s (batch switch / prologue code, avg 5.4 pairs) at ~120-250 ns each, i.e. the
    per-block dispatch (the Switch default `GOW_VU1N_DIRECT=0 GOW_VU1N_TRACES=0` of c985a11 makes every block a runNative round trip),
  - code changes (MPG) 600-1500 per 5 s and slot set-ups 33-47 k per 5 s cost only ~70 ms + ~95 ms (`codeMs`/`slotMs` in `[vu1n]`),
    so they are not the problem (the HLE `tried` reset is not either).
- The title menu is 94 % two loops with NO kernel yet: `5181a0b08a6d0989` (28 pairs, 66 %: two-matrix lit/skinned two-block cycle, vf14 weight,
  loi 0x4b0000ff clamp) and `a2eaa96b750883ce` (11 pairs, 28 %: unlit ST-less vertex loop family), seen with `GOW_VU1N_LIVEPROF=1
  GOW_VU1N_DIAG=1` (`[vu1n-prof]` top 14 code hashes per 5 s; with the cached hash it still halves the fps: use it for a look only). A menu
  recording exists only on the console SD (`switch/gow/menu.rec`, pulled to the scratchpad; `fight.rec` too; keep out of git); disassembly
  from `tools/vu1_hle/findblk.py` (want.prof lines `k <hash> <pc> <pairs> 1 9000000`).
- **Measured win, no code: `GOW_VU1_HLE=1 GOW_VU1N_DIRECT=1 GOW_VU1N_TRACES=1` in env.txt: title menu 38-40 fps (31 without), bot fight
  22.2-23.6 fps (same as before).** The menu is the steady, deterministic scene: use it as the A/B workload (the fight varies per run).
  Same with HLE off (`GOW_VU1N_DIRECT=1 GOW_VU1N_TRACES=1` only, run `20261023_dt`): menu 37-39, fight 22.4-23 then 25.6-30.5 in the later
  windows (the HLE run showed 22-23.6 there; scenes differ per run). So DIRECT+TRACES are the win, HLE adds nothing yet. To do: confirm
  in a second fight run, then make them the Switch default (reverts the measurement of c985a11, which was without the kernels / on the ship scene).
- Next (in order): (1) A/B DIRECT/TRACES x HLE in the menu and fight (4 runs, same package); (2) kernels for `5181a0b0` and `a2eaa96b`
  (`kUnlitSt` in ps2_vu1_hle.inl is the pattern; verify with the harness `cmp.sh`/`hlecmp.py` on `menu.rec`: the old reference dump is lost,
  re-make it with HLE off); (3) shrink the batch-switch / prologue blocks (the ~22 M leftover blocks) as a kernel of their own;
  (4) the GPU path only after these: the CPU side is dispatch-bound, not arithmetic-bound.
- Intro movie fix (3873093) is in this build but the glitch frame has not been checked on the console yet (the bot skips the movie with +).

## Handoff (2026-10-20, stopped on request after step 2 started)
- Worktree `ext/PS2Recomp-kl-vu1gpu`, branch `agent/sw-vu1gpu` from b632e46 (sw-vu1split), untracked build files
  copied from the vu1split worktree (generated EE code, native IOP files, overrides, deko3d shader table). Commits:
  c985a11 (Switch defaults `GOW_VU1N_TRACES=0 GOW_VU1N_DIRECT=0`, measured ~3 % faster on the A57), ad33516 (HLE
  framework + `unlit_st`, profiling aids), 6a8114e (WIP: `lit_st`, `skin_lit_st`, `cull`, `cull_swap`).
- Nothing new is on the console: it still has `port/switch/nsp/out/update_vsplit5` (b632e46, sw-vu1split's build)
  and was not touched in this session. No Switch build of this branch exists yet, no PC build folder either.
- WSL: copy of the worktree `/root/src-vgpu` (all tracked files synced; sync script below), VU1 blocks
  `/root/vu1gpu/vu1pregen` (= ext/PS2Recomp-kl-vu1disp/out/vu1pregen), x86 harness `/root/vu1h/vg0`, aarch64 harness
  `/root/vu1h/vga0`; reference dumps `/root/vu1gpu/ref_vg0_ship.dump`.
- Verified: all five kernels are **bit-exact** in data memory and GIF packets on the ship recording (2000 records,
  `hlecmp.py`: 334053 GIF writes exact, 0 data words differ); only the MAC flag register at the program's end differs
  (kernels do not keep the flag pipeline). qemu (aarch64, s30, with `unlit_st` only): VU1 functions per warm pass
  2.487 M -> 1.956 M (-21 %); `unlit_st` itself 0.107 M where the generated cycle + its dispatches cost ~0.42 M.
- Not yet done: the seq and PC-zone recordings (`seq.rec`, `game.rec`; the PC zone's hot loops `5181a0b0` and
  `a2eaa96b` have no kernel yet), qemu totals with all kernels, a PC build + zone gate, the Switch build and the
  console replay benchmark / bot fight, anything GPU.

### Next steps (in order)
1. Kernels for the remaining hot blocks (table below): `5181a0b0` (PC zone 56 %), `a2eaa96b` (PC zone 24 %),
   `5bb209bd`, `40c1d3cd`, `476fc4cf`; check each with `cmp.sh` on ship, seq and game.
2. qemu cost with all kernels (`icb2.sh vga0 s30.rec` and `g30.rec`), then a Switch build (copy of
   docs/switch-deko3d.md's WSL flow from `/root/src-vgpu`, `VU1_PREGEN=/root/vu1gpu/vu1pregen`) and the console
   replay benchmark with `GOW_VU1_HLE=1` vs 0 (env.txt, docs/vu1-native-flags.md "Measuring on the A57"), then the
   bot fight (scratchpad `core1/cycle.sh` style script with `UPDATE=<package>`).
3. If the CPU kernels make VU1 (core 2) no longer the limit, the GPU step pays only by also removing the GS stage's
   per-vertex work (core 1); see "GPU path" below. Retest traces/direct chaining with the kernels on (the loops no
   longer go through them; runNative's dispatch of the remaining code is then the main VU1 cost: s30 runNative 1.03 M
   with traces/direct off, 0.52 M with them on).
4. Vectorise the kernels' steady state (4 vertices per SIMD op) only if the console says the scalar kernels are not
   enough.

### Commands
- Sync worktree -> WSL copy: scratchpad `vu1gpu/sync.sh` (Git Bash: tracked + modified + untracked-not-ignored files
  of the worktree tarred into `/root/src-vgpu`; `ALL=1` every tracked file). wsl.exe expands `$` in `bash -c '...'`:
  run scripts from files.
- Harness: `bash <scratch>/vu1gpu/hb.sh x86|arm <name>` (tools/vu1_harness/build.sh on `/root/src-vgpu`, prints
  compile errors; touch `ps2_vu1_core.cpp` first: the .inl files are not dependencies).
- Exact vs HLE: `bash <scratch>/vu1gpu/cmp.sh vg0 ship` (reference dump once with the HLE off, HLE dump, then
  `python3 tools/vu1_hle/hlecmp.py ref hle [n]`: GIF writes by register with tolerances: ST/Q rel 1e-4, RGBA +-2,
  XY +-2 (1/16 px), Z +-4 or rel 1e-5, ADC exact; final VI / pc; data words).
- Profile by block code (pc independent): `GOW_VU1N_BLOCKPROF=<file>` now also writes `k <hash> <pc> <pairs> <runs>
  <cycles>`; `python -I tools/vu1_hle/krank.py <file>` ranks them; `tools/vu1_hle/findblk.py <prof> <rec> <dir>`
  writes a listing (tools/vu1_hle/vudis.py) of each hot block from the first record holding it;
  `tools/vu1_hle/reghash.py <rec> <index> <start> <pairs>` gives a kernel's region hash.
- `GOW_VU1N_PEEK=<block hash>[:n]`: registers and the data memory around vi2 / vi3 before and after the first n
  dispatches of that block (reading a loop's buffer layout).

## The program (step 1)
- 99 % of the VU1 runs start at 0x00 / 0x18 / 0x30 of one microprogram (fixed code 0x0000-0x2828 in 1973 of 2000 ship
  records) that sets vi1 = 0 / 341 / 683 (triple-buffered input) and dispatches on the batch header (`jr vi4`).
- From 0x2828 the micro memory holds **modules** in 0x350-byte slots (123 different slot contents across the three
  recordings, position independent, loaded at different slots: the game streams them in with MPG). A module is a
  prologue, a call of the batch switch routine (pair 27/28: `xtop`, next batch's pointers and count, matrices
  vf20..23 from the batch, lighting / bounding tests, XGKICK of the finished batch) that returns into the module's
  vertex loop, and the loop; the loop's fall-through calls the batch switch again, which returns into the loop for the
  next batch. The loops are software pipelined: the first passes store values from before the loop (the pipeline
  carries the previous batch's last vertices), so a kernel must run the passes exactly as the VU does, not "per
  vertex" from the inputs.
- Vertex buffers (from vi2 + 1, vi4 qwords per vertex): ST ints (12 fractional bits), [normal ints (15 fractional bits),
  w = 2^23], RGBA ints, XYZ ints (4 fractional bits, w = flags: bit 15 no kick, bit 0 winding, or a skinning
  weight). Output (from vi3 + 1, often another buffer): STQ floats, RGBA as 2^23-biased floats (colour in the
  mantissa, clamped at 255), XYZ ints with the ADC word in w; some lanes are written by a second loop of the module.
- Where the cycles go (`k` profile, one warm pass, cycles attributed to the dispatched block incl. its chained passes):

| block code | pairs | kernel | ship | consecutive | PC zone |
|---|---|---|---|---|---|
| 57dcf3d01a567c49 | 22 | `lit_st`: lit textured (3 lights, colour matrix) | 27.5 % | 24.8 % | 0.1 % |
| 515a49593a9fb86f (+ 5797a1f6) | 9+9 | `unlit_st`: ST/Q + transform, two-block cycle | 25.0 % | 18.2 % | 15.2 % |
| 570237771ea0bdea | 31 | `skin_lit_st`: two matrices blended by a weight, lit | 21.7 % | 24.1 % | - |
| 4c9671fb01ff6c3b | 13 | `cull`: XYZ + strip winding cull (ADC) | 9.8 % | 11.1 % | - |
| 9330611e7b80a713 | 13 | `cull_swap`: the other winding | 6.6 % | 11.1 % | - |
| 5bb209bd93a479eb | 9 | none yet | 2.6 % | 2.6 % | 0.8 % |
| 5181a0b08a6d0989 | 28 | none yet | - | - | 55.6 % |
| a2eaa96b750883ce | 11 | none yet | 0.2 % | - | 23.8 % |
| 40c1d3cda1601098 | 13 | none yet | 0.1 % | - | 2.3 % |
| 476fc4cf0fe83040 | 12 | none yet | 1.3 % | - | - |

## CPU HLE kernels (step 2, ps2_vu1_hle.inl, `GOW_VU1_HLE=1`, off by default; on by default on the Switch)
- Kernels: vertex loops `unlit_st`, `lit_st`, `skin_lit_st`, `skin_lit28`, `unlit11`, shadow culling `cull` /
  `cull_swap`, batch-front scatter loops `scat_b`, `scat_bw`, `scat`, `scat_w` (cycle-exact), clip-flag culling strip
  loop `cull_clip` (values and flags exact, cycles ~3 % high); details in the night-6 handoff above.
- A kernel names its loop region by FNV-1a 64 of the region's bytes (no game code in the source) and its entry
  blocks by offset; runNative checks each dispatch pc once per code change (`VU1HleState`, reset in the code-change
  path) and then calls the kernel instead of the block: it runs all remaining passes in plain float (the native blocks'
  round-toward-zero mode, no operand normalisation / clamps / wide MADD, no cycle model: no history entries, flag
  ring pushes or FDIV timing per pass), writes VU memory and registers as the loop leaves them, adds the passes'
  idle cycles and returns the fall-through pc; runNative continues there with `histCount = 0`, `arraysReadyBy = 0`
  (nothing in flight) and the FDIV entry cleared (else commitDue writes an older Q over the kernel's: the first cull
  bug). A kernel declines (`kDecline`, the native block runs) when the count register would wrap (vi5 <= 0).
- Q: an instruction reading Q sees the newest DIV (the native model stalls on it), so a kernel's MULq uses the DIV
  of its pipeline stage; the cull loops' FMEQ reads the MSUBy.x of the pass before (4 cycles back, steady state also
  with the pass's 1-cycle stall at FTOI4), the first pass reads the MAC ring at entry + 2 (`ringAt`), and the kernel
  pushes its last two MAC writers when it ends.
- `GOW_VU1_HLE_MASK=<hex>`: only the kernels whose bit is set; `[vu1-hle]` (with `GOW_PERF_DIAG` /
  `GOW_VU1_HLE_STATS`, and at the end of a replay): calls / VU cycles per kernel.
- VU cycles: the kernels add the idle pass length (e.g. 31 per skinned vertex) where the exact engine also counts
  stalls; the cycle counts differ a little (only timing diagnostics read them in this port: unverified for XGKICK
  ordering, check in game).

## GPU path (step 3): measured, dropped (2026-10-23)
Console numbers (handoff "night 3" above): the vertex kernels are 6.7 % of the fight's VU1 thread (skin_lit_st 2.1 %), 15 % in the
title menu; that is the most a GPU vertex path could remove from core 2. What it would have needed, for whoever revisits it:
- Data flow: the VU1 run keeps running the batch code (it writes GIF tags, ADC words, XGKICKs); the kernel would skip the vertex math
  and leave a marker in the output buffer; the GS stage (gs_stage / gs_frontend PACKED path) would pass "vertex block by reference"
  (input buffer copy + vf12..31 / I / Q per batch) to the backend, and a deko3d vertex shader would rebuild HwVertex (aPos, aZ, aCol,
  aSTQ) from it, strips expanded by vertex index (previous two vertices for ADC/winding).
- What the backend does with CPU vertex positions today (gs_hw_backend.cpp): `rt.usedW/usedH` = the real drawn extent per draw (not
  the scissor; `findReinterpretSource` uses usedH for the target's memory span), `isZInversion` (full-screen bbox test),
  the shadow-volume wrap groups and texture barriers (per-triangle boxes), `flippedZ`, the soft-blend flat-colour check, debug boxes.
  The skinned/lit draws would need at least a conservative extent (the batch's bounding test result) for usedW/usedH.
- Not worth it unless the VU1 thread's non-kernel part first drops to the same size as the kernels.

### Earlier notes (before the measurement)
- The loops' output goes into GIF packets that the batch switch XGKICKs; the CPU GS stage decodes them (fast PACKED
  path) and the backend batches them for deko3d. A GPU kernel that writes VU memory would need a GPU round trip per
  run (~135k runs per 5 s); the useful form keeps the inputs (VIF-unpacked vertex buffers + the batch's matrices /
  lights) on the GPU and has a vertex or compute shader produce the backend's vertex format, with the GS stage
  receiving a placeholder for the vertex block (PRIM/registers from the packet, vertices by reference).
- The kernels above are the reference for those shaders (same pass structure); the strip culling needs the previous
  two vertices (a compute pass per batch, or per-triangle work in the shader).
- Decide after the console numbers of the CPU kernels: if VU1 stops being the limit, the GPU step is worth it only
  for the GS stage's per-vertex cost on core 1 (~180 ns per kick, 0.8-1.2 s per 5 s in fights).
