"""read_sfo.py - lee un PARAM.SFO (formato PSF) e imprime sus claves.

    python tools/read_sfo.py game/PARAM.SFO [otro/PARAM.SFO ...]

Con dos o mas archivos dice ademas si todas las claves coinciden. Solo lee: no
escribe nada ni copia bytes del juego.
"""

import struct
import sys


def read_sfo(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x00PSF':
        raise SystemExit(f'{path}: no es PSF (magic {d[:4].hex()})')
    _ver, key_off, data_off, n = struct.unpack_from('<IIII', d, 4)
    out = {}
    for i in range(n):
        k_off, fmt, length, _max, d_rel = struct.unpack_from('<HHIII', d, 20 + i * 16)
        key = d[key_off + k_off:d.index(b'\x00', key_off + k_off)].decode('ascii')
        raw = d[data_off + d_rel:data_off + d_rel + length]
        if fmt == 0x0404:
            out[key] = struct.unpack('<I', raw[:4])[0]
        else:   # 0x0204 utf-8 terminada en NUL, 0x0004 utf-8 sin terminar
            out[key] = raw.rstrip(b'\x00').decode('utf-8', 'replace')
    return out


def main():
    paths = sys.argv[1:]
    if not paths:
        raise SystemExit(__doc__)
    sfos = [read_sfo(p) for p in paths]
    for p, s in zip(paths, sfos):
        print(f'== {p}')
        for k in sorted(s):
            v = s[k]
            print(f'  {k:<16} {v!r}' if isinstance(v, str) else f'  {k:<16} {v} ({v:#x})')
    if len(sfos) > 1:
        same = all(s == sfos[0] for s in sfos[1:])
        print('coinciden: si' if same else 'coinciden: NO')


if __name__ == '__main__':
    main()
