#!/usr/bin/env python3
"""hlsl_stats.py - 4j.2: compila fuera del juego los shaders de SHADER_DUMP y mide cada uno.

Uso: python tools/hlsl_stats.py <dir> [--kind vs] [--flags 0] [--sub "viejo=>nuevo"]... [--dump dir] [--bytecode]

Una fila por archivo: bytes, lineas, constantes declaradas, instrucciones del bytecode (por tamano),
ms de D3DCompile con los mismos argumentos que el backend. --sub reescribe el texto antes de compilar,
para probar una idea sin tocar el generador. --bytecode imprime un resumen de hash del DXBC.
"""
import argparse
import ctypes
import glob
import hashlib
import os
import re
import time
from ctypes import POINTER, byref, c_char_p, c_size_t, c_uint, c_void_p

d3dc = ctypes.WinDLL('d3dcompiler_47.dll')


class Blob:
    """ID3DBlob: vtable [QI, AddRef, Release, GetBufferPointer, GetBufferSize]."""

    def __init__(self, p):
        self.p = p
        vt = ctypes.cast(p, POINTER(POINTER(c_void_p))).contents
        self._ptr = ctypes.WINFUNCTYPE(c_void_p, c_void_p)(vt[3])
        self._size = ctypes.WINFUNCTYPE(c_size_t, c_void_p)(vt[4])

    def bytes(self):
        return ctypes.string_at(self._ptr(self.p), self._size(self.p))


def compile_hlsl(src, target, flags):
    code, err = c_void_p(), c_void_p()
    t = time.perf_counter()
    hr = d3dc.D3DCompile(c_char_p(src), c_size_t(len(src)), b'x', None, None, b'main', target,
                         c_uint(flags), c_uint(0), byref(code), byref(err))
    ms = (time.perf_counter() - t) * 1000
    return hr & 0xFFFFFFFF, ms, Blob(code).bytes() if code else b'', Blob(err).bytes() if err else b''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--kind', default='vs')
    ap.add_argument('--flags', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--sub', action='append', default=[])
    ap.add_argument('--dump', default='')
    ap.add_argument('--only', default='')
    a = ap.parse_args()
    subs = [s.split('=>', 1) for s in a.sub]
    target = (a.kind + '_5_0').encode()
    total, worst, n = 0.0, 0.0, 0
    print('archivo                         bytes  lineas  const  saltos  dxbc     ms     hash')
    for path in sorted(glob.glob(os.path.join(a.dir, a.kind + '_*.hlsl'))):
        if a.only and a.only not in path:
            continue
        text = open(path, newline='').read()
        for old, new in subs:
            text = text.replace(old.replace('\\n', '\n'), new.replace('\\n', '\n'))
        src = text.encode()
        hr, ms, code, err = compile_hlsl(src, target, a.flags)
        if a.dump:
            with open(os.path.join(a.dump, os.path.basename(path)), 'w', newline='') as f:
                f.write(text)
        const = sum(int(m) for m in re.findall(r'float4 \w+\[(\d+)\];\s*\n', text.split('main(')[0])) + \
            len(re.findall(r'^\s+float4 vp_pos', text, re.M))
        flow = text.count('/* FLOW')
        h = hashlib.sha1(code).hexdigest()[:8] if code else '-'
        print(f'{os.path.basename(path):30s} {len(src):6d} {text.count(chr(10)):6d} {const:6d} {flow:6d} '
              f'{len(code):6d} {ms:7.1f}  {h}' + ('' if hr == 0 else f' FALLO {hr:08X} {err[:160]!r}'))
        total += ms
        worst = max(worst, ms)
        n += 1
    print(f'n={n} suma={total:.1f} ms max={worst:.1f} ms flags=0x{a.flags:X}')


if __name__ == '__main__':
    main()
