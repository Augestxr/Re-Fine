#!/usr/bin/env python3
"""pso_time.py - Resume una corrida con PSO_TIME=1 y PS3_WATCHDOG_S=5.

Uso: python tools/pso_time.py work/run/<tag>.err.log [--from S] [--to S]

Primeras compilaciones ([PSOT] pipelines, [VST] vertex shaders): cuantas, suma, maximo y las que
pasan de un frame (33 ms). Flips/s por tramo de 5 s, sacados de las lineas [WATCHDOG] t= frames=.
Los fallos, de [FP] FAIL y [FPFAIL].
"""
import argparse
import re

PSOT = re.compile(r'\[(PSOT|VST)\] t=([\d.]+) ms=([\d.]+) hlsl=([\d.]+) dxc=([\d.]+) pso=([\d.]+) '
                  r'id=(0x[0-9A-F]+) ucode=(0x[0-9A-F]+) ok=(\d)')
WD = re.compile(r'\[WATCHDOG\] t=([\d.]+) s frames=(\d+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--from', dest='t0', type=float, default=0.0)
    ap.add_argument('--to', dest='t1', type=float, default=1e9)
    a = ap.parse_args()

    rows, wd, fails, last_ff = [], [], [], ''
    for line in open(a.log, errors='replace'):
        m = PSOT.search(line)
        if m:
            rows.append((m[1], float(m[2]), float(m[3]), float(m[4]), float(m[5]), float(m[6]),
                         m[7], m[8], int(m[9])))
            continue
        m = WD.search(line)
        if m:
            wd.append((float(m[1]), int(m[2])))
            continue
        if '[FP] FAIL' in line:
            fails.append(line.strip())
        elif '[FPFAIL]' in line:
            last_ff = line.strip()

    for tag in ('PSOT', 'VST'):
        r = [x for x in rows if x[0] == tag and a.t0 <= x[1] <= a.t1]
        if not r:
            print(f'{tag}: 0')
            continue
        ms = sorted(x[2] for x in r)
        worst = max(r, key=lambda x: x[2])
        print(f'{tag}: n={len(r)} suma={sum(ms):.1f} ms media={sum(ms) / len(ms):.1f} '
              f'p50={ms[len(ms) // 2]:.1f} max={ms[-1]:.1f} >33ms={sum(1 for v in ms if v > 33.3)} '
              f'ok={sum(x[8] for x in r)}')
        print(f'  por etapa: hlsl={sum(x[3] for x in r):.1f} dxc={sum(x[4] for x in r):.1f} '
              f'pso={sum(x[5] for x in r):.1f} ms')
        print(f'  peor: t={worst[1]:.1f} ms={worst[2]:.1f} hlsl={worst[3]:.1f} dxc={worst[4]:.1f} '
              f'pso={worst[5]:.1f} id={worst[6]} ucode={worst[7]}')
        # Suma por segundo: lo que un frame llega a tragarse.
        per = {}
        for x in r:
            per[int(x[1])] = per.get(int(x[1]), 0.0) + x[2]
        top = sorted(per.items(), key=lambda kv: -kv[1])[:8]
        print('  ms por segundo (peores): ' + ' '.join(f'{k}s:{v:.0f}' for k, v in sorted(top)))

    print(f'fallos registrados {len(fails)}')
    for f in fails:
        print(' ', f)
    if last_ff:
        print(' ', last_ff)

    if len(wd) > 1:
        print('flips/s por tramo:')
        out = []
        for (t0, f0), (t1, f1) in zip(wd, wd[1:]):
            if t1 > t0 and a.t0 <= t1 <= a.t1:
                out.append(f'{t0:.0f}:{(f1 - f0) / (t1 - t0):.1f}')
        print('  ' + ' '.join(out))


if __name__ == '__main__':
    main()
