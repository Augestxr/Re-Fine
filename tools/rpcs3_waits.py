"""rpcs3_waits.py - esperas por syscall en una ventana de un RPCS3.log (4g.1).

    python tools/rpcs3_waits.py work/rpcs3/primer_arranque.log --from 170 --to 230

Por (syscall, sitio) cuenta las llamadas, el timeout pedido y el tiempo medio y maximo hasta la
siguiente linea del mismo hilo. Esa es la cota superior del tiempo bloqueado: RPCS3 no registra el
retorno de estas syscalls, solo la entrada.
"""

import argparse
import re

LINE = re.compile(r'^\S+ (\d+):(\d+):(\d+\.\d+) \{PPU\[(0x[0-9a-f]+)\] Thread \(([^)]*)\) \[([^\]]*)\]\} (.*)$')
WAITS = ('sys_cond_wait(', '_sys_lwcond_queue_wait(', 'sys_mutex_lock(', 'sys_event_queue_receive(',
         'sys_semaphore_wait(', 'sys_timer_usleep(', 'sys_timer_sleep(', 'sys_rwlock_rlock(', 'sys_rwlock_wlock(',
         'cellGcm')
TMO = re.compile(r'(?:timeout|sleep_time)=(0x[0-9a-f]+|\d+)')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('log')
    ap.add_argument('--from', dest='t0', type=float, required=True)
    ap.add_argument('--to', dest='t1', type=float, required=True)
    a = ap.parse_args()

    pending = {}   # tid -> (clave, t)
    stats = {}     # clave -> [n, suma, max, {timeouts}]
    lines = 0
    with open(a.log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            t = int(m[1]) * 3600 + int(m[2]) * 60 + float(m[3])
            if t < a.t0:
                continue
            if t > a.t1:
                break
            lines += 1
            tid = m[4]
            if tid in pending:
                key, t_in = pending.pop(tid)
                s = stats[key]
                d = t - t_in
                s[1] += d
                s[2] = max(s[2], d)
            msg = m[7]
            for w in WAITS:
                if w in msg:
                    name = msg[msg.index(w):].split('(')[0]
                    key = (name, m[5], m[6])
                    s = stats.setdefault(key, [0, 0.0, 0.0, {}])
                    s[0] += 1
                    tm = TMO.search(msg)
                    if tm:
                        s[3][tm[1]] = s[3].get(tm[1], 0) + 1
                    pending[tid] = (key, t)
                    break

    print(f'ventana {a.t0}-{a.t1} s, {lines} lineas de PPU')
    print(f'{"llamadas":>9} {"medio ms":>9} {"max ms":>9}  syscall / hilo / sitio / timeouts')
    for k, s in sorted(stats.items(), key=lambda kv: -kv[1][1]):
        tmo = ' '.join(f'{v}x{c}' for v, c in sorted(s[3].items(), key=lambda x: -x[1])[:3])
        print(f'{s[0]:>9} {1000 * s[1] / s[0]:>9.3f} {1000 * s[2]:>9.1f}  {k[0]} / {k[1]} / {k[2]} / {tmo}')


if __name__ == '__main__':
    main()
