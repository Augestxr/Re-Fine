"""hair_trace.py - 2a: reads the [hair-trace] lines of a run (REFINE_HAIR_TRACE=<song s>, 8 s of song) and says,
per half second of song and over all the osages: frames, ticks, and how the parent bone (world) and the hair
tip (in the bone's own axes) move frame by frame: the median move on tick frames against the other frames
(a 30 Hz stutter is a ratio far from 1), frames in which the bone did not move at all between ticks (the body
at 30 Hz: no blend), and the largest move. Usage: python -I tools/hair_trace.py work/run/<tag>.err.log"""
import math, re, sys, collections
pat = re.compile(r'\[hair-trace\] t=([\d.]+) f=(\d+) o=(\w+) tick=(\d) a=([\d.]+) sp=([\d.]+) tip \S+ \S+ \S+ par (\S+) (\S+) (\S+) loc (\S+) (\S+) (\S+)')
rows = collections.defaultdict(list)
for line in open(sys.argv[1], errors='replace'):
    m = pat.search(line)
    if m:
        g = m.groups()
        rows[g[2]].append((int(g[1]), float(g[0]), int(g[3]), tuple(map(float, g[6:9])), tuple(map(float, g[9:12]))))
B = collections.defaultdict(lambda: {'n': 0, 'ticks': 0, 'par': ([], []), 'loc': ([], []), 'still': 0, 'max': 0.0})
for o, r in rows.items():
    r.sort()
    for k in range(1, len(r)):
        if r[k][0] != r[k - 1][0] + 1: continue
        b = B[int(r[k][1] * 2) / 2]
        tick = r[k][2]
        dp, dl = math.dist(r[k][3], r[k - 1][3]), math.dist(r[k][4], r[k - 1][4])
        b['n'] += 1; b['ticks'] += tick
        b['par'][tick].append(dp); b['loc'][tick].append(dl)
        if not tick and dp < 1e-7: b['still'] += 1
        b['max'] = max(b['max'], dl)
med = lambda v: sorted(v)[len(v) // 2] if v else 0.0
print('song s  frames ticks | bone move tick/other (ratio) still | hair-in-bone move tick/other (ratio) max')
for t in sorted(B):
    b = B[t]
    pt, pn, lt, ln = med(b['par'][1]), med(b['par'][0]), med(b['loc'][1]), med(b['loc'][0])
    print('%6.1f  %6d %5d | %.5f / %.5f (x%.2f) %4d | %.5f / %.5f (x%.2f) %.5f' %
          (t, b['n'], b['ticks'], pt, pn, pt / pn if pn else 0, b['still'], lt, ln, lt / ln if ln else 0, b['max']))
