"""rob_cmp.py - where the characters of a song are, at two frame rates.

Reads the [rob] lines of two runs (REFINE_ROB_TRACE=<from>-<to>, src/rob_interp.cpp) and compares, per character (its
order in the game's list) and second of the song, where the middle of its skeleton is drawn. A second where the median
distance is over --tol metres is printed, with the camera's difference (a camera that also differs means the two runs
are on another shot, not that the character is out of place).

Usage:  python tools/rob_cmp.py work/run/<30 fps>.err.log work/run/<other>.err.log [--tol 0.1]
"""
import argparse

import numpy as np


def load(path):
    d = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            if ln.startswith("[rob] "):
                v = ln.split()
                if len(v) == 10:
                    d.setdefault(int(v[2]), []).append([float(v[1])] + [float(x) for x in v[3:]])
    out = {}
    for k, v in d.items():
        a = np.array(v)
        a = a[np.isfinite(a).all(axis=1)]
        a = a[np.argsort(a[:, 0], kind="stable")]
        out[k] = a[np.r_[a[1:, 0] - a[:-1, 0] > 1e-6, True]]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("other")
    ap.add_argument("--tol", type=float, default=0.1)
    a = ap.parse_args()
    A, B = load(a.ref), load(a.other)
    print("characters:", sorted(A), sorted(B))
    bad = 0
    for k in sorted(set(A) & set(B)):
        x, y = A[k], B[k]
        t = x[:, 0]
        m = (t > y[0, 0]) & (t < y[-1, 0])
        x, t = x[m], t[m]
        q = np.stack([np.interp(t, y[:, 0], y[:, c]) for c in range(1, 8)], 1)
        e = np.linalg.norm(q[:, :3] - x[:, 1:4], axis=1)
        cam = np.linalg.norm(q[:, 4:7] - x[:, 5:8], axis=1)
        fr = q[:, 3] - x[:, 4]
        print(f"character {k}: {len(t)} frames, median {np.median(e):.4f} m, p95 {np.percentile(e, 95):.3f}, over {a.tol} m: {100 * (e > a.tol).mean():.1f} %")
        run = None
        secs = []
        for s in range(int(t[0]), int(t[-1]) + 1):
            j = (t >= s) & (t < s + 1)
            if j.sum() > 5 and np.median(e[j]) > a.tol:
                secs.append((s, np.median(e[j]), np.median(cam[j]), np.median(fr[j])))
        bad += len(secs)
        for s, d, c, f in secs:
            print(f"   {s:4d} s: {d:.2f} m, camera {c:.2f} m, frame {f:+.1f}")
    print(f"{bad} seconds out of place")


if __name__ == "__main__":
    main()
