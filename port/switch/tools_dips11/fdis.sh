#!/bin/bash
# fdis.sh <elf> <symbol grep> : disassemble the first matching function
B=/opt/devkitpro/devkitA64/bin
L=$($B/aarch64-none-elf-nm -C -S "$1" | grep -- "$2" | head -1)
s=0x$(echo "$L" | awk '{print $1}'); r=0x$(echo "$L" | awk '{print $2}')
e=$(printf "0x%x" $((s + r)))
echo "# $L" >&2
$B/aarch64-none-elf-objdump -d --no-show-raw-insn --start-address=$s --stop-address=$e "$1" | grep -E "^ *[0-9a-f]+:"
