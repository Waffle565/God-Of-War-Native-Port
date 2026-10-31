# hc.sh <base> <new>: sync /root/src-mlang VU files into /root/natvu1x, build <new>, natcheck base+new (x86 plain/fd, a64 plain), diff
b=$1; n=$2; N=/root/gowr/tools/vu1_nat
cd /root/src-mlang/ps2xRuntime
for f in src/lib/vu/*.inl src/lib/vu/*.cpp include/runtime/*.h; do cmp -s "$f" "/root/natvu1x/ps2xRuntime/$f" || { echo "sync $f"; cp "$f" "/root/natvu1x/ps2xRuntime/$f"; }; done
bash /root/gowr/tools/vu1_nat/dips_hb2.sh $n
grep -l "error" /root/vu1h/b_$n.log /root/vu1h/b_${n}a.log && exit 1
for h in $b $n; do [ -f /root/vu1h/nca_$h.txt ] || bash $N/nca.sh $h >/dev/null; [ -f /root/vu1h/nca_${h}_fd.txt ] || bash $N/nca.sh $h fd >/dev/null; done
for h in ${b}a ${n}a; do [ -f /root/vu1h/nca_$h.txt ] || bash $N/nca.sh $h >/dev/null; done
for s in "" _fd; do echo "diff x86$s: $(diff /root/vu1h/nca_$b$s.txt /root/vu1h/nca_$n$s.txt | grep -c '^[<>]')"; done
echo "diff a64: $(diff /root/vu1h/nca_${b}a.txt /root/vu1h/nca_${n}a.txt | grep -c '^[<>]')"
grep -hE 'differ [1-9]|differs [1-9]|restored: [1-9]' /root/vu1h/nca_$n.txt /root/vu1h/nca_${n}_fd.txt | sort | uniq -c | head
echo HC_DONE
