#!/usr/bin/env python3
"""What the EE asks of 989snd in a sound trace (GOW_SND_LIBSDLOG file, docs/sound.md "Sound trace").

Counts the 989snd commands, with the batch command 0x4D unpacked (word 0 = count, then per entry: s16 command,
s16 argument bytes (rounded up to 4), the arguments; 989NOMID 0x177c) and the plugin calls split by plugin function
(commands 0x4C / 0x68 with 'SMPD' = 0x534D5044 as their first argument word, the function in the second), plus the
LIBSD calls per ordinal and the SIF transfers.

  python -I tools/sndtrace_stats.py TRACE [--top N]
"""
import argparse
import collections
import struct
import sys

SMPD = 0x534D5044


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--top", type=int, default=80)
    args = ap.parse_args()
    cmds = collections.Counter()
    sd = collections.Counter()
    other = collections.Counter()
    for line in open(args.trace, encoding="latin-1"):
        p = line.split()
        if not p:
            continue
        if p[0] == "sd":
            sd[int(p[1])] += 1
            continue
        if p[0] != "rpc":
            other[p[0]] += 1
            continue
        sid, fn = p[1], int(p[2], 16)
        words = [int(x, 16) for x in p[p.index(":") + 1:]] if ":" in p else []
        entries = []
        if sid == "123456" and fn == 0x4D and words:
            raw = struct.pack("<%dI" % len(words), *words)
            count, off = words[0], 4
            for _ in range(count):
                if off + 4 > len(raw):
                    entries.append(("?", []))
                    break
                cmd, size = struct.unpack_from("<hh", raw, off)
                off += 4
                if size & 3:
                    size += 4 - (size & 3)
                a = list(struct.unpack_from("<%dI" % (min(size, len(raw) - off) // 4), raw, off))
                off += size
                entries.append((cmd, a))
        else:
            entries.append((fn, words))
        for cmd, a in entries:
            key = "%s %02x" % (sid, cmd) if cmd != "?" else sid + " ?"
            if cmd in (0x4C, 0x68) and len(a) >= 2 and a[0] == SMPD:
                key += " SMPD fn %02x" % a[1]
            cmds[key] += 1
    print("989snd commands (batches unpacked):")
    for k, n in cmds.most_common(args.top):
        print("  %-28s %8d" % (k, n))
    print("LIBSD calls:", ", ".join("#%d %d" % (k, sd[k]) for k in sorted(sd)))
    print("other lines:", dict(other))
    return 0


if __name__ == "__main__":
    sys.exit(main())
