"""rpcs3_threads.py - hilos PPU de un RPCS3.log: nombre, prioridad, pila, entry y ultima llamada.

    python tools/rpcs3_threads.py work/rpcs3/primer_arranque.log [-o hilos.tsv]

Junta cada `_sys_ppu_thread_create(... prio=, stacksize=, ...)` con su
`Thread "nombre" created (id=, func=...)`, y para cada hilo busca su ultima linea en
el log (la llamada en la que estaba al cerrar RPCS3) y cuantas veces llamo a
sys_cond_wait / _sys_lwcond_queue_wait / sys_event_queue_receive (las esperas
largas que interesan para PvOsageManager y compania).
"""

import argparse
import re

CREATE = re.compile(r'^\S+ (\S+) \{PPU\[(0x[0-9a-f]+)\] Thread \(([^)]*)\).*_sys_ppu_thread_create\(.*prio=(-?\d+), stacksize=(0x[0-9a-f]+)')
CREATED = re.compile(r'Thread "([^"]*)" created \(id=(0x[0-9a-f]+), func=\*?(0x[0-9a-f]+)')
LINE = re.compile(r'^\S+ (\S+) \{PPU\[(0x[0-9a-f]+)\] Thread \([^)]*\) \[([^\]]*)\]\} (.*)$')
WAITS = ('sys_cond_wait(', '_sys_lwcond_queue_wait(', 'sys_event_queue_receive(', 'sys_mutex_lock(')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('log')
    ap.add_argument('-o', '--output')
    a = ap.parse_args()

    threads = []          # [t, id, nombre, padre, prio, pila, func]
    pending = None
    last = {}             # id -> (t, origen, mensaje)
    waits = {}
    with open(a.log, encoding='utf-8', errors='replace') as f:
        for line in f:
            if '_sys_ppu_thread_create(' in line and 'prio=' in line:
                m = CREATE.match(line)
                if m:
                    pending = (m.group(3), m.group(4), m.group(5))
            elif ' created (id=' in line:
                m = CREATED.search(line)
                if m:
                    t = line.split(' ', 2)[1]
                    parent, prio, stack = pending if pending else ('?', '?', '?')
                    threads.append([t, m.group(2), m.group(1), parent, prio, stack, m.group(3)])
                    pending = None
            m = LINE.match(line)
            if m:
                tid = m.group(2)
                last[tid] = (m.group(1), m.group(3), m.group(4)[:150])
                for w in WAITS:
                    if w in line:
                        waits.setdefault(tid, {}).setdefault(w[:-1], 0)
                        waits[tid][w[:-1]] += 1

    rows = ['t\tid\tnombre\tpadre\tprio\tpila\tentry\tesperas\tultima_t\tultima_origen\tultima_llamada']
    for t, tid, name, parent, prio, stack, func in threads:
        w = ', '.join(f'{k} {v}' for k, v in sorted(waits.get(tid, {}).items()))
        lt, lo, lm = last.get(tid, ('', '', ''))
        rows.append('\t'.join([t, tid, name, parent, prio, stack, func, w, lt, lo, lm]))
    out = '\n'.join(rows) + '\n'
    if a.output:
        open(a.output, 'w', encoding='utf-8').write(out)
        print(f'{len(threads)} hilos -> {a.output}')
    else:
        print(out, end='')


if __name__ == '__main__':
    main()
