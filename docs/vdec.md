# Diseño del 4c.3: cellVdec y cellAdec

Fuentes: RPCS3 `1f59f0f2` (copias en `work/ref/rpcs3src/vdec/`, sin versionar), `work/rpcs3/primer_arranque.log`, el decompilado del reproductor (`work/4c3/decomp1.log`, `decomp2.log`) y `libs/codec/` de ps3recomp v0.18.0.

**Licencia:** RPCS3 es GPL-2.0. Se copia el comportamiento, nunca el código. FFmpeg va como DLL aparte ([decisions.md](decisions.md)).

## Qué hace el juego con `adv.pam`

Primera película, hasta `cellVdecEndSeq` (línea 2 060 058 del log). Las cuentas del 4c.0 mezclaban las dos películas.

| Hilo (RPCS3) | Llamada | Veces | LR |
|---|---|---|---|
| `main_thread` | `cellVdecQueryAttrEx`, `OpenEx`, `StartSeq` | 1 | `0x83d294`, `0x83d328`, `0x83d33c` |
| `main_thread` | `cellAdecQueryAttr`, `Open`, `StartSeq` | 1 | `0x83d500`, `0x83d570`, `0x83d58c` |
| `0x100000d` (vídeo) | `cellVdecDecodeAu` | **2805** | `0x83abc8` |
| `0x100000e` (imagen) | `GetPicItem` + `GetPicture` | **2804** cada una | `0x83ee7c`, `0x83ef10` |
| `0x1000010` (audio) | `cellAdecDecodeAu` | **2196** | `0x83b860` |
| `0x1000011` (PCM) | `GetPcmItem` | **2196** | `0x83addc` |
| `0x1000011` | `GetPcm(NULL)` / `GetPcm(buf)` | **1** / **2195** | `0x83ae04` / `0x83aecc` |
| `main_thread` | `cellAdecEndSeq`, `Close`, `cellVdecEndSeq`, `Close` | 1 | `0x83c5cc`, `0x83c5d8`, `0x83c7e4`, `0x83c7f0` |
| HLE Video Decoder | `AUDONE` / `PICOUT` / `SEQDONE` | 2805 / 2804 / 1 | — |
| HLE ATRAC3plus | `AUDONE` / `PCMOUT` | 2196 / 2196 | — |

**2804 imágenes de 2805 AU:** MPEG-2 con B saca cada imagen una AU más tarde, y `Close` llega justo tras `EndSeq` y aborta el vaciado (`AU decoding: aborted`, `cmd_id=2806`). No es un fallo.

## Formatos que pide el juego

| Dato | Valor | Evidencia |
|---|---|---|
| Códec de vídeo | `codecType=0` (MPEG-2), perfil `3` (`> MP_ML`) | 4c.2 |
| Imagen | **RGBA32** (`formatType=1`), `alpha=255`, **`colorMatrixType=1` (BT.709)**, 1280×720, paso `w*4` | `cellVdecGetPictureExt: ... formatType=1, out_f=26, use_alpha=1, alpha=255, colorMatrixType=1` |
| Salida del juego | un búfer propio por imagen, separados `0x384010` (= `1280*720*4 + 0x10`) | `outBuff` de las 3 primeras llamadas |
| Códec de audio | `audioCodecType=13` (`ATRACX_2CH`) | el valor 13 se escribe en el campo 0x142 (en palabras) de la estructura de parámetros, en `0x83cb68`; 4c.2 |
| Parámetro de `StartSeq` | `bw_pcm=0x84` (float), `downmix=0`, `au_includes_ats_hdr=1`; tasa y canales salen de la cabecera ATS de la primera AU | decompilado; `set_config_info(48000, 2, 0x2b0)` tras la primera `DecodeAu` |
| AU de audio | 0x2B8 B = 8 de cabecera ATS + 0x2B0 de trama | log |
| PCM | **f32 big-endian entrelazado**, 2048 muestras × 2 canales = **0x4000 B** | `pcmSize=0x4000` ×2195 |
| Primera trama | **`pcmSize=0`** y silencio: se descarta la primera de cada secuencia | `pcmSize=0x0` ×1; el juego responde con `GetPcm(NULL)` |

### Lo que el juego lee

