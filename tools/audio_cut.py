#!/usr/bin/env python3
"""audio_cut.py - Casa lo que salio por cada dispositivo con la mezcla del juego.

Uso:  python tools/audio_cut.py <prefijo_loopback> <mezcla.f32> [--names archivo]

Entrada: lo que escribe tools/audio_loopback.c (<prefijo>_<n>.f32/.tsv) y AUDIO_WAV de una
corrida (<mezcla>.f32, 48 kHz estereo, y <mezcla>.f32.qpc, el QPC de cada bloque).

Las dos corrientes deben coincidir muestra a muestra (dispositivo a 48 kHz, estereo, sin
efectos): cada ventana de la captura se busca en la mezcla por su contenido. Da:
  - tramos: de que frame de la mezcla a cual salio por cada dispositivo, y a que hora;
  - entre un tramo y el siguiente: el hueco de sonido en ms y los frames perdidos o repetidos;
  - latencia del mezclador a la salida del motor de Windows: QPC del bloque frente al QPC de
    captura de esos mismos frames.
"""
import argparse
import array
import bisect
import glob
import os
import struct

BLOCK = 256
WIN = 64          # frames por ventana
QPC_HZ = 10_000_000


def load_f32(path):
    a = array.array('f')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    return a


def key_of(a, frame):
    # Signo de la pendiente de cada canal: no depende del volumen de la sesion.
    k = 0
    w = a[frame * 2:(frame + 25) * 2]
    for i in range(2, len(w)):
        k = k * 2 + (w[i] > w[i - 2])
    return k


def nonsilent(a, frame):
    return any(abs(x) > 1e-3 for x in a[frame * 2:(frame + 25) * 2])


def agree(a, b, fa, fb):
    # Dos ventanas son la misma si casi todas las pendientes coinciden.
    x, y = key_of(a, fa), key_of(b, fb)
    return bin(x ^ y).count('1') <= 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prefix')
    ap.add_argument('mix')
    ap.add_argument('--names')
    ap.add_argument('--merge-ms', type=float, default=60, help='une tramos separados menos de esto')
    args = ap.parse_args()

    mix = load_f32(args.mix)
    mix_frames = len(mix) // 2
    qpc = array.array('q')
    with open(args.mix + '.qpc', 'rb') as f:
        qpc.frombytes(f.read())
    print(f'mezcla: {mix_frames} frames, {len(qpc)} bloques')

    # Indice de la mezcla por contenido. Un frame se busca en una rejilla de WIN.
    index = {}
    for fr in range(0, mix_frames - 26, WIN):
        if nonsilent(mix, fr):
            index.setdefault(key_of(mix, fr), fr)

    names = {}
    if args.names:
        with open(args.names, encoding='utf-8', errors='replace') as f:
            for line in f:
                n, _, v = line.strip().partition('\t')
                names[n] = v

    spans = []
    lat = []
    for path in sorted(glob.glob(args.prefix + '_*.f32')):
        n = path[len(args.prefix) + 1:-4]
        tsv = path[:-4] + '.tsv'
        packets = []
        at = 0
        with open(tsv) as f:
            head = f.readline()
            for line in f:
                t, fr, _ = line.split('\t')
                packets.append((at, float(t), int(fr)))
                at += int(fr)
        cap = load_f32(path)
        if len(cap) < at * 2:
            at = len(cap) // 2
        starts = [p[0] for p in packets]

        def wall(frame):
            i = bisect.bisect_right(starts, frame) - 1
            return packets[i][1] + (frame - packets[i][0]) / 48000.0

        # Barrido: cada frame de captura, hasta casar; luego salta de WIN en WIN.
        cur = None
        hits = 0
        fr = 0
        while fr < at - 26:
            if cur is not None:
                # sigue el mismo tramo si la mezcla sigue igual
                m = cur['m_end'] + (fr - cur['c_end'])
                if m + 26 < mix_frames and agree(cap, mix, fr, m):
                    cur['c_end'] = fr
                    cur['m_end'] = m
                    fr += WIN
                    continue
                if nonsilent(cap, fr) or (fr - cur['c_end']) > 4800:
                    pass
            m = index.get(key_of(cap, fr)) if nonsilent(cap, fr) else None
            if m is not None:
                hits += 1
                if cur is None or m - cur['m_end'] != fr - cur['c_end']:
                    cur = {'dev': n, 'c_start': fr, 'c_end': fr, 'm_start': m, 'm_end': m}
                    spans.append(cur)
                else:
                    cur['c_end'] = fr
                    cur['m_end'] = m
                fr += WIN
            else:
                fr += 1
        for s in spans:
            if s['dev'] == n and 't0' not in s:
                s['t0'] = wall(s['c_start'])
                s['t1'] = wall(s['c_end'])
        # latencia: bloque a bloque del tramo
        for s in spans:
            if s['dev'] != n:
                continue
            for b in range(s['m_start'] // BLOCK + 1, s['m_end'] // BLOCK, 48):
                if b >= len(qpc):
                    break
                c = s['c_start'] + (b * BLOCK - s['m_start'])
                lat.append((wall(c) - qpc[b] / QPC_HZ) * 1000)
        print(f'dispositivo {n} {names.get(n, "")}: {at} frames capturados, {head.strip()}')

    spans.sort(key=lambda s: s['t0'])
    # une tramos contiguos del mismo dispositivo con la mezcla continua
    merged = []
    for s in spans:
        if s['m_end'] - s['m_start'] < WIN * 2:
            continue
        if merged and merged[-1]['dev'] == s['dev'] and s['t0'] - merged[-1]['t1'] < args.merge_ms / 1000 \
                and abs((s['m_start'] - merged[-1]['m_end']) - (s["t0"] - merged[-1]["t1"]) * 48000) < 32:
            merged[-1]['m_end'] = s['m_end']
            merged[-1]['t1'] = s['t1']
        else:
            merged.append(dict(s))
    print('tramos (dispositivo, t desde el primero, frames de mezcla):')
    t0 = merged[0]['t0'] if merged else 0
    for i, s in enumerate(merged):
        line = f"  {s['dev']}  {s['t0'] - t0:9.3f} - {s['t1'] - t0:9.3f} s  mezcla {s['m_start']} - {s['m_end']}"
        if i:
            p = merged[i - 1]
            gap = (s['t0'] - p['t1']) * 1000
            skipped = s['m_start'] - p['m_end'] - 1
            line += f"   hueco {gap:+.2f} ms, mezcla {skipped:+d} frames ({skipped / 48:+.2f} ms)"
        print(line)
    if lat:
        lat.sort()
        print(f'latencia mezclador -> salida del motor: media {sum(lat) / len(lat):.2f} ms, '
              f'p1 {lat[len(lat) // 100]:.2f}, p50 {lat[len(lat) // 2]:.2f}, p99 {lat[len(lat) * 99 // 100]:.2f} (n={len(lat)})')


if __name__ == '__main__':
    main()
