"""video_glitch.py - counts one-frame glitches in a video of the game (a capture of the author's) or in a burst of
tools/win_burst.ps1 (-Raw): a frame that differs from both its neighbours far more than they differ from each
other (a sprite drawn somewhere else for one frame). A video needs ffmpeg in the PATH. No frame is kept.
Usage: python -I tools/video_glitch.py <video> [from_s] [to_s]
       python -I tools/video_glitch.py -Raw <burst.raw> [parts]     (the burst cut in that many equal parts)"""
import subprocess, sys
W, H = 96, 54
n = W * H
def count(d, lo, hi):
    def diff(i, j):
        x = d[i * n:(i + 1) * n]; y = d[j * n:(j + 1) * n]
        return sum(abs(p - q) for p, q in zip(x, y)) / n
    adj = {i: diff(i, i - 1) for i in range(lo, hi + 1)}
    return [i for i in range(lo, hi) if (lambda s: adj[i] > 3 * s + 1.5 and adj[i + 1] > 3 * s + 1.5)(diff(i - 1, i + 1))]
if sys.argv[1] == '-Raw':
    d = open(sys.argv[2], 'rb').read(); N = len(d) // n
    parts = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    for k in range(parts):
        lo, hi = max(1, N * k // parts), min(N - 1, N * (k + 1) // parts) - 1
        print('part %d: %d captures, %d one-frame glitches' % (k + 1, hi - lo + 1, len(count(d, lo, hi))))
else:
    a = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    b = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
    fps = 60.0
    cmd = ['ffmpeg', '-v', 'error', '-i', sys.argv[1], '-vf', 'fps=%g,scale=%d:%d,format=gray' % (fps, W, H), '-f', 'rawvideo', '-']
    d = subprocess.run(cmd, stdout=subprocess.PIPE).stdout
    N = len(d) // n
    lo, hi = max(1, int(a * fps)), min(N - 1, int(b * fps)) - 1
    gl = count(d, lo, hi)
    secs = (hi - lo) / fps
    print('%d frames (%.1f s): %d one-frame glitches (%.2f per second)' % (hi - lo, secs, len(gl), len(gl) / secs if secs else 0))
    print('at s: ' + ' '.join('%.2f' % (i / fps) for i in gl[:40]))
