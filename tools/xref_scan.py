#!/usr/bin/env python3
"""xref_scan.py - 6d: quien usa una direccion del EBOOT, sin abrir Ghidra.

Uso:  python tools/xref_scan.py <addr hex> [<addr hex> ...]

Por cada direccion: entradas de datos con ese puntero (la TOC y las tablas), lecturas
`lwz rN,d(r2)` de esas entradas para las TOC del EBOOT, y `bl` directos a la direccion. Da
los sitios de codigo; las funciones se miran luego con Ghidra. Solo lee game/EBOOT.elf.
"""
import array
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def segments(d):
    ph = struct.unpack('>Q', d[32:40])[0]
    n = struct.unpack('>H', d[56:58])[0]
    out = []
    for i in range(n):
        t, _, off, va, _, fs, _, _ = struct.unpack('>IIQQQQQQ', d[ph + 56 * i:ph + 56 * i + 56])
        if t == 1 and fs:
            out.append((va, off, fs))
    return out


def main():
    d = open(os.path.join(ROOT, 'game', 'EBOOT.elf'), 'rb').read()
    segs = segments(d)
    va0, off0, size0 = segs[0]
    words = array.array('I', d[off0:off0 + (size0 & ~3)])
    words.byteswap()
    data = []
    for va, off, fs in segs:
        w = array.array('I', d[off:off + (fs & ~3)])
        w.byteswap()
        data.append((va, w))
    # Las TOC, de los descriptores .opd (entrada, toc): pares con entrada en codigo.
    tocs = {}
    for va, w in data[1:]:
        for i in range(len(w) - 1):
            if va0 <= w[i] < va0 + size0 and w[i] % 4 == 0 and 0xBF0000 < w[i + 1] < 0xD00000:
                tocs[w[i + 1]] = tocs.get(w[i + 1], 0) + 1
    tocs = [t for t, n in tocs.items() if n > 20]
    print('TOC:', ' '.join(hex(t) for t in tocs))
    for arg in sys.argv[1:]:
        target = int(arg, 16)
        print(f'== {target:#x}')
        slots = []
        for va, w in data:
            for i, v in enumerate(w):
                if v == target:
                    slots.append(va + i * 4)
        print(' datos:', ' '.join(hex(s) for s in slots[:24]))
        want = {}
        for toc in tocs:
            for s in slots:
                if -0x8000 <= s - toc < 0x8000:
                    want[(toc, s - toc & 0xFFFF)] = s
        off_set = {o for _, o in want}
        for i, x in enumerate(words):
            if x >> 26 == 32 and (x >> 16 & 31) == 2 and (x & 0xFFFF) in off_set:
                print(f'  lwz {va0 + i * 4:#x} r{x >> 21 & 31} toc+{x & 0xFFFF:#x}')
            elif x >> 26 == 18 and x & 3 == 1:
                li = x & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                if va0 + i * 4 + li == target:
                    print(f'  bl  {va0 + i * 4:#x}')


if __name__ == '__main__':
    main()
