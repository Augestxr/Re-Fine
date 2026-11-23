"""check_4b2.py - check reproducible del sub-paso 4b-2 (title ID desde PARAM.SFO).

Evalua los logs de una corrida hecha con:

    powershell -ExecutionPolicy Bypass -File tools\\run.ps1 -Timeout 60 -Tag check_4b2 ^
        -FsLog -Env GCM_FLIPCOUNT=1
    python tools/check_4b2.py check_4b2

y comprueba:
  1. cellGame lee el title ID del PARAM.SFO del disco: 'BLES02029'
     (src/main.cpp llama a cellGame_init_from_paramsfo).
  2. cellGameBootCheck devuelve ese title ID (RPCS3: BLES02029).
  3. El juego comprueba su cache con ese ID: DataCheck(type=3, dir='BLES02029_CACHE'),
     como RPCS3 (RPCS3_sync.log, 0:00:12.76).
  4. Ninguna linea de los logs menciona el valor fijo 'BLES00000'.
  5. Lo que cerro el 4b-1 sigue igual: 0 excepciones "invalid vector", 0 imports con exito
     falso y al menos 600 flips.
Sale con 0 si todo se cumple.
"""

import os
import re
import sys


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_logs(tag):
    d = os.path.join(ROOT, "work", "run")
    out = open(os.path.join(d, f"{tag}.out.log"), encoding="utf-8", errors="replace").read()
    err = open(os.path.join(d, f"{tag}.err.log"), encoding="utf-8", errors="replace").read()
    return out, err


def checks_4b2(out, err):
    """Lista de (nombre, valor, bien) del 4b-2; tools/check_4b3a.py la reutiliza."""
    both = out + err

    sfo = re.findall(r"\[cellGame\] title id from PARAM\.SFO \('[^']*'\): '([^']*)'", both)
    boot = re.findall(r"\[cellGame\] BootCheck: type=\d+, titleId='([^']*)'", both)
    cache = both.count("[cellGame] DataCheck(type=3, dir='BLES02029_CACHE')")
    placeholder = both.count("BLES00000")

    exc = out.count("invalid vector<T> subscript")
    faking_ok = len(re.findall(r"faking CELL_OK", err))
    last_flip = re.findall(r"^\[flip\] (\d+) at", err, re.M)
    flips_n = int(last_flip[-1]) if last_flip else 0

    return [
        ("title id leido de PARAM.SFO", sfo[0] if sfo else "-", sfo == ["BLES02029"]),
        ("titleId de cellGameBootCheck", boot[0] if boot else "-", boot == ["BLES02029"]),
        ("DataCheck(type=3, dir='BLES02029_CACHE')", cache, cache >= 1),
        ("lineas con 'BLES00000'", placeholder, placeholder == 0),
        ("excepciones invalid vector", exc, exc == 0),
        ("imports con exito falso (faking CELL_OK)", faking_ok, faking_ok == 0),
        ("flips presentados", flips_n, flips_n >= 600),
    ]


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "check_4b2"
    checks = checks_4b2(*read_logs(tag))
    ok = True
    for name, val, good in checks:
        print(f"{'OK ' if good else 'MAL'} {name}: {val}")
        ok &= good
    print("CHECK 4b-2:", "PASA" if ok else "FALLA")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
