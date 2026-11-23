/*
 * Re:Fine -- overrides HLE especificos de Project DIVA F (BLUS31319).
 *
 * Empieza vacio a proposito. Un override de f2nd-recomp solo entra aqui si F
 * importa ese mismo NID (ver docs/diff-f2nd.md) y el log de RPCS3 de F lo
 * respalda; cada uno se apunta en docs/hle-log.md con su evidencia.
 *
 * Lo unico que hay es el gancho vacio que main.cpp necesita para enlazar
 * (el mismo de external/ps3recomp/templates/project/stubs.cpp): F no carga
 * ningun PRX traducido.
 */

#include "ps3emu/ps3types.h"

extern "C" void ps3_load_prx_modules(void)
{
}
