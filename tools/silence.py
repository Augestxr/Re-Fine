#!/usr/bin/env python3
"""silence.py - 4c.3: % de silencio por tramos en un AUDIO_WAV de Re:Fine (f32le estereo 48 kHz).

Uso: python tools/silence.py <audio> <from_s> <to_s> [--step S]

Silencio = bloques de 10 ms con pico por debajo de 1e-4. Tiempos en s de archivo, no de proceso.
"""
import argparse
import array

RATE = 48000
BLOCK = RATE // 100


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('audio')
    ap.add_argument('frm', type=float)
    ap.add_argument('to', type=float)
    ap.add_argument('--step', type=float, default=5.0)
    args = ap.parse_args()
    t = args.frm
    with open(args.audio, 'rb') as f:
        while t < args.to:
            f.seek(int(t * RATE) * 8)
            a = array.array('f')
            a.frombytes(f.read(int(args.step * RATE) * 8 // 8 * 8))
            quiet = total = 0
            for i in range(0, len(a) - BLOCK * 2, BLOCK * 2):
                total += 1
                if max(abs(x) for x in a[i:i + BLOCK * 2]) < 1e-4:
                    quiet += 1
            if total:
                print(f'{t:6.1f}: {100.0 * quiet / total:5.1f} %')
            t += args.step


if __name__ == '__main__':
    main()
