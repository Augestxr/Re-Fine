#!/usr/bin/env python3
"""frame_steps.py - 6d.10: cuantos frames reales cambian de verdad en una zona de la imagen.

Uso:  python -I tools/frame_steps.py <dir> <x0,y0,x1,y1> [umbral]

<dir> tiene los frames consecutivos que vuelca el backend con CELLMARK_DUMP=N y
CELLMARK_DUMP_SKIP=M (frame_000.bmp, frame_001.bmp, ...; 24 bits, de abajo arriba). Para cada
par de frames seguidos da la diferencia media de luminancia en el recorte (pixeles de la
imagen volcada) y cuenta los pares por debajo del umbral (por defecto 0,05): un personaje que
avanza a 30 Hz a 120 fps deja 3 pares quietos de cada 4; uno interpolado, ninguno.
Solo lee las capturas: no interpreta ningun formato del juego.
"""
import glob
import os
import sys


def load(path, box):
    with open(path, 'rb') as f:
        data = f.read()
    off = int.from_bytes(data[10:14], 'little')
    w = int.from_bytes(data[18:22], 'little')
    h = int.from_bytes(data[22:26], 'little')
    stride = (w * 3 + 3) & ~3
    x0, y0, x1, y1 = box
    out = []
    for y in range(y0, min(y1, h)):
        row = data[off + (h - 1 - y) * stride:]
        for x in range(x0, min(x1, w)):
            b, g, r = row[3 * x], row[3 * x + 1], row[3 * x + 2]
            out.append(0.114 * b + 0.587 * g + 0.299 * r)
    return out


def main():
    d, box = sys.argv[1], tuple(int(v) for v in sys.argv[2].split(','))
    thr = float(sys.argv[3]) if len(sys.argv) > 3 else 0.05
    files = sorted(glob.glob(os.path.join(d, '*.bmp')))
    ims = [load(f, box) for f in files]
    diffs = [sum(abs(a - b) for a, b in zip(p, q)) / len(p) for p, q in zip(ims, ims[1:])]
    still = sum(1 for v in diffs if v < thr)
    print(f'{len(files)} frames, recorte {box}, umbral {thr}')
    print('dif ' + ' '.join(f'{v:.2f}' for v in diffs))
    print(f'pares quietos {still} de {len(diffs)}')


if __name__ == '__main__':
    main()
