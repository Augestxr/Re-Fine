#!/usr/bin/env python3
"""pv_sync.py - 4j.1: linea de tiempos de la PV600 a partir de los volcados de audio.

Uso:
  python tools/pv_sync.py env  <audio> <byte_ancla> [--wav] [--from S] [--to S]
      Envolvente RMS cada 100 ms, en s desde el ancla (apertura de pv_600.ogg).
  python tools/pv_sync.py lag  <a> <ancla_a> <b> <ancla_b> [--wav-a] [--wav-b] [--win S] [--step S]
      Desfase de b frente a a por tramos: correlacion de envolventes (10 ms) en ventanas de --win s.
      Positivo = b suena mas tarde que a, contando desde el ancla de cada uno.

<audio> es AUDIO_WAV de Re:Fine (f32le estereo 48 kHz, sin cabecera) o, con --wav, el volcado de RPCS3.
Solo lee muestras de la mezcla final: no interpreta ningun formato del juego. Sin dependencias.
"""
import argparse
import array
import math
import struct

RATE = 48000


def load(path, is_wav):
    """Devuelve (mono envelope 10 ms como lista de float, bytes por trama, offset de datos)."""
    with open(path, 'rb') as f:
        raw = f.read()
    off, ch, bits, fmt = 0, 2, 32, 3
    rate = RATE
    if is_wav:
        p = 12
        while p + 8 <= len(raw):
            cid, size = raw[p:p + 4], struct.unpack('<I', raw[p + 4:p + 8])[0]
            if cid == b'fmt ':
                fmt, ch, rate, _, _, bits = struct.unpack('<HHIIHH', raw[p + 8:p + 24])
            if cid == b'data':
                off = p + 8
                break
            p += 8 + size + (size & 1)
    data = array.array('f' if bits == 32 and fmt == 3 else 'h')
    n = (len(raw) - off) // data.itemsize
    data.frombytes(raw[off:off + n * data.itemsize])
    scale = 1.0 if data.typecode == 'f' else 1 / 32768.0
    hop = rate // 100
    env = []
    for i in range(0, len(data) // ch - hop, hop):
        s = 0.0
        for v in data[i * ch:(i + hop) * ch:ch]:
            s += v * v
        env.append(math.sqrt(s / hop) * scale)
    return env, ch * data.itemsize, off, rate


def anchor_idx(byte_anchor, frame_bytes, off, rate):
    return (byte_anchor - off) / frame_bytes / rate * 100.0


def corr(a, b):
    n = min(len(a), len(b))
    if n < 8:
        return 0.0
    ma, mb = sum(a[:n]) / n, sum(b[:n]) / n
    x = [v - ma for v in a[:n]]
    y = [v - mb for v in b[:n]]
    d = math.sqrt(sum(v * v for v in x) * sum(v * v for v in y))
    return sum(p * q for p, q in zip(x, y)) / d if d else 0.0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    e = sub.add_parser('env')
    e.add_argument('audio')
    e.add_argument('anchor', type=int)
    e.add_argument('--wav', action='store_true')
    e.add_argument('--from', dest='t0', type=float, default=-5)
    e.add_argument('--to', dest='t1', type=float, default=70)
    l = sub.add_parser('lag')
    l.add_argument('a')
    l.add_argument('anchor_a', type=int)
    l.add_argument('b')
    l.add_argument('anchor_b', type=int)
    l.add_argument('--wav-a', action='store_true')
    l.add_argument('--wav-b', action='store_true')
    l.add_argument('--win', type=float, default=6)
    l.add_argument('--step', type=float, default=6)
    l.add_argument('--from', dest='t0', type=float, default=0)
    l.add_argument('--to', dest='t1', type=float, default=60)
    l.add_argument('--range', dest='rng', type=float, default=8)
    a = ap.parse_args()

    if a.cmd == 'env':
        env, fb, off, rate = load(a.audio, a.wav)
        i0 = anchor_idx(a.anchor, fb, off, rate)
        print(f'duracion {len(env) / 100:.2f} s; ancla en {i0 / 100:.3f} s del volcado')
        t = a.t0
        row = []
        while t < a.t1:
            s, en = int(i0 + t * 100), int(i0 + (t + 0.5) * 100)
            seg = env[max(s, 0):max(en, 0)]
            row.append(f'{t:+.1f}:{20 * math.log10(max(sum(seg) / max(len(seg), 1), 1e-5)):.0f}')
            t += 0.5
        for i in range(0, len(row), 12):
            print(' '.join(row[i:i + 12]))
        for th in (0.001, 0.01):
            first = next((k for k in range(int(i0), len(env)) if env[k] > th), None)
            print(f'primer tramo de 10 ms sobre {th}: '
                  f'{(first - i0) / 100 if first is not None else float("nan"):+.2f} s')
    else:
        ea, fa, oa, ra = load(a.a, a.wav_a)
        eb, fb, ob, rb = load(a.b, a.wav_b)
        ia, ib = anchor_idx(a.anchor_a, fa, oa, ra), anchor_idx(a.anchor_b, fb, ob, rb)
        t = a.t0
        print('tramo(s desde ancla a)  desfase_b-a(s)  correlacion')
        while t + a.win <= a.t1:
            s = int(ia + t * 100)
            n = int(a.win * 100)
            seg = ea[s:s + n]
            best = (-2, 0)
            for lag in range(int(-a.rng * 100), int(a.rng * 100) + 1):
                p = int(ib + t * 100) + lag
                if p < 0:
                    continue
                c = corr(seg, eb[p:p + n])
                if c > best[0]:
                    best = (c, lag)
            print(f'{t:6.1f}-{t + a.win:<6.1f} {best[1] / 100:+7.2f}  {best[0]:.3f}')
            t += a.step


if __name__ == '__main__':
    main()
