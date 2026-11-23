"""draw_census.py - which drawn things only move on the 30 Hz ticks.

Reads the file of REFINE_DRAW_DUMP=<file>:<from s>-<to s> (0197, libs/video/rsx_d3d12_backend.c): per frame, what every
draw moves (the vertex-program constants written for it and up to six of its vertices). For each draw, followed from
frame to frame by its programs, texture and vertex count:

  jerk     of every value that moves: the sum of |x[i-1] - 2 x[i] + x[i+1]| over the sum of |x[i+1] - x[i-1]| / 2.
           Near 0 = it moves evenly; 2 = it stands still between ticks and jumps on them; more = it turns back.
  on tick  the share of that sum that falls on the two frames around each tick: near 1 = the ticks make it, about
           2 / (frames per tick) = something else does.
  bad      the share of the moving values with jerk over 0.7, on tick over 0.8 and jumps big enough to be seen.

Usage:  python tools/draw_census.py <dump> [--top 40] [--min 0.05] [--key <text>] [--regs]
"""
import argparse
import collections
import struct
import warnings

import numpy as np

HEAD = struct.Struct("<IIIIiifId")
DRAW = np.dtype([("call", "<u8"), ("fp", "<u4"), ("vs", "<u4"), ("nv", "<u4"), ("tex", "<u4"), ("rt", "<u4"),
                 ("blk", "<u4"), ("am", "<u2"), ("vpw", "<u2"), ("blend", "<u4"), ("v", "<f4", (6, 8))])


def load(path):
    """[(tick, alpha, {key: {name: value}})] per presented frame."""
    data = open(path, "rb").read()
    o, frames, cur, cnt, info = 0, [], {}, collections.Counter(), {}
    while o + HEAD.size <= len(data):
        magic, n, nblk, present, seq, tick, alpha, _, t = HEAD.unpack_from(data, o)
        if magic != 0x46444444:
            break
        o += HEAD.size
        blocks, regs = [], None
        try:
            for _ in range(nblk):
                m, = struct.unpack_from("<I", data, o); o += 4
                idx = np.frombuffer(data, "<u2", m, o); o += 2 * m
                val = np.frombuffer(data, "<f4", 4 * m, o).reshape(m, 4); o += 16 * m
                blocks.append((idx, val))
            draws = np.frombuffer(data, DRAW, n, o); o += DRAW.itemsize * n
        except ValueError:
            break
        for d in draws:
            k = (int(d["vs"]), int(d["fp"]) & 0xFFFFFFFF, int(d["nv"]), int(d["tex"]), int(d["vpw"]), int(d["am"]), int(d["blend"]))
            k += (cnt[k],)
            cnt[k[:-1]] += 1
            b = int(d["blk"])
            cur[k] = (blocks[b] if 0 < b < len(blocks) else None, d["v"])
        if present:
            frames.append((seq, tick, alpha, t, cur))
            cur, cnt = {}, collections.Counter()
    return frames


def series(frames, k):
    """(T, values) with NaN where the draw is missing, and the names of the columns."""
    regs = None
    for f in frames:
        if k in f[4] and f[4][k][0] is not None:
            r = set(f[4][k][0][0].tolist())
            regs = r if regs is None else regs & r
    regs = sorted(regs or [])
    names = [f"c{r}.{'xyzw'[i]}" for r in regs for i in range(4)] + [f"v{i}.a{0 if j < 4 else 8}{'xyzw'[j % 4]}" for i in range(6) for j in range(8)]
    out = np.full((len(frames), len(names)), np.nan, np.float64)
    for i, f in enumerate(frames):
        if k not in f[4]:
            continue
        blk, v = f[4][k]
        if regs:
            if blk is None:
                continue
            pos = np.searchsorted(blk[0], regs)
            out[i, :4 * len(regs)] = blk[1][pos].reshape(-1)
        out[i, 4 * len(regs):] = v.reshape(-1)
    return names, out


