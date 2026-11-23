"""rpcs3_aet.py - the 2D objects (AET) of the game as RPCS3 runs it, read from outside (read only).

Samples the manager's objects (task vtable 0xBA8310, map at +0x60) of a running rpcs3.exe about 200 times a second for
some seconds and writes them in the format of REFINE_HUD_TRACE (src/evt.cpp), so tools/hud_cmp.py and friends read both.
It also prints, per layer, how fast its frame (+0x74) goes and, once a second, the game's speed block and flip counter.

Usage:  python tools/rpcs3_aet.py <pid> <seconds> <out.bin>
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

import numpy as np

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
BASE = 0x300000000


class Mem:
    def __init__(self, pid):
        self.h = k32.OpenProcess(0x0410, False, pid)      # VM_READ | QUERY_INFORMATION

    def read(self, ea, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, BASE + ea, buf, n, ctypes.byref(got)) or got.value != n:
            return None
        return buf.raw

    def u32(self, ea):
        b = self.read(ea, 4)
        return struct.unpack(">I", b)[0] if b else 0

    def f32(self, ea):
        b = self.read(ea, 4)
        return struct.unpack(">f", b)[0] if b else float("nan")


def ok(a):
    return 0x10000 <= a < 0x40000000


def manager(m):
    work = m.u32(m.u32(0xBF2A4C))
    head = m.u32(work + 4) if ok(work) else 0
    node = m.u32(head) if ok(head) else 0
    for _ in range(300):
        if not ok(node) or node == head:
            return 0
        task = m.u32(node + 8)
        if ok(task) and m.u32(task) == 0xBA8310:
            return task
        node = m.u32(node)
    return 0


def nodes(m, mgr):
    head = m.u32(mgr + 0x60)
    out, stack = [], [m.u32(head + 4)]
    while stack and len(out) < 600:
        n = stack.pop()
        if not ok(n) or n == head:
            continue
        b = m.read(n, 0x1D8)
        if not b or b[0x1D5]:
            continue
        out.append((n, b))
        stack += [struct.unpack(">I", b[0:4])[0], struct.unpack(">I", b[8:12])[0]]
    return out


def main():
    pid, secs, path = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
    m = Mem(pid)
    if m.u32(0xC00014) != 0x426FC28F or m.u32(0xBF2880) != 0x41F00000:
        sys.exit("not this game at guest base 0x%X" % BASE)
    mgr = manager(m)
    print("manager %08X" % mgr)
    t0 = time.perf_counter()
    last, prev, n_frames, t_log, flips = {}, None, 0, 0, None
    with open(path, "wb") as f:
        while (t := time.perf_counter() - t0) < secs:
            objs = nodes(m, mgr)
            sig = tuple((n, b[0x10 + 0x74:0x10 + 0x78]) for n, b in objs)
            if sig != prev:
                prev = sig
                n_frames += 1
                f.write(struct.pack("<IIdfIfI", 0x31445548, len(objs), t + t0, t, 1, m.f32(0xC22F50), 0))
                for n, b in objs:
                    f.write(struct.pack("<II", struct.unpack(">I", b[0xC:0x10])[0], n) + b[0x10:0x10 + 0x1C4])
            if t - t_log >= 1:
                t_log = t
                sp = [m.f32(0xC22E70 + o) for o in (0x14, 0x18, 0x24, 0x28)]
                print("%.1f s  speed %.4f frac %.4f +24 %.4f +28 %.4f flag20 %d flagEC %d  changes %d" %
                      (t, *sp, m.read(0xC22E90, 1)[0], m.read(0xC22F5C, 1)[0], n_frames))
            time.sleep(0.002)
    print("states seen per second: %.1f" % (n_frames / secs))


if __name__ == "__main__":
    main()
