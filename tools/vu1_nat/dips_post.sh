# post.sh <base> <new>: diff counts (x86, x86 fd, a64), npart symbols in the a64 harness, x86 native us
b=$1; n=$2; cd /root/vu1h
for s in "" _fd; do echo "diff x86$s: $(diff nca_$b$s.txt nca_$n$s.txt | grep -c '^[<>]')"; done
echo "diff a64: $(diff nca_${b}a.txt nca_${n}a.txt | grep -c '^[<>]')"
aarch64-linux-gnu-nm -S -C ${n}a | grep -i 'npart' | grep ' [TtWw] ' | awk '{print $1,$2,$4}'
for h in $b $n; do t=$(cat nca_$h/nrs_*.full | grep -o 'native [0-9]* us' | awk '{t+=$2} END {print t}'); echo "$h native us $t"; done
