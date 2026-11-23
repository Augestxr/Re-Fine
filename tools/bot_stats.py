#!/usr/bin/env python3
"""bot_stats.py - Lee las lineas [BOT] de una corrida con PAD_BOT y cruza pulsacion y nota.

Uso:  python tools/bot_stats.py work/run/<tag>.err.log [...]

Cada contador de grado que sube tras una pulsacion se atribuye a esa pulsacion, solo si no hay otra
nota pulsada a menos de 0,12 s. Contadores: +0C COOL, +10 GOOD, +14 SAFE, +18 BAD, +1C AWFUL.
"""
import collections
import re
import sys

GRADE = {'0C': 'COOL', '10': 'GOOD', '14': 'SAFE', '18': 'BAD', '1C': 'AWFUL'}
by = collections.defaultdict(list)
for path in sys.argv[1:]:
    press, counts = [], []
    totals = {}
    for line in open(path, errors='replace'):
        m = re.match(r'\[BOT\] press t=([\d.]+) type=(\d+) want=(-?[\d.]+) r=(-?[\d.]+)', line)
        if m:
            press.append((float(m[1]), int(m[2]), float(m[4])))
            continue
        m = re.match(r'\[BOT\] count t=([\d.]+) \+0x(\w+)=(\d+)', line)
        if m:
            counts.append((float(m[1]), m[2]))
            totals[m[2]] = int(m[3])
    print(path, 'pulsaciones', len(press), 'finales', {GRADE.get(k, k): v for k, v in totals.items()})
    for i, (t, ty, r) in enumerate(press):
        if ty >= 8:
            continue
        if (i and t - press[i - 1][0] < 0.12) or (i + 1 < len(press) and press[i + 1][0] - t < 0.12):
            continue
        for ct, c in counts:
            if t < ct < t + 0.07 and c in GRADE:
                by[GRADE[c]].append(r)
                break
for g in ('COOL', 'GOOD', 'SAFE', 'BAD', 'AWFUL'):
    v = sorted(by[g])
    if v:
        print(f'{g:6} n={len(v):3}  r al pulsar: {v[0]:+.4f} .. {v[-1]:+.4f}   |r| {min(abs(x) for x in v):.4f} .. {max(abs(x) for x in v):.4f}')
