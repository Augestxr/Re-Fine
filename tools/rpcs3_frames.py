"""rpcs3_frames.py - 6d.13 (Q3): frames de la grabacion de RPCS3 en segundos de cancion.

Uso:  python -I tools/rpcs3_frames.py work/5c/<Tag> <dir_salida> 20,29.1,45.6 [--n 1] [--cuts <err.log de Re:Fine>]

Entrada (de tools/rpcs3_song.ps1 -Record):
  <Tag>.mkv          la ventana de juego (ddagrab, 30 fps)
  <Tag>.sync         "wall <s> qpc <s>": los dos relojes tomados al lanzar ffmpeg
  <Tag>.watch        rpcs3_watch: bloques de audio (tipo 1) con su QPC; <Tag>.watch.txt, la terna
Cadena: segundo de cancion -> QPC del bloque de audio -> tiempo desde que se lanzo ffmpeg.
ddagrab no da la hora de su primer frame, asi que el desfase se mide por contenido con --cuts: los
cortes de camara que Re:Fine apunta en su log ("cam cut song X") son saltos grandes de imagen; se
buscan en el video cerca de donde deberian caer y la mediana de la diferencia corrige el desfase.
Con --n N saca N frames seguidos desde ese instante (rp_<seg>_<k>.png).
"""
import os
import re
import statistics
import subprocess
import sys

W, H = 64, 36


def song_to_qpc(pre):
    """(segundo de cancion, QPC). El reloj f32 de rpcs3_watch se para a los ~18 s (q3cf); los bloques
    de audio siguen: con la terna (bloque B en la posicion P) y 187,5 bloques/s (5c),
    cancion = P + (bloque - B) / 187,5."""
    terna = re.findall(r'bloque (\d+) posicion ([0-9.]+)', open(pre + '.watch.txt', errors='replace').read())
    b0, p0 = int(terna[-1][0]), float(terna[-1][1])
    pts = []
    for line in open(pre + '.watch'):
        f = line.split()
        if len(f) >= 3 and f[1] == '1':
            pts.append((p0 + (int(f[2], 16) - b0) / 187.5, float(f[0])))
    return pts


def frame_diffs(mkv):
    raw = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', mkv, '-vf', 'scale=%d:%d,format=gray' % (W, H),
                          '-f', 'rawvideo', '-'], capture_output=True, check=True).stdout
    n = len(raw) // (W * H)
    d = [0.0]
    for i in range(1, n):
        a, b = raw[(i - 1) * W * H:i * W * H], raw[i * W * H:(i + 1) * W * H]
        d.append(sum(abs(x - y) for x, y in zip(a, b)) / (W * H))
    return d


def main():
    a = sys.argv[1:]
    n, cutlog = 1, None
    if '--n' in a:
        i = a.index('--n'); n = int(a[i + 1]); del a[i:i + 2]
    if '--cuts' in a:
        i = a.index('--cuts'); cutlog = a[i + 1]; del a[i:i + 2]
    pre, outdir, secs = a[0], a[1], [float(x) for x in a[2].split(',')]
    cuts = []
    if cutlog:
        cuts = sorted({round(float(m) * 30) / 30 for m in re.findall(r'cam cut song ([0-9.]+)', open(cutlog, errors='replace').read())})
    terna = re.findall(r'bloque (\d+)', open(pre + '.watch.txt', errors='replace').read())
    if terna:
        qpc0 = float(open(pre + '.sync', encoding='utf-8-sig').read().split()[3])
        pts = song_to_qpc(pre)

        def vt(s):
            hit = next(((v, q) for v, q in pts if v >= s), None)
            return None if hit is None else hit[1] - qpc0
    else:
        vt = lambda s: s
    corr = 0.0
    if cuts:
        d = frame_diffs(pre + '.mkv')
        spikes = [k / 30.0 for k in range(2, len(d) - 1) if d[k] > 20 and d[k] >= d[k - 1] and d[k] >= d[k + 1]
                  and d[k] > 4 * max(1.0, statistics.median(d[max(1, k - 15):k + 15]))]
        exp = [(c, vt(c)) for c in cuts if vt(c) is not None]
        import bisect

        def near(x):                                 # the spike closest to x, or None past 50 ms
            i = bisect.bisect_left(spikes, x)
            c = [spikes[j] for j in (i - 1, i) if 0 <= j < len(spikes)]
            y = min(c, key=lambda v: abs(v - x)) if c else None
            return y if y is not None and abs(y - x) < 0.05 else None
        best = (0, 0.0)
        for sp in spikes:                            # every spike-cut pair proposes an offset
            for _, t in exp:
                off = sp - t
                hits = sum(1 for _, u in exp if near(u + off) is not None)
                if hits > best[0]:
                    best = (hits, off)
        offs = [near(u + best[1]) - u for _, u in exp if near(u + best[1]) is not None]
        if offs:
            corr = statistics.median(offs)
            print('desfase por cortes: %+.3f s (%d de %d cortes con salto en el video, %d saltos; dispersion %.3f s)'
                  % (corr, len(offs), len(exp), len(spikes), statistics.pstdev(offs)))
        else:
            print('sin cortes reconocibles: sin correccion')
    os.makedirs(outdir, exist_ok=True)
    for s in secs:
        t = vt(s)
        if t is None:
            print('%.3f: el reloj no llega' % s); continue
        t += corr
        name = os.path.join(outdir, 'rp_%07.3f_%%02d.png' % s)
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-ss', '%.3f' % t, '-i', pre + '.mkv',
                        '-frames:v', str(n), name], check=True)
        print('%.3f s de cancion -> video %.3f s -> %s' % (s, t, name))


if __name__ == '__main__':
    main()
