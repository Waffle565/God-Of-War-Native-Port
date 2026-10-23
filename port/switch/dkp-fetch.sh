#!/bin/sh
# pacman XferCommand for devkitPro's repos (docs/switch-env-setup.md).
# pkg.devkitpro.org sits behind Cloudflare, which blocks Linux curl/wget (TLS fingerprint) with 403 but lets Windows'
# curl.exe through with the user agent "pacman/6.0.2". WSL interop runs curl.exe; the body goes through stdout.
# Usage (pacman): dkp-fetch.sh <url> <output file>
url="$1"; out="$2"
CURL=/mnt/c/Windows/System32/curl.exe
[ -x "$CURL" ] || { echo "dkp-fetch: $CURL missing (WSL interop off?)" >&2; exit 1; }
"$CURL" -s -f -L --retry 3 -A "pacman/6.0.2" "$url" > "$out.part" && mv "$out.part" "$out" || { rm -f "$out.part"; exit 1; }