| Función del juego | Qué hace |
|---|---|
| Callback de vídeo `0x83a76c` (OPD `0xbd4270`) | `AUDONE`: en vuelo −1, AU por soltar +1. `PICOUT`: imágenes pendientes +1. Siempre despierta el cond `+0x3cc` |
| Alimentador `0x83aa50` | Una AU en vuelo como máximo. Suelta con `ReleaseAu`, pide con `GetAuEx`, entrega con `DecodeAu`. Con `BUSY` (`0x80610103`) duerme y reintenta |
| Imagen `0x83edec` | `GetPicItem`; mira `status` (+0x40) contra `CELL_VDEC_ERROR_PIC`, `attr` (+0x44, 1 = saltada), y lee ancho y alto en `picInfo` (+0x48). **El tiempo de cada imagen lo calcula con su número de orden y la tasa: no lee `pts`** |
| Callback de audio `0x83a660` (OPD `0xbd4268`) | `AUDONE`: en vuelo −1, AU por soltar +1. `PCMOUT`: PCM pendientes +1 |
| PCM `0x83ad2c` | `GetPcmItem`; lee `size` (+0x0C). Si es 0, `GetPcm(NULL)`. Si no, `GetPcm(buf)` y empuja el PCM a un anillo propio de 0x5DC00 B |

Conclusión: el ritmo lo lleva el propio juego con sus contadores. No necesita que el decodificador lo frene, salvo el `BUSY` y la espera de salida del audio.

## Lo que hay en ps3recomp v0.18.0

| Pieza | Estado | Qué se hace |
|---|---|---|
| `vdec_ffmpeg.c`, `ffmpeg_runtime.c` | sirven: MPEG-2 con carga en ejecución. Solo `avcodec` y `avutil`, sin `configuration()` ni `swscale` | se amplían |
| `cellVdec.c` | decodifica y llama a los callbacks **en el hilo que llama a `DecodeAu`**; sin FFmpeg, imagen negra con éxito falso | hilo propio; sin FFmpeg, error real |
| `cellVdec.c` `vdec_idle_thread` | hilo de sondeo cada 50 ms que arregla un juego que no llama a `EndSeq` | fuera del camino de F: `EndSeq` vacía de verdad. Se quita con los hilos por decodificador |
| `cellAdec.c` | stub: silencio de 1024 muestras en el hilo que llama. Cuando se escribió el comentario de 0x4000 no existía la trama real | se reescribe |
| Códigos | `CELL_ADEC_TYPE_*` de `cellAdec.h` no son los del SDK (`ATRACX_2CH` es 13) | corregidos los que se usan |
| `at3_standalone` | LGPL enlazado estático; solo lo usa `cellAtrac`, que F no importa | se quita si FFmpeg da el mismo resultado |

## Modelo de hilos

Como `dmux_pamf`: un hilo host por decodificador, callbacks por `ps3_invoke_guest`, espera nativa con `SRWLOCK` y `CONDITION_VARIABLE`, sin sondeo.

| Evento | Quién | Qué hace |
|---|---|---|
| `Open` | juego | reserva hueco, crea el hilo. Sin FFmpeg: `CELL_*_ERROR_FATAL`, aviso en stderr, sin hilo |
| `StartSeq` | juego → hilo | encola; el hilo vacía el códec y la cola |
| `DecodeAu` | juego | copia la info, encola y vuelve al momento. `BUSY` si hay 4 comandos sin acabar (vídeo) o 3 PCM en uso (audio) |
| decodificar | hilo | AU → imagen o trama |
| `AUDONE`, `PICOUT`, `PCMOUT`, `SEQDONE` | hilo | en el orden de RPCS3: `AUDONE` antes que `PICOUT`/`PCMOUT` |
| `EndSeq` | juego → hilo | vídeo: comando de vaciado y comando de fin. Audio: comando de fin |
| `Close` | juego | marca parada, despierta, `join` sin tener el lock |

### Vídeo

| Tema | Decisión | Por qué |
|---|---|---|
| Cola | hasta 61 imágenes; llena, el hilo espera en una condición | `out_max = 60` en RPCS3, que aquí sondea cada 10 ms. El juego no debería llegar: se mide |
| `pts`/`dts` | AU con PTS: se usa. Sin él: anterior + 3003 (`next_pts`), igual con `dts` | `Got picture ... pts=0x16b4b[0x8000...]`, salen de sumar `0xBBB`. Se compara la lista entera |
| `GetPicItem` | cada llamada da la **siguiente imagen sin item** (`pic_item_received`), no la cabeza de la cola | comportamiento de RPCS3. El `picItem` va en la memoria del decodificador, en anillo |
| `startAddr`, `size` del item | `0x123`, `align(1382400, 128)` | valores de RPCS3; el juego no los lee |
| Conversión | swscale **en el hilo vdec**, a la imagen ya en cola, cuando se conoce el formato; `GetPicture` solo copia. Las imágenes que llegan antes de la primera `GetPicture`, o con otro formato, se convierten en esa llamada | CLAUDE.md: no cargar el hilo del juego. El primer `GetPicture` no puede saber el formato de antemano |
| Matriz | **se honra `colorMatrixType`** (BT.709 aquí) | RPCS3 lo deja como `TODO: color matrix` y usa la de swscale por defecto (601). ASSUMPTION: la consola aplica la matriz pedida; RPCS3 no sirve de oráculo aquí. Se mide la diferencia en las capturas |
| Filtro | `SWS_POINT`, como RPCS3 | sin escalado |
| Hilos de FFmpeg | `thread_count=1` | una imagen por AU, determinista; si la CPU medida lo pide, se revisa |

