#!/usr/bin/env python3
"""export_patch.py - Escribe patches/NNNN-x.patch con lo que cambia entre dos copias.

Uso:  python tools/export_patch.py patches/NNNN-x.patch RUTA_EN_SUBMODULO=COPIA_ANTERIOR ...

El submodulo tiene CRLF en el arbol de trabajo y LF en el indice: `git diff` a secas
compara contra el commit fijado y mezcla todos los parches. Aqui cada archivo se compara
con su copia de antes de editar, los dos con LF, y las rutas salen como las del
submodulo. No toca la config de git. Una copia vacia ('-') es un archivo nuevo.
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUB = os.path.join(ROOT, 'external', 'ps3recomp')


def lf(path):
    with open(path, 'rb') as f:
        return f.read().replace(b'\r\n', b'\n')


out = sys.argv[1]
parts = []
with tempfile.TemporaryDirectory() as tmp:
    for pair in sys.argv[2:]:
        rel, old = pair.split('=', 1)
        a, b = os.path.join(tmp, 'a'), os.path.join(tmp, 'b')
        with open(a, 'wb') as f:
            f.write(b'' if old == '-' else lf(old))
        with open(b, 'wb') as f:
            f.write(lf(os.path.join(SUB, rel)))
        r = subprocess.run(['git', '-c', 'core.autocrlf=false', 'diff', '--no-index', '--no-color', a, b],
                           capture_output=True)
        if r.returncode not in (0, 1):
            sys.exit(r.stderr.decode())
        d = r.stdout.decode('utf-8')
        d = re.sub(r'^diff --git .*$', f'diff --git a/{rel} b/{rel}', d, count=1, flags=re.M)
        d = re.sub(r'^--- .*$', '--- /dev/null' if old == '-' else f'--- a/{rel}', d, count=1, flags=re.M)
        d = re.sub(r'^\+\+\+ .*$', f'+++ b/{rel}', d, count=1, flags=re.M)
        if old == '-':
            d = re.sub(r'^index .*$', 'new file mode 100644', d, count=1, flags=re.M)
        else:
            d = re.sub(r'^index .*\n', '', d, count=1, flags=re.M)
        parts.append(d)
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write(''.join(parts))
print(out, sum(p.count('\n') for p in parts), 'lineas')
