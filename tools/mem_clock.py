#!/usr/bin/env python3
"""mem_clock.py - Busca en volcados MEM_DUMP valores que avanzan como un reloj.

Uso:  python tools/mem_clock.py <prefijo> [n_volcados]

Lee <prefijo>_0.bin ... y, para cada direccion alineada, prueba float, double, u32 y u64 en
big-endian. Se queda con los que crecen a ritmo constante en todos los tramos e imprime el
ritmo por segundo. Solo mira memoria del juego en marcha: no interpreta ningun formato. Sin
dependencias.
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


def main():
    prefix = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    dumps = [load(f'{prefix}_{i}.bin') for i in range(n)]
    ts = [d[0] for d in dumps]
    print('tiempos', [round(t - ts[0], 3) for t in ts])
    dt = [ts[i + 1] - ts[i] for i in range(n - 1)]
    total = ts[-1] - ts[0]
    found = []

    def scan(addr, typecode, size, name):
        arrs = []
        for t, recs in dumps:
            a = array.array(typecode)
            buf = recs[addr]
            a.frombytes(buf[:len(buf) - len(buf) % size])
            a.byteswap()
            arrs.append(a)
        for i, vals in enumerate(zip(*arrs)):
            if vals[0] == vals[-1]:
                continue
            d = [vals[k + 1] - vals[k] for k in range(n - 1)]
            if d[0] <= 0 or d[-1] <= 0 or vals[0] != vals[0]:
                continue
            rate = (vals[-1] - vals[0]) / total
            if rate != rate or rate < 0.05 or rate > 1e8:
                continue
            ok = all(d[k] > 0 and abs(d[k] / dt[k] - rate) <= 0.06 * rate + 1e-9 for k in range(n - 1))
            if ok:
                found.append((addr + i * size, name, rate, vals[0], vals[-1]))

    for addr in sorted(dumps[0][1]):
        if not all(addr in d[1] and len(d[1][addr]) == len(dumps[0][1][addr]) for d in dumps):
            continue
        scan(addr, 'f', 4, 'f32')
        scan(addr, 'd', 8, 'f64')
        scan(addr, 'I', 4, 'u32')
        scan(addr, 'Q', 8, 'u64')
    found.sort(key=lambda x: x[2])
    groups = collections.Counter(round(x[2], 1) for x in found)
    print(len(found), 'candidatos; ritmos:', groups.most_common(40))
    for addr, name, rate, a, b in found:
        print(f'{addr:08x} {name} {rate:14.4f}/s   {a!r} -> {b!r}')


if __name__ == '__main__':
    main()
