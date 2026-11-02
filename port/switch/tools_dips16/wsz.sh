for e in /root/elf_dips38.elf /root/elf_dips39.elf /root/elf_boss17.elf; do echo "$e $(aarch64-linux-gnu-nm -C -S $e | grep "windLoop<" | awk "{print \$2}" | tr "\n" " ")"; done
