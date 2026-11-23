"""hair_cmp.py - 2a: compares the hair of two runs of the same song stretch (REFINE_HAIR_TRACE=<song s>), usually
30 fps (the original physics) against a higher rate. Per osage, on the tip in its parent bone's own axes, by song
time: the swing (RMS of the move around its own 0.5 s mean) in each run and its ratio, and the RMS distance
between the two runs. A ratio far under 1 is stiffer hair than the original; over 1, looser.
Usage: python -I tools/hair_cmp.py work/run/<ref>.err.log work/run/<other>.err.log"""
import math, re, sys, collections, bisect
pat = re.compile(r'\[hair-trace\] t=([\d.]+) f=\d+ o=(\w+) tick=\d a=[\d.]+ sp=[\d.]+ tip (\S+) (\S+) (\S+) par (\S+) (\S+) (\S+) loc (\S+) (\S+) (\S+)')
def load(path):
    rows = collections.OrderedDict()
    for line in open(path, errors='replace'):
        m = pat.search(line)
        if not m: continue
        g = m.groups(); v = list(map(float, g[2:]))
        rows.setdefault(g[1], []).append((float(g[0]), tuple(v[6:9])))
    out = collections.OrderedDict()
    for o, r in rows.items():   # one sample per song time (the song clock stands still between ticks in some modes)
        d = collections.OrderedDict()
        for t, v in r: d[t] = v
        out[o] = sorted(d.items())
    return out
def at(r, ts, t):
    i = bisect.bisect_left(ts, t)
    if i <= 0: return r[0][1]
    if i >= len(r): return r[-1][1]
    (t0, a), (t1, b) = r[i - 1], r[i]
    u = (t - t0) / (t1 - t0) if t1 > t0 else 0.0
    return tuple(a[k] + (b[k] - a[k]) * u for k in range(3))
def series(r, t0, t1, n=480):
    ts = [x[0] for x in r]
    return [at(r, ts, t0 + (t1 - t0) * k / (n - 1)) for k in range(n)]
def swing(s, win):
    acc = 0.0
    for k in range(len(s)):
        a, b = max(0, k - win), min(len(s), k + win + 1)
        m = [sum(x[i] for x in s[a:b]) / (b - a) for i in range(3)]
        acc += sum((s[k][i] - m[i]) ** 2 for i in range(3))
    return math.sqrt(acc / len(s))
A, B = load(sys.argv[1]), load(sys.argv[2])
tot = [0.0, 0.0]
for (oa, ra), (ob, rb) in zip(A.items(), B.items()):
    t0, t1 = max(ra[0][0], rb[0][0]), min(ra[-1][0], rb[-1][0])
    if t1 - t0 < 1.0: continue
    sa, sb = series(ra, t0, t1), series(rb, t0, t1)
    win = int(0.25 * len(sa) / (t1 - t0))
    wa, wb = swing(sa, win), swing(sb, win)
    dist = math.sqrt(sum(sum((x[i] - y[i]) ** 2 for i in range(3)) for x, y in zip(sa, sb)) / len(sa))
    tot[0] += wa; tot[1] += wb
    print('%s/%s  %.1f s  swing ref %.5f other %.5f (x%.2f)  distance %.5f' % (oa, ob, t1 - t0, wa, wb, wb / wa if wa else 0, dist))
print('all: swing x%.2f' % (tot[1] / tot[0] if tot[0] else 0))
