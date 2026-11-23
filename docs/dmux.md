# Diseño del 4c.2: cellDmux

Diseño aprobado e implementado en `patches/0113` (opción A); resultado en [STATUS.md](STATUS.md) 4c.2 y riesgos al final. Fuentes: RPCS3 `1f59f0f2` (copias en `work/ref/rpcs3src/dmux/`, no
versionadas), `work/rpcs3/primer_arranque.log` y el código traducido. Rutas de RPCS3 bajo
`rpcs3/Emu/Cell/Modules/`.

**Licencia**: RPCS3 es GPL-2.0 y ps3recomp y nuestros parches son MIT. De RPCS3 se toma el comportamiento, no
el código: nada de copiar ni traducir funciones. ASSUMPTION: el formato y la semántica no heredan la licencia;
es el mismo criterio del fork en cellPamf ("reimplemented, not copied").

## Qué hace el juego con cellDmux (`adv.pam`, primera reproducción)

| Hilo (RPCS3) | Llamadas | Veces | Sitio (LR) |
|---|---|---|---|
| `main_thread` | `QueryAttr`, `Open`, `QueryEsAttr` y `EnableEs` por ES; al final `DisableEs` ×2, `Close` | 1 y 2 | `0x83ce10`, `0x83ce88`, `0x83cf78`, `0x83cfcc`, `0x83d0bc`, `0x83d110`, `0x83c97c`, `0x83c9b8`, `0x83c9f4` |
| `0x1000013` (alimentador) | `SetStream` de 0x80000 B | 467 | `0x83b16c` |
| `0x1000013` | `FlushEs` | 132 (66 por ES, de 1:53,06 a 1:55,22) | `0x83b1d8`, `0x83b1e4` |
| `0x100000d` (vídeo) | `GetAuEx` + `ReleaseAu` | 2 805 cada una (= 93,6 s × 29,97) | `0x83acfc`, `0x83ab14` |
| `0x1000010` (audio) | `GetAuEx` + `ReleaseAu` | 2 196 cada una (= 93,6 s × 48 000 / 2 048) | `0x83b980`, `0x83b7c4` |
| HLE PAMF demuxer | callbacks al juego: AU encontrada, fin de demux, flush hecho | 5 001, 467, 132 | — |

Notas: `0x1000012` lee el archivo a 0x10000 B por lectura. El juego carga `libdmux` y `libdmuxpamf` con
`cellSysmoduleLoadModule(0x2d, 0x2a)`. El mismo patrón se repite a los 3:58 con el vídeo que sigue a la demo.

## Cómo lo hace RPCS3

| Pieza | Archivo | Qué hace |
|---|---|---|
| Capa pública | `cellDmux.cpp` | valida, lleva la cola de AU por ES y traduce errores (`get_error`, `:32`: 0 OK; 1 FATAL; 2-5 ARG; resto FATAL). `pop_au` (`:1038`) es `GetAu`, `PeekAu` y sus `Ex`; `CellDmuxAuInfoEx` es `CellDmuxAuInfo` (`cellDmux.h:167`) |
| AU encontrada | `cellDmux.cpp:147` `notify_es_au_found` | copia la info a la cola; **si está llena devuelve 1** (`:190`) y llama al `cbFunc` de la ES con `AU_FOUND` |
| Hilo PPU | `cellDmuxPamf.cpp:1294` `DmuxPamfContext::exec` | saca eventos de la "SPU", llama a los callbacks y **espera en un cond** mientras haya una cola llena (`wait_au_released_or_stream_reset`, `:1208`) |
| Hilo "SPU" | `cellDmuxPamf.cpp:929` | hace de la tarea SPURS: parsea (`process_next_pack`, `:664`) y escribe AU. Se comunica con colas: 4 + 2·ES eventos |
| Prioridad y pila | log | `HLE PAMF demuxer`: prio 780, pila 0x4000, flags 0x1 |

## Modelo de hilos propuesto

Un hilo host por demuxer (`dmux_pamf`), creado en `cellDmuxOpen`, con los callbacks por `ppu_guest_call`:

