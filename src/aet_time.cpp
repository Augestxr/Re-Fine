/* The 2D objects (AET) end and loop on the original's steps at any frame rate (2026-11-22).
 *
 * 0x1416C steps one object a frame: frame (+0x74) += rate (+0x60) * the scene's fps (*(+8) + 0xC) / 30 * the game speed,
 * and when the frame goes past the last one (+0x18) the object ends, or goes back to its first (+0x14) when it loops.
 * At 30 fps the step is 2 frames of a 60 fps animation, so an object always lives a whole number of steps: one of 29
 * frames ends on step 15, 500 ms. At 120 fps the step is a half and it ended after 58 of them, 483 ms, and a loop lost
 * what it ran past the end. Each life was up to a step short, and whatever is started again when it ends, and every
 * loop, gained on the song: 35 turns of one of the HUD's in Cat Food were 318 ms early (tools/hud_cmp.py, runs hs30 and
 * hs120). So an object that would end waits, on the last frame it reached, for the step it ends on at 30 fps, counted
 * from where it began. Those the game does not scale by its speed (flag 0x400000) are scaled too while
 * the speed is ours (the g_fps_high patch), so they are no exception. REFINE_AET_END=0 turns it off. Nothing at 30 fps. */
#include "ppu_recomp.h"
#include "win32_compat.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unordered_map>
#include <vector>

extern "C" int g_fps_high;

namespace {

const uint32_t SPEED = 0xC22E70;           /* +0x14 the speed, +0x20 a second factor is on, +0xEC step twice */
enum { REVERSE = 2, PAUSED = 4, LOOP = 0x10000 };

struct State { uint32_t node, seen; float after, first, last, step; int rev; double base, run; };
std::unordered_map<uint32_t, State> g_state;   /* by the object's key in the manager */
struct Moved { uint32_t key, obj; };
std::vector<Moved> g_moved;                /* stepped in this pass */
std::vector<uint32_t> g_held;              /* objects paused for this pass */
std::vector<uint32_t> g_nodes;
uint32_t g_pass;
int g_on = -1;

uint32_t rd(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return _byteswap_ulong(v); }
void wr(uint32_t a, uint32_t v) { v = _byteswap_ulong(v); memcpy(vm_base + a, &v, 4); }
float rdf(uint32_t a) { const uint32_t v = rd(a); float f; memcpy(&f, &v, 4); return f; }
bool ptr_ok(uint32_t a) { return a >= 0x10000u && a < 0x40000000u; }

/* The manager's std::map (task + 0x60): a node has +0 left, +8 right, +0xC key, +0x10 the object, +0x1D5 nil. */
void walk(uint32_t node, uint32_t head, int depth)
{
    if (!ptr_ok(node) || node == head || depth > 40 || g_nodes.size() > 4000 || vm_base[node + 0x1D5]) return;
    walk(rd(node), head, depth + 1);
    g_nodes.push_back(node);
    walk(rd(node + 8), head, depth + 1);
}

} // namespace

