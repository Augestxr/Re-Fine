#!/usr/bin/env python3
"""task_list.py - 6d: lista las tareas vivas del juego en un volcado de MEM_DUMP.

Uso:  python tools/task_list.py <volcado.bin> [...]

Recorre la lista de tareas (0xBF2A4C, +4) y saca de cada una su clase por el RTTI, su grupo
(+4), su estado (+0xC, +0x10) y la marca de 0x4143F0 (+0x21: solo corre en pasos enteros de
velocidad). Solo mira memoria del juego: no interpreta ningun formato.
"""
import bisect
import struct
import sys


def load(path):
    with open(path, 'rb') as f:
        f.read(8)
        recs = {}
        while True:
            h = f.read(8)
            if len(h) < 8:
                break
            addr, size = struct.unpack('<II', h)
            recs[addr] = f.read(size)
    return recs


class Mem:
    def __init__(self, recs):
        self.recs = recs
        self.keys = sorted(recs)

    def bytes(self, a, n):
        i = bisect.bisect_right(self.keys, a) - 1
        if i < 0:
            return None
        base = self.keys[i]
        data = self.recs[base]
        return data[a - base:a - base + n] if a + n <= base + len(data) else None

    def u32(self, a):
        b = self.bytes(a, 4)
        return struct.unpack('>I', b)[0] if b else 0

    def cstr(self, a):
        b = self.bytes(a, 80) or b''
        return b.split(b'\0')[0].decode('ascii', 'replace')


def main():
    for path in sys.argv[1:]:
        m = Mem(load(path))
        work = m.u32(m.u32(0xBF2A4C))
        head = m.u32(work + 4)
        print(path)
        node, seen = m.u32(head), 0
        while node != head and seen < 300:
            task = m.u32(node + 8)
            vt = m.u32(task)
            name = m.cstr(m.u32(m.u32(vt - 4) + 4))
            flag = (m.bytes(task + 0x21, 1) or b'\0')[0]
            print(f'  {task:08X} grp {m.u32(task + 4)} est {m.u32(task + 0xC)}/{m.u32(task + 0x10)} '
                  f'flag {flag} vt {vt:08X} {name}')
            node = m.u32(node)
            seen += 1


if __name__ == '__main__':
    main()
