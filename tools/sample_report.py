"""sample_report.py - Suma por funcion las muestras [SAMPLE] de REFINE_SAMPLE (src/main.cpp), con build/ReFine.map.

Uso:  python tools/sample_report.py <run .err.log> [build/ReFine.map] [N]

Imprime las N funciones (40 por defecto) con mas muestras: % del hilo del juego, muestras y nombre. Las
funciones traducidas del EBOOT se llaman func_<direccion PPU>; lo demas es el runtime o el port.
"""
import bisect
import re
import sys

sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
from crash_symbols import load_map  # noqa: E402


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    log = sys.argv[1]
    mp = sys.argv[2] if len(sys.argv) > 2 else 'build/ReFine.map'
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    syms = load_map(mp)
    starts = [s[0] for s in syms]
    by = {}
    total = 0
    head = ''
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.match(r'\[SAMPLE\] (\d+) ReFine\.exe\+0x([0-9A-Fa-f]+)', line)
            if m:
                c, rva = int(m.group(1)), int(m.group(2), 16)
                i = bisect.bisect_right(starts, rva) - 1
                name = syms[i][1] if i >= 0 else '?'
                by[name] = by.get(name, 0) + c
                total += c
            elif line.startswith('[SAMPLE]'):
                head = line.strip()
    print(head)
    for name, c in sorted(by.items(), key=lambda kv: -kv[1])[:top]:
        print(f'{100.0 * c / max(total, 1):5.1f} %  {c:6d}  {name}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
