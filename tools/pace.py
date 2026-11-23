"""pace.py - how even the frames are: when the game starts them, how far each one moves the game, when they present.

Reads the [pace] lines of a run (REFINE_PACE=<from>-<to>, src/main.cpp). Per quantity: median, p1, p99 and the spread
(half of p84 - p16, what one standard deviation would be without the outliers), in ms.

  start    interval between two frame starts on the game's thread
  step     what the game moved in the frame, as time (one 30 Hz tick = 33.367 ms)
  present  interval between two presents that returned (with V-Sync it follows the screen)
  long     presents over 1.5 times the median interval (a frame missed)
  on screen  a model, with V-Sync: how many frames stay up another number of refreshes than the usual, and how far what
             a frame moved is from how long its picture was up, which is what the eye sees as uneven motion

Usage:  python tools/pace.py work/run/<run>.err.log [more logs]
"""
import math
import re
import statistics as st
import sys

TICK = 1000 / 29.97


def q(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, int(len(s) * p))]


def row(name, v):
    print(f"  {name:8s} n {len(v):5d}  median {st.median(v):7.3f}  p1 {q(v, .01):7.3f}  p99 {q(v, .99):7.3f}  "
          f"spread {(q(v, .84) - q(v, .16)) / 2:6.3f}")


for path in sys.argv[1:]:
    g, p, v = [], [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\[pace\] g ([-\d.]+) ([-\d.]+) (\d)", line)
        if m:
            g.append((float(m[1]), float(m[2]) * TICK))
        m = re.match(r"\[pace\] v ([-\d.]+) ([-\d.]+)", line)
        if m:
            v.append((float(m[1]), float(m[2])))
        m = re.match(r"\[pace\] p ([-\d.]+) ([-\d.]+)", line)
        if m:
            # The second call of a frame finds nothing to show and returns at once: not a present.
            if float(m[2]) - float(m[1]) > 0.02:
                p.append((float(m[1]), float(m[2])))
    print(path)
    if len(g) < 20 or len(p) < 20:
        print("  not enough [pace] lines", len(g), len(p))
        continue
    start = [b[0] - a[0] for a, b in zip(g, g[1:])]
    step = [s for _, s in g[1:]]
    pres = [b[1] - a[1] for a, b in zip(p, p[1:])]
    row("start", start)
    row("step", step)
    row("present", pres)
    row("in call", [b - a for a, b in p])
    med = st.median(pres)
    print(f"  long     {sum(1 for x in pres if x > 1.5 * med)} of {len(pres)}; "
          f"total step / time {sum(step) / (g[-1][0] - g[0][0]):.4f}")
    if not v:
        continue
    # A frame started at g[i] is the first present that begins after it. With V-Sync its picture goes up on the first
    # refresh after that and at least one after the picture before it. Where the refreshes fall against the presents is
    # not known (the compositor's readings of it wander by a third of a refresh), so every phase is tried: the mean
    # over all of them is what an unknown screen shows.
    per = st.median(x for _, x in v)
    shown, j = [], 0
    for t, s in g:
        while j < len(p) and p[j][0] <= t:
            j += 1
        if j >= len(p):
            break
        shown.append((p[j][1], s))
    miss, dev = [], []
    for k in range(32):
        last, rows = None, []
        for t, s in shown:
            n = math.ceil(t / per + k / 32)
            if last is not None:
                n = max(n, last + 1)
                rows.append((n - last, s))
            last = n
        hold = [n for n, _ in rows]
        common = max(set(hold), key=hold.count)
        miss.append(sum(1 for h in hold if h != common) / len(hold))
        dev.append(st.mean(abs(s - n * per) for n, s in rows))
    print(f"  on screen ({1000 / per:.2f} Hz, over all phases): {common} refresh per frame, "
          f"{100 * st.mean(miss):.1f} % of the frames another number (best {100 * min(miss):.1f}, worst {100 * max(miss):.1f}); "
          f"motion minus time on screen {st.mean(dev):.3f} ms")
