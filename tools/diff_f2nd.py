"""diff_f2nd.py - compara los imports y el tamano de F con los de F2nd (docs/diff-f2nd.md).

Entradas (salidas de ppu_loader.py + tools/name_imports.py de los dos proyectos):

    python tools/diff_f2nd.py work/loader_out ../f2nd-recomp/work/loader_out \
        --hle-log ../f2nd-recomp/docs/hle-log.md

Imprime en Markdown:
  1. conteo de descriptores OPD, funciones e imports (EBOOT.loader.json); el tamano
     de .text sale de tools/elf_info.py;
  2. modulos: imports comunes (mismo NID), solo en F y solo en F2nd;
  3. la lista NID a NID de lo que solo esta en uno de los dos;
  4. con --hle-log, cada NID que aparece en el hle-log de F2nd: si F lo importa.
"""

import argparse
import collections
import json
import os
import re


def load(d):
    named = os.path.join(d, 'EBOOT.named.imports.json')
    imps = json.load(open(named if os.path.exists(named) else os.path.join(d, 'EBOOT.imports.json')))
    ldr = json.load(open(os.path.join(d, 'EBOOT.loader.json')))
    funcs = json.load(open(os.path.join(d, 'EBOOT.functions.json')))
    return imps, ldr, funcs


def key(e):
    return (e['library'], int(e['nid'], 16))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('f_dir')
    ap.add_argument('f2nd_dir')
    ap.add_argument('--hle-log')
    a = ap.parse_args()

    fi, fl, ff = load(a.f_dir)
    si, sl, sf = load(a.f2nd_dir)
    fmap = {key(e): e for e in fi}
    smap = {key(e): e for e in si}
    fnids = {int(e['nid'], 16): e for e in fi}

    print('## Tamano\n')
    print('| | F (BLUS31319) | F2nd (BLES02029) |')
    print('|---|---|---|')
    print(f"| descriptores OPD | {fl['opd_count']} | {sl['opd_count']} |")
    print(f"| funciones del loader | {fl['function_count']} | {sl['function_count']} |")
    print(f'| imports de firmware | {len(fi)} | {len(si)} |')
    print(f"| modulos importados | {len({e['library'] for e in fi})} | {len({e['library'] for e in si})} |")

    mods = sorted({m for m, _ in fmap} | {m for m, _ in smap})
    print('\n## Imports por modulo\n')
    print('| modulo | F | F2nd | comunes | solo F | solo F2nd |')
    print('|---|---|---|---|---|---|')
    tot = collections.Counter()
    for m in mods:
        fk = {k for k in fmap if k[0] == m}
        sk = {k for k in smap if k[0] == m}
        row = (len(fk), len(sk), len(fk & sk), len(fk - sk), len(sk - fk))
        tot.update(dict(zip('abcde', row)))
        print(f'| {m} | ' + ' | '.join(str(x) if x else '-' for x in row) + ' |')
    print(f"| **total** | **{tot['a']}** | **{tot['b']}** | **{tot['c']}** | **{tot['d']}** | **{tot['e']}** |")

    def name(e):
        return e.get('name') or '?'

    print('\n## Solo en F\n')
    print('| modulo | NID | nombre | handler en ps3recomp |')
    print('|---|---|---|---|')
    for k in sorted(set(fmap) - set(smap)):
        e = fmap[k]
        print(f"| {k[0]} | `{k[1]:#010x}` | `{name(e)}` | {'si' if e.get('implemented') else 'NO'} |")

    print('\n## Solo en F2nd\n')
    print('| modulo | NID | nombre |')
    print('|---|---|---|')
    for k in sorted(set(smap) - set(fmap)):
        print(f'| {k[0]} | `{k[1]:#010x}` | `{name(smap[k])}` |')

    if a.hle_log:
        txt = open(a.hle_log, encoding='utf-8').read()
        nids = sorted({int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{8})\b', txt)})
        snids = {k[1]: smap[k] for k in smap}
        print('\n## NIDs citados en el hle-log de F2nd\n')
        print('| NID | nombre (F2nd) | ¿F lo importa? |')
        print('|---|---|---|')
        for n in nids:
            if n not in snids and n not in fnids:
                continue   # direcciones u otros numeros de 8 cifras, no NIDs importados
            nm = name(snids[n]) if n in snids else name(fnids[n])
            print(f"| `{n:#010x}` | `{nm}` | {'**si**' if n in fnids else 'no'} |")


if __name__ == '__main__':
    main()
