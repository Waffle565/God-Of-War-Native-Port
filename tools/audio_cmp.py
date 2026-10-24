#!/usr/bin/env python3
"""Compare two sound-engine recordings (GOW_AUDIO_RAW files: s16le stereo 48 kHz, the mix before the output's rate
adaptation, docs/sound.md).

Reports per file: length, RMS, peak, clipped samples, silent seconds; between the files: the identical prefix (samples),
whether the whole files are identical (FNV), and the per-second RMS profiles (correlation, mean relative difference)
aligned at the best offset within +-3 s (the runs start the sound at slightly different times).

  python -I tools/audio_cmp.py A.raw B.raw [--rate 48000]
"""
import argparse
import hashlib
import sys

import numpy as np


def load(path):
    data = np.fromfile(path, dtype="<i2")
    return data[: len(data) // 2 * 2].reshape(-1, 2).astype(np.int32)


def profile(x, rate):
    n = len(x) // rate
    if n == 0:
        return np.zeros(0)
    seg = x[: n * rate].reshape(n, rate, 2).astype(np.float64)
    return np.sqrt((seg ** 2).mean(axis=(1, 2)))


def describe(name, x, rate, prof):
    clipped = int(((x >= 32767) | (x <= -32768)).sum())
    silent = int((prof < 8.0).sum())
    print("%s: %.1f s, RMS %.0f, peak %d, clipped %d, silent seconds %d" %
          (name, len(x) / rate, np.sqrt((x.astype(np.float64) ** 2).mean()) if len(x) else 0, int(np.abs(x).max()) if len(x) else 0, clipped, silent))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--rate", type=int, default=48000)
    args = ap.parse_args()
    a, b = load(args.a), load(args.b)
    pa, pb = profile(a, args.rate), profile(b, args.rate)
    describe("A", a, args.rate, pa)
    describe("B", b, args.rate, pb)
    n = min(len(a), len(b))
    diff = np.nonzero((a[:n] != b[:n]).any(axis=1))[0]
    prefix = int(diff[0]) if len(diff) else n
    print("identical prefix: %d samples (%.2f s)%s" % (prefix, prefix / args.rate, " = whole common length" if prefix == n else ""))
    ha = hashlib.sha1(a.astype("<i2").tobytes()).hexdigest()[:16]
    hb = hashlib.sha1(b.astype("<i2").tobytes()).hexdigest()[:16]
    print("sha1: A %s, B %s%s" % (ha, hb, "  (identical)" if ha == hb else ""))
    best = None
    for off in range(-3, 4):
        x = pa[max(off, 0):]
        y = pb[max(-off, 0):]
        m = min(len(x), len(y))
        if m < 5:
            continue
        x, y = x[:m], y[:m]
        c = float(np.corrcoef(x, y)[0, 1]) if x.std() > 0 and y.std() > 0 else float("nan")
        rel = float(np.mean(np.abs(x - y) / np.maximum((x + y) / 2, 1.0)))
        if best is None or c > best[1]:
            best = (off, c, rel, m)
    if best:
        print("RMS profile (1 s): best offset %+d s, correlation %.4f, mean relative difference %.3f over %d s" % best)
    return 0


if __name__ == "__main__":
    sys.exit(main())
