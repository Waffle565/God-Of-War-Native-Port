#!/usr/bin/env bash
# Packs the Switch build into an installable NSP (title 0100D0D0600A0000) with the extracted disc in its RomFS.
# Run in WSL as root after switch/build.sh. Needs the user's own prod.keys (Lockpick_RCM); never commit or share them.
# Usage: build_nsp.sh <prod.keys>     Output: /mnt/c/.../port/switch/nsp/out/God of War.nsp (~8.3 GB, contains game data:
# for the user's own console only).
set -euo pipefail
KEYS="${1:?usage: build_nsp.sh <path to prod.keys>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
DKP=/opt/devkitpro
ELF="${ELF:-/root/gow-switch-build/ps2xRuntime/ps2EntryRunner.elf}"
PACK="${PACK:-/root/nsp-tools/hacBrewPack/hacbrewpack}"
# TITLE / NAME / ROMFS / OUT override the title id, home-menu name, RomFS folder and output file (a small test package:
# TITLE=0100D0D0600B0000 NAME="GoW icon test" ROMFS=<tiny dir> OUT=icontest.nsp)
TITLE="${TITLE:-0100D0D0600A0000}"
NAME="${NAME:-God of War}"
OUT="${OUT:-God of War.nsp}"
# VERSION: content meta version (hacBrewPack patched, hacbrewpack_version.patch). The home menu keeps the icon and name
# it first saw for a title id + version: a reinstall at the same version keeps showing the old icon (docs/switch.md)
export HBP_TITLE_VERSION="${VERSION:-0}"
WORK=/root/gow-nsp
export PATH="$DKP/tools/bin:$PATH"

[ -f "$KEYS" ] || { echo "error: no keys at $KEYS" >&2; exit 1; }
[ -f "$ELF" ] || { echo "error: no Switch build at $ELF (switch/build.sh)" >&2; exit 1; }
[ -x "$PACK" ] || { echo "error: hacBrewPack not built at $PACK (docs/switch.md)" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/exefs" "$WORK/control"
elf2nso "$ELF" "$WORK/exefs/main"
npdmtool "$HERE/gow.json" "$WORK/exefs/main.npdm"
nacptool --create "$NAME" "Waffle565" "0.1.0" "$WORK/control/control.nacp"
# nacptool fills 12 of the 16 language entries (0x300 bytes each): copy entry 0 into the rest, so the name shows on
# a console set to Korean, Chinese or Brazilian Portuguese too
python3 - "$WORK/control/control.nacp" <<'EOF'
import sys
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
for i in range(1, 16):
    data[i * 0x300:(i + 1) * 0x300] = data[0:0x300]
open(path, "wb").write(data)
EOF
# icon: the God of War logo as the native port renders it (port/usertest1/logo_native.png), 256x256 JPEG
for lang in AmericanEnglish BritishEnglish Japanese French German LatinAmericanSpanish Spanish Italian Dutch CanadianFrench Portuguese Russian Korean TraditionalChinese SimplifiedChinese BrazilianPortuguese; do
    cp "$HERE/icon.jpg" "$WORK/control/icon_$lang.dat"
done

# RomFS = disc-usa without the memory card (the card lives on the SD card: sdmc:/switch/gow/play.mcd). The tree is
# made of NTFS hard links (romfs/disc-usa) so the 8 GB of PAKs are not copied.
ROMFS="${ROMFS:-$HERE/romfs}"
[ -d "$ROMFS/disc-usa" ] || [ -n "${ROMFS_ANY:-}" ] || { echo "error: run make_romfs_links.sh on Windows first" >&2; exit 1; }

# hacBrewPack aborts on any value that is not 32 or 64 hex digits (newer Lockpick dumps have 34-digit *_kek_source
# entries and RSA keypairs it does not need): give it a filtered copy, deleted after packing.
KEYS_OK="$WORK/prod.keys"
tr -d '\r' < "$KEYS" | awk -F' *= *' 'NF==2 && (length($2)==32 || length($2)==64)' > "$KEYS_OK"
trap 'rm -f "$KEYS_OK"' EXIT

cd "$WORK"
"$PACK" -k "$KEYS_OK" --titleid "$TITLE" --titlename "$NAME" --titlepublisher "Waffle565" \
    --exefsdir "$WORK/exefs" --romfsdir "$ROMFS" --controldir "$WORK/control" --nologo \
    --tempdir "$WORK/temp" --ncadir "$WORK/nca" --backupdir "$WORK/backup" --nspdir "$HERE/out"
# hacBrewPack names the file by title id; the user downloads it by name (user 2026-10-18: "name the nsp god of war")
mv -f "$HERE/out/$(echo "$TITLE" | tr 'A-F' 'a-f').nsp" "$HERE/out/$OUT"
ls -la "$HERE/out"
