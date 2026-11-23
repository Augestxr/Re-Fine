#!/usr/bin/env python3
"""rpcs3_sync.py - Desfase reloj/audio en RPCS3, con lo que deja tools/rpcs3_song.ps1.

Uso:  python tools/rpcs3_sync.py work/5c/<tag>

Con las ternas (bloque, posicion) de <tag>.watch.txt y las lineas de <tag>.watch da el mismo
numero que song_sync.py en Re:Fine: instante en que aparece el bloque de la posicion p menos
instante en que el reloj de la cancion vale p. Lo que tarda ese bloque en salir lo da audio_cut.py.
"""
import re
import struct
import sys

pre = sys.argv[1]
song, tags = [], {}
for line in open(pre + '.watch'):
    p = line.split()
    if len(p) != 5:
        continue
    if p[1] == '0':
        song.append((float(p[0]), struct.unpack('>f', bytes.fromhex(p[2][:8]))[0]))
    else:
        tags[int(p[2], 16)] = float(p[0])
song = [s for s in song if 0 < s[1] < 400]


def when_clock(v):
    lo, hi = 0, len(song) - 1
    while hi - lo > 1:
        m = (lo + hi) // 2
        if song[m][1] <= v:
            lo = m
        else:
            hi = m
    return song[lo][0] + (v - song[lo][1])


for line in open(pre + '.watch.txt'):
    m = re.match(r'terna \S+ bloque (\d+) posicion ([\d.]+)', line)
    if m:
        tag, pos = int(m[1]), float(m[2])
        near = min(tags, key=lambda x: abs(x - tag))
        wall = tags[near] + (tag - near) * 256 / 48000
        print(f'pos {pos:8.3f}: bloque {tag} (visto {near}) {1000 * (wall - when_clock(pos)):+.2f} ms tras el reloj')
d = sorted((song[k + 1][0] - song[k][0]) * 1000 for k in range(len(song) - 1))
print(f'frames {len(d)}, media {sum(d) / len(d):.2f} ms, p99 {d[len(d) * 99 // 100]:.2f}')
print('ritmo del reloj %.5f' % ((song[-1][1] - song[0][1]) / (song[-1][0] - song[0][0])))
