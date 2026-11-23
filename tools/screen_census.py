#!/usr/bin/env python3
"""screen_census.py - 6d: va una pantalla por frames o por tiempo real?

Uso:  python -I tools/screen_census.py <dir_30> <dir_120> [paso ...]

Lee las capturas BMP de SHOTS (src/main.cpp): <dir>/<paso>_<k>.bmp, tomadas cada <ms> desde
la misma pulsacion y en el mismo reloj, en dos corridas a fps distintos (a = la de 30). Solo
mira la imagen de la ventana; no lee nada del juego. Para cada paso da:

  mov    diferencia media entre capturas seguidas de a (0-255): bajo 1, pantalla quieta
  s_anc  escala de tiempo que mejor lleva b sobre a con la pulsacion como ancla
         (b(t) ~ a(o + s t), o en +-0,3 s): 1 = misma velocidad, 4 = b va 4 veces mas rapido
  s_lag  lo mismo sin ancla: compara cuanto cambia la imagen en un desfase L en b con lo que
         cambia en a, y busca s con D_b(L) ~ D_a(s L). Vale para bucles que no empiezan con
         la pulsacion
  veredicto  igual si las dos dan ~1; por frames si las dos dan >= 1,8

Los pasos de SHOTS van con el intervalo en el nombre del log; aqui se leen de <dir>/shots.txt
(lineas 'paso k t'), que escribe tools/screen_census.ps1 desde el log.
"""
import os
import sys

W, H, F = 480, 270, 5          # captura y factor de reduccion: 96x54 en gris
SCALES = [0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0]


def load_bmp(path):
    with open(path, 'rb') as f:
        data = f.read()
    off = int.from_bytes(data[10:14], 'little')
    w = int.from_bytes(data[18:22], 'little')
    h = int.from_bytes(data[22:26], 'little')
    assert (w, h) == (W, H), path
    px = data[off:]
    sw, sh = W // F, H // F
    out = [0.0] * (sw * sh)
    stride = W * 3
    for y in range(H):
        row = px[(H - 1 - y) * stride:(H - y) * stride]   # BMP de abajo arriba
        oy = (y // F) * sw
        for x in range(0, W):
            b, g, r = row[3 * x], row[3 * x + 1], row[3 * x + 2]
            out[oy + x // F] += 0.114 * b + 0.587 * g + 0.299 * r
    n = F * F
    return [v / n for v in out]


def mad(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def times(d):
    t = {}
    with open(os.path.join(d, 'shots.txt')) as f:
        for line in f:
            name, k, s = line.split()
            t.setdefault(name, {})[int(k)] = float(s)
    return t


def series(d, name, tt):
    ks = sorted(tt.get(name, {}))
    ims, ts = [], []
    for k in ks:
        p = os.path.join(d, f'{name}_{k:02d}.bmp')
        if os.path.exists(p):
            ims.append(load_bmp(p)); ts.append(tt[name][k])
    return ts, ims


def at(ts, ims, t):
    """a en el instante t, interpolada entre las dos capturas vecinas."""
    if t < ts[0] - 1e-6 or t > ts[-1] + 1e-6:
        return None
    for i in range(len(ts) - 1):
        if ts[i] <= t <= ts[i + 1]:
            u = (t - ts[i]) / (ts[i + 1] - ts[i])
            return [x + (y - x) * u for x, y in zip(ims[i], ims[i + 1])]
    return ims[-1]


def anchored(ta, ia, tb, ib):
    best = None
    for s in SCALES:
        for o10 in range(-3, 4):
            o = o10 / 10
            costs = []
            for t, im in zip(tb, ib):
                ref = at(ta, ia, o + s * t)
                if ref is not None:
                    costs.append(mad(im, ref))
            if len(costs) >= 5:
                c = sum(costs) / len(costs)
                if best is None or c < best[0]:
                    best = (c, s, o)
    return best


def lag_curve(ts, ims, lags):
    out = []
    for L in lags:
        vals = []
        for i, t in enumerate(ts):
            j = min(range(len(ts)), key=lambda k: abs(ts[k] - (t + L)))
            if abs(ts[j] - (t + L)) < 0.03 and j != i:
                vals.append(mad(ims[i], ims[j]))
        out.append(sum(vals) / len(vals) if vals else None)
    return out


def lag_scale(ta, ia, tb, ib):
    dt = (ta[-1] - ta[0]) / max(1, len(ta) - 1)
    lags_a = [dt * k for k in range(1, len(ta) // 2)]
    da = lag_curve(ta, ia, lags_a)
    lags_b = [dt * k for k in range(1, 4)]
    db = lag_curve(tb, ib, lags_b)
    best = None
    for s in SCALES:
        err, n = 0.0, 0
        for L, v in zip(lags_b, db):
            ref = None
            x = s * L
            for k in range(len(lags_a) - 1):
                if lags_a[k] <= x <= lags_a[k + 1] and da[k] is not None and da[k + 1] is not None:
                    u = (x - lags_a[k]) / (lags_a[k + 1] - lags_a[k])
                    ref = da[k] + (da[k + 1] - da[k]) * u
            if x < lags_a[0] and da[0] is not None:
                ref = da[0] * x / lags_a[0]
            if ref is not None and v is not None:
                err += abs(v - ref); n += 1
        if n == len(lags_b) and (best is None or err < best[0]):
            best = (err / n, s)
    return best


def main():
    da, db = sys.argv[1], sys.argv[2]
    tta, ttb = times(da), times(db)
    names = sys.argv[3:] or [n for n in tta if n in ttb]
    print(f'{"paso":<12} {"n":>3} {"mov":>6} {"s_anc":>6} {"o":>5} {"c_anc":>6} {"c(1,0)":>6} {"s_lag":>6}  veredicto')
    for name in names:
        ta, ia = series(da, name, tta)
        tb, ib = series(db, name, ttb)
        if len(ia) < 6 or len(ib) < 6:
            print(f'{name:<12} faltan capturas ({len(ia)}, {len(ib)})'); continue
        mov = sum(mad(ia[i], ia[i + 1]) for i in range(len(ia) - 1)) / (len(ia) - 1)
        anc = anchored(ta, ia, tb, ib)
        c10 = sum(mad(x, y) for x, y in zip(ia, ib)) / min(len(ia), len(ib))
        lag = lag_scale(ta, ia, tb, ib) if mov >= 1.0 else None
        sa = anc[1] if anc else None
        sl = lag[1] if lag else None
        if mov < 1.0:
            v = 'quieta'
        elif sa is not None and sl is not None and sa >= 1.8 and sl >= 1.8:
            v = 'POR FRAMES'
        elif sa is not None and sl is not None and sa <= 1.25 and sl <= 1.25:
            v = 'igual'
        else:
            v = 'dudosa'
        fs = lambda x: f'{x:6.2f}' if x is not None else '     -'
        print(f'{name:<12} {len(ib):>3} {mov:6.2f} {fs(sa)} {fs(anc[2] if anc else None):>5} '
              f'{fs(anc[0] if anc else None)} {c10:6.2f} {fs(sl)}  {v}')


if __name__ == '__main__':
    main()
