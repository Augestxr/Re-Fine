"""hud_cmp.py - the 2D of a song (AET objects) at two frame rates, object by object.

Reads the files of REFINE_HUD_TRACE=<file>:<from>-<to> (src/evt.cpp): every 2D object of every frame of a stretch of a
song. An object is known by its layer in its scene (words +0x10 less +0x08) and its frame range, and paired with the one
of the other run that starts nearest in song time. For each layer: how many there are, how much later they start, how
long they live, and how fast their frame (+0x74) goes, in frames per second of real time.

What the score changes (gauge, judgement texts) only compares when both runs hit the same: at 30 fps the bot needs
PAD_BOT=-16 (it presses half a step early there), at 120 PAD_BOT=0; both then get every note COOL.

Usage:  python tools/hud_cmp.py <30 fps> <other> [--noise <another 30 fps>] [--tol 0.05] [--all]
"""
import argparse
import collections
import struct

import numpy as np

HEAD = struct.Struct("<IIdfIfI")
OBJ = np.dtype([("key", "<u4"), ("node", "<u4"), ("w", ">u4", 113)])


def load(path):
    data = open(path, "rb").read()
    o, frames = 0, []
    while o + HEAD.size <= len(data):
        magic, n, t, song, tick, speed, _ = HEAD.unpack_from(data, o)
        if magic != 0x31445548:
            break
        o += HEAD.size
        if o + n * OBJ.itemsize > len(data):
            break
        frames.append((t, song, tick, speed, np.frombuffer(data, OBJ, n, o)))
        o += n * OBJ.itemsize
    return frames


def f32(w):
    return struct.unpack(">f", struct.pack(">I", int(w)))[0]


def lives(frames):
    """{(layer, first, last, loop): [life]}; a life: start, end (song s), cut (seen at an edge), frames (t, frame)"""
    live, out = {}, collections.defaultdict(list)
    first_song, last_song = frames[0][1], frames[-1][1]

    def close(rec, song):                     # it ends on the first frame without it
        rec["end"] = song
        out[rec["id"]].append(rec)
    for t, song, tick, speed, objs in frames:
        seen = set()
        for x in objs:
            w = x["w"]
            k = int(x["key"])
            ident = ((int(w[4]) - int(w[2])) & 0xFFFFFFFF, f32(w[5]), f32(w[6]), int(w[7]) & 0x10000)
            seen.add(k)
            rec = live.get(k)
            if rec is None or rec["id"] != ident:
                if rec is not None:
                    close(rec, song)
                rec = live[k] = {"id": ident, "start": song, "t": [], "song": [], "frame": [], "flags": [], "pos": []}
            rec["t"].append(t)
            rec["song"].append(song)
            rec["frame"].append(f32(w[29]))
            rec["flags"].append(int(w[7]))
            rec["pos"].append((f32(w[14]), f32(w[15])))
        for k in [k for k in live if k not in seen]:
            close(live.pop(k), song)
    for rec in live.values():
        close(rec, last_song)
    for v in out.values():
        for r in v:
            r["cut"] = r["start"] <= first_song + 0.1 or r["end"] >= last_song - 0.1
        v.sort(key=lambda r: r["start"])
    return out


def rate(r):
    """Frames of the object per second, over the steps where it goes on; None when it does not, or when the
    game sets it itself (paused, flag 4: the gauges)."""
    if r["flags"][len(r["flags"]) // 2] & 4:
        return None
    d, dt = np.diff(r["frame"]), np.diff(r["t"])
    ok = d > 0
    # the first and last steps are cut short (created or held at the end in the middle of a frame)
    ok[:1] = ok[-2:] = False
    return float(d[ok].sum() / dt[ok].sum()) if ok.sum() >= 3 else None


def pair(a, b, tol=0.7):
    used, out = set(), []
    for x in a:
        best = None
        for j, y in enumerate(b):
            if j not in used and abs(y["start"] - x["start"]) < tol and (best is None or abs(y["start"] - x["start"]) < abs(b[best]["start"] - x["start"])):
                best = j
        if best is not None:
            used.add(best)
            out.append((x, b[best]))
    return out


def compare(A, B):
    res = {}
    for k in A.keys() | B.keys():
        p = pair(A.get(k, []), B.get(k, []))
        q = [(x, y) for x, y in p if not x["cut"] and not y["cut"]]
        ra = [rate(x) for x, _ in p if rate(x) is not None]
        rb = [rate(y) for _, y in p if rate(y) is not None]
        res[k] = {"n": (len(A.get(k, [])), len(B.get(k, []))),
                  "start": np.median([y["start"] - x["start"] for x, y in q]) if q else 0.0,
                  "startmax": max([abs(y["start"] - x["start"]) for x, y in q], default=0.0),
                  "life": np.median([(y["end"] - y["start"]) - (x["end"] - x["start"]) for x, y in q]) if q else 0.0,
                  "lifea": np.median([x["end"] - x["start"] for x, _ in q]) if q else 0.0,
                  "ra": np.median(ra) if ra else 0.0, "rb": np.median(rb) if rb else 0.0}
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("other")
    ap.add_argument("--noise")
    ap.add_argument("--tol", type=float, default=0.05)
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    A, B = lives(load(a.base)), lives(load(a.other))
    r = compare(A, B)
    nz = compare(A, lives(load(a.noise))) if a.noise else {}
    bad = 0
    for k, v in sorted(r.items()):
        z = nz.get(k)
        why = []
        if v["n"][0] != v["n"][1]:
            why.append("count")
        if abs(v["start"]) > a.tol:
            why.append("start")
        if abs(v["life"]) > a.tol:
            why.append("life")
        if v["ra"] and v["rb"] and abs(v["rb"] / v["ra"] - 1) > 0.05 or bool(v["ra"]) != bool(v["rb"]):
            why.append("rate")
        bad += bool(why)
        if why or a.all:
            print(f"{k[0]:08X} [{k[1]:g}-{k[2]:g}]{'loop' if k[3] else ''}: {v['n'][0]}/{v['n'][1]}"
                  + (f" ({z['n'][1]})" if z else "") + f"  start {1e3 * v['start']:+.0f} ms (max {1e3 * v['startmax']:.0f})"
                  + (f" noise {1e3 * z['start']:+.0f}" if z else "") + f"  life {1e3 * v['lifea']:.0f} {1e3 * v['life']:+.0f} ms"
                  + (f" noise {1e3 * z['life']:+.0f}" if z else "") + f"  rate {v['ra']:.1f} / {v['rb']:.1f}  {' '.join(why)}")
    print(f"{len(r)} layers, {bad} different")


if __name__ == "__main__":
    main()
