"""name_imports.py - pone nombre a los imports que saca ppu_loader.py.

ppu_loader.py escribe EBOOT.imports.json como [{library, nid, stub}], sin nombres.
Este script resuelve cada NID con la base de datos de ps3recomp
(tools/nid_database.py) y escribe EBOOT.named.imports.json con tres campos mas:

  name         nombre de la funcion, o null si el NID no esta en la base
  name_source  "ps3recomp" (nid_database.py), "extra" (--extra-names) o null
  implemented  true si el runtime de ps3recomp define una funcion con ese nombre
               (libs/ y runtime/), segun NIDDatabase.load_implemented()

Tambien imprime el resumen por modulo. No modifica el archivo del loader.

    python tools/name_imports.py work/loader_out/EBOOT.imports.json [--extra-names X.json]
"""

import argparse
import collections
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "external", "ps3recomp", "tools"))

from nid_database import NIDDatabase, get_default_db  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("imports", help="EBOOT.imports.json de ppu_loader.py")
    ap.add_argument("-o", "--output", help="por defecto: <dir>/EBOOT.named.imports.json")
    ap.add_argument("--extra-names", metavar="JSON",
                    help='{"0xNID": "nombre"} para los NIDs que la base de ps3recomp no tiene '
                         "(p. ej. los que nombra el cargador de RPCS3 en su log)")
    args = ap.parse_args()

    with open(args.imports) as f:
        imports = json.load(f)

    db = get_default_db()
    extra = {}
    if args.extra_names:
        with open(args.extra_names) as f:
            extra = {int(k, 16): v for k, v in json.load(f).items()}
    impl = NIDDatabase()
    impl.load_implemented(os.path.join(ROOT, "external", "ps3recomp"))

    out = []
    per_lib = collections.OrderedDict()
    for e in sorted(imports, key=lambda e: (e["library"], int(e["stub"], 16))):
        nid = int(e["nid"], 16)
        hit = db.lookup_nid(nid)
        name = hit[1] if hit else extra.get(nid)
        source = "ps3recomp" if hit else ("extra" if name else None)
        out.append(dict(e, name=name, name_source=source,
                        implemented=impl.lookup_nid(nid) is not None))
        s = per_lib.setdefault(e["library"], [0, 0, 0])
        s[0] += 1
        s[1] += name is not None
        s[2] += out[-1]["implemented"]

    dst = args.output or os.path.join(os.path.dirname(args.imports), "EBOOT.named.imports.json")
    with open(dst, "w") as f:
        json.dump(out, f, indent=1)

    print(f"{'modulo':24} {'imports':>7} {'con nombre':>10} {'en runtime':>10}")
    tot = [0, 0, 0]
    for lib, s in sorted(per_lib.items(), key=lambda kv: (-kv[1][0], kv[0])):
        print(f"{lib:24} {s[0]:7} {s[1]:10} {s[2]:10}")
        tot = [a + b for a, b in zip(tot, s)]
    print(f"{'TOTAL':24} {tot[0]:7} {tot[1]:10} {tot[2]:10}")
    print(f"escrito {dst}")


if __name__ == "__main__":
    main()