/* Before the manager's pass (0x172CC). */
extern "C" void aet_before(uint32_t mgr)
{
    if (g_on < 0) { const char* e = getenv("REFINE_AET_END"); g_on = !(e && *e == '0'); }
    g_moved.clear();
    /* A long frame steps up to a whole 30 fps step (speed 1): counted like the others, or every object would start its
     * count again on each one. */
    const float speed = rdf(SPEED + 0x14);
    if (!g_on || !g_fps_high || !(speed > 0.0f && speed <= 1.0f) || vm_base[SPEED + 0x20]) {
        if (!g_fps_high && !g_state.empty()) g_state.clear();
        return;
    }
    const uint32_t head = rd(mgr + 0x60);
    if (!ptr_ok(head)) return;
    g_pass++;
    g_nodes.clear();
    walk(rd(head + 4), head, 0);
    const float twice = vm_base[SPEED + 0xEC] ? 2.0f : 1.0f;
    for (uint32_t n : g_nodes) {
        const uint32_t key = rd(n + 0xC), o = n + 0x10, flags = rd(o + 0x1C);
        if (flags & PAUSED) continue;
        const uint32_t scene = rd(o + 8);
        const float step = ptr_ok(scene) ? rdf(o + 0x60) * rdf(scene + 0xC) / 30.0f * twice : 0.0f;
        if (!(step > 0.0f && step < 1000.0f)) continue;
        const float cur = rdf(o + 0x74), first = rdf(o + 0x14), last = rdf(o + 0x18);
        const int rev = (flags & REVERSE) != 0;
        auto it = g_state.find(key);
        if (it == g_state.end()) it = g_state.emplace(key, State{ ~0u, 0, 0, 0, 0, 0, 0, 0, 0 }).first;
        State& s = it->second;
        /* New, started again or moved by the game: it counts from here. */
        if (s.node != n || s.after != cur || s.first != first || s.last != last || s.step != step || s.rev != rev) {
            s.node = n; s.first = first; s.last = last; s.step = step; s.rev = rev; s.after = cur;
            s.base = cur; s.run = 0;
        }
        s.seen = g_pass;
        const double ds = (double)step * speed, next = rev ? cur - ds : cur + ds;
        s.run += ds;
        if (rev ? next < first : next > last) {
            const double way = rev ? s.base - first : last - s.base;
            const double due = way < 0 ? 0 : (floor(way / step) + 1) * step;      /* the 30 fps step that goes past it */
            if (s.run < due - 0.5 * ds) { wr(o + 0x1C, flags | PAUSED); g_held.push_back(o); continue; }
        }
        g_moved.push_back({ key, o });
    }
}

/* After it. */
extern "C" void aet_after(void)
{
    for (uint32_t o : g_held) wr(o + 0x1C, rd(o + 0x1C) & ~(uint32_t)PAUSED);
    g_held.clear();
    for (const Moved& m : g_moved) {
        auto it = g_state.find(m.key);
        if (it == g_state.end()) continue;
        State& s = it->second;
        const float now = rdf(m.obj + 0x74);
        const bool wrapped = (rd(m.obj + 0x1C) & LOOP) && (s.rev ? now > s.after : now < s.after);
        if (wrapped) { s.base = now; s.run = 0; }
        s.after = now;
    }
    if ((g_pass & 0x3FF) == 0x3FF)             /* the objects that are gone */
        for (auto it = g_state.begin(); it != g_state.end(); )
            if (g_pass - it->second.seen > 0x200) it = g_state.erase(it); else ++it;
}

/* What the curves show between the original's frames (2026-11-22).
 *
 * At 30 fps an object of a 60 fps animation is only ever seen on every second frame of it, and the animations carry
 * things on the frames between that nobody sees there: one of the song list's layers (0xD0 of its scene, 223-253, a loop
 * of 0.53 s) has a key on 224 that lifts every row 5 pixels, and the game shows 223, 225, 227... At 240 fps the rows
 * jumped twice a second (run ml240: 15 jumps in 8 s, 0 at 30 fps). So between two of the original's frames a curve that
 * has a key in there goes straight from the value on the one to the value on the next. A curve with no key between them
 * is the original's own, so everything else moves as finely as before. Past the last of them before the object ends or
 * loops it stays on that value, as the original does. REFINE_AET_KEYS=0 turns it off.
 *
 * 0x12D2C gives the value of a curve (r3: +0 count, +4 the keys' frames, then value and tangent of each) at a frame (f1).
 * It is called directly, so it is a function of config/func_hooks.json. The object being worked out is known from the
 * two methods that work out one layer, to draw it (0x152E0) or to tell the game where it is (0x17B1C): r3 the object,
 * r5 the layer, whose own frame, for what is inside it, is (f1 - +4) * +0x10 + +0xC. */
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

