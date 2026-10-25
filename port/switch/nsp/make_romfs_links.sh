#!/usr/bin/env bash
# Git Bash (Windows): builds port/switch/nsp/romfs/disc-usa as NTFS hard links to disc-usa (no 8 GB copy), without the
# memory card files (play.mcd, Mcd*.ps2, mc0/, mc1/). Used by build_nsp.sh as the NSP's RomFS.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../../../disc-usa"
DST="$HERE/romfs/disc-usa"
rm -rf "$HERE/romfs"
mkdir -p "$DST"
cd "$SRC"
find . -type d ! -path './mc0*' ! -path './mc1*' ! -path './lang*' | while read -r d; do mkdir -p "$DST/$d"; done
find . -type f ! -name 'play.mcd' ! -name '*.bak' ! -name 'Mcd*.ps2' ! -path './mc0/*' ! -path './mc1/*' ! -path './lang/*' | while read -r f; do ln "$f" "$DST/$f"; done
# language overlays (docs/settings.md "Switch"): the current tools/lang_pack.py output of the PC folder, without the
# Japanese dub (3.4 GB); disc-usa/lang is an older pack
LANG_SRC="${LANG_SRC:-$HERE/../../../dist/God of War (PC)/data/lang}"
cd "$LANG_SRC"
find . -type d ! -name voice_ja | while read -r d; do mkdir -p "$DST/lang/$d"; done
find . -type f ! -name '*_ja.*' ! -path './voice_ja/*' | while read -r f; do ln "$f" "$DST/lang/$f"; done
find "$DST" -type f | wc -l
