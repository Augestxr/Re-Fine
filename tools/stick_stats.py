#!/usr/bin/env python3
"""stick_stats.py - 5d: mide en una grabacion de stick_record.c el reposo y el rebote al soltar.

Uso:  python tools/stick_stats.py work/5d/sticks_ps4.csv [--dz 0.20 --enter 0.35]

Radio r = sqrt(x^2 + y^2) / 32767, topado a 1. Un golpe empieza cuando r pasa de 0.5 y acaba al
caer por debajo de 0.12. Tras cada golpe mira 300 ms: el pico de r, si la direccion va al lado
contrario y cuanto tarda en quedarse por debajo de 0.06. Con --dz y --enter simula, leyendo cada
33,3 ms como hace F, cuantas salidas del centro veria el juego con esa histeresis.
"""
import argparse
import csv
import math
import statistics as st


def load(path):
    rows = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.setdefault(int(r['pad']), []).append(
                (float(r['ms']), int(r['lx']), int(r['ly']), int(r['rx']), int(r['ry'])))
    return rows


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))] if v else 0.0


def analyse(t, x, y, name):
    r = [min(1.0, math.hypot(a, b) / 32767) for a, b in zip(x, y)]
    flicks, rest = [], []
    i, n = 0, len(t)
    while i < n:
        if r[i] < 0.06:
            rest.append(r[i])
        if r[i] > 0.5:
            s = i
            while i < n and r[i] >= 0.12:
                i += 1
            if i >= n:
                break
            peak = max(range(s, i), key=lambda k: r[k])
            end = t[i]
            pk, last = 0.0, 0.0
            opposite = 0.0
            j = i
            while j < n and t[j] - end < 300:
                if r[j] > pk:
                    pk = r[j]
                d = (x[j] * x[peak] + y[j] * y[peak])
                if d < 0 and r[j] > opposite:
                    opposite = r[j]
                if r[j] >= 0.06:
                    last = t[j] - end
                j += 1
            flicks.append((end - t[s], r[peak], pk, opposite, last))
            i = j
            continue
        i += 1
    print(f'  {name}: {len(flicks)} golpes (r>0.5)')
    if flicks:
        print('    duracion ms: mediana %.0f, max %.0f; pico mediana %.2f' % (
            st.median(f[0] for f in flicks), max(f[0] for f in flicks), st.median(f[1] for f in flicks)))
        reb = [f[2] for f in flicks]
        opp = [f[3] for f in flicks]
        dur = [f[4] for f in flicks]
        print('    rebote r: mediana %.3f, p90 %.3f, max %.3f; lado contrario max %.3f' % (
            st.median(reb), pct(reb, .9), max(reb), max(opp)))
        print('    hasta quedar < 0.06: mediana %.0f ms, p90 %.0f, max %.0f' % (
            st.median(dur), pct(dur, .9), max(dur)))
    return flicks


def simulate(t, x, y, dz, enter):
    """Salidas del centro vistas por F a 30 Hz con la histeresis, y sin nada."""
    out = False
    ref = (0, 0)
    exits = raw = 0
    was = False
    k = 0
    tt = t[0]
    while tt < t[-1]:
        while k + 1 < len(t) and t[k + 1] <= tt:
            k += 1
        r = min(1.0, math.hypot(x[k], y[k]) / 32767)
        if out and (r < dz or x[k] * ref[0] + y[k] * ref[1] < 0):
            out = False
        elif not out and r >= enter:
            out = True
            ref = (x[k], y[k])
            exits += 1
        hit = r >= 0.5
        if hit and not was:
            raw += 1
        was = hit
        tt += 33.3
    return exits, raw


def chain(t, x, y, dz, enter, rearm, hz):
    """6d: la cadena entera. El host a 500 Hz (rearm: ms bajo dz antes de mostrar el centro) y F
    a hz, que solo da el centro por bueno con 2 lecturas seguidas (0x379C4). Golpes que ve F."""
    out = False
    armed = True
    ref = (0, 0)
    since = None        # desde cuando esta bajo dz
    value = (0, 0)      # lo que el host entrega; (0, 0) es el centro
    k = 0
    reads = []
    tt = t[0]
    step = 2.0
    while tt < t[-1]:
        while k + 1 < len(t) and t[k + 1] <= tt:
            k += 1
        r = min(1.0, math.hypot(x[k], y[k]) / 32767)
        if out:
            if r < dz or x[k] * ref[0] + y[k] * ref[1] < 0:
                out = armed = False
                since = None
            else:
                value = (x[k], y[k])
        if not out:
            if not armed:
                if r >= dz:
                    since = None
                elif since is None:
                    since = tt
                if since is not None and tt - since >= rearm:
                    armed = True
            if armed:
                value = (0, 0)
                if r >= enter:
                    out = True
                    ref = value = (x[k], y[k])
        reads.append((tt, value))
        tt += step
    hits = 0
    seen_out = False
    centred = 0
    i = 0
    tt = t[0]
    while tt < t[-1]:
        while i + 1 < len(reads) and reads[i + 1][0] <= tt:
            i += 1
        off = reads[i][1] != (0, 0)
        centred = 0 if off else centred + 1
        if off and not seen_out:
            hits += 1
            seen_out = True
        elif not off and centred >= 2:
            seen_out = False
        tt += 1000.0 / hz
    return hits


ap = argparse.ArgumentParser()
ap.add_argument('csv')
ap.add_argument('--dz', type=float, default=0.0)
ap.add_argument('--enter', type=float, default=0.0)
ap.add_argument('--rearm', type=float, nargs='*', default=None, help='ms; tabla de la cadena entera')
a = ap.parse_args()
for pad, rows in load(a.csv).items():
    t = [r[0] for r in rows]
    print(f'mando {pad}: {len(rows)} lineas, {t[-1] / 1000:.1f} s')
    for name, ix, iy in (('izquierdo', 1, 2), ('derecho', 3, 4)):
        x = [r[ix] for r in rows]
        y = [r[iy] for r in rows]
        analyse(t, x, y, name)
        if a.dz:
            print('    simulado a 30 Hz: dz %.2f enter %.2f -> %d salidas; umbral 0.5 sin histeresis -> %d' % (
                (a.dz, a.enter) + simulate(t, x, y, a.dz, a.enter)))
        for rearm in a.rearm or []:
            print('    cadena rearm %3.0f ms: %s' % (rearm, ', '.join(
                '%d Hz %d' % (hz, chain(t, x, y, a.dz, a.enter, rearm, hz)) for hz in (30, 60, 120, 144))))