namespace {

struct Eval { uint32_t obj; double delta, step, left, scale; int depth; };
Eval g_eval;                                   /* the game's thread only; obj 0 = nothing known */
double g_child_scale = 1.0;
int g_keys_on = -1;
struct { uint32_t known, copies, unknown, bent; } g_kst;

/* The object's place between two frames of the original; false = draw it as it is. */
bool eval_set(Eval& e, uint32_t o)
{
    e.obj = o; e.delta = 0; e.step = 0; e.left = 0;
    if (!ptr_ok(o)) return false;
    auto it = g_state.find(rd(o - 4));
    if (it == g_state.end() || it->second.node != o - 0x10) {
        /* A copy made to ask where things are (0x17B1C): the object of the same layer at the same frame. */
        const uint32_t layer = rd(o + 0x10), frame = rd(o + 0x74);
        it = g_state.end();
        for (uint32_t n : g_nodes)
            if (rd(n + 0x20) == layer && rd(n + 0x84) == frame) { it = g_state.find(rd(n + 0xC)); if (it != g_state.end()) break; }
        if (it == g_state.end()) { g_kst.unknown++; return false; }
        g_kst.copies++;
    } else g_kst.known++;
    const State& s = it->second;
    const double cur = rdf(o + 0x74), gone = fabs(cur - s.base);
    if (!(s.step > 0.0f)) return false;
    double d = gone - floor(gone / s.step + 1e-3) * s.step;
    if (d < 1e-3 * s.step) d = 0;
    e.delta = s.rev ? -d : d;
    e.step = s.rev ? -s.step : s.step;
    e.left = s.rev ? (cur + d) - s.first : s.last - (cur - d);   /* frames from the original's last one to the end */
    return true;
}

void in_layer(ppu_context* ctx, void (*fn)(ppu_context*))
{
    const uint32_t o = (uint32_t)ctx->gpr[3], layer = (uint32_t)ctx->gpr[5];
    const Eval was = g_eval;
    const double was_child = g_child_scale;
    if (g_eval.obj != o) { eval_set(g_eval, o); g_eval.scale = 1.0; }
    else g_eval.scale = g_child_scale;
    g_child_scale = g_eval.scale * (ptr_ok(layer) ? (double)rdf(layer + 0x10) : 1.0);
    fn(ctx);
    g_eval = was;
    g_child_scale = was_child;
}
void hook_layer(ppu_context* ctx) { in_layer(ctx, func_000152E0); }     /* drawn */
void hook_place(ppu_context* ctx) { in_layer(ctx, func_00017B1C); }     /* asked where it is (the rows of a list) */

} // namespace

void func_00012D2C(ppu_context* ctx)
{
    const Eval& e = g_eval;
    if (!e.obj || e.delta == 0.0 || !g_fps_high) { func_00012D2C__lifted(ctx); return; }
    const uint32_t curve = (uint32_t)ctx->gpr[3];
    const int32_t n = (int32_t)rd(curve);
    if (n < 2) { func_00012D2C__lifted(ctx); return; }
    const double t = ctx->fpr[1], t0 = t - e.delta * e.scale, t1 = t0 + e.step * e.scale;
    /* Is there a key strictly between the two? */
    const uint32_t keys = rd(curve + 4);
    const double lo = t0 < t1 ? t0 : t1, hi = t0 < t1 ? t1 : t0;
    int a = 0, b = n;                                          /* first key past lo */
    while (a < b) { const int m = (a + b) / 2; if ((double)rdf(keys + 4 * m) > lo + 1e-3) b = m; else a = m + 1; }
    if (a >= n || (double)rdf(keys + 4 * a) >= hi - 1e-3) { func_00012D2C__lifted(ctx); return; }
    g_kst.bent++;
    ctx->fpr[1] = t0;
    func_00012D2C__lifted(ctx);
    const double v0 = ctx->fpr[1];
    if (fabs(e.step) > e.left + 1e-3) return;                  /* the original's next frame is past the end: it stays */
    ctx->gpr[3] = curve;
    ctx->fpr[1] = t1;
    func_00012D2C__lifted(ctx);
    ctx->fpr[1] = (double)(float)(v0 + (ctx->fpr[1] - v0) * (e.delta / e.step));
}

extern "C" void aet_time_init(void)
{
    const char* e = getenv("REFINE_AET_KEYS");
    g_keys_on = !(e && *e == '0');
    if (!g_keys_on) return;
    ppu_register_function(0x152E0, hook_layer);
    ppu_register_function(0x17B1C, hook_place);
}

extern "C" void aet_log(void)
{
    fprintf(stderr, "[aet-keys] objects known %u, copies %u, others %u, curves bent %u%c", g_kst.known, g_kst.copies, g_kst.unknown, g_kst.bent, 10);
}
