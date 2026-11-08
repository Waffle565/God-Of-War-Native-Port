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
| `port/switch/nsp/out/update_crash2d` | sw-crash2's diagnostic package (docs/game-internals.md, crash2_diag.patch). |
| `port/switch/sweep/*`, `port/switch/logs/*` (2026-11-08 pass) | A/B, bench, prof, boot, SEMO and crash runs from the dips4..dips39 / boss1..boss17 era, the full sweeps 20261030_1930 / 20261031_0203 / 20261101_1211, the boss RAM dumps (bossdump, bossx*), the env.txt variants (failgt/gpu/prof_env.txt). For partly tracked run folders only the untracked files were moved; the tracked ones stay in the repo. `port/switch/logs/clean2_plan.txt` lists every path. |
| `port/zones_spd*/`, `port/switch/sweep_dips/`, `port/pcsx2state/` | Old PC zone-speed runs (status.md 2026-10-22), an old dips sweep, the PCSX2 state dump (diagnostics.md). |
| `repo/`, `port/`, `scratchpad/`, `nat*/`, `switch/` | Earlier cleanup passes (2026-10-24/25): big run logs, VU1 work trees, old packages; `MOVED_LIST_first_pass.txt` lists them. |

| `ext/PS2Recomp-kl-*` (2026-11-08 pass, sw-clean3) | The side git worktrees of `ext/PS2Recomp` (natiop*, natvu1*, natpc, natspd, core1, dkdraw, iopthr, swdeko, swee*, swpresent, swstall, swvu1*, vu1cyc, vu1fast, vu1hle, vu1jr, vu1par, vu1split), moved whole (sources, untracked generated files, `out/vu1pregen`, any uncommitted edits). Their branches `agent/*` stay in `ext/PS2Recomp/.git`; the worktree entries were pruned. To use one again: `git -C ext/PS2Recomp worktree add ext/PS2Recomp-kl-<wt> agent/<branch>` (or copy the folder back and run `git worktree repair` in it). Scripts that name them (port/vu1jr, port/vu1fast, port/vu1cyc `hb.sh`/`build_pc.sh`, port/natiop*_gate.sh, tools/switch_stall/swb.sh) need the folder back first. |
| `gamedata/ref/proto/` | `ref/proto` (the 2004-12-23 Hydra demo prototype extracted: ISO + files/), also in `gamedata/God of War (Dec 23, 2004 prototype).7z`. |

Kept in the repo: `ext/PS2Recomp` (the repository), `ext/PS2Recomp-kl` (gow-usa-kl), `ext/PS2Recomp-kl-switch`, and
`ext/PS2Recomp-kl-vu1gpu` + `ext/PS2Recomp-kl-vu1disp`: the Switch rebuild path after a lost distro syncs these two
(docs/switch-env-setup.md: env_sync of vu1gpu with vu1disp's `out/vu1pregen`, generated inputs live in them).

`dist/God of War (PC)/data` is a hard-link view of `disc-usa/` (as is `port/switch/nsp/romfs/disc-usa`): it costs no
extra space, so it stays.

## Deleted, not archived (2026-11-01 cleanup)
- PC CMake build trees (`out/build-*`, ~1.3-4.6 GB each) of the side worktrees `ext/PS2Recomp-kl-*` (all but
  `-switch`): regenerable with the build commands in build-and-run.md. Their sources and `out/vu1pregen/` stay. The
  main trees `ext/PS2Recomp-kl/out`, `ext/PS2Recomp-kl-switch/out`, `ext/PS2Recomp/out` were kept.
- Superseded console packages in `port/switch/nsp/out/` older than `update_dips25` (dips1-24, nv*, k*, spd*, swfix*,
  lang*, ...) and the `icontest*.nsp` test files: rebuildable from git history if ever needed.

## Deleted, not archived (2026-11-08 cleanup, sw-clean2; repo 52 -> 49 GB: 1.1 GB deleted, 1.6 GB archived)
- Superseded console packages in `port/switch/nsp/out/`: `update_boss1-9`, `update_boss11-17`, `update_dips25-39`
  (all but `update_dips37c`). Kept: the default `update_dips40s`, its rollbacks `update_dips40` and `update_dips37c`,
  the boss test packages `update_boss10` / `update_boss18`, `lang_nx2/`.
- Console runs not named by any doc and superseded by later ones (sweep/ab2_*, ab4_dips9, ab8_dips18, ab38*, boot_dips6/7,
  v_dips6, mov_dips13, prof_dips4, prof_dips16lr*, the bench/restore `.out` stubs, logs/bench_d30_diagfs*/zva0,
  logs/d17_restore, logs/prof_dips4_*, logs/tmpclose), Python `__pycache__/` folders, an empty stray file at the root.
- Kept in the repo: the current checks (sweep/c40l_*, d17l_*, d17b_*, boot/mov_dips40*, logs/chk40, saves_test,
  default_env, d18) and everything git-tracked.

## Deleted, not archived (2026-11-08 cleanup, sw-clean3; 11.2 GB deleted, 6.4 GB archived: worktrees 5.0, ref/proto 1.4)
- The PC CMake build trees `ext/PS2Recomp-kl/out/build-kl` (5.6 GB), `ext/PS2Recomp/out/build` (3.3 GB) and
  `ext/PS2Recomp-kl-switch/out/build-pc` (2.2 GB); no tracked file was inside. **The PC tools `tools/zones.py`,
  `tools/movies.py`, `tools/perf_run.sh` and `tools/pc_release/make_pc_folder.ps1` run `ps2EntryRunner.exe` from
  `ext/PS2Recomp-kl/out/build-kl/ps2xRuntime/Release`: rebuild build-kl first** (configure + build as in
  build-and-run.md, `-B out/build-kl`, then `--config Release`). The Switch build (WSL `/root/src-*`) does not use them.
