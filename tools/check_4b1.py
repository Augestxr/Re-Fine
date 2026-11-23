"""check_4b1.py - check reproducible del sub-paso 4b-1 (sys_lwcond real, PvOsageManager).

Evalua los logs de una corrida hecha con:

    powershell -ExecutionPolicy Bypass -File tools\\run.ps1 -Timeout 60 -Tag check_4b1 ^
        -Env PS3_LWCOND_TRACE=1,GCM_FLIPCOUNT=1
    python tools/check_4b1.py check_4b1

y comprueba:
  1. 0 excepciones "invalid vector<T> subscript" (el fallo de PvOsageManager).
  2. 4 hilos PvOsageManager creados.
  3. Los 4 quedan esperando en su lwcond (LR 0x416CC8) sin ningun signal sobre ella,
     como en RPCS3 (_sys_lwcond_queue_wait timeout=0 durante todo el arranque).
  4. 0 imports que finjan exito: ninguna linea "faking CELL_OK" (PS3_HLE_UNRESOLVED=fail
     surte efecto, parche 0005).
  5. El juego presenta frames: al menos 600 flips (10 s a 60 fps) y al menos 17 draws.
Sale con 0 si todo se cumple.
"""

import os
import re
import sys


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tag = sys.argv[1] if len(sys.argv) > 1 else "check_4b1"
    d = os.path.join(root, "work", "run")
    out = open(os.path.join(d, f"{tag}.out.log"), encoding="utf-8", errors="replace").read()
    err = open(os.path.join(d, f"{tag}.err.log"), encoding="utf-8", errors="replace").read()

    exc = out.count("invalid vector<T> subscript")
    pv_tids = {int(t) for t in re.findall(r'sys_ppu_thread_create tid=(\d+) name="PvOsageManager"', err)}

    open_w, sig = {}, {}
    for m in re.finditer(r"\[LWC\] (wait  >|wait  <|signal\S*)\s+tid=(\d+) lwcond=0x([0-9A-F]+)(?:.*?lr=0x([0-9A-F]+))?", err):
        kind, tid, c, lr = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        if kind == "wait  >":
            open_w[(tid, c)] = lr
        elif kind == "wait  <":
            open_w.pop((tid, c), None)
        else:
            sig[c] = sig.get(c, 0) + 1
    pv_waiting = [(t, c) for (t, c), lr in open_w.items() if t in pv_tids and lr == "00416CC8"]
    pv_signalled = sum(sig.get(c, 0) for _, c in pv_waiting)

    faking_ok = len(re.findall(r"faking CELL_OK", err))
    flips = len(re.findall(r"^\[flip\] \d+", err, re.M))
    last_flip = re.findall(r"^\[flip\] (\d+) at", err, re.M)
    flips_n = int(last_flip[-1]) if last_flip else 0
    draws = re.findall(r"draw_arrays #(\d+)", out)
    draws_n = max((int(x) for x in draws), default=0)

    checks = [
        ("excepciones invalid vector", exc, exc == 0),
        ("hilos PvOsageManager creados", len(pv_tids), len(pv_tids) == 4),
        ("PvOsageManager esperando en lwcond (lr 0x416CC8)", len(pv_waiting), len(pv_waiting) == 4),
        ("signals sobre esas lwcond", pv_signalled, pv_signalled == 0),
        ("imports con exito falso (faking CELL_OK)", faking_ok, faking_ok == 0),
        ("flips presentados", flips_n, flips_n >= 600),
        ("draw_arrays (numero del ultimo registrado)", draws_n, draws_n >= 17),
    ]
    ok = True
    for name, val, good in checks:
        print(f"{'OK ' if good else 'MAL'} {name}: {val}")
        ok &= good
    print("CHECK 4b-1:", "PASA" if ok else "FALLA")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
