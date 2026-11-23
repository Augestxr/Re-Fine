#!/usr/bin/env python3
"""pv_align.py - 4j.1: alinea por imagen dos series de capturas de la PV600.

Uso: python tools/pv_align.py <a.txt> <b.txt> [--margin 3]   (filas de tools/shots_gray.ps1)
El tiempo de cada fila sale del nombre: "..._tNN[.N].png" (s desde el ancla), o --a-t0/--b-t0 = mtime
del ancla para las series con hora en el mtime (RPCS3). Para cada captura de a da la de b mas parecida
(correlacion de gris) y la diferencia de tiempos: si es constante no hay deriva.
"""
import argparse
import math
import re


def load(path, t0):
    rows = []
    for line in open(path):
        p = line.split()
        m = re.search(r'(?:^|_)t(\d+(?:\.\d+)?)\.png', p[0])
        t = float(m[1]) if m else float(p[1].replace(',', '.')) - t0
        rows.append((t, [float(v) for v in p[2:]]))
    return sorted(rows)


def corr(a, b):
    n = len(a)
    ma, mb = sum(a) / n, sum(b) / n
    x = [v - ma for v in a]
    y = [v - mb for v in b]
    d = math.sqrt(sum(v * v for v in x) * sum(v * v for v in y))
    return sum(p * q for p, q in zip(x, y)) / d if d else 0.0


ap = argparse.ArgumentParser()
ap.add_argument('a')
ap.add_argument('b')
ap.add_argument('--a-t0', type=float, default=0)
ap.add_argument('--b-t0', type=float, default=0)
ap.add_argument('--range', type=float, default=8)
a = ap.parse_args()
A, B = load(a.a, a.a_t0), load(a.b, a.b_t0)
print('t_a   t_b   b-a   corr')
for ta, va in A:
    best = max(((corr(va, vb), tb) for tb, vb in B if abs(tb - ta) <= a.range), default=(0, 0))
    print(f'{ta:5.1f} {best[1]:5.1f} {best[1] - ta:+5.1f} {best[0]:.3f}')
print('luma media por captura, a:', ' '.join(f'{t:.1f}:{sum(v) / len(v):.0f}' for t, v in A))
print('luma media por captura, b:', ' '.join(f'{t:.1f}:{sum(v) / len(v):.0f}' for t, v in B))
