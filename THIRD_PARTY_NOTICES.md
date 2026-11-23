# Avisos de terceros

Código de otros proyectos adaptado en este repositorio (incluidos los parches de
`patches/`), con su licencia.

## ico-recomp

- Origen: https://github.com/feanor-noldorin/ico-recomp, commit
  `55b66b94d2f411019ce2ac4f0d8f9df363cd3ecd`, archivo `port/stubs.cpp`.
- Adaptado en `patches/0002-cellSpurs-LFQueuePushBody-ANY2ANY.patch`
  (`libs/spurs/cellSpurs.c` de ps3recomp): `_cellSpursLFQueuePushBody` (push en una
  `CellSpursLFQueue`, incluida la dirección ANY2ANY), el envío de la señal al consumidor y
  el registro de tasksets por instancia de SPURS.
- Cambios: C en lugar de C++ (`SRWLOCK` en lugar de `std::mutex`), códigos de error de
  RPCS3 escritos explícitamente, accesos directos a memoria dentro de la sección crítica de
  la línea, sin el perfilado de ICO, y un observador opcional para diagnóstico.

```
MIT License

Copyright (c) 2026 ico-recomp contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## JonathanDC64/ps3recomp (fork de ps3recomp usado por DemonsSoulsRecomp)

- Origen: https://github.com/JonathanDC64/ps3recomp, commit
  `d9e5559fffe85934ebf8c11d399a49d450e9cdac` (autor del commit: Jonathan Del Corpo).
- Adaptado en `patches/0103-spu-thread-bind-queue-and-receive-event.patch`: los handlers de
  `sys_spu_thread_bind_queue`/`unbind_queue` (`runtime/syscalls/lv2_register.c`),
  `sys_event_queue_receive_host` (`runtime/syscalls/sys_event.c`) y el servicio de los stops
  0x110/0x111 en `spu_stop` (`runtime/spu/spu_channels.c`).
- Cambios: solo la parte de SPU de ese commit (nada de SPURS ni diagnósticos); sin el contador
  `waiters`, que la cola de este árbol no tiene.
- Commit `ce105e69cbc8cd3615f31c395971f2acf3bb9548`: `patches/0111-cellPamf-real-PAMF-header-from-fork.patch`,
  `libs/codec/cellPamf.c`, `cellPamf.h` y su test (`libs/codec/tests/test_cellPamf.c`), con los stubs
  `spu_lockline_*_ea` del commit `f7f5e6db`. Cambios: solo la nota de atribución en la cabecera.
- Commit `ca23b8bdad212604a09f487e1a27a5d3b00e1462` ("gcm: vblanks from a clock thread of their
  own (GCM_VBLANK_THREAD)", Jonathan Del Corpo), adaptado en el hilo `vblank_clock` de
  `src/main.cpp` (el suyo está en `runtime/ppu/tests/boot_main.cpp`). Cambios: plazos absolutos
  `t0 + n·periodo` (el suyo suma un periodo truncado a cada tick), sin la variable
  `GCM_VBLANK_THREAD` ni `VBLANK_MHZ`. Su parte del runtime (pila de callbacks por hilo,
  `ppu_guest_call_set_stack`) fue `patches/0105` hasta que ps3recomp v0.18.0 trajo lo mismo
  (#489, `b3a75d98`); el parche se quitó el 2026-10-29.

```
MIT License

Copyright (c) 2026 Ned Heller / sp00nznet

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## FFmpeg

- Uso: cellVdec (MPEG-2), cellAdec y cellAtrac (ATRAC3plus) y swscale. Las DLL no van en este repositorio:
  las descarga `tools/fetch_ffmpeg.ps1`, y el programa las carga en ejecución con `LoadLibrary`.
- Versión: FFmpeg `n9.0.2-39-g7adfbe40dc`, build `win64-lgpl-shared-9.0` de
  https://github.com/BtbN/FFmpeg-Builds, etiqueta `autobuild-2026-10-31-12-59`.