| Evento | Quién | Qué hace |
|---|---|---|
| `SetStream` | hilo del juego | deja el puntero bajo un `SRWLOCK`, despierta con `CONDITION_VARIABLE`, vuelve al momento (`BUSY` si sigue en marcha) |
| Parseo | `dmux_pamf` | pack a pack; copia AU a la memoria de la ES del juego |
| AU completa | `dmux_pamf` | la mete en la cola y llama `esCb(dmux, es, msg AU_FOUND, arg)` |
| Cola llena | `dmux_pamf` | espera en la condición, sin girar, hasta `ReleaseAu`, `ResetStream` o `Close` |
| Fin de sección | `dmux_pamf` | `cbMsg(dmux, msg DEMUX_DONE, arg)` |
| `FlushEs` | hilo del juego → `dmux_pamf` | emite la AU a medias y luego `FLUSH_DONE` |
| `Close`/`DisableEs` | hilo del juego | pide parar y hace join con espera nativa |

Claves del runtime (`ppu_guest_call`, `runtime/ppu/ppu_loader.cpp:3901`): resuelve el OPD y pone r2; pila de
callback por hilo host (`ppu_cb_stack_sp`); un hilo solo host recibe `thread_id` centinela
(`0x7FFF0000 | tid`). `vblank_clock` ya funciona así. **Riesgo**: los callbacks del juego pueden tomar
`sys_mutex`/`sys_lwmutex` con ese id (ver Riesgos).

## Parseo PS/PES + M2V + ATRAC3plus

Se reimplementa dentro de `libs/codec/cellDmux.c`: es la librería del sistema, no una herramienta. Nada en `tools/`.

| Capa | Regla observada en RPCS3 | Dato de `adv.pam` |
|---|---|---|
| Pack | 0x800 B fijos; `0x1BA` al inicio; `0x1B9` = fin de programa (`PROG_END`); stuffing en +0xD (3 bits) | 0xE928800 / 0x800 = 119 377 packs (1 275/s) |
| Cabecera de sistema | `0x1BB` opcional, seguida a veces de private stream 2 (`0x1BF`), que marca la siguiente AU de vídeo como RAP | — |
| PES | un PES por pack; id, longitud en +4, longitud de cabecera en +8; PTS/DTS según bits de +7, **bit 32 descartado y extensión de signo de s32 a u64** | vídeo `E0`, audio `BD/00` |
| Audio | 4 B de cabecera tras los datos PES (más un extra que RPCS3 no ha visto nunca distinto de 0) | — |
| M2V | la AU empieza en `0x100` (imagen) o `0x1B3` (secuencia, que se queda con la imagen que sigue) y acaba en el siguiente delimitador; `0x1B7` entra en la AU | máx. 0x12A800 B si `profileLevel > MP_ML` (MP@HL = 3), cola 0x40, búfer 0x255000 |
| ATRAC3plus | sync `0x0FD0`, tamaño `((h & 0x3FF) + 1) * 8 + 8` | AU de 0x2B8 B en `cellAdecDecodeAu`; máx. 0x1008, cola 0x40, búfer 0x6400 |
| Tiempos | PTS/DTS de la AU = los vigentes cuando empieza; sin valor, `0xFFFFFFFF` (`CODEC_TS_INVALID`) | `cellAdecDecodeAu` ve `pts=0x14dde` en la primera |
| Bordes | delimitador partido entre packs (caché de hasta 3 B); AU partida entre PES; cuando no cabe `au_max`, el búfer vuelve al principio; tamaño ATRAC que no cuadra: se descarta la AU y se avanza 1 B | — |

La AU es contigua en memoria del juego: `auAddr` apunta al búfer de la ES (la `memAddr` de `EnableEs`), y hay
que respetar la región entre `GetAuEx` y `ReleaseAu`, porque cellVdec y cellAdec leen de ahí.

## `cellDmuxReleaseAu`: qué ve el juego

**El juego ve `CELL_OK`.** Los ~5 000 `CELL_EPERM` no llegan a él:

