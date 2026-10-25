#!/usr/bin/env bash
# Copy the language overlays (disc-usa/lang, tools/lang_pack.py, ~60 MB) to sdmc:/switch/gow/lang/ over FTP/MTP.
# The Switch build looks for lang/<text>_<voice>.layout next to its disc (the NSP's RomFS) first, then there
# (docs/settings.md "Switch"); needed while the installed NSP predates the language packs.
# usage: tools/switch_push_lang.sh
set -u
cd "$(dirname "$0")/.."
. tools/switch_lib.sh
sw_transfer
n=0
for f in disc-usa/lang/*; do F put "$f" switch/gow/lang >/dev/null || { echo "put failed: $f"; exit 1; }; n=$((n+1)); done
echo "$n files -> sdmc:/switch/gow/lang"
