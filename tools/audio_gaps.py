#!/usr/bin/env python3
"""audio_gaps.py - 4c.5: bloques en silencio donde RPCS3 tiene sonido.

Uso: python tools/audio_gaps.py <refine.f32> <rpcs3.wav> <desfase> [--from S] [--to S]

<refine.f32> es AUDIO_WAV de Re:Fine; <rpcs3.wav>, el volcado de RPCS3 (m1.wav); <desfase>, muestras de
Re:Fine menos muestras de RPCS3 para la misma imagen de sonido (salida de --lag si no se conoce).
Bloque = 256 muestras (5,33 ms) alineadas con el archivo de Re:Fine. Tiempos en s de archivo de Re:Fine.

  --lag: busca el desfase de muestra exacta con un tramo de 0,2 s a mitad de pelicula.
Solo lee muestras de la mezcla final. Sin dependencias.
"""
import argparse
import array
import struct

RATE = 48000
BLOCK = 256


def load_wav(path):
    with open(path, 'rb') as f:
        raw = f.read()
    p = 12
    while p + 8 <= len(raw):
        tag, n = raw[p:p + 4], struct.unpack('<I', raw[p + 4:p + 8])[0]
        if tag == b'data':
            a = array.array('f')
            a.frombytes(raw[p + 8:p + 8 + (min(n, len(raw) - p - 8) & ~7)])
            return a
        p += 8 + n + (n & 1)
    raise SystemExit('sin data')


def load_f32(path):
    a = array.array('f')
    with open(path, 'rb') as f:
        d = f.read()
    a.frombytes(d[:len(d) & ~7])
    return a


def find_lag(a, b, at_s):
    """Busca en b, en todo el archivo, las 512 muestras de L de a en at_s."""
    i0 = int(at_s * RATE) * 2
    pat = a[i0:i0 + 1024:2]
    first = [(i, v) for i, v in enumerate(pat) if abs(v) > 0.003][:40]
    if len(first) < 40:
        return None
    for j in range(0, len(b) // 2 - 512):
        if abs(b[j * 2 + first[0][0] * 2] - first[0][1]) < 1e-5:
            if all(abs(b[(j + i) * 2] - v) < 1e-5 for i, v in first):
                return int(at_s * RATE) - j
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('lag', nargs='?', type=int)
    ap.add_argument('--from', dest='frm', type=float, default=20.0)
    ap.add_argument('--to', type=float, default=100.0)
    ap.add_argument('--anchor', type=float, default=60.0)
    args = ap.parse_args()
    a, b = load_f32(args.a), load_wav(args.b)
    lag = args.lag
    if lag is None:
        for t in [args.anchor + d for d in (0, 7, -11, 13, -19, 20, -27, 27, 33, -33)]:
            lag = find_lag(a, b, t)
            if lag is not None:
                break
        if lag is None:
            raise SystemExit('sin desfase: el tramo no aparece igual en RPCS3')
    print(f'desfase {lag} muestras ({lag / RATE:.4f} s)')
    tot = gaps = same = other = 0
    out = []
    i0 = int(args.frm * RATE) // BLOCK * BLOCK
    for i in range(i0, min(int(args.to * RATE), len(a) // 2 - BLOCK), BLOCK):
        j = i - lag
        if j < 0 or j + BLOCK > len(b) // 2:
            continue
        ra = b[j * 2:(j + BLOCK) * 2]
        if max(ra) < 1e-4 and min(ra) > -1e-4:
            continue          # RPCS3 calla: no cuenta
        oa = a[i * 2:(i + BLOCK) * 2]
        tot += 1
        peak = max(max(oa), -min(oa))
        if peak < 1e-6:
            gaps += 1
            out.append(i / RATE)
        else:
            e = max(abs(x - y) for x, y in zip(oa, ra))
            if e < 2e-3:
                same += 1
            else:
                other += 1
    print(f'bloques con sonido en RPCS3: {tot}; iguales {same}; en silencio {gaps}; distintos {other}')
    print('silencio en (s de archivo):', ' '.join(f'{t:.3f}' for t in out))


if __name__ == '__main__':
    main()
