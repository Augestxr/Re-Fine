"""hair_smooth.py - does the hair move smoothly between the 30 Hz ticks, or in steps?

Reads the [hair-trace] lines of one run (REFINE_HAIR_TRACE=<song second>, src/rob_interp.cpp): per osage chain,
the tip of the chain as drawn, every frame, with the flag of the frames that ran a tick. For each frame the
"jerk" is how much the per-frame move changed from the frame before (|v_f - v_f-1|). Motion that is smooth at
the frame rate has about the same jerk on every frame; motion that turns at each tick (a 30 Hz kink, what the eye
reads as stutter) has its jerk piled up on the tick frames.

Prints, over all chains: the median jerk on tick frames, on the other frames, and their ratio (1.0 = no trace
of the 30 Hz grid; the higher, the more the ticks show), plus the median move per frame for scale. Medians, and
frames that move more than JUMP_M are left out: a camera cut or a jump of the character moves everything at once
and a handful of those swamp a mean (first version of this script, 2026-11-21).

Usage:  python tools/hair_smooth.py work/run/<run>.err.log [more runs...]
"""
import collections
import math
import re
import sys

JUMP_M = 0.03   # a frame that moves the tip more than this is a cut or a jump, not hair motion

PAT = re.compile(r"\[hair-trace\] t=([\d.]+) f=(\d+) o=(\w+) tick=(\d) a=\S+ sp=\S+ tip (\S+) (\S+) (\S+)")


def load(path):
    chains = collections.defaultdict(dict)
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = PAT.search(line)
            if not m:
                continue
            frame, o, tick = int(m.group(2)), m.group(3), int(m.group(4))
            tip = tuple(float(x) for x in m.group(5, 6, 7))
            chains[o][frame] = (tick, tip)     # the last line of a frame is what was drawn
    return chains


def analyse(chains):
    jt, jn, mv = [], [], []
    for o, rows in chains.items():
        frames = sorted(rows)
        for a, b, c in zip(frames, frames[1:], frames[2:]):
            if b != a + 1 or c != b + 1:
                continue
            pa, pb, pc = rows[a][1], rows[b][1], rows[c][1]
            v1 = [pb[i] - pa[i] for i in range(3)]
            v2 = [pc[i] - pb[i] for i in range(3)]
            j = math.sqrt(sum((v2[i] - v1[i]) ** 2 for i in range(3)))
            m1, m2 = math.sqrt(sum(x * x for x in v1)), math.sqrt(sum(x * x for x in v2))
            if m1 > JUMP_M or m2 > JUMP_M:
                continue
            mv.append(m2)
            # the change of velocity that lands on frame c: a tick there turns the motion
            (jt if rows[c][0] else jn).append(j)
    return jt, jn, mv


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        chains = load(path)
        jt, jn, mv = analyse(chains)
        if not jt or not jn:
            print("%s: not enough [hair-trace] frames" % path)
            continue
        med = lambda v: sorted(v)[len(v) // 2]
        mt, mn = med(jt), med(jn)
        print("%s: %d chains, %d frames; median jerk on tick frames %.6f, others %.6f, ratio %.2f; median move %.5f m/frame"
              % (path, len(chains), len(jt) + len(jn), mt, mn, mt / mn if mn > 0 else float("inf"), med(mv)))


if __name__ == "__main__":
    main()
