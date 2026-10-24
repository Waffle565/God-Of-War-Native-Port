#!/usr/bin/env python3
"""Pad script of the deterministic fight benchmark (docs/benchmark.md).

New game from the title (wall-clock menu presses, as tools/movies.py), then from the gameplay start the console
bot's fight pattern: square, square, triangle every 5.5 s, Kratos does not walk. With GOW_BENCH=1 the "g" times
count game frames / 60, so every run presses on the same frames.

usage: bench_script.py [game seconds, default 240] [menu delay s, default 30] > bench.txt
"""
import sys

total = float(sys.argv[1]) if len(sys.argv) > 1 else 240.0
delay = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
keys = [f'{delay + d:g}:{k}' for d, k in ((0, 'start'), (9, 'x'), (15, 'x'), (21, 'x'), (27, 'x'))]
t = 3.0
while t < total:
    keys += [f'g{t:.2f}:cuadrado*0.15', f'g{t + 0.4:.2f}:cuadrado*0.15', f'g{t + 0.8:.2f}:triangulo*0.15']
    t += 5.5
print('\n'.join(keys))
