#!/usr/bin/env bash
# One-go setup of the Switch cross-build environment inside a fresh WSL Ubuntu 24.04 (root). Recreates what the lost
# distro held (docs/switch-env-setup.md). Started by env_setup.ps1, or by hand:
#   wsl -d Ubuntu -u root -- bash "/mnt/c/Users/lovas/Claude/God Of War/port/switch/env_setup.sh"
# Idempotent: safe to run again.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export DEBIAN_FRONTEND=noninteractive

printf '[user]\ndefault=root\n' > /etc/wsl.conf

# 1. Ubuntu packages: host build tools, aarch64 Linux cross compiler + qemu-user for the VU1 harness (vga0), pacman.
apt-get update -qq
apt-get install -y -qq build-essential cmake ninja-build python3 python3-pip git curl wget xz-utils ca-certificates \
    qemu-user gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libc6-arm64-cross pacman-package-manager libssl-dev \
    zip unzip rsync llvm bc

# The VU1 harness sources need GCC 14 (a constexpr sizeof of a reference parameter, rejected by Ubuntu 24.04's GCC 13).
apt-get install -y -qq g++-14 gcc-14 g++-14-aarch64-linux-gnu gcc-14-aarch64-linux-gnu
for t in g++ gcc aarch64-linux-gnu-g++ aarch64-linux-gnu-gcc; do
    update-alternatives --install /usr/bin/$t $t /usr/bin/$t-14 100 >/dev/null
done

# git: the repo is read from /mnt/c as another owner
git config --global --add safe.directory '*'

# 2. devkitPro through Ubuntu's pacman. pkg.devkitpro.org answers Linux curl/wget with 403 (Cloudflare), so pacman
#    fetches through Windows' curl.exe (dkp-fetch.sh). No signature checking (the keyring host is blocked too).
install -m755 "$HERE/dkp-fetch.sh" /usr/local/bin/dkp-fetch
mkdir -p /opt/devkitpro/{etc,db,cache,gnupg}
cat > /opt/devkitpro/pacman.conf <<'EOF'
[options]
RootDir = /
DBPath = /opt/devkitpro/db/
CacheDir = /opt/devkitpro/cache/
LogFile = /opt/devkitpro/pacman.log
GPGDir = /opt/devkitpro/gnupg/
HookDir = /opt/devkitpro/hooks/
Architecture = x86_64
XferCommand = /usr/local/bin/dkp-fetch %u %o
SigLevel = Never
LocalFileSigLevel = Never
[dkp-libs]
Server = https://pkg.devkitpro.org/packages
[dkp-linux]
Server = https://pkg.devkitpro.org/packages/linux/$arch/
EOF
printf '#!/bin/sh\nexec pacman --config /opt/devkitpro/pacman.conf "$@"\n' > /usr/local/bin/dkp-pacman
chmod +x /usr/local/bin/dkp-pacman
dkp-pacman -Sy
# switch-dev = devkitA64 + libnx + deko3d + uam (shader compiler) + switch-tools + cmake files; the rest are portlibs.
dkp-pacman -S --noconfirm --needed switch-dev devkitA64 switch-sdl2 switch-glfw switch-mesa switch-glad \
    switch-ffmpeg switch-zlib

cat > /etc/profile.d/devkitpro.sh <<'EOF'
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export PATH=$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH
EOF

# 3. NSP packer (docs/switch.md): hacBrewPack v3.05 from the mirror of the deleted original.
if [ ! -x /root/nsp-tools/hacBrewPack/hacbrewpack ]; then
    mkdir -p /root/nsp-tools
    git clone --depth 1 https://github.com/rlaphoenix/hacBrewPack /root/nsp-tools/hacBrewPack
    (cd /root/nsp-tools/hacBrewPack && cp config.mk.template config.mk 2>/dev/null || true; make -j"$(nproc)")
fi

/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc --version | head -1
echo "env_setup: done. Next: env_sync.sh <worktree> <name> <vu1pregen>, then switch/build.sh (docs/switch-env-setup.md)."
