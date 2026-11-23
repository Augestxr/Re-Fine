/* realtime.cpp - Re:Fine: the whole game by real time (docs/tiempo_real.md, 2026-11-13).
 *
 * Over 30 fps every task of the game runs every frame, with the game's speed (0x3BC0C) at the frame's real
 * share of a 1/30 s step. Code that counts frames instead then runs fast. Where such a counter is found it is
 * fixed in its own code (config/insn_patches.json). This file holds the rest:
 *
 *  - Tasks on whole steps. A task whose Ctrl counts frames all over, not yet converted, keeps the original
 *    pace: its Ctrl (vtable slot 3) runs on the 30 Hz ticks only (g_fps_not_tick, src/main.cpp), with the
 *    speed at 1.0 for that call, exactly what it sees at 30 fps. It is the game's own scheme for the
 *    character tasks (flag +0x21, 0x4BF40), applied by vtable. Its Disp and everything else still run every
 *    frame. The table below is the list still to convert; REFINE_RT_TICK=<vtable hex>,... adds to it and
 *    REFINE_RT_TICK=none empties it (tests).
 *
 * Nothing here runs at fps_target=30 (the hooks call straight through) nor with REFINE_REALTIME=0. */

#include "ppu_recomp.h"
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unordered_map>
#include <vector>

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" void (*ppu_lookup_function(uint32_t a))(ppu_context*);
extern "C" int g_fps_not_tick;

