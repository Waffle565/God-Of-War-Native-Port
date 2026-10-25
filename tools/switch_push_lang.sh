#!/usr/bin/env bash
# Copy the language overlays (tools/lang_pack.py output, the PC folder's data/lang) to sdmc:/switch/gow/lang/ over
# FTP/MTP, without the Japanese dub (3.4 GB, *_ja): text + voices en fr de it es, ~200 MB, ~500 files.
# The Switch build looks for lang/<text>_<voice>.layout next to its disc (the NSP's RomFS) first, then there
# (docs/settings.md "Switch"); needed while the installed NSP predates the language packs.
# usage: tools/switch_push_lang.sh [lang folder, default "dist/God of War (PC)/data/lang"]
set -u
cd "$(dirname "$0")/.."
. tools/switch_lib.sh
SRC=${1:-"dist/God of War (PC)/data/lang"}
[ -f "$SRC/it_it.layout" ] || { echo "no language pack in $SRC"; exit 1; }
sw_transfer
n=0
while IFS= read -r f; do
  rel=${f#"$SRC"/}; dir=switch/gow/lang; case "$rel" in */*) dir=$dir/${rel%/*};; esac
  F put "$f" "$dir" >/dev/null || { echo "put failed: $f"; exit 1; }; n=$((n+1))
done < <(find "$SRC" -type f ! -name '*_ja.*' ! -path '*/voice_ja/*' | sort)
echo "$n files -> sdmc:/switch/gow/lang"
