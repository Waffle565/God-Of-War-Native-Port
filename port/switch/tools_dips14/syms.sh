#!/bin/bash
# syms.sh <elf> <regex>: nm -S lines (address size type name) matching the regex
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm -n -S -C "$1" | grep -E "$2"
