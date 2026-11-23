#!/usr/bin/env python3
"""song_sync.py - Desfase entre el reloj de la cancion de F y el audio que sale.

Uso:  python tools/song_sync.py <watch> <mezcla.f32> <prefijo_volcados> <n_volcados> [--engine-ms 41]

<watch>: MEM_WATCH con 0x12CBE48 (segundos de cancion, indice 0) y 0x12D3228 (bloque, indice 1).
<mezcla>.qpc: el QPC de cada bloque mezclado (AUDIO_WAV). Volcados: MEM_DUMP en plena cancion; de
cada uno sale la terna del propio juego (bloque, marca, posicion del stream) de FUN_00536924, para
saber que posicion del audio va en que bloque. Solo memoria del juego: ningun archivo.

reloj(t): el valor de 0x12CBE48 menos el instante en que aparece, por frame.
pos(b):   posicion del stream en el bloque b, de las ternas.
sale(b):  QPC del bloque b + lo que tarda en salir del motor de Windows (audio_cut.py).
desfase = sale(b) - instante en que reloj == pos(b). Positivo: el audio suena tarde.
"""
import argparse
import array
import struct


def load(path):
    f = open(path, 'rb')
    t = struct.unpack('<d', f.read(8))[0]
    r = {}
    while True:
        h = f.read(8)
        if len(h) < 8:
            break
        a, s = struct.unpack('<II', h)
        r[a] = f.read(s)
    return t, r


def ring_triples(r, lo, hi):
    """Busca 10 ternas (u64 bloque, u64 marca, f64 posicion) con paso 0x18."""
    out = []
    for a, b in r.items():
        if a < 0x10000000:
            continue
        for p in range(0, len(b) - 0xF0, 8):
            tag = struct.unpack_from('>Q', b, p)[0]
            if not lo < tag < hi:
                continue
            ok = True
            tags = []
            for k in range(10):
                t, st, pos = struct.unpack_from('>QQd', b, p + k * 0x18)
                if not (lo < t < hi and st > 10**10 and 0 <= pos < 1000):
                    ok = False
                    break
                tags.append((t, st, pos))
            if ok:
                out.append((a + p, tags))
                return out
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('watch')
    ap.add_argument('mix')
    ap.add_argument('dumps')
    ap.add_argument('n', type=int)
    ap.add_argument('--engine-ms', type=float, default=41.0)
    args = ap.parse_args()

    song = []
    tags = []
    for line in open(args.watch):
        t, i, h, fl, vb = line.split()
        if i == '0':
            song.append((float(t), struct.unpack('>f', bytes.fromhex(h[:8]))[0]))
        elif i == '1':
            tags.append((float(t), int(h, 16)))
    song = [s for s in song if 0 < s[1] < 400]
    qpc = array.array('q')
    with open(args.mix + '.qpc', 'rb') as f:
        qpc.frombytes(f.read())
    hz = 10_000_000.0

    # bloque de la mezcla <-> indice de cellAudio: el juego lee el indice tras cada mezcla
    off = []
    j = 0
    for t, tag in tags[1000:5000:50]:
        while j + 1 < len(qpc) and qpc[j + 1] / hz <= t:
            j += 1
        off.append(j - tag)
    off.sort()
    block_off = off[len(off) // 2]
    print(f'bloque de mezcla = bloque del juego {block_off:+d} (rango {off[0]}..{off[-1]})')

    triples = []
    for n in range(args.n):
        t, r = load(f'{args.dumps}_{n}.bin')
        # el bloque ronda el de la vigilancia en ese instante
        near = min(tags, key=lambda x: abs(x[0] - t))[1]
        found = ring_triples(r, near - 64, near + 64)
        if not found:
            print('volcado', n, 'sin ternas')
            continue
        addr, tr = found[0]
        tr.sort()
        triples.append((t, addr, tr[-1]))
        print(f'volcado {n}: ternas en 0x{addr:08X}, bloque {tr[-1][0]} posicion {tr[-1][2]:.5f}, '
              f'pos-bloque*256/48000 = {tr[-1][2] - tr[-1][0] * 256 / 48000:.5f}')

    # recta del reloj
    def clock_at(t):
        lo, hi = 0, len(song) - 1
        while hi - lo > 1:
            m = (lo + hi) // 2
            if song[m][0] <= t:
                lo = m
            else:
                hi = m
        return song[lo][1] + (t - song[lo][0])

    def when_clock(v):
        lo, hi = 0, len(song) - 1
        while hi - lo > 1:
            m = (lo + hi) // 2
            if song[m][1] <= v:
                lo = m
            else:
                hi = m
        return song[lo][0] + (v - song[lo][1])

    print('desfase a lo largo de la cancion (ms):')
    res = []
    for t, addr, (tag, st, pos) in triples:
        b = tag + block_off
        if b >= len(qpc):
            continue
        mixed = qpc[b] / hz
        clock_t = when_clock(pos)
        mix_ms = (mixed - clock_t) * 1000
        res.append((pos, mix_ms))
        print(f'  pos {pos:8.3f} s: al mezclador {mix_ms:+8.2f}, a la salida del motor {mix_ms + args.engine_ms:+8.2f}')
    if len(res) > 1:
        dp = res[-1][0] - res[0][0]
        print(f'deriva {res[-1][1] - res[0][1]:+.2f} ms en {dp:.1f} s = '
              f'{(res[-1][1] - res[0][1]) / dp * 1000:+.1f} ppm')
    # latido de frame
    d = sorted((song[k + 1][0] - song[k][0]) * 1000 for k in range(len(song) - 1))
    print(f'frames: {len(d)}, media {sum(d) / len(d):.2f} ms, p99 {d[len(d) * 99 // 100]:.2f}, 1% low {1000 / (sum(d[len(d) * 99 // 100:]) / len(d[len(d) * 99 // 100:])):.1f} fps, max {d[-1]:.2f}')


if __name__ == '__main__':
    main()
