"""evq_window.py - eventos por cola de lv2 en una ventana del watchdog (PS3_EVQSTAT).

Uso:  python tools/evq_window.py work/run/<tag>.err.log 20 30

Necesita una corrida con PS3_WATCHDOG_S=1 y PS3_EVQSTAT=<n>: el runtime imprime cada n
pushes una linea "[evqstat] ... qN[pushed=X ...]" con el total acumulado por cola, y el
watchdog una linea "[WATCHDOG] t=S s" por segundo. Resta el ultimo [evqstat] anterior a
la primera linea del watchdog con t >= desde del ultimo anterior a la primera con t >= hasta,
y lo escala a 10 s (el oraculo de RPCS3 del 4b se cuenta en 10 s: 1875 / 3750 / 1875).
"""
import re
import sys

WD = re.compile(r"\[WATCHDOG\] t=([0-9.]+) s")
EQ = re.compile(r"q(\d+)\[pushed=(\d+)")


def main():
    path, t_from, t_to = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    last = {}
    marks = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("[evqstat]"):
                last = {int(q): int(n) for q, n in EQ.findall(line)}
                continue
            m = WD.search(line)
            if not m:
                continue
            t = float(m.group(1))
            for name, lim in (("from", t_from), ("to", t_to)):
                if name not in marks and t >= lim:
                    marks[name] = (t, dict(last))
    if "from" not in marks or "to" not in marks:
        sys.exit("la ventana no esta entera en el log")
    (ta, a), (tb, b) = marks["from"], marks["to"]
    span = tb - ta
    parts = []
    for q in sorted(b):
        n = b[q] - a.get(q, 0)
        parts.append("q%d: %d (%d por 10 s)" % (q, n, round(n * 10 / span)))
    print("ventana watchdog %.1f-%.1f s: %s" % (ta, tb, ", ".join(parts)))


if __name__ == "__main__":
    main()
