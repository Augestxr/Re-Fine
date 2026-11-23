"""cam_sync.py - where the character sits in the picture, against the original at 30 fps.

Reads the [cam-trace] lines of two runs (REFINE_CAM_TRACE=<song second>, src/rob_interp.cpp) and, for every
frame of the test run, compares the direction from the camera to the character's skeleton centre with the
reference run at the same song time (linearly interpolated between its two nearest frames). The angle between
the two directions is how far off the character is drawn in the picture, in degrees.

Frames next to a camera cut in the reference are left out: there the reference jumps and any small timing
difference reads as a huge angle. A shift search reports the time offset that best explains the test run
(e.g. +33 ms means the test run shows the reference's picture from 33 ms earlier, a tick late).

Usage:  python tools/cam_sync.py work/run/<ref>.err.log work/run/<test>.err.log [--from S] [--to S]
"""
import argparse
import bisect
import math
import re
import sys

LINE = re.compile(r"\[cam-trace\] song ([-\d.]+) world (\S+) (\S+) (\S+) view (\S+) (\S+) (\S+) eye (\S+) (\S+) (\S+) shot (\d+)")


def load(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if not m:
                continue
            v = [float(x) for x in m.groups()[:10]]
            rows.append({"t": v[0], "view": v[4:7], "eye": v[7:10]})
    rows.sort(key=lambda r: r["t"])
    # A song clock that stands still for several frames gives one sample.
    out = []
    for r in rows:
        if out and abs(r["t"] - out[-1]["t"]) < 1e-6:
            out[-1] = r
        else:
            out.append(r)
    return out


def unit(v):
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v] if n > 1e-9 else None


def angle(a, b):
    ua, ub = unit(a), unit(b)
    if not ua or not ub:
        return None
    d = max(-1.0, min(1.0, sum(x * y for x, y in zip(ua, ub))))
    return math.degrees(math.acos(d))


def find_cuts(ref, deg=8.0):
    """Reference frames where the view of the character jumps (a camera cut)."""
    cuts = []
    for i in range(1, len(ref)):
        a = angle(ref[i - 1]["view"], ref[i]["view"])
        if a is not None and a > deg:
            cuts.append(ref[i]["t"])
    return cuts


def ref_at(ref, times, t):
    i = bisect.bisect_left(times, t)
    if i <= 0 or i >= len(ref):
        return None
    a, b = ref[i - 1], ref[i]
    u = (t - a["t"]) / (b["t"] - a["t"])
    return [x + (y - x) * u for x, y in zip(a["view"], b["view"])]


def compare(ref, test, shift, lo, hi, cuts):
    times = [r["t"] for r in ref]
    errs = []
    for r in test:
        t = r["t"]
        if t < lo or t > hi:
            continue
        if any(abs(t - c) < 0.1 for c in cuts):
            continue
        v = ref_at(ref, times, t - shift)
        if v is None:
            continue
        a = angle(v, r["view"])
        if a is not None:
            errs.append(a)
    errs.sort()
    return errs


def summary(errs):
    if not errs:
        return "no frames"
    n = len(errs)
    return "%d frames, mean %.3f deg, median %.3f, p95 %.3f, max %.3f" % (
        n, sum(errs) / n, errs[n // 2], errs[min(n - 1, int(n * 0.95))], errs[-1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("test")
    ap.add_argument("--from", dest="lo", type=float, default=-1e9)
    ap.add_argument("--to", dest="hi", type=float, default=1e9)
    a = ap.parse_args()
    ref, test = load(a.ref), load(a.test)
    if len(ref) < 10 or len(test) < 10:
        sys.exit("not enough [cam-trace] lines (ref %d, test %d)" % (len(ref), len(test)))
    eye = max(max(abs(x) for x in r["eye"]) for r in ref)
    print("reference %d frames %.2f-%.2f s, test %d frames %.2f-%.2f s; eye through the view matrix: max %.4f (0 = convention right)"
          % (len(ref), ref[0]["t"], ref[-1]["t"], len(test), test[0]["t"], test[-1]["t"], eye))
    lo = max(a.lo, ref[0]["t"] + 0.2, test[0]["t"])
    hi = min(a.hi, ref[-1]["t"] - 0.2, test[-1]["t"])
    cuts = find_cuts(ref)
    print("cuts in the reference: %d" % len(cuts))
    print("as drawn:      " + summary(compare(ref, test, 0.0, lo, hi, cuts)))
    best = None
    for ms in range(-80, 81, 4):
        e = compare(ref, test, ms / 1000.0, lo, hi, cuts)
        if e:
            m = sum(e) / len(e)
            if best is None or m < best[1]:
                best = (ms, m)
    if best:
        print("best shift:    %+d ms (mean %.3f deg)  [+ = the test run shows the picture late]" % best)


if __name__ == "__main__":
    main()
