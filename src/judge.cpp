/* The judgement offset of the port's menu (6g, 2026-11-22): where the middle of a note's timing windows is, for slow
 * screens and controllers.
 *
 * 0x65B230 judges a note by the song seconds left to its moment (note + 0x10) against four pairs of limits of the
 * judgement object, early and late: +-0.03 COOL, +-0.07 FINE, +-0.10 SAFE, +-0.13 SAD, at 0x12CE588 + 0x241C..0x2438
 * (MEM_BP on the COOL counter, run bp1; Ghidra; MEM_WATCH, run lim). It is called directly, so a hook on it is never
 * reached; the limits are data, and they are moved instead: +20 ms makes a press that comes 20 ms late exact. What the
 * notes show, the song clock and the audio are not touched. At 0, the default, nothing is written. */
#include "ppu_recomp.h"
#include "win32_compat.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint32_t LIMITS = 0x012CE588u + 0x241C;
static volatile long g_offset_ms;                      /* set by the menu's thread */

extern "C" void port_set_judge_offset(int ms) { g_offset_ms = ms; }
extern "C" float port_judge_offset(void) { return (float)g_offset_ms * 1e-3f; }

/* Once per frame of the game, on its thread. A limit that is not what was written last is the game's own (a new song,
 * or the first time here) and becomes the base. */
extern "C" void judge_frame(void)
{
    static uint32_t wrote[8], base[8];
    static long last;
    const long ms = g_offset_ms;
    if (!ms && !last) return;
    uint32_t now[8];
    float lim[8];
    for (int i = 0; i < 8; i++) {
        memcpy(&now[i], vm_base + LIMITS + 4 * i, 4);
        if (!last || now[i] != wrote[i]) base[i] = now[i];
        const uint32_t host = _byteswap_ulong(base[i]);
        memcpy(&lim[i], &host, 4);
        const bool early = !(i & 1);
        if (!(early ? lim[i] > 0.0f && lim[i] < 0.5f : lim[i] < 0.0f && lim[i] > -0.5f)) return;   /* not the limits (yet) */
    }
    if (ms != last) fprintf(stderr, "[judge] offset %+ld ms on limits %.3f %.3f %.3f %.3f%c", ms, lim[0], lim[2], lim[4], lim[6], 10);
    for (int i = 0; i < 8; i++) {
        const float v = lim[i] - (float)ms * 1e-3f;
        uint32_t be;
        memcpy(&be, &v, 4);
        be = ms ? _byteswap_ulong(be) : base[i];
        wrote[i] = be;
        memcpy(vm_base + LIMITS + 4 * i, &be, 4);
    }
    last = ms;
}
