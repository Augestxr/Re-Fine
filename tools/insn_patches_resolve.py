"""insn_patches_resolve.py - config/insn_patches.json sin bytes del juego (hito 8, P0 de 6d.11).

En git, cada entrada lleva "check" (SHA-256 de los 32 bytes del EBOOT que empiezan en
"addr", en hex) y no "orig" (la instruccion original). Un hash de 32 bytes no se puede
invertir; uno de 4 bytes si (2^32 pruebas), por eso se cubren 8 instrucciones.

    resolve:  python tools/insn_patches_resolve.py game/EBOOT.elf config/insn_patches.json OUT.json
              comprueba el hash con el EBOOT del usuario y escribe OUT.json (en work/) con
              "orig" leido de su EBOOT, que es lo que pide ppu_lifter.py --insn-patches.
    convert:  ... --convert   pasa un JSON con "orig" a "check" (en sitio), verificando
              antes que "orig" coincide con el EBOOT.
"""

import argparse
import hashlib
import json
import struct
import sys

SPAN = 32


def segments(d):
    if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 2:
        raise SystemExit('se espera un ELF64 big-endian (EBOOT.elf descifrado)')
    phoff, = struct.unpack_from('>Q', d, 32)
    phentsize, phnum = struct.unpack_from('>HH', d, 54)
    out = []
    for i in range(phnum):
        p_type, _f, off, vaddr, _pa, filesz, _m, _al = struct.unpack_from('>IIQQQQQQ', d, phoff + i * phentsize)
        if p_type == 1:
            out.append((vaddr, off, filesz))
    return out


def read_at(d, segs, addr, n):
    for vaddr, off, filesz in segs:
        if vaddr <= addr and addr + n <= vaddr + filesz:
            return d[off + addr - vaddr: off + addr - vaddr + n]
    raise SystemExit(f'0x{addr:08X}: fuera de los segmentos del EBOOT')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('elf')
    ap.add_argument('spec')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--convert', action='store_true')
    a = ap.parse_args()
    d = open(a.elf, 'rb').read()
    segs = segments(d)
    spec = json.load(open(a.spec, encoding='utf-8'))
    bad = 0
    for e in spec['patches']:
        addr = int(e['addr'], 0)
        span = read_at(d, segs, addr, SPAN)
        h = hashlib.sha256(span).hexdigest()
        if a.convert:
            if 'orig' not in e:
                continue
            if int(e['orig'], 0) != int.from_bytes(span[:4], 'big'):
                print(f'{e["addr"]}: orig no coincide con el EBOOT', file=sys.stderr); bad += 1; continue
            new = {}
            for k, v in e.items():
                if k == 'orig':
                    new['check'] = h
                else:
                    new[k] = v
            e.clear(); e.update(new)
        else:
            if e.get('check') != h:
                print(f'{e["addr"]}: el hash no coincide: otro EBOOT o versión (se espera BLUS31319 01.00)',
                      file=sys.stderr); bad += 1; continue
            e['orig'] = f'0x{int.from_bytes(span[:4], "big"):08X}'
    if bad:
        raise SystemExit(f'{bad} entradas no cuadran: no se escribe nada')
    if a.convert:
        with open(a.spec, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(spec, f, indent=2, ensure_ascii=False); f.write('\n')
        print(f'convertido: {len(spec["patches"])} entradas con "check"')
    else:
        if not a.out:
            raise SystemExit('falta OUT.json')
        with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(spec, f, indent=2); f.write('\n')
        print(f'insn-patches: {len(spec["patches"])} comprobadas con el EBOOT -> {a.out}')


if __name__ == '__main__':
    main()