| Hecho | Evidencia |
|---|---|
| El EPERM es de `sys_cond_signal_to`: el hilo demuxer no estaba esperando en el cond | `cellDmuxPamf.cpp:2367`, se ignora a propósito (`ret != CELL_EPERM`) |
| `release_au` termina en `CELL_OK` si el unlock va bien | `cellDmuxPamf.cpp:2379` |
| RPCS3 lo registra igual: el `error_code` escribe "failed with" al construirse | `Stat: '_CellDmuxCoreOpReleaseAu' failed with 0x80010009 : CELL_EPERM, 0x100000b [x2196]` y `[x2805]` = 5 001, una por `ReleaseAu` |
| El juego sí mira el retorno: cualquier valor ≠ 0 se va por otro camino | tras la llamada en `0x83ab14` se compara r3 con 0 y se salta a `0x83aab0` si no es 0; igual tras `0x83b7c4`, con salto a `0x83b760` (`work/recompiled/ppu_recomp_004.cpp`) |

ASSUMPTION: cuál es ese camino (`0x83aab0`, `0x83b760`) no se ha leído: da igual para el diseño, porque devolvemos 0.

Con un error real (`auAddr` nulo o `auSize` 0) RPCS3 acumula `error_count`, `error_mem_addr` y `error_mem_size`
y lo reintenta en el siguiente `ReleaseAu` (`cellDmux.cpp:1110`). No hace falta para F, pero el retorno sí debe
respetarse: `ARG`, `SEQ` si no hay AU que soltar.

Regla para el nuestro: `ReleaseAu` libera de la cola y hace un signal nativo. Sin espera y sin error.

## `GetAuEx`

| Punto | Comportamiento |
|---|---|
| Firma | `cellDmuxGetAuEx(esHandle, **auInfoEx, **auSpecificInfo)`: escribe **punteros de 32 bits** a estructuras en memoria del juego; es el mismo tipo que `GetAu` |
| Sin AU | `CELL_DMUX_ERROR_EMPTY`; el juego espera en su cond (`sys_cond_wait` 6 982 en vídeo) |
| Validación | `esHandle`, su `_this`, y tipo PAMF: si no, `ARG` |
| Dónde viven | `CellDmuxAuInfo` (`auAddr`, `auSize`, `auMaxSize`, `isRap`, `userData`, `pts`, `dts`) en la cola, dentro de la memoria de la ES: **todo big-endian, por `vm_write*`** |
| `auMaxSize` | 0 al crear la cola (`cellDmux.cpp:912`, `:1023`); RPCS3 no lo rellena en cada AU (`cellDmuxPamf.cpp:1918`, "left uninitialized"): se deja a 0 |

`QueryAttr`: `memSize` lo pone HLE (`cellDmux.cpp:277`); `demuxerVerUpper = 0x260000`, `demuxerVerLower = 0x280000`.
Nuestro `memSize` tiene que ser ≥ lo que usemos. **El juego reserva con él**, y `EnableEs` falla si `mem_size` no
alcanza: `QueryEsAttr` debe devolver lo que pida la cola + `dmuxPamfGetAuQueueBufferSize` (0x255000 para vídeo).

## Coste en CPU

| Medida | Valor |
|---|---|
| Datos | 244 MB en 93,6 s = 2,6 MB/s |
| Packs | 1 275/s |
| Callbacks | ~53 AU/s + ~5 fin de demux/s |

Medido en el 4c.2 con
`-ThreadCpu` (nombre de hilo `dmux_pamf`): 0,3 % de un núcleo ([perf.md](perf.md)). El coste de verdad estará en cellVdec (4c.3).

Diseño no lento: sin espera activa, sin lock global, sin log en el camino caliente, lock solo para la cola.
Se escanea con `memchr` para el primer byte.

## Riesgos

