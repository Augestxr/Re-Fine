"""sheet.py - 6d.13: hoja de capturas (PNG o BMP) en rejilla, con ffmpeg. Uso: python -I tools/sheet.py salida.png cols ancho img1 img2 ..."""
import subprocess
import sys

out, cols, w, imgs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4:]
h = w * 9 // 16
args = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y']
for i in imgs:
    args += ['-i', i]
rows = (len(imgs) + cols - 1) // cols
f = ''.join('[%d:v]scale=%d:%d[v%d];' % (k, w, h, k) for k in range(len(imgs)))
lay = '|'.join('%d_%d' % ((k % cols) * w, (k // cols) * h) for k in range(len(imgs)))
f += ''.join('[v%d]' % k for k in range(len(imgs))) + 'xstack=inputs=%d:layout=%s:fill=black' % (len(imgs), lay)
if len(imgs) == 1:
    f = '[0:v]scale=%d:%d' % (w, h)
subprocess.run(args + ['-filter_complex', f, '-frames:v', '1', out], check=True)
print(out)
