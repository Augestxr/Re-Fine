"""hle_first_calls.py - orden de la primera llamada a cada import, frente a RPCS3.

Lee el stderr de una corrida con traza HLE:

    powershell -ExecutionPolicy Bypass -File tools\\run.ps1 -Timeout 60 -Tag <tag> -HleTrace 400000
    python tools/hle_first_calls.py work/run/<tag>.err.log [--lines] [--rpcs3 <tsv>]

y escribe, por cada NID, su primera llamada en el orden en que ocurre, con el nombre
(work/rpcs3/rpcs3_import_names.json) y su numero en la secuencia de RPCS3
(work/rpcs3/hle_unique_eboot.tsv, de tools/rpcs3_hle_seq.py --unique --from-eboot):

    RPCS3 # 95 lr=0x0001E5CC cellScreenShotEnable
       (no en RPCS3/LLE) lr=0x0048E68C cellFsWriteWithOffset

"no en RPCS3/LLE" = RPCS3 no la registra como llamada del EBOOT (la atiende un PRX en LLE o
no la llama). Sin --lines la salida no lleva numeros de linea, para poder comparar dos
corridas con diff.
"""

import csv
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    argv = sys.argv[1:]
    tsv = os.path.join(ROOT, "work", "rpcs3", "hle_unique_eboot.tsv")
    if "--rpcs3" in argv:   # --rpcs3 <tsv>: otra secuencia de RPCS3 (p. ej. primer_arranque_unique_eboot.tsv)
        i = argv.index("--rpcs3")
        tsv = argv[i + 1]
        del argv[i:i + 2]
    args = [a for a in argv if not a.startswith("--")]
    show_lines = "--lines" in argv
    names = json.load(open(os.path.join(ROOT, "work", "rpcs3", "rpcs3_import_names.json")))
    names = {int(k, 16): v for k, v in names.items()}
    rp = {}
    for row in csv.DictReader(open(tsv, encoding="utf-8"), delimiter="\t"):
        rp.setdefault(row["funcion"], int(row["n"]))

    seen = set()
    for i, line in enumerate(open(args[0], encoding="utf-8", errors="replace"), 1):
        m = re.match(r"\[HLETRACE\] nid=0x([0-9A-F]+) .* lr=0x([0-9A-F]+)", line)
        if not m:
            continue
        nid = int(m.group(1), 16)
        if nid in seen:
            continue
        seen.add(nid)
        n = names.get(nid, "?%08X" % nid)
        tag = "RPCS3 #%3d" % rp[n] if n in rp else "   (no en RPCS3/LLE)"
        where = f" line={i:9d}" if show_lines else ""
        print(f"{tag}{where} lr=0x{m.group(2)} {n}")


if __name__ == "__main__":
    main()
