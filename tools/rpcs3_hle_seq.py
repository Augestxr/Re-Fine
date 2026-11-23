"""rpcs3_hle_seq.py - saca del RPCS3.log la secuencia de llamadas a funciones del sistema.

RPCS3 (con los canales de los modulos en Trace) registra cada llamada a una funcion HLE
o a un syscall. El bloque {...} dice desde donde se hizo:

    {PPU[0x1000000] Thread (main_thread) [HLE:0x01255c1c, LR:0x0056ac24]}  funcion HLE;
                                         LR = direccion de retorno (llamada en LR-4)
    {PPU[0x1000000] Thread (main_thread) [0x006af4a4]}                     syscall (sc) en
                                         esa direccion
    {PPU[0x1000000] Thread (main_thread) [liblv2: 0x01274f74]}             desde un PRX
                                         cargado en LLE (aqui liblv2.sprx)

Se queda con las lineas cuyo mensaje es una llamada nombre(args) a una funcion de
firmware o syscall (cell*, sys*, sce*, _cell*, _sys*) y escribe un TSV:

    n  tiempo  hilo  origen  dir  canal  funcion  args  repeticiones

origen es EBOOT si la direccion cae en los segmentos del EBOOT (--eboot-end), o el
nombre del PRX. Con --from-eboot solo se guardan las de origen EBOOT. Con --collapse
las repeticiones consecutivas (mismo hilo, funcion y direccion) se juntan en una fila.
Con --unique solo se guarda la primera llamada a cada funcion (en repeticiones se cuentan
todas las demas): es la lista ordenada de lo que el runtime tiene que implementar.

    python tools/rpcs3_hle_seq.py RPCS3.log -o hle_seq.tsv [-n 200] [--from-eboot]
                                  [--collapse | --unique]
"""

import argparse
import re

LINE = re.compile(
    r"^\S+ (?P<time>\d+:\d\d:\d\d\.\d+) \{(?P<ctx>[^}]*)\} (?P<chan>[\w]+): (?P<msg>.*)$")
# Algunos formatos de RPCS3 no cierran el parentesis (cellPamfReaderGetStreamTypeAndChannel):
# sin ningun ")" en el mensaje tambien es una llamada. "nombre(): texto" no lo es.
CALL = re.compile(r"^(?P<func>_?(?:cell|sys|sce|_sys|_cell)\w*)\((?P<args>.*\)\s*$|[^)]*$)")
THREAD = re.compile(r"Thread \((?P<name>[^)]*)\)")
WHERE = re.compile(
    r"\[(?:HLE:0x[0-9a-fA-F]+, LR:0x(?P<lr>[0-9a-fA-F]+)"
    r"|(?P<mod>[\w.]+): 0x(?P<modpc>[0-9a-fA-F]+)"
    r"|0x(?P<pc>[0-9a-fA-F]+))\]\s*$")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-n", type=int, default=0, help="quedarse con las primeras N filas (0 = todas)")
    ap.add_argument("--from-eboot", action="store_true", help="solo llamadas hechas desde el EBOOT")
    ap.add_argument("--collapse", action="store_true", help="juntar repeticiones consecutivas")
    ap.add_argument("--unique", action="store_true", help="solo la primera llamada a cada funcion")
    ap.add_argument("--eboot-end", type=lambda s: int(s, 0), default=0x1000000,
                    help="fin del espacio del EBOOT (por defecto 0x1000000; los PRX van despues)")
    args = ap.parse_args()

    rows = []
    first = {}
    with open(args.log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if not m or not m["ctx"].startswith("PPU["):
                continue  # solo llamadas desde hilos PPU
            c = CALL.match(m["msg"])
            if not c:
                continue
            th = THREAD.search(m["ctx"])
            w = WHERE.search(m["ctx"])
            if w and w["lr"]:
                addr, where = int(w["lr"], 16), f"LR:0x{int(w['lr'], 16):08x}"
            elif w and w["pc"]:
                addr, where = int(w["pc"], 16), f"PC:0x{int(w['pc'], 16):08x}"
            elif w and w["mod"]:
                addr, where = None, f"{w['mod']}:0x{int(w['modpc'], 16):08x}"
            else:
                addr, where = None, "?"
            origin = "EBOOT" if addr is not None and addr < args.eboot_end else (
                w["mod"] if w and w["mod"] else "PRX")
            if args.from_eboot and origin != "EBOOT":
                continue
            row = [m["time"], th["name"] if th else "?", origin, where, m["chan"], c["func"],
                   c["args"].rstrip().removesuffix(")")[:160], 1]
            if args.unique:
                if row[5] in first:
                    first[row[5]][7] += 1
                    continue
                if args.n and len(rows) >= args.n:
                    continue  # se sigue leyendo para contar las repeticiones
                first[row[5]] = row
                rows.append(row)
                continue
            last = rows[-1] if rows else None
            if args.collapse and last and last[1] == row[1] and last[3] == row[3] and last[5] == row[5]:
                last[7] += 1
                continue
            if args.n and len(rows) >= args.n:
                break
            rows.append(row)

    with open(args.output, "w", encoding="utf-8") as f:
        f.write("n\ttiempo\thilo\torigen\tdir\tcanal\tfuncion\targs\trepeticiones\n")
        for i, r in enumerate(rows, 1):
            f.write(f"{i}\t" + "\t".join(str(x) for x in r) + "\n")
    print(f"{len(rows)} filas -> {args.output}")


if __name__ == "__main__":
    main()
