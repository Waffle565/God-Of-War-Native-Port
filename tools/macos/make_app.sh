#!/usr/bin/env bash
# Assembles dist/God of War.app from this machine's build (tools/macos/README.md). Local use only: the runner holds
# recompiled game code, so the app is never redistributed. The game data stays in the disc folder (the app links it
# from ~/Library/Application Support/God of War/data at every start).
#   Contents/MacOS/God of War   launcher (tools/macos/launcher/*.swift, swiftc)
#   Contents/MacOS/GodOfWar     the runner (out/build-mac/ps2xRuntime/ps2EntryRunner)
#   Contents/Resources/AppIcon.icns  from port/switch/nsp/icon.jpg when present
#   Contents/Resources/keyprompts/   keyboard button prompt generator (tools/macos/keyprompts + the tools/ modules
#                                    it imports); the launcher runs it on Play when the kb_ overlays are missing
# usage: tools/macos/make_app.sh            (RUNNER=<path> for another runner build)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RUNNER="${RUNNER:-$ROOT/ext/PS2Recomp-kl/out/build-mac/ps2xRuntime/ps2EntryRunner}"
DISC="$ROOT/disc-usa"
CARD="$ROOT/ext/PS2Recomp-kl/out/build-mac/run/Mcd001.ps2"
ICON_SRC="$ROOT/port/switch/nsp/icon.jpg"
APP="$ROOT/dist/God of War.app"
VERSION="1.0"

[ -x "$RUNNER" ] || { echo "Runner not built: $RUNNER (docs/macos.md \"Build\")" >&2; exit 1; }
command -v xcrun > /dev/null || { echo "Xcode command line tools not found (xcrun)" >&2; exit 1; }
[ -f "$DISC/SCUS_973.99" ] || { echo "note: $DISC has no SCUS_973.99; choose the disc folder in the launcher"; DISC=""; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

# launcher
xcrun swiftc -O -swift-version 5 -target arm64-apple-macos15.0 -framework AppKit \
    "$ROOT"/tools/macos/launcher/*.swift -o "$tmp/launcher"
# files are replaced by rename, never rewritten in place (macOS caches code signatures per file)
mv -f "$tmp/launcher" "$APP/Contents/MacOS/God of War"
cp "$RUNNER" "$tmp/GodOfWar"
mv -f "$tmp/GodOfWar" "$APP/Contents/MacOS/GodOfWar"

# keyboard button prompts (stdlib Python 3 only); the disc's kb_ overlays are refreshed now when it has lang/
mkdir -p "$tmp/keyprompts"
cp "$ROOT"/tools/macos/keyprompts/make_kb_overlays.py "$ROOT"/tools/macos/keyprompts/keymap.py \
    "$ROOT"/tools/lang_pack.py "$ROOT"/tools/gow_wording.py "$ROOT"/tools/gow_text.py "$tmp/keyprompts/"
rm -rf "$APP/Contents/Resources/keyprompts"
mv "$tmp/keyprompts" "$APP/Contents/Resources/keyprompts"
if [ -n "$DISC" ] && [ -d "$DISC/lang" ]; then
    python3 -I "$ROOT/tools/macos/keyprompts/make_kb_overlays.py" "$DISC" | tail -1
else
    echo "note: no language overlays in the disc folder; keyboard prompts need tools/lang_pack.py first"
fi

# icon
ICON_KEY=""
if [ -f "$ICON_SRC" ] && command -v iconutil > /dev/null; then
    set_dir="$tmp/AppIcon.iconset"
    mkdir -p "$set_dir"
    for size in 16 32 128 256 512; do
        sips -s format png -z "$size" "$size" "$ICON_SRC" --out "$set_dir/icon_${size}x${size}.png" > /dev/null
        double=$((size * 2))
        sips -s format png -z "$double" "$double" "$ICON_SRC" --out "$set_dir/icon_${size}x${size}@2x.png" > /dev/null
    done
    iconutil -c icns "$set_dir" -o "$tmp/AppIcon.icns"
    mv -f "$tmp/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
    ICON_KEY="<key>CFBundleIconFile</key><string>AppIcon</string>"
else
    echo "note: no icon ($ICON_SRC missing or iconutil unavailable)"
fi

xml() { sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' <<< "$1"; }

cat > "$tmp/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>God of War</string>
    <key>CFBundleDisplayName</key><string>God of War</string>
    <key>CFBundleIdentifier</key><string>local.godofwar.port</string>
    <key>CFBundleExecutable</key><string>God of War</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    $ICON_KEY
    <key>LSMinimumSystemVersion</key><string>15.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>LSSupportsGameMode</key><true/>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSPrincipalClass</key><string>NSApplication</string>
    <key>GOWDefaultDiscFolder</key><string>$(xml "$DISC")</string>
    <key>GOWImportCard</key><string>$(xml "$CARD")</string>
</dict>
</plist>
EOF
plutil -lint -s "$tmp/Info.plist"
mv -f "$tmp/Info.plist" "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"

# ad-hoc signature: the runner first, then the bundle (seals Info.plist and the icon)
codesign --force --sign - "$APP/Contents/MacOS/GodOfWar"
codesign --force --sign - "$APP"
codesign --verify --strict "$APP"

echo "Built: $APP"
echo "Run:   open \"$APP\"   (or: open \"$APP\" --args --play)"
