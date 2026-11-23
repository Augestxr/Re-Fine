"""crash_symbols.py - Names the ReFine.exe+0x... addresses of the [crash] lines (src/crash_report.cpp).

Uso:  python tools/crash_symbols.py <log .err.log> [build/ReFine.map]
      python tools/crash_symbols.py 0x1234AB [build/ReFine.map]

Lee el .map del enlazador (link /MAP, columna "Rva+Base") y da, para cada RVA, la funcion que la contiene
y el desplazamiento dentro de ella. El .map tiene que ser el de la misma compilacion que el exe que fallo.
"""
import bisect
import re
import sys

BASE = 0x140000000


def load_map(path):
    syms = []
    pat = re.compile(r'^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s')
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = pat.match(line)
            if m:
                va = int(m.group(2), 16)
                if va >= BASE:
                    syms.append((va - BASE, m.group(1)))
    syms.sort()
    return syms


def name(syms, rva):
    i = bisect.bisect_right([s[0] for s in syms], rva) - 1
    if i < 0:
        return '?'
    return f'{syms[i][1]}+0x{rva - syms[i][0]:X}'


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    mp = sys.argv[2] if len(sys.argv) > 2 else 'build/ReFine.map'
    syms = load_map(mp)
    arg = sys.argv[1]
    if re.fullmatch(r'0x[0-9a-fA-F]+', arg):
        print(name(syms, int(arg, 16)))
        return 0
    with open(arg, encoding='utf-8', errors='replace') as f:
        for line in f:
            if not line.startswith('[crash]'):
                continue
            line = line.rstrip()
            m = re.search(r'ReFine\.exe\+0x([0-9A-Fa-f]+)', line)
            print(line + (f'   {name(syms, int(m.group(1), 16))}' if m else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