### Audio

| Tema | Decisión | Por qué |
|---|---|---|
| Salida | **una** trama viva a la vez: antes de decodificar, el hilo espera a que `GetPcm` suelte la anterior | `output_locked` en RPCS3. Es el freno real del audio |
| Identificadores PCM | 3 | `_CellAdecCoreOpGetPcmHandleNum_atracx` |
| Memoria | item, bsi y PCM en la memoria de trabajo del juego (`CellAdecResource.startAddr`), no en la ventana HLE: 0x4000 B no caben en los 0x3F00 de `ADEC_SCRATCH` | `pcmAddr=*0x32d12480`, dentro de esa memoria |
| Cabecera ATS | se lee en la primera AU de la secuencia; sync `0x0FD0` o `CELL_ADEC_ERROR_ATX_ATSHDR`-equivalente | `parse_ats_header` |
| Primera trama | decodificada, ceros y `size=0` | `skip_next_frame` |
| Salida numérica | saturar a ±1 y el 1 positivo a `0x3F7FFFFF`, big-endian | comportamiento de RPCS3 |
| PTS | sin PTS: anterior + 3840 (a 48 kHz) | `correct_pts_value` |
| Otros códecs | `CELL_ADEC_ERROR_FATAL` en `Open` | F solo usa ATRAC3plus; sin éxito falso |

## FFmpeg

| Punto | Decisión |
|---|---|
| Versión | serie 9.0 (`avcodec-63`, la de las cabeceras que esperan los comentarios de ps3recomp) |
| Origen | build LGPL shared de BtbN/FFmpeg-Builds, enlazada desde ffmpeg.org para Windows, con fecha y hash fijos en `tools/fetch_ffmpeg.ps1`. Compilar aquí pide `make` y `nasm`, que no están |
| DLL | `avcodec`, `avutil`, `swscale` y sus dependencias, en `build/` por el script; las cabeceras, en `work/ffmpeg/`. Nada en git |
| Códecs | `mpeg2video` y `atrac3p`: uno solo cierra la deuda de `at3_standalone` |
| Carga | `LoadLibrary`, como hasta ahora. Al arrancar se lee `avcodec_configuration()` y se para si sale `--enable-gpl` o `--enable-nonfree` |
| Falta la DLL | `Open` falla con un error real y el juego se salta la película |

## Riesgos

| Riesgo | Mitigación |
|---|---|
| El callback corre en un hilo con `thread_id` centinela | ya probado en dmux: 4123 `sys_cond_signal` sin fallo |
| `Close` con el hilo dentro de un callback o esperando salida | parada antes del `join`; toda espera mira `quit` |
| El audio del juego deja de sonar por una suma mal entendida (`pcmHandle`, `bsiInfo`) | se compara PCM con el volcado de RPCS3 |
| Dos películas seguidas en la misma ranura | `Close` limpia y `StartSeq` vuelve a empezar; se ve a los ~6 min |
| CPU del hilo vdec | se mide con `-ThreadCpu` |

## Resultado (4c.3)

| Riesgo | ¿Se dio? | Evidencia |
|---|---|---|
| `thread_id` centinela | no | dos películas sin fallo |
| `Close` con el hilo esperando | no | dos cierres limpios; `Close` tras `EndSeq` da 2804 imágenes, como RPCS3 |
| Audio del juego | no | PCM con correlación 1,00000 |
| Dos películas en la misma ranura | no | segunda película con las mismas cuentas |
| Cola de imágenes | **sí llega a 61** | el juego no frena antes: por eso se convierten solo las 4 próximas |
| No previsto: textura leída durante la copia | **sí**, cerrado en el 4c.5 | D14, [STATUS.md](STATUS.md) 4c.5: 65,14 % → 0,00 % con `0128` |

Cambió sobre el diseño: la conversión es a 4 imágenes vista, y `dts` toma el de la AU que se está decodificando cuando sale la imagen (`pkt_dts`), no solo el primero.

