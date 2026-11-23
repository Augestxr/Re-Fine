#!/usr/bin/env python3
"""speed_cmp.py - la prueba de velocidad: compara los eventos del juego ([ev] de src/evt.cpp) de dos corridas
del mismo recorrido a distintos fps.

Uso:  python tools/speed_cmp.py <base.err.log> <otra.err.log> [--noise <base2.err.log>] [--tol 40] [--all]

Cada evento se mide desde la ultima pulsacion o marca del guion (la causa). Si en la otra corrida llega mas
de --tol ms antes o despues que en la base, algo cuenta frames en vez de tiempo. Con --noise (otra corrida a
los fps de la base) los eventos que ya varian entre dos corridas iguales (cargas de disco) se listan aparte.
Tambien resume, por tramo, cuantos frames dibujados difieren del anterior (lineas motion).
Sale con 1 si hay diferencias.
"""
import difflib
import re
import sys

EV = re.compile(r'^\[ev\] ([\d.]+) (\S+) (.*)$')


def load(path):
    evs, motion = [], []
    anchor_t, anchor = 0.0, 'boot'
    mark = 'boot'
    fresh = False
    with open(path, encoding='utf-8', errors='replace') as f:
        for ln in f:
            m = EV.match(ln.rstrip())
            if not m:
                continue
            t, kind, sig = float(m.group(1)), m.group(2), m.group(3)
            if kind == 'motion':
                d = dict(x.split('=') for x in sig.split())
                motion.append((mark, int(d['frames']), int(d['moved']), int(d['ticks']), float(d.get('pace', 0))))
                continue
            if kind in ('route', 'count'):
                continue
            if kind == 'mark':
                mark = sig
            if kind in ('key', 'mark'):
                anchor_t, anchor = t, kind + ' ' + sig
                fresh = kind == 'key' or fresh
                evs.append({'t': t, 'sig': kind + ' ' + sig, 'rel': 0.0, 'anchor': anchor, 'mark': mark, 'isanchor': True, 'react': False})
                continue
            # lo primero que pasa tras una pulsacion es la reaccion del juego: su retraso depende de en que
            # momento del paso cae la tecla (hasta un paso de 33 ms a 30 fps). Se informa aparte y lo que sigue
            # se mide desde ella.
            react = fresh
            evs.append({'t': t, 'sig': kind + ' ' + sig, 'rel': t - anchor_t, 'anchor': anchor, 'mark': mark, 'isanchor': False, 'react': react})
            if fresh:
                fresh = False
                anchor_t, anchor = t, 'reaccion a ' + anchor
    return evs, motion


def match(a, b):
    sm = difflib.SequenceMatcher(None, [e['sig'] for e in a], [e['sig'] for e in b], autojunk=False)
    pairs = []
    for i, j, n in sm.get_matching_blocks():
        for k in range(n):
            pairs.append((a[i + k], b[j + k]))
    return pairs


def main():
    args = sys.argv[1:]
    tol, noise, show_all = 50.0, None, False
    pos = []
    i = 0
    while i < len(args):
        if args[i] == '--tol':
            tol = float(args[i + 1]); i += 2
        elif args[i] == '--noise':
            noise = args[i + 1]; i += 2
        elif args[i] == '--all':
            show_all = True; i += 1
        else:
            pos.append(args[i]); i += 1
    a, ma = load(pos[0])
    b, mb = load(pos[1])
    noisy = set()
    if noise:
        n, _ = load(noise)
        qx = qy = None
        for x, y in match(a, n):
            if abs((x['t'] - (qx['t'] if qx else 0.0)) - (y['t'] - (qy['t'] if qy else 0.0))) > tol:
                noisy.add(id(x))
            qx, qy = x, y
    pairs = match(a, b)
    na, nb = len(a), len(b)
    bad, loud, lat = [], [], []
    px = py = None
    for x, y in pairs:
        # el intervalo desde el evento emparejado anterior: una carga que tarda distinto solo mueve un intervalo
        ix = x['t'] - (px['t'] if px else 0.0)
        iy = y['t'] - (py['t'] if py else 0.0)
        x['iv'], y['iv'] = ix, iy
        x['prev'] = px['sig'] if px else 'boot'
        px, py = x, y
        if x['isanchor']:
            continue
        d = iy - ix
        if x['react']:
            lat.append((x['rel'], y['rel']))
            if abs(d) <= 2.5 * tol:
                continue
        if abs(d) > tol:
            (loud if id(x) in noisy else bad).append((x, y, d))
    print('eventos: base %d, otra %d, emparejados %d (sin pareja: %d / %d)' % (na, nb, len(pairs), na - len(pairs), nb - len(pairs)))
    if lat:
        print('reaccion a las pulsaciones (ms, media / peor): base %.0f / %.0f, otra %.0f / %.0f' % (
            sum(l[0] for l in lat) / len(lat), max(l[0] for l in lat), sum(l[1] for l in lat) / len(lat), max(l[1] for l in lat)))
    by = {}
    for x, y, d in bad:
        by.setdefault(x['mark'], []).append((x, y, d))
    print('DIFERENCIAS (intervalo desde el evento anterior, > %.0f ms): %d' % (tol, len(bad)) + ('; ruidosas aparte: %d' % len(loud) if noise else ''))
    for mk, lst in by.items():
        print('  [%s] %d' % (mk, len(lst)))
        for x, y, d in (lst if show_all else lst[:10]):
            print("    %+8.0f ms  %-44s base %.0f ms tras '%s'" % (d, x['sig'][:44], x['iv'], x['prev'][:40]))
        if not show_all and len(lst) > 10:
            print('    ... %d mas (--all)' % (len(lst) - 10))

    def marks(evs):
        return [(e['sig'], e['t']) for e in evs if e['sig'].startswith('mark ')]
    A, B = marks(a), marks(b)
    print('tramos (ms entre marcas): base / otra / diferencia')
    for k in range(1, min(len(A), len(B))):
        da, db = A[k][1] - A[k - 1][1], B[k][1] - B[k - 1][1]
        print('  %-16s %9.0f %9.0f %+8.0f' % (A[k - 1][0][5:], da, db, db - da))

    def mot(m):
        out = {}
        for mk, fr, mv, tk, pc in m:
            o = out.setdefault(mk, [0, 0, 0, 0.0, 0]); o[0] += fr; o[1] += mv; o[2] += tk; o[3] += pc; o[4] += 1
        return out
    MA, MB = mot(ma), mot(mb)
    print('movimiento (frames que cambian / frames; ticks; ritmo = tiempo del juego / tiempo real): base | otra')
    for mk in MB:
        x = MA.get(mk, [0, 0, 0, 0.0, 0]); y = MB[mk]
        print('  %-16s %5d/%5d (%5d) ritmo %.3f  |  %5d/%5d (%5d) ritmo %.3f  %5.1f %%' % (mk, x[1], x[0], x[2], x[3] / max(1, x[4]), y[1], y[0], y[2], y[3] / max(1, y[4]), 100.0 * y[1] / max(1, y[0])))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
