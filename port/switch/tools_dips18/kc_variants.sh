#!/bin/bash
# kc_variants.sh (WSL): /root/kc18/q1.inl = src-mlang's ps2_vu1_hle.inl + tools_dips17/kcullclip_defer.patch; q1r = every
# deferred pass redone (forced redo); q1f = undo log of 1 entry (a strip with 2+ stores redoes through the full-log path).
# Check each with tools_dips17/hb17.sh <name> /root/kc18/<name>.inl (X=1 for q1: forced exact too); all must equal n1.
mkdir -p /root/kc18 && cd /root/kc18
cp /root/src-mlang/ps2xRuntime/src/lib/vu/ps2_vu1_hle.inl q1.inl
patch q1.inl < "/mnt/c/Users/lovas/Claude/God Of War/port/switch/tools_dips17/kcullclip_defer.patch"
sed "s/if (dfr \&\& (undoFull || anyv(bad)))/if (dfr)/" q1.inl > q1r.inl
sed "s/constexpr uint32_t kUndo = 256u;/constexpr uint32_t kUndo = 1u;/" q1.inl > q1f.inl
