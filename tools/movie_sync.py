#!/usr/bin/env python3
"""movie_sync.py - 4c.3: compara el audio de la pelicula entre dos volcados de la mezcla final.

Uso:
  python tools/movie_sync.py <a> <b> [--wav-a] [--wav-b] [--from S] [--to S] [--step S]

<a> y <b> son AUDIO_WAV de Re:Fine (f32le estereo 48 kHz, sin cabecera) o, con --wav-*, el volcado
de RPCS3. Los dos empiezan en momentos distintos, asi que primero alinea las envolventes de 10 ms
sobre todo el archivo y luego, cada --step s de <a>, mide el desfase a la muestra y la correlacion
del tramo. Positivo = el suceso esta mas tarde en b que en a. La deriva es la diferencia entre
tramos. Solo lee muestras de la mezcla: no interpreta ningun formato del juego. Sin dependencias.
"""
import argparse
import array
import math
import struct

RATE = 48000
ENV = RATE // 100


def load(path, is_wav):
    with open(path, 'rb') as f:
        raw = f.read()
    off, size = 0, len(raw)
    if is_wav:
        p = 12
        while p + 8 <= len(raw):
            tag, n = raw[p:p + 4], struct.unpack('<I', raw[p + 4:p + 8])[0]
            if tag == b'data':
                off, size = p + 8, min(n, len(raw) - p - 8)
                break
            p += 8 + n + (n & 1)
    a = array.array('f')
    a.frombytes(raw[off:off + (size & ~7)])
    return a[0::2], a[1::2]


def envelope(left):
    out = []
    for i in range(0, len(left) - ENV, ENV):
        s = left[i:i + ENV]
        out.append(math.sqrt(sum(x * x for x in s) / ENV))
    return out


def corr(a, b):
    n = min(len(a), len(b))
    ma = sum(a[:n]) / n
    mb = sum(b[:n]) / n
    sab = saa = sbb = 0.0
    for i in range(n):
        x, y = a[i] - ma, b[i] - mb
        sab += x * y
        saa += x * x
        sbb += y * y
    return sab / math.sqrt(saa * sbb) if saa and sbb else 0.0


def coarse(ea, eb, start, length):
    """Mejor posicion en eb del tramo [start, start+length) de ea, en unidades de 10 ms."""
    tpl = ea[start:start + length]
    best, at = -2.0, 0
    for pos in range(0, len(eb) - length, 2):
        c = corr(tpl, eb[pos:pos + length])
        if c > best:
            best, at = c, pos
    for pos in range(max(0, at - 2), min(len(eb) - length, at + 3)):
        c = corr(tpl, eb[pos:pos + length])
        if c > best:
            best, at = c, pos
    return at, best


def fine(a, b, at_a, guess_b, span=960, win=9600):
    """Desfase a la muestra alrededor de guess_b, con un tramo de win muestras."""
    tpl = a[at_a:at_a + win]
    best, at = -2.0, guess_b
    for pos in range(guess_b - span, guess_b + span + 1, 8):
        c = corr(tpl[::4], b[pos:pos + win][::4])
        if c > best:
            best, at = c, pos
    for pos in range(at - 8, at + 9):
        c = corr(tpl, b[pos:pos + win])
        if c > best:
            best, at = c, pos
    return at, best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--wav-a', action='store_true')
    ap.add_argument('--wav-b', action='store_true')
    ap.add_argument('--from', dest='frm', type=float, default=20.0, help='s de a del primer tramo')
    ap.add_argument('--to', type=float, default=100.0)
    ap.add_argument('--step', type=float, default=10.0)
    args = ap.parse_args()

    al, ar = load(args.a, args.wav_a)
    bl, br = load(args.b, args.wav_b)
    ea, eb = envelope(al), envelope(bl)
    print(f'a {len(al) / RATE:.2f} s, b {len(bl) / RATE:.2f} s')

    start = int(args.frm * 100)
    at, c = coarse(ea, eb, start, 800)
    lag = (at - start) * ENV
    print(f'alineado grueso: {lag / RATE:+.3f} s (corr envolvente {c:.4f})')

    lags = []
    t = args.frm
    while t <= args.to and int(t * RATE) + 9600 < len(al):
        at_a = int(t * RATE)
        pos, cl = fine(al, bl, at_a, at_a + lag)
        _, cr = fine(ar, br, at_a, pos, span=0)
        d = pos - at_a
        lags.append(d)
        print(f'{t:6.1f} s de a  desfase {d / RATE:+.5f} s ({d:+d} muestras)  corr L {cl:.5f}  R {cr:.5f}')
        lag = d
        t += args.step
    if lags:
        print(f'deriva: {(max(lags) - min(lags)) / RATE * 1000:.2f} ms (min {min(lags)}, max {max(lags)} muestras)')


if __name__ == '__main__':
    main()
