# nat-iop8 gate: $1 = name, $2 = slots, then env assignments (e.g. GOW_NATIVE_SMPD=0 ...)
cd "/c/Users/lovas/Claude/God Of War"
name=$1; slots=$2; shift 2
env "$@" GOW_SND_LIBSDLOG={zone}/snd.trace GOW_AUDIO_RAW={zone}/mix.raw python -I tools/zones.py --exe "/c/Users/lovas/Claude/God Of War/ext/PS2Recomp-kl-natiop2/out/build-pc/ps2xRuntime/RelWithDebInfo/ps2EntryRunner.exe" --slots $slots --silent --out "C:/Users/lovas/Claude/God Of War/port/natiop8/$name" > port/natiop8/$name.log 2>&1
echo done $name
