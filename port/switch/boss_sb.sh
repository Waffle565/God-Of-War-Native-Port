# boss_sb.sh <tag>: build the boss test tree (/root/src-boss, /root/gow-switch-boss) in a private mount namespace where
# they appear at the paths of the mlang tree (identical paths = incremental ninja build)
set -u
cp "/root/gowr/port/overrides-usa/gow_overrides.cpp" /root/src-boss/ps2xRuntime/src/runner/gow_overrides.cpp
unshare -m bash -c "mount --bind /root/src-boss /root/src-mlang && mount --bind /root/gow-switch-boss /root/gow-switch-mlang && bash /root/dt/dips_sb.sh $1"
