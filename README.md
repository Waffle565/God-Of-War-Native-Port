<div align="center">

# God of War — Native Port

**A native PC port of *God of War* (PS2, NTSC-U `SCUS-97399`): the game's code is statically recompiled to C++, and
the PS2 hardware it talked to is replaced by native code, not emulated.**

![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6?logo=windows&logoColor=white)
![Next](https://img.shields.io/badge/next-Nintendo%20Switch-E60012?logo=nintendoswitch&logoColor=white)
![Language](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Renderer](https://img.shields.io/badge/renderer-OpenGL%204.3-5586A4?logo=opengl&logoColor=white)
![Based on](https://img.shields.io/badge/based%20on-PS2Recomp-8A2BE2)
![Status](https://img.shields.io/badge/status-experimental-orange)

</div>

---

> [!IMPORTANT]
> This repository **does not contain any game files** (ISO, executable, IRX modules, saves) nor the C++ code generated
> from them. You need **your own legal copy** of God of War for the PS2.

## Contents

- [What is this?](#what-is-this)
- [Current status](#current-status)
- [Repository layout](#repository-layout)
- [Requirements](#requirements)
- [Building and running](#building-and-running)
- [How it works](#how-it-works)
- [Documentation](#documentation)
- [Credits and license](#credits-and-license)

## What is this?

Instead of emulating the PlayStation 2, this project **translates the game's executable (`SCUS_973.99`) into C++**
with [PS2Recomp](https://github.com/ran-j/PS2Recomp) and compiles it into a Windows program. On top of that, every
piece of PS2 hardware the game used is being replaced, layer by layer, by native code that produces the same result
from the game's own data:

| Layer | Native replacement |
|---|---|
| **EE CPU** (game code) | Static recompilation to C++ (6.4k functions) |
| **VU1** (vector unit microcode) | ~500 microprograms compiled ahead of time to C++ |
| **GS** (graphics) | Native OpenGL render-target renderer, internal 2x resolution, 16:9; texture/CLUT store instead of a VRAM model |
| **IOP** (I/O CPU) | The game's 8 IRX modules compiled ahead of time to C++ |
| **Sound** (SPU2) | Native sound engine driven by the game's LIBSD calls |
| **Pad / memory card** | Native services (no SIO2 chip model) |
| **DMA / VIF / GIF** | The game's packets are handed straight to VU1 and the GS (no DMA controller model) |

The end goal: after the PC port is complete, the same native port running on the **Nintendo Switch**.

**macOS (Apple Silicon):** the USA build also runs natively on arm64 Macs, with a Metal GS renderer, native VU1/IOP
code, a launcher app, PCSX2-format HD texture packs and Spanish/French/German/Italian text and voices from your own
European disc. See [`docs/macos.md`](docs/macos.md) and [`tools/macos/README.md`](tools/macos/README.md).

## Current status

<a href="docs/status-map/detail.md"><img src="docs/status-map/map.svg" alt="Port status map: PS2 hardware layers and game features, coloured by status" width="100%"></a>

Per-component breakdown: [`docs/status-map/detail.md`](docs/status-map/detail.md). Regenerated from
`docs/status-map/data.toml` by `tools/status_map.py` (and automatically on push to `main`).

<p align="center">
  <img src="docs/captures/olympus.jpg" alt="Olympus" width="49%">
  <img src="docs/captures/desert.jpg" alt="Desert of Lost Souls" width="49%">
  <img src="docs/captures/cliffs.jpg" alt="Cliffs of Madness" width="49%">
  <img src="docs/captures/final.jpg" alt="Save slot 4" width="49%">
</p>

Native Windows build, 1920x1080 window, captured by the automatic zone test (`tools/zones.py`).

| Milestone | Status |
|---|:---:|
| Recompiling `SCUS_973.99` to C++ and building the native executable (MSVC, x64) | ✅ |
| Boot, menu, loading saves, gameplay | ✅ |
| Kratos animation correct (the "walk twist" was a miscompiled MMI `PMULTH`) | ✅ |
| VU1 microcode as native code (no VU interpreter on the hot path) | ✅ |
| Native GS renderer: world, lighting, reflections, fog/shadow mask passes, HUD, menu blur | ✅ |
| 16:9 widescreen, 1920x1080 output | ✅ |
| IOP modules as native code (boot to gameplay 95 s → 20 s) | ✅ |
| Native pad, memory card (load/save verified), sound engine | ✅ |
| Native DMA data path; SIF already native | ✅ |
| GS memory image → native texture/CLUT store; GS display registers native | ✅ |
| EE timers → native clock (recompiler turns timer reads into native clock calls) | ✅ |
| 60 fps in all tested zones (Olympus, Desert, Cliffs, slot 4, new-game ship start) | ✅ |
| Game speed exact (1.00) at 30 and 60 fps, matching PCSX2 | ✅ |
| Game data from the extracted disc files (no ISO reads) | ✅ |
| FMV: movies play natively at real speed with sound (libmpeg2 + native sound engine) | ✅ |
| Nintendo Switch build | ⏳ after the PC port |

Latest zone check (4 save slots, ~2 minutes each): all zones reach gameplay with no runtime errors, fps
60 / 60 / 60 / 60, save test passes. Details and the ordered next steps: [`docs/status.md`](docs/status.md) and
[`docs/native-port.md`](docs/native-port.md).

## Repository layout

The project lives in two branches of this repository:

- **`main`** (this branch): documentation, tools, game overrides, recompiler configuration.
- **`runtime`**: the runtime (fork of PS2Recomp): native GS renderer, VU1 and IOP code generators, sound engine,
  native services. It goes into `ext/PS2Recomp-kl/` of a `main` checkout.

```
.
├── docs/                    # Documentation (single source of truth, start with docs/README.md)
│   └── captures/            # Screenshots for this page
├── port/
│   ├── functions.csv        # Function map
│   ├── gow.toml, gow-kl.toml  # PS2Recomp configurations
│   └── overrides-usa/       # Hand-written game overrides (gow_overrides.cpp)
├── tools/
│   ├── zones.py             # Zone regression test: load every save slot, walk, capture, judge fps
│   ├── savetest.py          # Memory card save test
│   ├── movies.py            # Movie test: play every movie of the disc, judge picture and sound
│   ├── vu1/ iop/            # Ahead-of-time code generators (VU1 microcode, IOP modules)
│   ├── dis.py, irxdis.py    # Disassemblers for the game ELF and IRX modules
│   └── switch/              # Switch smoke-link helpers
├── gemini-work/             # Requests/results of the read-only helper agent
├── play.cmd                 # Launcher (release settings)
├── disc-usa/, iso-usa/      # YOUR game files go here (ignored by git)
└── ext/PS2Recomp-kl/        # The `runtime` branch (ignored by git on main)
```

## Requirements

- Windows 10/11 x64, a GPU with OpenGL 4.3.
- Visual Studio Build Tools (MSVC, x64) with its bundled CMake; Python 3.11 with `capstone` and `Pillow`.
- Your own copy of God of War (USA): the ISO and `SCUS_973.99`.

## Building and running

The pipeline is still manual; the exact steps and their build cost are in
[`docs/build-and-run.md`](docs/build-and-run.md) and the top of [`docs/status.md`](docs/status.md). In short:

1. Check out `runtime` into `ext/PS2Recomp-kl/`, configure with CMake (`out/build-kl`), build `ps2_recomp`.
2. Recompile `SCUS_973.99` with `ps2_recomp` and copy the generated code plus `port/overrides-usa/gow_overrides.cpp`
   into `ps2xRuntime/src/runner/`.
3. Generate the native IOP and VU1 code (`tools/iop/iopn_gen.py`, `tools/vu1/`; see docs/native-port.md).
4. Build `ps2EntryRunner` (Release) and run it with `play.cmd`.

Test runs: `python tools/zones.py --slots 1,2,3,4` (sound on, `--silent` mutes) and `python tools/savetest.py`; every movie of the disc: `python tools/movies.py` (muted, sound recorded and checked).

## How it works

- **EE:** PS2Recomp turns every MIPS R5900 function into a C++ function; the runtime provides the kernel (threads,
  syscalls, interrupts) as native code.
- **VU1:** the vector-unit microprograms are compiled ahead of time to C++ per instruction pair, verified against an
  interpreter A/B.
- **GS:** a render-target renderer: real GPU render targets and textures, paletted textures as index + palette
  textures, the game's framebuffer tricks (channel shuffles, aliased Z, CRTC merge) reproduced natively.
- **IOP:** the game's IRX modules are compiled ahead of time to C++; the hardware behind them (SPU2, SIO2, DMA) is
  replaced by native services.

## Documentation

Everything is in [`docs/`](docs/README.md): status, build and run, diagnostics, native-port inventory, performance,
sound, memory card, game internals, Switch notes.

## Credits and license

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors: the recompiler and runtime this port is
  built on (GPL-3.0; the `runtime` branch keeps that license).
- [KIexster/god-of-war-recomp](https://github.com/KIexster/god-of-war-recomp): a parallel God of War recompilation;
  its patch stack and function map were the starting point of the USA build, and its game-speed meter was ported.
- [PCSX2](https://github.com/PCSX2/pcsx2): used as the reference for verification.
- [LightVelox](https://github.com/LightVelox) and [HenryKun55/celosia](https://github.com/HenryKun55/celosia)
  (Shadow of the Colossus on PS2Recomp, including its macOS/Metal work): references for the macOS port.
- [PCSX2](https://github.com/PCSX2/pcsx2)'s Metal renderer and texture-replacement format: reference for the Metal GS
  backend and HD texture pack support.
- [sse2neon](https://github.com/DLTcollab/sse2neon), [raylib](https://github.com/raysan5/raylib) and libmpeg2.
- macOS port by [bekandev](https://github.com/bekandev).
- Developed by Waffle565 with [Claude](https://claude.com/claude-code) (Anthropic) as co-author.

God of War is a trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by Sony.
