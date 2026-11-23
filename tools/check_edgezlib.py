"""check_edgezlib.py - oraculo de descompresion para la tarea Edge zlib traducida.

Lee lo que vuelca F2ndRecomp.exe con F2ND_EDGEZLIB_DUMP=<N> (src/stubs.cpp) en
work/edgezlib/: NNN.in (bloque comprimido, copiado al hacer el push en la cola) y
NNN.out (buffer de salida, copiado cuando el contador de trabajo llega a cero).
Descomprime cada .in con el zlib estandar de Python y compara byte a byte con el .out.

    python tools/check_edgezlib.py [work/edgezlib]

Sale con 0 solo si todos los bloques con .out coinciden y hay al menos uno.
"""

import os
import sys
import zlib


def inflate(data):
    # Los bloques de data.psarc llevan cabecera zlib (78 DA); si no, deflate crudo.
    try:
        return zlib.decompress(data), "zlib"
    except zlib.error:
        return zlib.decompress(data, -15), "raw"


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "work", "edgezlib")
    rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(d, "index.tsv"))][1:]
    ok = bad = missing = 0
    for n, in_ea, in_size, out_ea, out_size in rows:
        out_size = int(out_size, 16)
        src = open(os.path.join(d, f"{n}.in"), "rb").read()
        out_path = os.path.join(d, f"{n}.out")
        try:
            ref, kind = inflate(src)
        except zlib.error as e:
            print(f"{n}: el .in no descomprime con zlib ({e})")
            bad += 1
            continue
        if not os.path.exists(out_path):
            print(f"{n}: in={len(src)} B -> zlib {len(ref)} B; sin .out (no termino)")
            missing += 1
            continue
        got = open(out_path, "rb").read()
        if got[:len(ref)] == ref and len(ref) == out_size:
            print(f"{n}: OK  in={len(src)} B ({kind}) -> {len(ref)} B, coincide byte a byte")
            ok += 1
        else:
            first = next((i for i, (a, b) in enumerate(zip(got, ref)) if a != b), min(len(got), len(ref)))
            print(f"{n}: DIFERENTE  zlib={len(ref)} B, esperado={out_size} B, primer byte distinto en 0x{first:X}"
                  f" (spu={got[first:first+8].hex()} zlib={ref[first:first+8].hex()})")
            bad += 1
    print(f"total: {ok} coinciden, {bad} distintos, {missing} sin salida")
    sys.exit(0 if ok and not bad else 1)


if __name__ == "__main__":
    main()
