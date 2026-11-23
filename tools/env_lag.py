#!/usr/bin/env python3
"""env_lag.py - Retardo entre una mezcla con QPC por bloque y lo capturado en loopback.

Uso:  python tools/env_lag.py <mezcla.f32> <prefijo_loopback_n> [--from S] [--secs S]

Correla la envolvente de 1 ms de las dos sobre el tiempo de pared, sin mirar el contenido
muestra a muestra: vale cuando la captura es otra mezcla de canales, como en RPCS3.
"""
import argparse
import array
import math

ap = argparse.ArgumentParser()
ap.add_argument('mix')
ap.add_argument('cap')
ap.add_argument('--from', dest='start', type=float, default=2)
ap.add_argument('--secs', type=float, default=12)
ap.add_argument('--max-lag', type=int, default=300)
a = ap.parse_args()

mix = array.array('f')
mix.frombytes(open(a.mix, 'rb').read())
qpc = array.array('q')
qpc.frombytes(open(a.mix + '.qpc', 'rb').read())
hz = 1e7
t0 = qpc[0] / hz + a.start
n = int(a.secs * 1000)
span = a.max_lag + 50
env_m = [0.0] * (n + span)
env_c = [0.0] * (n + span)
for b in range(len(qpc)):
    tb = qpc[b] / hz
    for i in range(0, 256, 8):
        k = int((tb + i / 48000 - t0) * 1000)
        if 0 <= k < n + span:
            env_m[k] += abs(mix[(b * 256 + i) * 2]) + abs(mix[(b * 256 + i) * 2 + 1])
cap = array.array('f')
cap.frombytes(open(a.cap + '.f32', 'rb').read())
at = 0
for line in open(a.cap + '.tsv'):
    if line.startswith('#'):
        continue
    t, fr, _ = line.split('\t')
    t, fr = float(t), int(fr)
    for i in range(0, fr, 8):
        k = int((t + i / 48000 - t0) * 1000)
        if 0 <= k < n + span and (at + i) * 2 + 1 < len(cap):
            env_c[k] += abs(cap[(at + i) * 2]) + abs(cap[(at + i) * 2 + 1])
    at += fr


def norm(v):
    m = sum(v) / len(v)
    w = [x - m for x in v]
    s = math.sqrt(sum(x * x for x in w)) or 1
    return [x / s for x in w]


x = norm(env_m[:n])
best = []
for lag in range(0, a.max_lag):
    y = norm(env_c[lag:lag + n])
    best.append((sum(p * q for p, q in zip(x, y)), lag))
best.sort(reverse=True)
print('mejor retardo', best[0][1], 'ms, correlacion %.3f; siguientes' % best[0][0], [b[1] for b in best[1:4]])
