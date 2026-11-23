#!/usr/bin/env python3
"""tex_race.py - 4c.5: lee un volcado TEX_RACE (patches/0127) de la textura de la pelicula.

Uso: python tools/tex_race.py <volcado> [--frames N] [--from T] [--detail]

Una subida esta entera si todas sus filas firmadas coinciden con UNA imagen de las que entrego
cellVdecGetPicture (eventos PIC). Cortada: no coincide con ninguna, pero sus filas salen de dos
imagenes o mas. Se comparan todas las subidas, no capturas sueltas. --detail da la linea de
tiempos de los frames, con los tiempos relativos a la primera fila de la copia (TOP).
"""
import argparse
import collections
import struct
import sys

NAMES = {1: 'PUT', 2: 'TOP', 3: 'BOT', 4: 'REC', 5: 'LABEL', 6: 'FLIP', 7: 'UP', 8: 'PIC',
         9: 'DRAIN', 10: 'DRAIN_END', 11: 'UP_SRC'}


def read(path):
    ev = []
    with open(path, 'rb') as f:
        d = f.read()
    o = 0
    qpf = None
    while o + 24 <= len(d):
        t, ty, a, b, n = struct.unpack_from('<qIIII', d, o)
        o += 24
        if o + n * 4 > len(d):
            break
        sig = struct.unpack_from(f'<{n}I', d, o) if n else None
        o += n * 4
        if ty == 0:
            qpf = t
            continue
        ev.append((t, ty, a, b, sig))
    return qpf, ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--frames', type=int, default=6)
    ap.add_argument('--at', type=int, default=600, help='subida desde la que dar el detalle')
    ap.add_argument('--detail', action='store_true')
    args = ap.parse_args()
    qpf, ev = read(args.dump)
    ms = lambda t: t * 1000.0 / qpf
    c = collections.Counter(e[1] for e in ev)
    print('eventos:', {NAMES.get(k, k): v for k, v in sorted(c.items())})
    pics = [e for e in ev if e[1] == 8]
    if not pics:
        sys.exit('sin imagenes')
    rows = collections.defaultdict(set)      # (fila, firma) -> imagenes
    uniq = {}
    for i, p in enumerate(pics):
        uniq.setdefault(p[4], i)
        for y, s in enumerate(p[4]):
            rows[(y, s)].add(i)

    def classify(sig):
        whole = sig in uniq
        if whole:
            return 'entera', uniq[sig], 0
        src = [rows.get((y, s), set()) for y, s in enumerate(sig)]
        known = sum(1 for s in src if s)
        # cuantas imagenes hacen falta, de arriba abajo, para explicar las filas
        parts, cur = 0, set()
        cuts = []
        for y, s in enumerate(src):
            if not s:
                continue
            nxt = cur & s
            if not nxt:
                parts += 1
                if cur:
                    cuts.append(y * 4)
                nxt = s
            cur = nxt
        return ('cortada' if parts >= 2 else 'desconocida'), known, cuts[0] if cuts else -1

    for name, ty in (('REC', 4), ('UP', 7)):
        res = collections.Counter()
        cuts = []
        for e in ev:
            if e[1] != ty:
                continue
            k, a, b = classify(e[4])
            res[k] += 1
            if k == 'cortada':
                cuts.append(b)
        tot = sum(res.values())
        pct = 100.0 * res['cortada'] / tot if tot else 0
        print(f'{name}: {tot} total, {dict(res)}, cortadas {pct:.2f} %'
              + (f', fila del corte min {min(cuts)} max {max(cuts)}' if cuts else ''))

    # orden de presentacion: cada subida entera, ?la imagen va en orden?
    seq = [classify(e[4]) for e in ev if e[1] == 7]
    ids = [s[1] for s in seq if s[0] == 'entera']
    d = collections.Counter(b - a for a, b in zip(ids, ids[1:]))
    print('salto entre subidas enteras seguidas (imagenes):', sorted(d.items())[:12])

    # linea de tiempos: por subida, tiempos respecto a TOP anterior
    tops = [e[0] for e in ev if e[1] == 2]
    bots = [e[0] for e in ev if e[1] == 3]
    puts = [e[0] for e in ev if e[1] == 1]
    import bisect

    def stats(v):
        v = sorted(v)
        return f'n={len(v)} p5={v[len(v)//20]:.2f} p50={v[len(v)//2]:.2f} p95={v[len(v)*19//20]:.2f}' if v else 'n=0'

    dur, rec_after_put, up_after_rec, top_after_put, top_after_lab, top_after_flip = [], [], [], [], [], []
    labels = [e[0] for e in ev if e[1] == 5]
    flips = [e[0] for e in ev if e[1] == 6]
    recs = [e[0] for e in ev if e[1] == 4]
    ups = [e[0] for e in ev if e[1] == 11]
    for t in tops:
        i = bisect.bisect_right(bots, t)
        if i < len(bots) and ms(bots[i] - t) < 30:
            dur.append(ms(bots[i] - t))
        for src, out in ((puts, top_after_put), (labels, top_after_lab), (flips, top_after_flip)):
            j = bisect.bisect_left(src, t) - 1
            if j >= 0 and ms(t - src[j]) < 40:
                out.append(ms(t - src[j]))
    for t in recs:
        j = bisect.bisect_left(puts, t) - 1
        if j >= 0:
            rec_after_put.append(ms(t - puts[j]))
        i = bisect.bisect_right(ups, t)
        if i < len(ups) and ms(ups[i] - t) < 40:
            up_after_rec.append(ms(ups[i] - t))
    print('ms: copia TOP->BOT      ', stats(dur))
    print('ms: put->TOP            ', stats(top_after_put))
    print('ms: label->TOP          ', stats(top_after_lab))
    print('ms: flip->TOP           ', stats(top_after_flip))
    print('ms: put->REC            ', stats(rec_after_put))
    print('ms: REC->UP             ', stats(up_after_rec))
    labc = collections.Counter(e[2] & 0x7FFFFFFF for e in ev if e[1] == 5)
    print('labels (offset: veces):', [(hex(k), v) for k, v in labc.most_common(10)])

    if args.detail:
        idx = [i for i, e in enumerate(ev) if e[1] == 7]
        for u in idx[args.at:args.at + args.frames]:
            t0 = ev[u][0]
            lo = bisect.bisect_left([e[0] for e in ev], t0 - qpf // 24)
            hi = bisect.bisect_right([e[0] for e in ev], t0 + qpf // 200)
            print('--- subida', classify(ev[u][4]))
            last = None
            for e in ev[lo:hi]:
                if e[1] in (9, 10) and last in (9, 10):
                    last = e[1]
                    continue
                last = e[1]
                print(f'  {ms(e[0] - t0):+9.3f} {NAMES.get(e[1], e[1]):9s} {e[2]:#x} {e[3]:#x}')


if __name__ == '__main__':
    main()