namespace {

typedef void (*ppu_fn)(ppu_context*);

const uint32_t SPEED = 0xC22E70 + 0x14;       /* this frame's speed, what 0x3BC0C returns */

struct TickTask { uint32_t vt; const char* name; const char* why; };
/* Tasks whose Ctrl still counts frames (found with tools\speed_test.ps1); see docs/tiempo_real.md. */
const TickTask kTick[] = {
    { 0xBB3CC0, "14TaskWaitScreen", "its Ctrl (0x565B40) counts frames at +0x44/+0x48/+0x4C: the wait of the menus ended four times as fast at 120 fps (speed test: SelMain 3>4 500 ms early)" },
    /* The screens' own state machines. Each keeps a frame counter in its object (+0x8C, and +0x26C in SelStart)
     * that the frame counter scan (REFINE_EVT_SCAN=1) flags, and their Ctrl is a jump table on their state
     * (+0x198, +0xA1C in SelPv): logic, not motion. What the screens draw -- the 2D animations (AET objects)
     * and the characters -- steps every frame on its own. */
    { 0xBB24E8, "7SelMain", "the main menu" },
    { 0xBB0C98, "11SelFreeplay", "the mode select" },
    { 0xBB2C98, "5SelPv", "the song list" },
    { 0xBA8220, "12TaskAdvTitle", "the title" },
    { 0xBA8270, "11TaskAdvLogo", "the logos" },
    { 0xBB3C00, "13TaskTouchArea", "the touch areas" },
    { 0xBB0850, "8HelpInfo", "the rotating help of the song list" },
    { 0xBB1070, "16TaskPauseManager", "the pause menu" },
    { 0xBB08C4, "10InfoWindow", "the notices of the menus (its Ctrl 0x32F4E4 is a jump table on +0x198)" },
    { 0xBA98E0, "15ThumbnailWindow", "the song's thumbnail (jump table on +0x11C)" },
    { 0xBB3B28, "11StageResult", "the results screen" },
    /* NOT here either: 8SelStart (0xBB34B0), the song's start screen. With its Ctrl on the ticks the song did
     * not begin one time in three at 120 fps -- the game read the whole 109 ms press and the screen stayed --
     * and with a press every 2,5 s it never began at all (work/run/ss*, nv*). Every frame: 3/3 (`ns*`).
     */
    /* NOT here, tried and reverted (2026-11-14): the loaders and bookkeeping (9DlcLoader 0xBAC1A8,
     * 12TaskDataInit 0xBA9948, 12TaskSaveData 0xBB3D48, pv_db::TaskPvDB 0xBB15C8, 16SkinParamManager
     * 0xBB3AA0). Their chains do advance one hop a frame (4 of the differences the speed test still shows,
     * 51-74 ms), but with their Ctrl on the ticks **the song never began**: the start screen stayed up and the
     * song clock never moved (work/run/ht2, ht3 against bi_only_ and bi_none). They step every frame.
     */
};

/* ---- Constants the game adds once a frame without the game speed (2026-11-14, the author's videos) ----------
 * The game's own way of stepping something by time is "x += speed * k" (0x3BC0C). A few places add a flat
 * constant instead, because at 30 fps a frame IS a step: those run four times as fast at 120 fps. Each of
 * these constants has exactly ONE reader in the whole game (searched in the lifted code by its TOC offset),
 * and that reader adds it to an accumulator, so writing "constant * this frame's speed" into it every frame
 * makes the step real time and leaves everything else alone. At 30 fps the speed is 1 and nothing changes.
 * A constant that is a threshold and not a step (0xBF526C, compared and subtracted in 0x10DE3C) is NOT here.
 * REFINE_RT_CONST=0 turns the scaling off. */
struct RtConst { uint32_t addr; const char* who; const char* what; float orig; };
RtConst kConst[] = {
    { 0xBF35D4, "0x99294 (20CsExplainInformation::Ctrl)", "1,5 a frame off the scrolling text of the menus", 0 },
    { 0xBFD34C, "0x424850 (8TaskWind)", "1/30 a frame onto the wind's ramp (+0x48), which pushes hair and cloth", 0 },
};
int g_const_on = 1;

/* ---- The song task while its song loads (2026-11-22, Rin-chan Now!) ------------------------------------------
 * In Rin-chan Now! (PV 641) the two singers stood at the stage's origin in the idle loop (frames 0-88) for the
 * whole song at 100 fps and over -- with the phones of the PV floating where their hands should have been --
 * and were right at 30, 60, 75 and 90 (REFINE_ROB_TRACE, character 0 at 2.9 s: -0.38 1.70 0.65 frame 174 at 30,
 * 0.00 1.13 -0.01 frame 78 at 120, work/run/rs120). 90 fps is exactly 3 frames a tick and 100 is sometimes 4:
 * something the song task (7EEDBE2110TaskPvGame) does every frame while the song loads is read by the
 * character tasks, which run on the ticks, and gets lost. With the song task's Ctrl on the ticks only until
 * the song clock (0x12CBE48) passes 0.05 s the singers are where they are at 30 (-0.38 1.70 0.65, frame 174,
 * run rk3); with it on the ticks the whole song too (rk2), but then the notes would go at 30 Hz.
 * So: on the ticks from when the task appears, or its clock goes back (a retry), until the clock moves past
 * 0.05 s. A clock still showing the last song does not count as moving. REFINE_PV_LOAD_TICK=0 turns it off. */
const uint32_t PV_GAME_VT = 0xBB3B98;
const uint32_t SONG_CLOCK = 0x12CBE48;
int      g_pv_load_on = 1;
uint32_t g_pv_code;                            /* its Ctrl */
uint32_t g_frame_no, g_pv_seen;                /* frames (realtime_frame) and the last one the task ran in */
float    g_pv_last;
int      g_pv_loading;

int g_active;                                  /* real time on and over 30 fps, this frame */
std::unordered_map<uint32_t, ppu_fn> g_orig;   /* Ctrl code address -> the lifted function */
struct { uint32_t run, skipped; } g_st;

uint32_t rd(uint32_t a) { return _byteswap_ulong(*(const uint32_t*)(vm_base + a)); }

int pv_loading(void)                           /* once per call of the song task's Ctrl, at any frame rate */
{
    const uint32_t w = rd(SONG_CLOCK);
    float song; memcpy(&song, &w, 4);
    if (g_pv_seen + 1 < g_frame_no || song < g_pv_last - 0.001f) g_pv_loading = 1;   /* a new task, a retry */
    else if (g_pv_loading && song > 0.05f && song > g_pv_last) g_pv_loading = 0;
    g_pv_seen = g_frame_no;
    g_pv_last = song;
    return g_pv_loading;
}

void hook_tick_ctrl(ppu_context* ctx)
{
    const uint32_t task = (uint32_t)ctx->gpr[3];
    const uint32_t code = rd(rd(rd(task) + 0xC));
    const auto it = g_orig.find(code);
    if (it == g_orig.end()) { fprintf(stderr, "[realtime] no original for Ctrl 0x%X%c", code, 10); ctx->gpr[3] = 0; return; }
    uint32_t keep; memcpy(&keep, vm_base + SPEED, 4);
    if (code == g_pv_code && !pv_loading()) { it->second(ctx); return; }   /* the song playing: every frame */
    if (!g_active) { it->second(ctx); return; }                  /* 30 fps: as it is */
    if (g_fps_not_tick) { g_st.skipped++; ctx->gpr[3] = 0; return; }   /* 0: the task goes on */
    /* The game stopped (speed 0): the pause menu, a song loading. Until 2026-11-21 these tasks then ran every frame:
     * the pause menu (16TaskPauseManager), whose object counts frames (+0x44, +0x48, +0x4C in REFINE_EVT_SCAN), went
     * on at the frame rate while its 2D waited for the ticks (g_fps_aet_hold). Now it keeps the ticks too, with the
     * game's own speed (0), and the pad is held between ticks meanwhile (src/main.cpp, stopped_not_tick). */
    if (keep == 0) { it->second(ctx); g_st.run++; return; }
    const uint32_t one = _byteswap_ulong(0x3F800000u);
    memcpy(vm_base + SPEED, &one, 4);
    it->second(ctx);
    memcpy(vm_base + SPEED, &keep, 4);
    g_st.run++;
}

uint32_t add_tick(uint32_t vt, const char* name)
{
    if (vt < 0x10000u || vt >= 0x2000000u) return 0;
    const uint32_t opd = rd(vt + 0xC), code = rd(opd);
    if (g_orig.count(code)) return 0;
    const ppu_fn fn = ppu_lookup_function(code);
    if (!fn) { fprintf(stderr, "[realtime] vtable 0x%X: Ctrl 0x%X is not a lifted function%c", vt, code, 10); return 0; }
    g_orig[code] = fn;
    ppu_register_function(code, hook_tick_ctrl);
    fprintf(stderr, "[realtime] task %s (vtable 0x%X): Ctrl 0x%X on 30 Hz ticks%c", name ? name : "?", vt, code, 10);
    return code;
}

}  // namespace