| Riesgo | Por qué | Mitigación |
|---|---|---|
| `thread_id` centinela en callbacks | el callback puede tomar mutex con dueño o despertar colas; RPCS3 corre en un PPU real | primer check del 4c.2: callbacks con `-HleTrace`; si falla, opción C |
| Orden AU frente a cola llena | sin la espera, el juego pierde AU y el vídeo se rompe | test con cola forzada a 2 |
| `ppu_stwcx`/atómicos del juego en callback | el callback modifica contadores con `lwarx`/`stwcx` desde un hilo host | ya funciona igual en `vblank_clock`; vigilar `g_trampoline_fn` por hilo |
| `DEMUX_DONE` antes de tiempo | el alimentador manda el siguiente bloque al recibirlo; si llega antes de copiar todo, `SetStream` da `BUSY` | emitirlo tras la última AU de la sección |
| `FlushEs` repetido | 66 por ES durante 2 s al final (quizá el juego espera algo más); sin leer | responder cada una con `FLUSH_DONE`, como RPCS3 (`:144-160`) |
| `DisableEs`/`Close` con el hilo dentro de un callback | interbloqueo si el callback espera al hilo que cierra | join sin tener el lock; pedir parada antes |
| Stubs vecinos (cellVdec sin FFmpeg, cellAdec en silencio) | con dmux real, el vídeo avanzará sin imagen y sin audio hasta 4c.3-4c.4; el juego puede atascarse si nadie consume | el check del 4c.2 mide AU y callbacks, no imagen |
| Licencia | ver arriba | reimplementar sin mirar al escribir; con tests propios |

## Opciones

| Opción | Pros | Contras | Veredicto |
|---|---|---|---|
| **A. Un hilo host con parser y callbacks** | el mínimo que da el comportamiento observado; poco código; sin colas de eventos | no reproduce el reparto SPU/PPU; un callback lento frena el parseo; `thread_id` centinela | **recomendada** |
| B. Dos hilos host (parser + callbacks), como RPCS3 | parseo no bloqueado por callbacks; orden igual al de RPCS3 | más estados y más carreras, sin beneficio visible a 2,6 MB/s | solo si A se atasca por el ritmo |
| C. A, pero el hilo es un PPU real (creado con la maquinaria de `sys_ppu_thread`, prio 780, pila 0x4000) | identidad real en mutex; prioridad como RPCS3 | necesita un entry en código del runtime llamado como hilo guest; más invasivo | plan B si `thread_id` falla |
| D. LLE: traducir `libdmux.sprx`/`libdmuxpamf.sprx` y su tarea SPU | el código original (regla 9); semántica exacta | exige SPURS (F no lo importa), los PRX del firmware y su tarea SPU; RPCS3 tampoco los usa ("Ignored module" en el log) | descartada, con este motivo |

## Plan del 4c.2

| Paso | Qué | Check |
|---|---|---|
| 1 | `QueryAttr`, `QueryEsAttr`: los 3 imports que faltan se añaden al parche | el log pasa de `QueryAttr` a `Open` |
| 2 | `Open`/`Close`/`EnableEs`/`DisableEs`, hilo y parser | secuencia hasta `SetStream` igual a RPCS3; sin crash |
| 3 | AU por memoria del juego + `GetAuEx`/`ReleaseAu` | por 60 s de `adv.pam`: nº de AU de cada ES igual a RPCS3; `size` y `pts` iguales a los de `cellAdecDecodeAu` (cellVdec solo registra un puntero); 467 `DEMUX_DONE` |
| 4 | `FlushEs`, fin de vídeo, repetición a los 3:58 | sigue hasta el título; 30 flips/s; audio del título intacto |

Un test nativo con cabecera sintética, como `test_cellPamf.c`, y un check por `-ThreadCpu`.

## Resultado de los riesgos (4c.2)

| Riesgo | ¿Se dio? | Evidencia |
|---|---|---|
| `thread_id` centinela | no | 387 mutex y 4123 `sys_cond_signal`, retorno 0 |
| Orden AU frente a cola llena | no | test con cola 2 y `adv.pam` con cola 2: 2805 + 2196 AU |
| `ppu_stwcx`/atómicos en callback | no | 2 películas sin crash |
| `DEMUX_DONE` antes de tiempo | no | 467 `SetStream`, igual que RPCS3: sin reintentos (el retorno no se registra) |
| `FlushEs` repetido | explicado | el alimentador repite cada 33 ms hasta que lo paran; 4 608 con stubs |
| `DisableEs`/`Close` en un callback | no | 2 cierres limpios; `Close` con la cola llena en el test |
| Stubs vecinos | no bloquean | el reloj de la película no depende de la imagen: cierra a los 94,2 s |
| Licencia | sigue abierto | ASSUMPTION de la sección de licencia |
