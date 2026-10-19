# VU1 vertex loops off the exact path: CPU HLE kernels first, then the Switch GPU (ext agent/sw-vu1gpu)

User decision (2026-10-19): move the dominant VU1 vertex program to the Switch GPU (deko3d); not bit-exact, it must
look the same (pixel comparison against the CPU path, tolerances documented). Plan: (1) understand the program,
(2) a plain-float CPU reference (HLE) compared with the exact path on the recordings, which also says whether a fast
CPU HLE alone reaches the target, (3) GPU path, (4) console fps / per-core busy / screenshots.

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

## CPU HLE kernels (step 2, ps2_vu1_hle.inl, `GOW_VU1_HLE=1`, off by default)
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

## GPU path (step 3, not started): what the data says so far
- The loops' output goes into GIF packets that the batch switch XGKICKs; the CPU GS stage decodes them (fast PACKED
  path) and the backend batches them for deko3d. A GPU kernel that writes VU memory would need a GPU round trip per
  run (~135k runs per 5 s); the useful form keeps the inputs (VIF-unpacked vertex buffers + the batch's matrices /
  lights) on the GPU and has a vertex or compute shader produce the backend's vertex format, with the GS stage
  receiving a placeholder for the vertex block (PRIM/registers from the packet, vertices by reference).
- The kernels above are the reference for those shaders (same pass structure); the strip culling needs the previous
  two vertices (a compute pass per batch, or per-triangle work in the shader).
- Decide after the console numbers of the CPU kernels: if VU1 stops being the limit, the GPU step is worth it only
  for the GS stage's per-vertex cost on core 1 (~180 ns per kick, 0.8-1.2 s per 5 s in fights).
