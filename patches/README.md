# Parches a ps3recomp

`external/ps3recomp` está fijado a un commit (`4e8f3ca`, v0.18.0, desde el 2026-10-29; antes `a6d76c8`) y no se edita en sitio. Cualquier cambio
al runtime va aquí como un `.patch` (UTF-8, LF), con un nombre que diga qué arregla, y su porqué en
`docs/decisions.md` o `docs/hle-log.md`. `tools/apply_patches.ps1` los aplica en orden de nombre
sobre el commit fijado (antes de correrlo, todo cambio tiene que estar exportado aquí y con commit).

Numeración: `00xx` = heredados de f2nd-recomp (mismo nombre y cambio; desde la v0.18.0 regenerados sobre ese commit, así que el contexto de los hunks ya no es el de F2nd); `01xx` = propios de Re:Fine.

Todos se regeneraron sobre v0.18.0 el 2026-10-29 (tabla y evidencia en `docs/decisions.md`).

| Parche | Qué arregla | Origen |
|---|---|---|
| 0001 | `cellSpursInitializeWithAttribute2` | f2nd-recomp@7d19a0a |
| 0002 | `_cellSpursLFQueuePushBody` ANY2ANY (de ico-recomp, MIT) | f2nd-recomp@7d19a0a |
| 0003 | syscall 988 sin efecto, como RPCS3 | f2nd-recomp@7d19a0a |
| 0004 | `sys_lwcond` con wait/signal reales (semántica de RPCS3) | f2nd-recomp@7d19a0a |
| 0005 | `PS3_HLE_UNRESOLVED=fail` funciona de verdad | f2nd-recomp@7d19a0a |
| 0006 | `cellGameContentPermit` con rutas vacías y sin crear directorios, como RPCS3 | f2nd-recomp@7d19a0a |
| 0101 | `CellVideoOutDeviceInfo` con `colorInfo`; `GetResolutionAvailability` como RPCS3 | propio (según RPCS3 `1f59f0f2`) |
| 0102 | `sys_spu_thread_group_start` sin imagen traducida devuelve `CELL_ENOSYS` (antes, éxito falso) | propio |
| 0103 | `sys_spu_thread_bind_queue`/`unbind_queue` reales y servicio de `receive_event` (stops 0x110/0x111) | adaptado de JonathanDC64/ps3recomp `d9e5559f` (MIT) |
| 0104 | `cellGcmGetVBlankCount` devuelve el contador sin incrementarlo, como RPCS3 | propio (según RPCS3 `1f59f0f2`) |
| ~~0105~~ | **quitado** (2026-10-29): pila de callbacks por hilo host; upstream v0.18.0 lo trae (#489, `b3a75d98`) | era adaptado de JonathanDC64/ps3recomp `ca23b8bd` (MIT) |
| 0106 | `sys_timer_usleep` duerme con un temporizador de alta resolución por hilo, sin espera activa | propio |
| 0107 | la vía rápida de imports hoja (v0.18.0) escribe la TOC en `sp+0x28`, como `_TocGuard`; sin esto r2 sale 0 y F se cae antes de `main` | propio |
| 0108 | el signo de `viewport_scale.y` llega al vertex shader de D3D12 (F salía invertido) | propio (según RPCS3 `1f59f0f2`) |
| 0109 | diagnóstico de audio, apagado por defecto: formato del dispositivo (`AUDIO_RATE`) y tiempo de synth2 por bloque (`SPU_BLOCK_STATS`) | propio |
| 0110 | cellAudio entrega un bloque cada 5,33 ms por reloj, con el dispositivo como cola | propio (según RPCS3 `1f59f0f2`) |
| 0111 | `cellPamf` real: formato PAMF de verdad, big-endian por `vm_*`, y las 8 funciones que usa F | JonathanDC64/ps3recomp `ce105e69` (MIT) |
| 0112 | diagnóstico `PAMF_TRACE`, apagado por defecto: argumentos, retorno y salidas de cellPamf | propio |
| 0113 | `cellDmux` real: hilo `dmux_pamf`, parser PS/PES, M2V y ATRAC3plus, `test_cellDmux.c`; `DMUX_TRACE` apagado por defecto | propio (comportamiento según RPCS3 `1f59f0f2`, sin su código) |
| 0114 | la caché de `vp_get_fp_pso` recuerda los fallos y cada programa se registra una vez. Parche de rendimiento, no arreglo: D9 abierta | propio |
| 0115 | D3D12 con las 16 unidades de textura de fragmento: root signature, ventana de SRV por paridad, trabajo por máscara de unidades; banco declarado por unidad si hay cubos; aviso de lectura de textura de vértice sin enlace. Cierra D9. Imagen de la PV600 y parón D11 abiertos. Candidato a f2nd, sin probar allí | propio |
| 0117 | superficie en el offset 0 de la memoria local (`RT_OFF_ZERO`) y constantes inline en la clave del PSO. La escena 3D llega a pantalla; la imagen sigue mal; sube D12. Candidato a f2nd, sin probar allí | propio |
| 0118 | constantes del FP en `b1`, leídas al grabar cada draw; test alfa; un programa, un pipeline (quita la clave completa de 0117). Diagnóstico `FP_CONST_STATS`. Candidato a f2nd, sin probar allí | propio |
| 0119 | `SET_RESTART_INDEX(_ENABLE)` y strips indexados continuos entre `DRAW_INDEX_ARRAY` de 256 índices. Candidato a f2nd, sin probar allí | propio |
| 0120 | un clear a cero por otra vista de los mismos bytes no recrea la superficie. Diagnóstico `RTG_TIME` | propio |
| 0116 | fallos de pipeline en `rsx_fp_fail_table.h`, sin desalojo ni `vp_gen`, con `test_fp_fail_table.c`; `PSO_TIME` para medir primeras compilaciones, apagado por defecto. Candidato a f2nd | propio |
| 0121 | diagnóstico `SHADER_DUMP=<dir>`, apagado por defecto: escribe cada shader generado | propio |
| 0122 | el VS generado compila más rápido con el mismo resultado: `vp_mul` sobre vectores y una pasada si no hay saltos atrás. Candidato a f2nd, sin probar allí | propio |
| 0123 | caché de bytecode en disco (`rsx_shader_cache.h`, `test_shader_cache.c`), fuera del repo; D10 cerrada desde la segunda ejecución. Candidato a f2nd, sin probar allí | propio |
| 0124 | `cellVdec` real: hilo por decodificador, MPEG-2 con FFmpeg en DLL, swscale en ese hilo, puerta de licencia en `ffmpeg_runtime`; sin FFmpeg, `Open` falla. `VDEC_TRACE` apagado por defecto | propio (comportamiento según RPCS3 `1f59f0f2`, sin su código) |
| 0125 | `cellAdec` real: hilo por decodificador, ATRAC3plus con FFmpeg (`adec_ffmpeg`); `cellAtrac` pasa a FFmpeg y `at3_standalone` sale del build. `ADEC_TRACE` apagado por defecto | propio (comportamiento según RPCS3 `1f59f0f2`, sin su código) |
| 0126 | la copia de pantalla (`SCREEN_AS_TEX`) solo sustituye texturas cuya memoria del guest está vacía; la película salía negra. Candidato a f2nd, sin probar allí | propio |
| 0127 | diagnóstico `TEX_RACE=<archivo>[,<ea>]`, apagado por defecto: marca con hora `put`, copia, draw, flip y subida de una textura, con firma por fila. Lo lee `tools/tex_race.py` | propio |
| 0128 | una textura de la memoria del juego se lee al grabar el draw, no al subirla: si cambió desde el último lote se copia entonces y la subida usa esa copia. Cierra D14. `TEX_SNAP=0` lo apaga; `TEX_SNAP_STATS` da las cuentas. Candidato a f2nd, sin probar allí | propio |
| 0129 | diagnóstico `AUDIO_LATE=1`, apagado por defecto: cuántos bloques va adelantado el escritor de cada puerto y qué bloques no llegó a escribir | propio |
| 0130 | el mezclador de cellAudio marca el bloque leído y espera al escritor que llega tarde (hasta 2 periodos, 4 si está a medias), sin espera activa. Candidato a f2nd, sin probar allí | propio (comportamiento según RPCS3 `1f59f0f2`, sin su código) |
| 0131 | `sys_spu_thread_disconnect_event` real, con `test_spu_disconnect_event.c`; antes `CELL_OK` sin efecto | propio (comportamiento de RPCS3 `1f59f0f2`) |
| 0132 | `cellSaveDataEnableOverlay`: handler ctx que guarda el flag y no toca `r3`. `cellSaveData.c` tiene finales de línea mezclados: el parche va en LF | propio (comportamiento de RPCS3 `1f59f0f2`) |
| 0133 | `cellSaveData` guarda bajo `PS3_HDD0_ROOT` (`home/00000001/savedata`), no bajo el directorio de trabajo | propio (ruta de RPCS3) |
| 0134 | cerrar la ventana o Escape es una petición: la ventana sigue hasta que el juego sale; 20 s de plazo y `[quit] WARNING` si no sale. Va con el cambio de `src/main.cpp`. Candidato a f2nd, sin probar allí | propio |
| 0135 | `cellSearch` sin contenido, `libs/system/cellSearch.c` con `test_cellSearch.c`; `cellSysutilQueueGuestCallbackBefore` | propio (comportamiento de RPCS3 `1f59f0f2`, sin su código) |
| 0136 | `stop 0x101`/`0x102` paran el SPU, antes el drenaje los repetía sin fin; `group_terminate` con id desconocido da `ESRCH`. Candidato a f2nd, sin probar allí | propio (comportamiento de RPCS3 `1f59f0f2`) |
| 0137 | capa de interfaz del sistema: `libs/system/sysui.c` con `test_sysui.c`, pase en el present de D3D12 y captura del mando en `cellPadGetData`. Cerrar la ventana contesta lo que esté abierto. Candidato a f2nd, sin probar allí | propio |
| 0138 | `cellSaveData`: `ListLoad2`/`ListSave2` con lista del sistema, confirmación y `CELL_CANCEL`; `FixedLoad2`/`FixedSave2` confirman; `errDialog` real. Candidato a f2nd, sin probar allí | propio (comportamiento de RPCS3 `1f59f0f2`, sin su código) |
| 0139 | `cellMsgDialog` real, sin respuestas automáticas, con `cellMsgDialogOpenErrorCode`. Candidato a f2nd, sin probar allí | propio (comportamiento de RPCS3 `1f59f0f2`) |
| 0140 | pantallas con la distribución que enseña RPCS3 (`ed2`), reloj, fecha sin cero, fila de detalle | propio |
| 0141 | la pulsación que abre una pantalla no la contesta | propio |
| 0142 | teclado de Re:Fine junto al mando en el puerto 0; Esc deja de cerrar; `PAD_KEYS` y `PAD_KEYCYCLE` para corridas automáticas | propio |
| 0143 | la salida WASAPI sigue al dispositivo predeterminado: hilo `audio device`, `IMMNotificationClient`, stream nuevo desde el historial de la mezcla, desconexión, dispositivo dormido y arranque sin salida; `REFINE_AUDIO_DEVICE`; `AUDIO_SWITCH` y `<AUDIO_WAV>.qpc` de diagnóstico. Candidato a f2nd, sin probar allí | propio |
| 0144 | diagnóstico de 5c, apagado por defecto: `AUDIO_SLOW` y las teclas de `PAD_BOT` | propio |
| 0145 | todos los mandos por SDL3, en un hilo propio: botones por posición, conexión en caliente, foco y vibración; XInput fuera del build; `PAD_VPAD` y `PAD_VKEYS` para corridas automáticas | propio |
| 0146 | diagnóstico `AUDIO_STALL=1`, apagado por defecto: el mezclador avisa cuando el escritor vuelve de un retraso, y anillo de syscalls de los hilos 3 a 5 | propio |
| 0147 | los hilos de libmixer (prio 256-511) y los grupos SPU de prio < 128 van a `THREAD_PRIORITY_HIGHEST` | propio, candidato a f2nd |
| 0148 | zona muerta radial e histéresis de los sticks, con `stick_deadzone` y `stick_hysteresis`; cambiar de sentido cuenta como volver al centro; con `PAD_VPAD`, solo el mando virtual | propio |
| 0149 | `cellUsbdRegisterExtraLdd` y `UnregisterExtraLdd` reales sobre la tabla de LDD; `cellKbRead` con los retornos de RPCS3 y sin teclado hasta que el host informe de uno, con `test_cellKb.c` | propio (comportamiento de RPCS3 `1f59f0f2`, sin su código) |
| 0150 | diagnóstico `FRAME_PROF=<desde>-<hasta>`, apagado por defecto: fases de `render_frame`, marcas D3D12 de GPU y ganchos del guest en `ResetFlipStatus` y `GetVBlankCount`; va con `src/main.cpp` | propio |
| 0151 | resolución interna y present escalado: objetivo de pantalla `rw`x`rh` propio, superficies offscreen y profundidad por un factor `rs` (el guest sigue viendo sus tamaños), pase bilineal 16:9 con bandas al swap chain, `WM_SIZE` con `ResizeBuffers`; `render_height` en `refine.ini`, va con `src/main.cpp`. Con 720 la pantalla sale byte a byte igual. Candidato a f2nd, sin probar allí | propio |
| 0152 | `cellGcmTickPace`: un latido del ritmo de frame sin vblank; `cellGcmGetVBlankCount` lo suma, y el manejador de vblank del juego sigue a 59,94 Hz; `g_fps_guest_frame` una vez por frame en el hilo del juego. Con `fps_target=30` no se usa ninguno. Candidato a f2nd, sin probar allí. Va con `src/main.cpp` | propio |
| 0153 | diagnóstico: `FRAME_PROF` reparte el `drain` en draws, arrays, `vp_record_cb`, constantes, texturas y hash de VS. Va con `src/main.cpp` | propio |
| 0154 | strips y fans indexados por la vía de índices (33 MB por frame menos al heap de subida), y fetch de vértices con plan por draw, con `VTX_PLAN_CHECK` y `VTX_PLAN_OFF`. Sube 120 de 93 a 120 fps. Candidato a f2nd, sin probar allí | propio |
| 0158 | ganchos del overlay del port (`libs/video/port_overlay.h`): dibujo sobre el swap chain tras el present escalado (solo si `g_port_overlay_active`), mensajes de la ventana antes del backend y el mando retenido mientras el overlay está abierto (`cellPadGetData`). El runtime solo los llama; los pone `src/port_menu/` | propio; candidato a f2nd (6h) |
| 0159 | ganchos de la barra rápida y de los ajustes (`port_overlay.h`): el device al crear el swap chain (el menú se prepara fuera del frame), un observador de cada `cellPadGetData` (SELECT largo, reloj de canción), botones inyectados en el puerto 0 tras la retención (START para la pausa del juego), clic del panel táctil por SDL3, nombre del mando del puerto 0, vibración desactivable y volumen general en la salida WASAPI | propio; candidato a f2nd (6h) |
| 0160 | diagnóstico: cuando el juego deja de contar pads y teclado porque su ventana no está al frente, el log dice qué proceso y qué clase de ventana tiene el frente | propio |
| 0161 | `render_height` en la partida en marcha: recrea display target, profundidad, superficies fuera de pantalla, copia de pantalla, vista del blit y textura de diálogos entre frames (`rsx_d3d12_set_render_height`); `pad_set_fps_high` para el rearme del stick | propio |
| 0162 | `rsx_d3d12_backend.c`: si `render_height` falla al recrear las superficies, vuelve a la altura anterior y avisa al menu (`rsx_d3d12_take_render_height_failure`); `RH_FAIL_TEST=<h>` fuerza el fallo (diagnostico) |
| 0156 | diagnóstico: `CELLMARK_DUMP_ARMED=1` hace que el volcado de `CELLMARK_DUMP` espere a `rsx_d3d12_dump_arm()` del host (`src/rob_interp.cpp`, `REFINE_DUMP_AT=<segundo de canción>`). Sin la variable, nada cambia | propio |
| 0155 | rearme del stick en tiempo real: `stick_rearm_ms` (34 por encima de 30 fps, 0 a 30). Va con `src/main.cpp` | propio; candidato a f2nd |

6d.10 (opción C) no añade parche: la interpolación del esqueleto es código del port (`src/rob_interp.cpp`) y engancha dos métodos con `ppu_register_function`, que ps3recomp ya exporta. `config/insn_patches.json` pierde la entrada de `0x4C008` y queda con 9.

Si F los necesita o no lo dice `docs/diff-f2nd.md`.
| 0164 | gancho de pantalla de inicio al final del init del backend D3D12 (`g_port_launcher`, `rsx_d3d12_launcher_frame`) y accesores del title id/version | propio |
| 0165 | filtrado anisotropico minimo y nitidez CAS en el present, desde el menu del port (en caliente) | propio |
| 0171 | reasignación en caliente (6f p1): teclado (dos teclas por acción) y mando (un origen por botón PS3, gatillos incluidos) desde tablas que publica el menú del port; `pad_raw_sources` para "pulsa un botón" | propio |
| 0172 | `pad_pulse` (vibración pedida por el port, puerto 0, sobre la del juego) y salida de audio elegida en caliente (`port_audio_set_device`, `port_audio_outputs`; nombre exacto antes que trozo) | propio |
| 0173 | en un frame en que la lógica del juego no avanza (menús a más de 30 fps), `cellPadGetData` le devuelve su último paquete: un "recién pulsado" se ve en el tick | propio |
| 0175 | menús entre ticks: los frames sin tick de los menús se dibujan entre los dos últimos estados (vértices y constantes de VP/FP mezclados por draw emparejado, el estado nuevo detectado en los draws, la fase con el reloj del juego); vértices del frame en memoria del sistema y copiados al heap; `g_port_draw_*` en `port_overlay.h`. `REFINE_INTERP_LOG=1/2` diagnóstico | propio |
| 0176 | mandos PS4/PS5: Share abre el menú del port y el clic del panel táctil es SELECT; deslizar el dedo por el panel es un flick del stick izquierdo (estrellas de F), `g_port_touch_stars` (fila "Touchpad stars", encendida) | propio |
| 0177 | mods (6e): puntero `ps3_vfs_redirect_hook` en `sys_fs.c`, consultado primero por los tres traductores de rutas (`sys_fs`, `cellFs`, `ppu_fs`); el port (`src/mods.cpp`) lo pone si hay mods activos | propio |
| 0178 | N1.1 V1: backend Vulkan para Windows (archivo nuevo `libs/video/rsx_vk_win.c`), elegido con `REFINE_RENDERER=vulkan`: `vulkan-1.dll` cargada en marcha, ventana en su hilo, dispositivo, swapchain con recreación, cada flip presentado con el color de borrado del juego; los draws se cuentan, aún no se dibujan (V2) | propio |
| 0179 | N1 V2a: el backend Vulkan pasa cada programa que usa un draw por los decompiladores (mismo HLSL que D3D12) y por DXC a SPIR-V dentro del proceso (`rsx_vk_spirv.cpp`, `dxcompiler.dll` junto al exe): `-HV 2018`, `-fvk-use-dx-layout`, b -> 0-1, t -> 16-31, s -> 32-47; cuenta aciertos y fallos (`REFINE_VK_SHADER_DUMP=<dir>` guarda el HLSL que falla) | propio |
| 0180 | N1 V2b: con `REFINE_RENDERER=vulkan` el backend D3D12 no crea dispositivo; graba como siempre (con todos los `01xx`) en los buffers de subida del ejecutor Vulkan (`rsx_vk_win.c`, `rsx_vk_exec.h`), `vp_get_vs` pasa los programas de vértices a SPIR-V, y `render_frame` entrega cada tanda al ejecutor y hace los mismos reinicios de fin de frame. Sin lanzador ni menú del port en Vulkan aún | propio |
| 0181 | N1 V2c: el ejecutor Vulkan dibuja los registros: superficies, borrados, pipelines (dynamic rendering), constantes, superficies como textura, texturas del juego (rsx_texture_decode) y blit a la swapchain. Falta: profundidad como textura, cubemaps, samplers por unidad, copia de pantalla | propio |
| 0182 | Vulkan: cada textura se comprueba (hash de sus bytes) una vez por tanda, no en cada draw |  |
| 0183 | Vulkan: cubemaps; los programas que declaran el banco de texturas por unidad usan un segundo layout de descriptores |  |
| 0184 | Vulkan: samplers por unidad (modos de borde y filtros del juego); contador de profundidad leída como textura |  |
| 0185 | cellPad: sondeo en cada frame (`cellPad_poll`), `GetAsyncKeyState` con el bit de pulsada desde la última consulta y botones guardados hasta la lectura que el juego usa: ninguna pulsación corta se pierde. `REFINE_PAD_LOG` |  |
| 0186 | Vulkan: el overlay del port (menú F1 y pantalla de inicio, RmlUi) se dibuja en el ejecutor sobre la imagen presentada (`rsx_vk_ui_*`, `g_rsx_vk_ui`); `rsx_renderer_is_vulkan` |  |
| 0187 | Vulkan por defecto, con vuelta a Direct3D 12 si no arranca (`REFINE_RENDERER`, `renderer` en refine.ini); anisotropía; pase de presentación con nitidez y remuestreo por área; aviso si un draw lee una profundidad como textura |  |
| 0188 | Vulkan más rápido: una imagen por offset, tamaño y formato de superficie (antes se rehacía cada tanda), suma muestreada de texturas, solo las unidades que el programa usa; cronómetros `[vk-time]` |  |
| 0189 | Vulkan: el SPIR-V de cada programa en la caché de disco (`rsx_shader_cache.h`, la carpeta de `0123`); sin tirones de compilación desde la segunda vez |  |
| 0190 | Prueba de velocidad: hash de lo que dibuja cada frame (`g_port_frame_hash`) y `ppu_lookup_function` para enganchar por vtable (`src/evt.cpp`, `src/realtime.cpp`) |  |
| 0191 | cellPad: cola de pulsaciones con hora (ninguna se pierde ni se funden dos) y lecturas con `len` válido cuando cambian los botones (`0191c`: el juego ignora `len = 0`) |  |
| 0192 | `dev_flash` relativo al exe si no hay otro (instalaciones sin rutas absolutas) |  |
| 0193 | cellPad: en canción, entre ticks, START y SELECT conservan el estado del último paquete (`g_port_pad_hold_sys`): la pausa, que corre en los ticks, ya no pierde un START corto a fps altos (`docs/revision-2026-11-21.md`) |  |
| 0194 | cellGame: `cellGame_sfo_string` lee PARAM.SFO de otra carpeta (pantalla de inicio: detectar el disco de F2nd, plan 6h S1) |  |
| 0195 | Vulkan: las pantallas del sistema (`sysui.c`: lista de partidas, "Load this data?", diálogos de `cellSaveData`/`cellMsgDialog`) se dibujan en la pasada del overlay (`vk_sysui_draw`), como `sysui_present` en D3D12. Antes, con Vulkan, LOAD GAME no enseñaba nada y el mando contestaba a una lista invisible | propio |
| 0197 | Vulkan/D3D12, solo diagnóstico: `REFINE_DRAW_DUMP=<archivo>:<de>-<a>` escribe lo que mueve cada dibujo (constantes y seis vértices) para `tools/draw_census.py` |  |
| 0196 | cellPad: con el menú del port o una pantalla del sistema abiertos, la cola de pulsaciones de 0191 se vacía: se sumaba al paquete después de los dos bloqueos y el juego veía todo lo pulsado en el menú (una Cruz reanudaba la canción pausada) |  |
