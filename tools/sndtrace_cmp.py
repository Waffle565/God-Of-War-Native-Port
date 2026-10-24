#!/usr/bin/env python3
"""Compare two sound traces (GOW_SND_LIBSDLOG files, docs/sound.md "Sound trace").

Lines: "sd <ordinal> <args> [-> <result>] [extra] @<iop cycle>" (LIBSD calls of 989snd), "rpc|rep <sid> <fn> <mode>
<size> <fnv> : <words>" (RPCs to 989snd and their replies), "dma|god <src> <dst> <size> <fnv>" (SIF transfers).

The game is not deterministic run to run (EE timing), so two runs of the same build already differ. The report:
  - lines per kind and per LIBSD ordinal in each trace;
  - the first line where the traces part (and how many lines before it are identical);
  - anchored comparison: the RPC lines of both traces are aligned (difflib); between two aligned RPCs the LIBSD calls
    of each trace are compared as multisets; "segments equal" = segments with the same LIBSD calls.
Ordinals whose result depends on the mixer's timing (6 GetParam: ENVX, 8 GetSwitch: ENDX, 10 GetAddr: NAX, 20
BlockTransStatus) are compared without their result unless --strict.

  python -I tools/sndtrace_cmp.py A.trace B.trace [--strict] [--limit N] [--show K]
"""
import argparse
import collections
import difflib
import re
import sys

VOLATILE = {"6", "8", "10", "20"}
CYCLE = re.compile(r" @\d+$")


def canon(line, strict):
    line = CYCLE.sub("", line.rstrip("\n"))
    if not strict and line.startswith("sd "):
        parts = line.split(" ")
        if parts[1] in VOLATILE and "->" in parts:
            line = " ".join(parts[: parts.index("->")]) + " -> ?"
    return line


def load(path, strict, limit):
    out = []
    with open(path, encoding="latin-1") as f:
        for i, line in enumerate(f):
            if limit and i >= limit:
                break
            if line.strip():
                out.append(canon(line, strict))
    return out


def kinds(lines):
    c = collections.Counter()
    for l in lines:
        p = l.split(" ", 2)
        c[p[0]] += 1
        if p[0] == "sd":
            c["sd#" + p[1]] += 1
        elif p[0] == "rpc":
            c["rpc " + p[1] + ":" + l.split(" ")[2]] += 1
    return c


def segments(lines):
    """[(anchor line or None, [lines up to the next rpc])]"""
    segs = [(None, [])]
    for l in lines:
        if l.startswith("rpc "):
            segs.append((l, []))
        else:
            segs[-1][1].append(l)
    return segs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--limit", type=int, default=0, help="read only the first N lines of each trace")
    ap.add_argument("--show", type=int, default=5, help="differing segments shown")
    args = ap.parse_args()
    a = load(args.a, args.strict, args.limit)
    b = load(args.b, args.strict, args.limit)
    ka, kb = kinds(a), kinds(b)
    print("lines: A %d, B %d" % (len(a), len(b)))
    for k in sorted(set(ka) | set(kb), key=lambda s: (s.split("#")[0].split(" ")[0], len(s), s)):
        if not k.startswith("rpc ") or ka[k] != kb[k]:
            mark = "" if ka[k] == kb[k] else "   <-- differs"
            print("  %-22s %9d %9d%s" % (k, ka[k], kb[k], mark))
    first = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
    print("identical prefix: %d lines" % first)
    if first < min(len(a), len(b)):
        print("  first difference:\n    A: %s\n    B: %s" % (a[first][:200], b[first][:200]))
    sa, sb = segments(a), segments(b)
    anchors_a = [s[0] or "" for s in sa]
    anchors_b = [s[0] or "" for s in sb]
    sm = difflib.SequenceMatcher(None, anchors_a, anchors_b, autojunk=False)
    matched = equal = 0
    shown = 0
    sd_total = sd_diff = 0
    for block in sm.get_matching_blocks():
        for k in range(block.size):
            i, j = block.a + k, block.b + k
            matched += 1
            ca = collections.Counter(l for l in sa[i][1] if l.startswith("sd "))
            cb = collections.Counter(l for l in sb[j][1] if l.startswith("sd "))
            sd_total += sum(ca.values()) + sum(cb.values())
            d = (ca - cb) + (cb - ca)
            sd_diff += sum(d.values())
            if not d:
                equal += 1
            elif shown < args.show:
                shown += 1
                print("segment after %s (A seg %d, B seg %d): %d calls differ, e.g." % ((sa[i][0] or "start")[:90], i, j, sum(d.values())))
                for l, n in list((ca - cb).items())[:3]:
                    print("    only A x%d: %s" % (n, l[:150]))
                for l, n in list((cb - ca).items())[:3]:
                    print("    only B x%d: %s" % (n, l[:150]))
    print("rpc segments: A %d, B %d, aligned %d, with equal LIBSD calls %d (%.2f %%)" %
          (len(sa), len(sb), matched, equal, 100.0 * equal / max(matched, 1)))
    print("LIBSD calls in aligned segments: %d, differing %d (%.3f %%)" % (sd_total, sd_diff, 100.0 * sd_diff / max(sd_total, 1)))
    for tag in ("rpc", "rep", "dma", "god"):
        la = [l for l in a if l.startswith(tag + " ")]
        lb = [l for l in b if l.startswith(tag + " ")]
        r = difflib.SequenceMatcher(None, la, lb, autojunk=False).ratio() if len(la) + len(lb) < 400000 else float("nan")
        print("%s lines: A %d, B %d, sequence similarity %.4f" % (tag, len(la), len(lb), r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