- SHA-256 del zip: `e907b01fa4783913151237e37df2a10fa97d99a1598a021bf59d4ae62ffed4c0`.
- Licencia: LGPL versión 3 o posterior (`avcodec_license()`), sin `--enable-gpl` ni `--enable-nonfree`;
  el programa se niega a cargar una build con esas opciones. Texto en `work/ffmpeg/LICENSE.txt`.
- Cumplimiento: enlace dinámico; el usuario puede sustituir las DLL. Fuente: https://git.ffmpeg.org/ffmpeg.git,
  commit `7adfbe40dc`. Quien distribuya las DLL debe incluir la licencia y ofrecer esa fuente.

## SDL3

- Uso: cellPad, todos los mandos (`patches/0145`). La DLL no va en este repositorio: `tools/fetch_sdl3.ps1` la
  descarga, y CMake la copia junto al ejecutable.
- Versión: SDL `3.4.18`, paquete oficial `SDL3-devel-3.4.18-VC.zip` de https://github.com/libsdl-org/SDL, etiqueta
  `release-3.4.18`.
- SHA-256 del zip: `78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd`.
- Licencia: zlib. Texto en `work/sdl3/LICENSE.txt`. Quien distribuya la DLL no tiene otra obligación que no borrar
  ese aviso de las fuentes; aun así se incluye.

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## Vulkan-Headers y DXC (plan nativo, N1.0)

- Uso: compilar el backend Vulkan (cabeceras) y traducir los programas del juego de HLSL a SPIR-V (DXC). Nada va en
  este repositorio: `tools/fetch_vulkan.ps1` los descarga en `work/vulkan-headers` y `work/dxc`.
- Vulkan-Headers de https://github.com/KhronosGroup/Vulkan-Headers, etiqueta `vulkan-sdk-1.4.363.0`; SHA-256 del zip
  `f4be95220ff0ee0b1c620301ff4fe9ae48592d71f4c30f2d5c571351a153ae81`. Licencia Apache-2.0 o MIT (`LICENSE.md`).
- DXC de https://github.com/microsoft/DirectXShaderCompiler, release `v1.9.2609`, `dxc_2026_09_29.zip`; SHA-256
  `ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1`; `dxc.exe` y `dxcompiler.dll` firmados por
  Microsoft (el script lo comprueba). Licencias: University of Illinois/NCSA (LLVM), MIT y la de Microsoft para
  `dxil.dll` (`LICENSE-*.txt`, `LICENCE-MIT.txt`). Desde `0179` `dxcompiler.dll` se copia junto al exe (el backend Vulkan
  compila ahí los programas del juego a SPIR-V): quien lo distribuya lleva con él `LICENSE-LLVM.txt` y
  `LICENCE-MIT.txt` (el instalador, hito 8).

## at3_standalone

`third_party/at3_standalone` de ps3recomp (LGPL-2.1-or-later) sigue en el submódulo, pero desde `patches/0125`
**no se compila ni se enlaza** en Re:Fine.


## RmlUi, FreeType e Inter (menú del port, 6b+6c)

Ninguno va en este repositorio salvo sus licencias: `tools/fetch_rmlui.ps1` descarga las fuentes (SHA-256
comprobado) en `work/rmlui/`; CMake compila RmlUi y FreeType estáticos con el compilador del port y copia las
fuentes tipográficas a `ui/fonts/` junto al ejecutable. El renderer D3D12 (`src/port_menu/rml_d3d12.cpp`) es
propio; no usa el de RmlUi.

