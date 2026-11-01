# Storage: what lives where, cleanup policy

The project folder (`C:\Users\lovas\Claude\God Of War`, SSD) holds only what current work needs. Material that may
be needed again but not soon lives on the slow HDD in **`D:\Claude Archive\GoW-Archive`** (same relative paths as in
the repo where possible). Nothing there is used by builds or runs; copy a thing back before using it.

## Policy
- **Useless** (regenerable build intermediates, superseded test packages, scratch): delete.
- **Maybe needed, not urgent**: move to `D:\Claude Archive\GoW-Archive`.
- **Game data** (disc images, extracted discs, the original archives): never delete, only move.
- **Unsure**: move, don't delete.
- Never touched by a cleanup: git-tracked files, `.git`, `ext/` sources, `docs/`, `tools/`, `port/switch/*.patch`,
  `port/switch/bench_env/`, `port/switch/env_setup.ps1`, the WSL distro, the console default package and its
  rollback (`tools/switch_default.sh` names the default), anything modified in the last few hours (a running agent).
- Bulky binary dumps (RAM dumps, traces, raw audio, frame captures) older than 2 days are deleted unless a doc names
  their path; then they are moved instead.

## Archive contents (`D:\Claude Archive\GoW-Archive`)
| Path | What |
|---|---|
| `gamedata/*.zip`, `*.7z` | The original game archives: Asia, Japan, Korea, God of War II (USA), the 2004-12-23 prototype. |
| `gamedata/iso/` | The European redump ISO (`disc/` holds it extracted; docs/disc.md). |
| `gamedata/iso-asia/`, `iso-jpn/`, `iso-kor/` | Regional ISOs. |
| `gamedata/disc-asia/`, `disc-jpn/`, `disc-kor/` | Regional discs extracted (docs/settings.md "Other regional discs"). `disc-jpn/` is the Japanese dub source for `tools/lang_pack.py --ja` / `tools/pc_release/make_pc_folder.ps1`: copy it back to the repo root before building a language pack with Japanese voices. |
| `gamedata/ref/gow2/`, `ref/gow2demo/` | God of War II and its demo (ISO + extracted files), reference only. |
| `port/natiop*/`, `port/core1/`, `port/natpc/` | Native IOP / core 1 gate runs (2026-10-19..26) named in sound.md, memory-card.md, switch-deko3d.md: logs, traces, captures. |
| `port/switch/logs/`, `port/switch/sweep/`, `port/switch/sweep_movlang/`, `port/switch/shots/` | Console logs, sweep reports and screenshots older than 2 days at cleanup time (git-tracked ones stay in the repo too). |
| `port/switch/nsp/out/` | `base_20261018_v0.nsp` (the full 10-18 NSP, needed as `REUSE_NSP` for a control-only package, docs/switch.md), old language pack `lang_nx/` (superseded by `lang_nx2/`), the former defaults `update_bugs1`, `update_mlangfog1`, `gow_icon_v65536.nsp`. |
| `repo/`, `port/`, `scratchpad/`, `nat*/`, `switch/` | Earlier cleanup passes (2026-10-24/25): big run logs, VU1 work trees, old packages; `MOVED_LIST_first_pass.txt` lists them. |

`dist/God of War (PC)/data` is a hard-link view of `disc-usa/` (as is `port/switch/nsp/romfs/disc-usa`): it costs no
extra space, so it stays.

## Deleted, not archived (2026-11-01 cleanup)
- PC CMake build trees (`out/build-*`, ~1.3-4.6 GB each) of the side worktrees `ext/PS2Recomp-kl-*` (all but
  `-switch`): regenerable with the build commands in build-and-run.md. Their sources and `out/vu1pregen/` stay. The
  main trees `ext/PS2Recomp-kl/out`, `ext/PS2Recomp-kl-switch/out`, `ext/PS2Recomp/out` were kept.
- Superseded console packages in `port/switch/nsp/out/` older than `update_dips25` (dips1-24, nv*, k*, spd*, swfix*,
  lang*, ...) and the `icontest*.nsp` test files: rebuildable from git history if ever needed.
