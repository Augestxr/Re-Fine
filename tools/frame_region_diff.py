"""frame_region_diff.py - 6d.12: cuanto cambia una zona de la imagen entre frames volcados seguidos.

Uso: python -I tools/frame_region_diff.py <dir con frame_NNN.bmp> x0,y0,x1,y1 [x0,y0,x1,y1 ...]

Zonas en fraccion de la imagen (0-1). Por cada par de frames seguidos, diferencia media de luma
(0-255) en cada zona. Una zona que solo cambia cada 4 frames a 120 fps va a 30 Hz. Solo lee BMP de
24 bits (los de CELLMARK_DUMP)."""
import glob, os, sys


def load(path):
    d = open(path, 'rb').read()
    off = int.from_bytes(d[10:14], 'little'); w = int.from_bytes(d[18:22], 'little'); h = int.from_bytes(d[22:26], 'little')
    stride = (w * 3 + 3) & ~3
    return w, h, stride, d[off:]


def region(img, r, step=2):
    w, h, stride, px = img
    x0, y0, x1, y1 = int(r[0] * w), int(r[1] * h), int(r[2] * w), int(r[3] * h)
    out = []
    for y in range(y0, y1, step):
        row = (h - 1 - y) * stride
        for x in range(x0, x1, step):
            o = row + 3 * x
            out.append(0.114 * px[o] + 0.587 * px[o + 1] + 0.299 * px[o + 2])
    return out


def main():
    files = sorted(glob.glob(os.path.join(sys.argv[1], 'frame_*.bmp')))
    regs = [[float(v) for v in a.split(',')] for a in sys.argv[2:]]
    prev = None
    print('par      ' + '  '.join(f'zona{i}' for i in range(len(regs))))
    for f in files:
        cur = [region(load(f), r) for r in regs]
        if prev:
            ds = [sum(abs(a - b) for a, b in zip(p, c)) / len(c) for p, c in zip(prev, cur)]
            print(os.path.basename(f)[6:9], '   ', '  '.join(f'{v:5.2f}' for v in ds))
        prev = cur


main()
