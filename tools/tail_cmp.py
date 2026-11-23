"""tail_cmp.py - the long hair (an Auth3D animation) between ticks, and against the original at 30 fps.

Reads the [tail] lines of a run (REFINE_TAIL_TRACE=<song second>, src/rob_interp.cpp): per frame and equipment
object, the last bone of its animation (object 15, the hair: the tip of a tail) and the head bone, both in the
character's space. Prints, per object:

  saw     how much the tip turns back on a tick: the median, over the ticks, of the step on the tick frame projected
          on the mean step of the frames around it. 1 = the tick frame moves like the others, 0 = it stands still,
          negative = it jumps back (what reads as stuttering).
  jerk    the median distance between the tip and the middle of its two neighbours, in mm, on tick frames and on the
          others. Smooth motion has both alike; the 30 fps run is not comparable here (its steps are longer).
  error   with a 30 fps run as reference: the median distance to it at the same song time, in mm, after the time
          shift that fits best (+ = this run is late).

Frames where the tip moves over 15 cm (camera cuts, teleports) and their neighbours are left out.

Usage:  python tools/tail_cmp.py work/run/<run>.err.log [--ref work/run/<30 fps run>.err.log]
"""
import argparse
import bisect
import math
import re
import statistics as st

LINE = re.compile(r"\[tail\] t=([-\d.]+) obj=(\d+) tick=(\d) a=([-\d.]+) frame=([-\d.]+) game=([-\d.]+) "
                  r"tip (\S+) (\S+) (\S+) head (\S+) (\S+) (\S+)")
JUMP = 0.15


def load(path):
    objs = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if not m:
                continue
            g = m.groups()
            p = tuple(float(x) for x in g[6:9])
            if not all(math.isfinite(x) for x in p):
                continue
            rows = objs.setdefault(int(g[1]), [])
            t = float(g[0])
            if rows and t <= rows[-1][0]:          # the song clock stood still: keep the newest
                rows[-1] = (rows[-1][0], int(g[2]) or rows[-1][1], p)
            else:
                rows.append((t, int(g[2]), p))
    return objs


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def norm(a):
    return math.sqrt(sum(x * x for x in a))


def smooth(rows):
    saw, jt, jn = [], [], []
    for i in range(2, len(rows) - 2):
        w = [rows[k][2] for k in range(i - 2, i + 3)]
        steps = [sub(w[k + 1], w[k]) for k in range(4)]
        if max(norm(s) for s in steps) > JUMP:
            continue
        mid = tuple((a + b) / 2 for a, b in zip(w[1], w[3]))
        (jt if rows[i][1] else jn).append(1000 * norm(sub(w[2], mid)))
        if rows[i][1]:
            others = [steps[0], steps[2], steps[3]]          # steps[1] is the step onto the tick frame
            mean = tuple(sum(s[k] for s in others) / 3 for k in range(3))
            n = norm(mean)
            if n > 2e-4:
                saw.append(sum(a * b for a, b in zip(steps[1], mean)) / (n * n))
    return saw, jt, jn


def error(ref, rows, shift):
    ts = [r[0] for r in ref]
    d = []
    for t, _, p in rows:
        x = t - shift
        i = bisect.bisect_right(ts, x)
        if i <= 0 or i >= len(ts):
            continue
        a, b = ref[i - 1], ref[i]
        if norm(sub(a[2], b[2])) > JUMP or b[0] - a[0] > 0.06:
            continue
        u = (x - a[0]) / (b[0] - a[0])
        d.append(1000 * norm(sub(p, tuple(m + (n - m) * u for m, n in zip(a[2], b[2])))))
    return st.median(d) if len(d) > 50 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--ref")
    a = ap.parse_args()
    run, ref = load(a.run), load(a.ref) if a.ref else {}
    if not run:
        raise SystemExit("no [tail] lines")
    for o, rows in sorted(run.items()):
        saw, jt, jn = smooth(rows)
        line = f"obj {o}: {len(rows)} frames"
        if saw:
            line += f"  saw {st.median(saw):+.2f} ({len(saw)} ticks)"
        if jt and jn:
            line += f"  jerk tick {st.median(jt):.2f} mm, others {st.median(jn):.2f} mm"
        if o in ref:
            best = min((e, s) for s in range(-60, 61, 2) if (e := error(ref[o], rows, s / 1000)) is not None)
            here = error(ref[o], rows, 0.0)
            line += f"  error {here:.2f} mm, best {best[0]:.2f} mm at {best[1]:+d} ms"
        print(line)


if __name__ == "__main__":
    main()
