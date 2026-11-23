"""text_jitter.py - 6d.14 A4: centroide de los pixeles de texto (blancos, >235) por zona y frame en los BMP de CELLMARK_DUMP. Uso: python -I tools/text_jitter.py <dir> [paso]. Zonas: filas 2-5 de la lista de canciones a 1280x720."""
import glob, os, sys
ZONES = {'f2': (100, 243, 340, 273), 'f3': (100, 315, 340, 345), 'f4': (100, 387, 340, 417), 'f5': (100, 459, 340, 489), 'bpm': (790, 340, 960, 375), 'hs': (800, 478, 1000, 500)}
def load(p):
    d = open(p, 'rb').read()
    off = int.from_bytes(d[10:14], 'little'); w = int.from_bytes(d[18:22], 'little'); h = int.from_bytes(d[22:26], 'little')
    return w, h, (w * 3 + 3) & ~3, d[off:]
def cen(img, z, thr=235):
    w, h, st, px = img; x0, y0, x1, y1 = z; n = sx = sy = 0
    for y in range(y0, y1):
        row = (h - 1 - y) * st
        for x in range(x0, x1):
            i = row + 3 * x
            if px[i] > thr and px[i + 1] > thr and px[i + 2] > thr:
                n += 1; sx += x; sy += y
    return (n, sx / n if n else 0, sy / n if n else 0)
fs = sorted(glob.glob(os.path.join(sys.argv[1], 'frame_*.bmp')))
step = int(sys.argv[2]) if len(sys.argv) > 2 else 1
for k, f in enumerate(fs[::step]):
    img = load(f)
    print(k * step, ' '.join('%s %d %.2f %.2f' % ((z,) + cen(img, ZONES[z])) for z in ZONES))
