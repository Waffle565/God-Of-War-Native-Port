# sb.sh <tag>: build /root/gow-switch-mlang from /root/src-mlang, ELF /root/elf_<tag>.elf, package out/update_<tag>
export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64; cd /root/gow-switch-mlang
taskset -c 0-7 ninja -j6 > /root/gow-switch-$1.log 2>&1 || { tail -c 1500 /root/gow-switch-$1.log; echo SB_FAIL; exit 1; }
tail -2 /root/gow-switch-$1.log
cp ps2xRuntime/ps2EntryRunner.elf /root/elf_$1.elf
bash /root/gowr/port/switch/nsp/make_update_to.sh /root/elf_$1.elf /root/gowr/port/switch/nsp/out/update_$1 | tail -2
md5sum /root/gowr/port/switch/nsp/out/update_$1/atmosphere/contents/0100D0D0600A0000/exefs/main
echo SB_DONE
