#!/usr/bin/env python3
"""func_hooks.py - deja sitio para ganchos en funciones del juego que se llaman directamente.

Uso:  python tools/func_hooks.py <work/recompiled> <config/func_hooks.json>

ppu_register_function solo alcanza las llamadas indirectas (vtables, OPD): una llamada directa es un `func_X(ctx)` de
C. Aqui, tras el lift, la definicion de cada funcion de la lista pasa a llamarse func_X__lifted y src/ da la func_X
que llaman todas (y que puede llamar a func_X__lifted). Se puede repetir sobre una salida ya cambiada. Si falta la
func_X de src/, el enlazado falla: no hay forma de olvidarla.
"""
import glob
import json
import os
import re
import sys

out, spec = sys.argv[1], sys.argv[2]
hooks = [h['addr'] for h in json.load(open(spec, encoding='utf-8'))['hooks']]
names = ['func_%08X' % int(a, 16) for a in hooks]
todo = set(names)
for path in sorted(glob.glob(os.path.join(out, 'ppu_recomp*.cpp'))):
    with open(path, encoding='utf-8', newline='') as f:
        s = f.read()
    new = s
    for n in sorted(todo):
        new, k = re.subn(r'^void %s\(ppu_context\* ctx\) \{' % n, 'void %s__lifted(ppu_context* ctx) {' % n, new, flags=re.M)
        if k == 1 or ('void %s__lifted(ppu_context* ctx) {' % n) in new:
            todo.discard(n)
    if new != s:
        with open(path, 'w', encoding='utf-8', newline='') as f:
            f.write(new)
h = os.path.join(out, 'ppu_recomp.h')
with open(h, encoding='utf-8', newline='') as f:
    s = f.read()
new = s
for n in names:
    if n + '__lifted' not in new:
        new = new.replace('void %s(ppu_context* ctx);' % n, 'void %s(ppu_context* ctx); void %s__lifted(ppu_context* ctx);' % (n, n))
if new != s:
    with open(h, 'w', encoding='utf-8', newline='') as f:
        f.write(new)
if todo:
    sys.exit('func_hooks: no encontradas: ' + ' '.join(sorted(todo)))
print('func_hooks: %d funcion(es) con gancho: %s' % (len(names), ' '.join(names)))
