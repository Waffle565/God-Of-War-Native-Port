#!/bin/bash
# pcprof.sh <pcs> <elf> [args...]: tools/switch_pcprof.py with devkitA64 nm/addr2line
export NM=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm ADDR2LINE=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line
cd "/mnt/c/Users/lovas/Claude/God Of War"
python3 tools/switch_pcprof.py "$@"
