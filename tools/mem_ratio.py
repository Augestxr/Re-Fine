#!/usr/bin/env python3
"""mem_ratio.py - compara dos tandas de volcados MEM_DUMP (a 30 y a 120 fps) y lista las
direcciones cuyo avance por segundo de tiempo real es ~R veces mayor en la segunda tanda.

Uso:  python tools/mem_ratio.py <prefijo_30> <prefijo_120> [n] [ratio=4]
Tipos (big-endian): f32, u32/s32 (por diferencia con signo), u64 no. Alineado a 4. Solo lee."""
import array
import struct
import sys


def load(path):
    with open(path, 'rb') as f:
        t = struct.unpack('<d', f.read(8))[0]
        recs = {}
        while True:
            h = f.read(8)
            if len(h) < 8:
                break
            a, s = struct.unpack('<II', h)
            recs[a] = f.read(s)
    return t, recs


def arr(buf, code):
    a = array.array(code)
    a.frombytes(buf[:len(buf) - len(buf) % 4])
    a.byteswap()
    return a


def rates(prefix, n, code):
    ds = [load(f'{prefix}_{i}.bin') for i in range(n)]
    ts = [d[0] for d in ds]
    out = {}
    for addr, b0 in ds[0][1].items():
        if not all(addr in d[1] and len(d[1][addr]) == len(b0) for d in ds):
            continue
        A = [arr(d[1][addr], code) for d in ds]
        res = []
        for i, vals in enumerate(zip(*A)):
            if vals[0] == vals[-1] and vals[1] == vals[2]:
                continue
            if code == 'f' and any(v != v or abs(v) > 1e9 for v in vals):
                continue
            dl = sorted(abs(vals[k + 1] - vals[k]) / (ts[k + 1] - ts[k]) for k in range(n - 1))
            if dl[0] <= 0:
                continue
            r = dl[(n - 1) // 2]            # mediana: los saltos por vuelta del bucle quedan fuera
            if r != r or r < 1e-3 or r > 1e7:
                continue
            res.append((addr + i * 4, r, vals[0], vals[-1]))
        for x in res:
            out[x[0]] = x[1:]
    return out


def main():
    p30, p120 = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    R = float(sys.argv[4]) if len(sys.argv) > 4 else 4.0
    for code, name in (('f', 'f32'), ('i', 's32')):
        a = rates(p30, n, code)
        b = rates(p120, n, code)
        hits = []
        for ad, (r1, v0, v1) in a.items():
            if ad in b:
                r2 = b[ad][0]
                if r1 and abs(r2 / r1 / R - 1) < 0.25:
                    hits.append((ad, r1, r2, v0, v1))
        print(f'## {name}: {len(a)} suben a 30, {len(b)} a 120, {len(hits)} con ratio ~{R}')
        for ad, r1, r2, v0, v1 in hits[:60]:
            print(f'{ad:08x} {name} r30={r1:12.4f}/s r120={r2:12.4f}/s v30 {v0!r}->{v1!r}')


if __name__ == '__main__':
    main()
