<div align="center">

# Re:Fine

**Port nativo para PC de *Hatsune Miku: Project DIVA F* (PS3), hecho por recompilación estática.**
<br>El código del propio juego, traducido a C/C++ y corriendo nativo en Windows, a cualquier fps.

[![Plataforma](https://img.shields.io/badge/plataforma-Windows%2010%2F11%20x64-0078D6?logo=windows&logoColor=white)](#-requisitos)
[![Renderizador](https://img.shields.io/badge/renderizador-Vulkan%20%7C%20D3D12-AC162C?logo=vulkan&logoColor=white)](#-qué-ofrece)
[![FPS](https://img.shields.io/badge/fps-de%2030%20a%201000%20%2B%20sin%20l%C3%ADmite-39C5BB)](#-qué-ofrece)
[![Hecho con](https://img.shields.io/badge/hecho%20con-ps3recomp-555555)](https://github.com/sp00nznet/ps3recomp)
[![Lenguaje](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](src/)
[![Licencia](https://img.shields.io/badge/licencia-MIT-green)](LICENSE)
[![Estado](https://img.shields.io/badge/estado-en%20desarrollo-orange)](#-problemas-conocidos)

[English](README.md) · **Español**

</div>

> [!IMPORTANT]
> **Proyecto de fans. Sin relación con SEGA ni con Crypton Future Media, ni respaldado o patrocinado por ellas.**
> Necesitas **tu propia copia del juego, volcada legalmente**. **Aquí no se incluye ni se reparte ningún
> archivo del juego, firmware, clave ni recurso**, y nunca se descarga nada de eso. Las marcas son de sus
> dueños y solo se usan para nombrar el juego con el que funciona este port.

## 💡 ¿Qué es?

Re:Fine toma el ejecutable de PS3 de *Project DIVA F* (BLUS31319, EE. UU., versión 01.00) y lo traduce
por adelantado a C/C++ con [ps3recomp](https://github.com/sp00nznet/ps3recomp): tanto el código del PPU
como los programas de los SPU. El resultado se enlaza con un runtime que hace las veces de la PS3
(gráficos, audio, mandos, archivos, hilos) y se compila en un `ReFine.exe` normal.

**No es un emulador** ni **un remake**: lo que corre es el código original de SEGA, en tu PC, compilado
a partir de tu propia copia. Este repositorio solo tiene las herramientas, los parches y el runtime que
lo hacen posible.

## ✨ Qué ofrece

- 🎞️ **Cualquier fps.** 60 fps de serie; el menú ofrece de 30 a 360 y sin límite, y `refine.ini` acepta
  de 30 a 1000, `unlimited` o `display` (los del monitor principal). El juego va a su velocidad normal y
  el juicio de notas y el audio siguen atados al reloj de audio.
- 🎯 **A 30 fps es el original.** A 30 fps no se cambia nada: es el juego tal como salió.
- 🖥️ **Renderizador Vulkan** con Direct3D 12 de respaldo; altura de render hasta 2160p, filtrado
  anisótropo, nitidez y reescalado.
- ⚙️ **Menú del port con F1** para vídeo, audio, controles e interfaz. La configuración está en
  `%LOCALAPPDATA%\ReFine\refine.ini` y los logs en `%LOCALAPPDATA%\ReFine\logs`.
- 🎮 Teclado, ratón en los menús del juego y mandos con SDL3.
- 🧰 **Extras opcionales** (apagados de serie): desfase del juicio, timing display, saltar la película de
  inicio, copias automáticas de la partida, capturas con F12 y silencio en segundo plano. Discord Rich
  Presence es opcional.
- 🔁 Cada arreglo para fps altos tiene un interruptor `REFINE_*` que lo apaga y devuelve el comportamiento original.

## 📋 Requisitos

- Windows 10 u 11, x64.
- Una GPU con Vulkan (preferido) o Direct3D 12.
- **Tu propia copia** del disco (BLUS31319, EE. UU., v01.00, sin actualización), extraída en una carpeta
  que contenga `PS3_GAME`.
- [RPCS3](https://rpcs3.net/) con su firmware instalado, que se usa **una vez** para descifrar `EBOOT.BIN`
  (*Utilities → Decrypt PS3 Binaries*). Re:Fine no tiene código de descifrado ni claves.
- Unos 10 GB libres en disco, y permisos de administrador una vez si hay que instalar las Visual Studio
  Build Tools (carga de trabajo de C++). *Son estimaciones, no medidas en un PC limpio.*
- Tiempo: el juego se traduce y se compila **en tu PC**. En el PC de desarrollo solo la compilación tarda
  unos 6 minutos con todos los hilos; en máquinas más modestas, más.

## 🚀 Instalación

1. Consigue el instalador, `ReFineSetup-<versión>.exe`. Todavía no hay ninguna release publicada aquí, así
   que por ahora genéralo tú con `tools\installer\build_setup.ps1 -Version <x.y.z>`
   (ver [docs/BUILD.md](docs/BUILD.md)).
2. Ábrelo y elige la carpeta del juego. Si faltan LLVM, Python o las Build Tools, el instalador ofrece
   bajarlos de sus fuentes oficiales y comprueba sus hashes y firmas.
3. Espera a que terminen la traducción y la compilación. El resultado es `ReFine.exe` en la carpeta de instalación.
4. A jugar. Pulsa **F1** cuando quieras para abrir el menú del port.

### Compilar desde el código

```powershell
git clone --recurse-submodules https://github.com/Augestxr/Re-Fine.git
cd Re-Fine
```

Después sigue [docs/BUILD.md](docs/BUILD.md): aplica los parches a ps3recomp, baja las bibliotecas
fijadas, pon tu ejecutable descifrado en `game\EBOOT.elf` y corre los scripts de lift y de compilación.
El instalador se explica en [docs/INSTALLER.md](docs/INSTALLER.md).

## 🐞 Problemas conocidos

- **Física del pelo y de la ropa.** Por encima de 30 fps, los mechones pequeños (flequillo) y la física
  del pecho siguen avanzando 30 veces por segundo, con mezcla entre medias. La ropa no se ha mirado en
  todas las canciones.
- **Ritmo de frames.** A 60 fps algunos frames aún llegan desiguales. Con V-Sync, unos fps que no dividen
  los del monitor (120 en una pantalla de 180 Hz) pueden verse irregulares: usa **Match display**.
- **No se han revisado todas las canciones** a fps altos; el censo del resto está en curso.
- **El modo `display`** lee solo el monitor principal; con la ventana en otro monitor de otra frecuencia
  puede elegir un valor equivocado.
- **Selector F / F2nd.** El menú ya cambia entre F y F2nd, pero F2nd aún no está portado y sale "port pending".
- **Mandos.** La vibración, la reasignación y las estrellas dobles con un mando físico no están del todo comprobadas.
- **Probado en pocos PC.** Sin probar en PC con poca RAM ni con rutas de instalación con caracteres no
  ASCII. Las máquinas sin un driver de Vulkan reciente pasan a D3D12.

## 🗂️ Estructura del repositorio

| Ruta | Qué es |
|---|---|
| [`src/`](src/) | el runtime propio del port: ventana, entrada, audio, enlace con el renderizador, tiempo, menú |
| [`patches/`](patches/) | parches sobre ps3recomp (fijado en `external/`), numerados en orden |
| [`config/`](config/) | configuración del lifter: ganchos de funciones y parches de instrucciones (por dirección y hash) |
| [`tools/`](tools/) | scripts de compilación, lift, pruebas y medida, y el instalador |
| [`third_party/`](third_party/) | bibliotecas incluidas con sus licencias |
| [`docs/`](docs/) | cómo compilar, el instalador y notas sobre la decodificación de vídeo |

## 🤖 sobre la inteligencia artificial

Sí, en este proyecto se usa inteligencia artificial, y a su autor le da exactamente igual lo que la gente
opine de eso. Hay quien solo critica: quizá debería intentar hacer proyectos, porque nunca aporta nada a
la comunidad y solo critica. A criticar se apunta cualquiera; lo difícil es construir algo. Y si alguien
cree que puede hacerlo mejor, el código está abierto.

## 🙏 Créditos y licencia

Nuestro código va con la [licencia MIT](LICENSE). El código de terceros y sus licencias están en
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) (MIT): el recompilador estático y el runtime sobre
  los que se construye el port.
- [ico-recomp](https://github.com/feanor-noldorin/ico-recomp) (MIT) y el
  [fork de ps3recomp de JonathanDC64](https://github.com/JonathanDC64/ps3recomp): arreglos e ideas que adaptamos.
- [SDL3](https://github.com/libsdl-org/SDL) (zlib), [FFmpeg](https://ffmpeg.org/) (LGPL-3.0, DLL aparte),
  [RmlUi](https://github.com/mikke89/RmlUi) (MIT), FreeType (FTL), DirectX Shader Compiler,
  Vulkan-Headers y stb.
- Fuentes: Inter, Barlow, Chakra Petch, Delagothic e IBM Plex Mono (SIL OFL 1.1).
- [RPCS3](https://rpcs3.net/) (GPL-2.0) solo se usa como referencia externa de comportamiento. No se
  incluye ni se enlaza, y aquí no hay código copiado de RPCS3.

Gracias a los proyectos ps3recomp, ico-recomp y RPCS3, y a todos los que mantienen vivo este juego. 💙