def analyse(frames, args):
    """[(share bad, moving values, jerk, on tick, key, names of the bad ones, how far they turn back)]"""
    ticks = [bool(f[1]) for f in frames]
    # phase: frames since the last tick, counted on the dump's own frames (the renderer may be a frame behind)
    ph, c = [], 0
    for x in ticks:
        c = 0 if x else c + 1
        ph.append(c)
    ph = np.array(ph)
    P = int(ph.max()) + 1
    tickish = np.array(ticks) | np.roll(np.array(ticks), -1)
    keys = collections.Counter(k for f in frames for k in f[4])
    rows = []
    with np.errstate(all="ignore"), warnings.catch_warnings():
        warnings.simplefilter("ignore")
        for k, n in keys.items():
            if n < len(frames) * 0.6:
                continue
            names, x = series(frames, k)
            d2 = np.abs(x[:-2] - 2 * x[1:-1] + x[2:])
            d1 = np.abs(x[2:] - x[:-2]) / 2
            ok = np.isfinite(d2)
            # a cut: every value jumps at once; leave out the frames where d1 is over 12 times its median
            med = np.nanmedian(np.where(d1 > 0, d1, np.nan), axis=0)
            ok &= ~(d1 > 12 * med)
            s1 = np.where(ok, d1, 0).sum(axis=0)
            s2 = np.where(ok, d2, 0).sum(axis=0)
            scale = np.nanmedian(np.abs(x), axis=0)
            moving = (s1 > 0.002 * len(frames) / 360 * np.maximum(scale, 1.0)) & ((np.where(ok, d1, 0) > 0).sum(axis=0) > len(frames) * 0.2)
            if not moving.any():
                continue
            jerk = s2 / s1
            by = np.array([np.where(ok & (ph[1:-1] == p)[:, None], d2, 0).sum(axis=0) for p in range(P)])
            tot = by.sum(axis=0)
            top2 = np.sort(by, axis=0)[-2:].sum(axis=0) / np.where(tot > 0, tot, 1) if P > 2 else np.ones(len(names))
            # big enough to be seen: half of its jumps on the ticks over 1.5 mm (constants) or 0.7 (vertices, pixels)
            d2t = np.where(ok & tickish[1:-1][:, None] & (d2 > 0), d2, np.nan)
            size = np.nanmedian(d2t, axis=0)
            eps = np.array([args.eps if n[0] == "c" else 0.7 for n in names])
            bad = moving & (jerk > 0.7) & (top2 > 0.8) & (size > eps)
            rows.append((bad.sum() / moving.sum(), int(moving.sum()), float(np.median(jerk[moving])),
                         float(np.median(top2[moving])), k, [names[i] for i in np.where(bad)[0][:10]], x, names, bad))
    return rows


def show(k):
    return f"vs={k[0]} fp={k[1]:08X} nv={k[2]} tex={k[3]:08X} vpw={k[4]} am={k[5]:04X} blend={k[6]} #{k[7]}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--min", type=float, default=0.05, help="least share of bad values for a draw to count")
    ap.add_argument("--win", type=float, default=3.0, help="seconds of each piece the dump is cut into")
    ap.add_argument("--eps", type=float, default=0.0015, help="least jump of a constant that counts (0 = any)")
    ap.add_argument("--key", help="only the draws whose line has this text, with the values of their worst columns")
    a = ap.parse_args()
    frames = load(a.dump)
    print(f"{len(frames)} frames, {sum(bool(f[1]) for f in frames)} with a tick")
    best, moves = {}, collections.Counter()
    t0 = frames[0][3]
    n = max(1, round((frames[-1][3] - t0) / a.win))
    for w in range(n):
        part = [f for f in frames if w * a.win <= f[3] - t0 < (w + 1) * a.win]
        if len(part) < 30:
            continue
        for r in analyse(part, a):
            moves[r[4]] += 1
            if r[4] not in best or r[0] > best[r[4]][1][0]:
                best[r[4]] = (w * a.win, r, part)
    rows = sorted(best.values(), key=lambda b: (b[1][0] <= a.min, -b[1][0] * b[1][1], -b[1][0]))
    print(f"{len(rows)} draws that move; {sum(b[1][0] > a.min for b in rows)} with over {100 * a.min:.0f} % of their values jumping on the ticks")
    for at, (fr, nm, j, tk, k, names, x, all_names, bad), part in rows[:a.top]:
        s = show(k)
        if a.key and a.key not in s:
            continue
        print(f"bad {100 * fr:5.1f} % of {nm:4d}  jerk {j:5.2f}  on tick {tk:4.2f}  at {at:4.1f} s  {s}  {' '.join(names)}")
        if a.key:
            for q in np.where(bad)[0][:6]:
                print("   ", all_names[q], " ".join(f"{'*' if part[t][1] else ''}{x[t, q]:.5g}" for t in range(min(40, len(x)))))


if __name__ == "__main__":
    main()
