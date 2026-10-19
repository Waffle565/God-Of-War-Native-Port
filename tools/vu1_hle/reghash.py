# reghash.py <rec> <record idx> <region start hex> <pairs>: FNV-1a 64 of the region bytes (the HLE kernel signature)
import sys, struct
rs = 33448; st = 664
data = open(sys.argv[1], 'rb').read()
i = int(sys.argv[2]); a = int(sys.argv[3], 16); n = int(sys.argv[4])
code = data[i*rs+16+st:i*rs+16+st+16384]
h = 1469598103934665603
for x in code[a:a+n*8]: h = ((h ^ x) * 1099511628211) & 0xffffffffffffffff
print(f'0x{h:016x}ull')
