// Native VU1 rendering harness (docs/native-render-study.md): GOW_VIF1_REC recordings through the native VIF decoder,
// the run classifier and the recompiled VU1 as the oracle. All logic lives in the runtime (ps2_vu1_nat.inl).
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_nat.h"

int main(int argc, char **argv) { return VU1NatAccess::harness(argc, argv); }
