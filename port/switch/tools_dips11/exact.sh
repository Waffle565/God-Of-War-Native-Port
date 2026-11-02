#!/bin/bash
# exact.sh <name>: harness from /root/natvu1x with FastOps::ok() forced false (every fast pass redone exactly),
# natcheck x86 + a64, compare with f1; then restore natvu1x's lit.inl from src-mlang.
n=$1; L=/root/natvu1x/ps2xRuntime/src/lib/vu/ps2_vu1_nat_lit.inl
grep -c 'VU1N_INLINE bool ok() const { return vmaxvq_u32' $L
sed -i 's/VU1N_INLINE bool ok() const { return vmaxvq_u32(vreinterpretq_u32_m128i(hot)) <= 0x7E800000u; }/VU1N_INLINE bool ok() const { return false; }/; s/VU1N_INLINE bool ok() const { return _mm_movemask_epi8(_mm_cmpgt_epi32(hot, _mm_set1_epi32(0x7E800000))) == 0; }/VU1N_INLINE bool ok() const { return false; }/' $L
grep -n 'bool ok() const' $L
bash /root/dt/vu1_nat/dips_hb2.sh $n
cp /root/src-mlang/ps2xRuntime/src/lib/vu/ps2_vu1_nat_lit.inl $L
touch $L
bash /root/dt/vu1_nat/nca.sh $n > /dev/null
bash /root/dt/vu1_nat/nca.sh ${n}a > /dev/null
echo "diff x86: $(diff /root/vu1h/nca_f1.txt /root/vu1h/nca_$n.txt | grep -c '^[<>]')"
echo "diff a64: $(diff /root/vu1h/nca_f1a.txt /root/vu1h/nca_${n}a.txt | grep -c '^[<>]')"
echo EXACT_DONE