/* After the lifted table is registered (src/main.cpp), with the known EBOOT. */
extern "C" void realtime_init(void)
{
    /* REFINE_RT_TICK=<vtable hex>,... (or +) adds to the table above; "none" empties it; "only:<list>" replaces it
     * (to find which task breaks a boot: docs/tiempo_real.md). */
    { const char* c = getenv("REFINE_RT_CONST"); g_const_on = !(c && *c == '0'); }
    const char* e = getenv("REFINE_RT_TICK");
    const int only = e && !strncmp(e, "only:", 5);
    if (only) e += 5;
    if (!only && !(e && !strcmp(e, "none")))
        for (const TickTask& t : kTick) if (t.vt) add_tick(t.vt, t.name);
    { const char* c = getenv("REFINE_PV_LOAD_TICK"); g_pv_load_on = !(c && *c == '0'); }
    if (g_pv_load_on && !(e && !strcmp(e, "none"))) g_pv_code = add_tick(PV_GAME_VT, "7EEDBE2110TaskPvGame, while its song loads");
    if (e && strcmp(e, "none"))
        for (const char* p = e; *p; ) {
            char* end = nullptr;
            const uint32_t vt = (uint32_t)strtoul(p, &end, 16);
            if (end == p) break;
            add_tick(vt, "REFINE_RT_TICK");
            p = (*end == ',' || *end == '+') ? end + 1 : end;   /* '+': tools/run.ps1 -Env splits on commas */
        }
}

/* Once a frame on the game's thread, before its logic (fps_guest_frame). speed = this frame's share of a
 * 1/30 s step (1.0 at 30 fps), what the game's own 0x3BC0C returns. */
extern "C" void realtime_frame(int active, float speed)
{
    g_active = active;
    g_frame_no++;
    if (!g_const_on || !(speed > 0.0f) || speed > 1.0f) return;
    for (RtConst& c : kConst) {
        if (!c.orig) {                                  /* the first frame keeps the game's own value */
            uint32_t w = rd(c.addr); float f; memcpy(&f, &w, 4);
            if (!(f > 0.0f) || f > 1e6f) { c.orig = -1.0f; continue; }
            c.orig = f;
            fprintf(stderr, "[realtime] %s: %s, %g a frame -> por tiempo real%c", c.who, c.what, (double)f, 10);
        }
        if (c.orig < 0) continue;
        const float v = c.orig * speed;
        uint32_t w; memcpy(&w, &v, 4); w = _byteswap_ulong(w);
        memcpy(vm_base + c.addr, &w, 4);
    }
}
