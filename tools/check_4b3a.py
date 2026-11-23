"""check_4b3a.py - check reproducible del sub-paso 4b-3a (cellGameContentPermit, parche 0006).

Evalua los logs y el VFS de una corrida hecha con:

    powershell -ExecutionPolicy Bypass -File tools\\run.ps1 -Timeout 60 -Tag check_4b3a ^
        -FsLog -Env GCM_FLIPCOUNT=1
    python tools/check_4b3a.py check_4b3a

y comprueba:
  1. Todo lo del 4b-2 (tools/check_4b2.py) sigue pasando.
  2. ContentPermit de la cache, que no existe, devuelve las dos rutas vacias:
     "ContentPermit(dir='BLES02029_CACHE') -> contentInfoPath='' usrdirPath=''"
     (RPCS3 cellGame.cpp:984-990).
  3. 0 carpetas o archivos creados durante la corrida bajo work/vfs/bdvd (la copia del disco)
     ni en work/vfs/hdd0/game/BLES02029. "Durante la corrida" = fecha de creacion posterior
     a la del log de stdout, que run.ps1 crea al lanzar el proceso.
Sale con 0 si todo se cumple.
"""

import os
import re
import sys

from check_4b2 import ROOT, checks_4b2, read_logs


def created_since(top, t0):
    """Rutas bajo top (incluida) cuya fecha de creacion es >= t0."""
    found = []
    if not os.path.exists(top):
        return found
    if os.stat(top).st_birthtime >= t0:
        found.append(top)
    for dirpath, dirnames, filenames in os.walk(top):
        for n in dirnames + filenames:
            p = os.path.join(dirpath, n)
            if os.stat(p).st_birthtime >= t0:
                found.append(p)
    return found


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "check_4b3a"
    out, err = read_logs(tag)
    both = out + err
    checks = checks_4b2(out, err)

    permits = re.findall(r"\[cellGame\] ContentPermit\(dir='([^']*)'\) -> "
                         r"contentInfoPath='([^']*)' usrdirPath='([^']*)'", both)
    cache = [(c, u) for d, c, u in permits if d == "BLES02029_CACHE"]
    empty = sum(1 for _, c, u in permits if not c and not u)

    t0 = os.stat(os.path.join(ROOT, "work", "run", f"{tag}.out.log")).st_birthtime
    vfs = os.path.join(ROOT, "work", "vfs")
    new_bdvd = created_since(os.path.join(vfs, "bdvd"), t0)
    new_tid = created_since(os.path.join(vfs, "hdd0", "game", "BLES02029"), t0)

    checks += [
        ("ContentPermit de BLES02029_CACHE con rutas vacias", cache,
         len(cache) >= 1 and all(c == "" and u == "" for c, u in cache)),
        ("ContentPermit con rutas vacias (total)", empty, empty >= 1),
        ("carpetas/archivos creados bajo bdvd", len(new_bdvd), not new_bdvd),
        ("carpetas/archivos creados en hdd0/game/BLES02029", len(new_tid), not new_tid),
    ]
    ok = True
    for name, val, good in checks:
        print(f"{'OK ' if good else 'MAL'} {name}: {val}")
        ok &= good
    for p in new_bdvd + new_tid:
        print("    creado:", os.path.relpath(p, ROOT))
    for d, c, u in permits:
        print(f"    ContentPermit(dir='{d}') -> '{c}' '{u}'")
    print("CHECK 4b-3a:", "PASA" if ok else "FALLA")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
