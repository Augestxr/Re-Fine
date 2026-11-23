#!/usr/bin/env python3
"""frame_census.py - 6d: que avanza por frame y que por tiempo real.

Uso:  python tools/frame_census.py <prefijo_a> <prefijo_b> <primero> <n> [ratio]

Lee <prefijo>_<primero>.bin ... (volcados de MEM_DUMP de la misma escena, corrida a) a un
fps y b al otro) y busca en cada corrida los f32 y u32 de big-endian que crecen o bajan a ritmo
constante. De los que estan en las dos, imprime el cociente de ritmos b/a: 1 = tiempo real,
el cociente de fps = por frame. Solo mira memoria del juego: no interpreta ningun formato.
"""
import array
import collections
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
            addr, size = struct.unpack('<II', h)
            recs[addr] = f.read(size)
    return t, recs


def clocks(prefix, first, n):
    dumps = [load(f'{prefix}_{i}.bin') for i in range(first, first + n)]
    ts = [d[0] for d in dumps]
    total = ts[-1] - ts[0]
    dt = [ts[i + 1] - ts[i] for i in range(n - 1)]
    found = {}
    for addr in sorted(dumps[0][1]):
        if not all(addr in d[1] and len(d[1][addr]) == len(dumps[0][1][addr]) for d in dumps):
            continue
        for code, name in (('f', 'f32'), ('I', 'u32'), ('i', 's32')):
            arrs = []
            for _, recs in dumps:
                a = array.array(code)
                buf = recs[addr]
                a.frombytes(buf[:len(buf) & ~3])
                a.byteswap()
                arrs.append(a)
            for i, vals in enumerate(zip(*arrs)):
                if vals[0] == vals[-1] or vals[0] != vals[0]:
                    continue
                rate = (vals[-1] - vals[0]) / total
                if not 0.05 < abs(rate) < 1e6:
                    continue
                if all(abs((vals[k + 1] - vals[k]) / dt[k] - rate) <= 0.08 * abs(rate) for k in range(n - 1)):
                    found[(addr + i * 4, name)] = (rate, vals[0], vals[-1])
    return found


def main():
    a_pre, b_pre, first, n = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    want = float(sys.argv[5]) if len(sys.argv) > 5 else 2.0
    a, b = clocks(a_pre, first, n), clocks(b_pre, first, n)
    print(f'a: {len(a)} relojes, b: {len(b)}, en las dos: {len(a.keys() & b.keys())}')
    hist = collections.Counter()
    rows = []
    for k in sorted(a.keys() & b.keys()):
        r = b[k][0] / a[k][0]
        hist[round(r, 1)] += 1
        rows.append((k, r))
    print('cocientes:', sorted(hist.items(), key=lambda x: -x[1])[:16])
    for (addr, name), r in rows:
        if abs(r - 1) > 0.1:
            tag = 'POR FRAME' if abs(r - want) < 0.15 * want else ''
            print(f'{addr:08x} {name} a {a[(addr, name)][0]:12.3f}/s  b {b[(addr, name)][0]:12.3f}/s  b/a {r:6.3f} {tag}')
    only = sorted(set(b) - set(a))
    print(f'solo en b: {len(only)}; solo en a: {len(set(a) - set(b))}')


if __name__ == '__main__':
    main()