| Componente | Versión | Origen | SHA-256 del paquete | Licencia (texto) |
|---|---|---|---|---|
| RmlUi | 6.3 (etiqueta `6.3`) | https://github.com/mikke89/RmlUi | `bfb340660e4bce38d4211cffc836cfbe1be676aa36ef9cc48e7df3855ee74fe3` | MIT (`third_party/rmlui/LICENSE.txt`) |
| FreeType | 2.14.3 | https://download.savannah.gnu.org/releases/freetype/ | `e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1` | FreeType License, FTL (`third_party/freetype/FTL.TXT`); se elige la FTL de su licencia doble FTL/GPLv2 |
| Inter | 4.1 (Regular y SemiBold, `extras/ttf`) | https://github.com/rsms/inter | `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e` | SIL OFL 1.1 (`third_party/inter/LICENSE.txt`) |
| Barlow Condensed | SemiBold y Bold, google/fonts `60824dce48f7` | https://github.com/jpt/barlow | `7b619d14bc23...` (SemiBold), `e476562ec9c1...` (Bold), completos en `tools/fetch_rmlui.ps1` | SIL OFL 1.1 (`third_party/barlow/OFL.txt`) |
| Dela Gothic One | Regular, google/fonts `60824dce48f7` | https://github.com/syakuzen/DelaGothic | `4ff87a0965f1...`, completo en `tools/fetch_rmlui.ps1` | SIL OFL 1.1 (`third_party/delagothicone/OFL.txt`) |
| Chakra Petch | Medium, SemiBold y Bold Italic, google/fonts `60824dce48f7` | https://github.com/m4rc1e/Chakra-Petch | `d480f4f97405...`, `45264de3204d...`, `bf012ce1894f...`, completos en `tools/fetch_rmlui.ps1` | SIL OFL 1.1 (`third_party/chakrapetch/OFL.txt`) |
| IBM Plex Mono | Medium, google/fonts `60824dce48f7` | https://github.com/IBM/plex | `a9b4c49bb299...`, completo en `tools/fetch_rmlui.ps1` | SIL OFL 1.1 (`third_party/ibmplexmono/OFL.txt`) |

La FTL pide mencionar en la documentación del producto que usa FreeType: "Portions of this software are
copyright © The FreeType Project (www.freetype.org). All rights reserved."

Los degradados del menú (2026-11-11, `src/port_menu/rml_d3d12.cpp`) leen los parámetros de RmlUi como su renderer de referencia `Backends/RmlUi_Renderer_GL3.cpp` (RmlUi 6.3, MIT, misma licencia que arriba) y siguen la fórmula de su shader de degradado; el código D3D12/HLSL es propio.

## stb_image v2.30 (Sean Barrett) — dominio público / MIT
`third_party/stb/stb_image.h`, SHA-256 `1f8c1b6b408f26e3b20cbfbbd4758afb3dc9b837ff1e17c258928f406148a87c` (copia del que lleva ps3recomp en `libs/codec/`). Se usa solo para PNG: las dos imágenes de la pantalla de inicio se leen del disco del usuario en ejecución; ninguna va al repo.

## Sharpening (parche 0165)
El filtro de nitidez del present sigue el algoritmo CAS de AMD FidelityFX (MIT, Copyright (c) Advanced Micro Devices, Inc.); la implementacion HLSL es propia, no copia su codigo.

## Runtime de Visual C++ (Microsoft)
`msvcp140.dll`, `vcruntime140.dll` y `vcruntime140_1.dll` (Microsoft Visual C++ Redistributable, x64) van junto a `ReFine.exe` para que arranque en un PC sin ese paquete instalado. Son redistribuibles según los términos de redistribución de Visual Studio / Build Tools (archivos `.CRT`); no se modifican.

## DXC en ejecución
`dxcompiler.dll` (DirectX Shader Compiler, `v1.9.2609`, mismo SHA-256 del zip que arriba) acompaña al exe de la versión privada: el backend Vulkan lo carga para traducir los shaders del juego a SPIR-V. Licencias en `licenses\DXC-*.txt` (LLVM Apache-2.0 con excepciones, Microsoft, MIT). `dxil.dll` y `dxc.exe` no se reparten: el exe no los usa.
