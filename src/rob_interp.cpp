/* rob_interp.cpp - Re:Fine 6d (option C): the character's pose between two 30 Hz ticks.
 *
 * Over 30 fps the character's tasks (the ones 0x4143F0 flags: TaskRobPrepareAction,
 * TaskRobBase, TaskRobMotionModifier, TaskRobCollision) run on the game's own whole speed
 * steps, as in the original: 30 times a second at any frame rate (0x4BF40, steps at
 * 0xC22E70 + 0x1C). Their result is each character's skeleton, model-space 3x4 matrices
 * in two vectors of rob_chara_bone_data (+0x1C: object bones, +0x2C: the rest), and its
 * place in the world (rob_chara + 0x1F64, copied to the equipment in TaskRobDisp::Disp).
 * Everything that draws reads them every frame: TaskRobDisp::Ctrl (0x3FFCC8) runs the
 * equipment, hair and cloth physics from them, and TaskRobDisp::Disp hands the meshes
 * pointers into them (STATUS 6d.10).
 *
 * So every frame, just before TaskRobDisp::Ctrl, this writes a pose blended between the
 * last two ticks, and puts the tick's true pose back when the frame ends, before any
 * logic reads it again. It is state interpolation: every frame is still drawn by the
 * game's code. The blend is position and scale lerp and rotation nlerp, per bone.
 *   alpha = fraction of a speed step since the last tick + this frame's step, at most 1:
 *   the pose shown trails the newest tick by (1 - speed) of a tick (25 ms at 120 fps).
 * No blend, straight to the new pose, where it would show a pose that never existed:
 * the first tick of a character, a new skeleton, a write outside the ticks, and a jump
 * (any bone over ROB_JUMP_M or ROB_JUMP_DEG in one tick: cuts and teleports).
 *
 * Off at fps_target=30 (nothing is registered) and with REFINE_ROB_INTERP=0 (the
 * character then steps at 30 Hz). REFINE_ROB_INTERP_STATS=1 prints counters every 5 s.
 * Hooks through ppu_register_function, the runtime's address map for indirect calls:
 * both methods are only reached through their vtables. No generated code is edited. */

#include "ppu_recomp.h"
#include "win32_compat.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <unordered_map>
#include <vector>

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" void rsx_d3d12_dump_arm(void);    /* patches/0156: CELLMARK_DUMP_ARMED */
extern "C" int port_in_song(void);           /* src/main.cpp: the game's pv_game task is alive */

namespace {

const uint32_t SPEED_BASE      = 0xC22E70;   /* +0x14 this frame's speed, +0x18 fraction, +0x1C steps */
const uint32_t ROBDISP_CTRL    = 0x3FFCC8;   /* TaskRobDisp vtable 0xBB1888, slot 3 */
const uint32_t ROBPREPARE_CTRL = 0x428ACC;   /* TaskRobPrepareAction vtable 0xBB20D0, slot 3: the first flagged task */
const int      MAX_CHARA = 8;
const int      MAX_MATS  = 256;
const float    ROB_JUMP_M   = 0.7f;          /* metres a bone may move in one tick: dance up to 0.48, cuts from 0.89 (6d.10) */
const float    ROB_JUMP_DEG = 100.0f;        /* degrees a bone may turn in one tick: dance up to 82, cuts 88-180 */
/* The game's camera (6d.11, found in memory dumps of Cat Food): view point at +0, interest
 * at +0xC, fov in degrees at +0x1C; view matrix at 0xC0ABA4, its inverse at 0xC0ABE4. */
const uint32_t CAM_BASE     = 0xC0AB58;

struct Slot {
    uint32_t chara, bd, m1, n1, m2, n2;      /* guest addresses and matrix counts */
    int      used, has_cur, valid, pending;
    int      valid2;                         /* the tick before (prev2 -> prev) was a move too, not a cut */
    int      hold;                           /* a tick ahead (g_lead): the pose of after a cut came in on this tick */
    uint32_t ob_lo, ob_hi;                   /* this character's equipment objects */
    uint32_t seen;
    uint8_t  cur[(MAX_MATS + 1) * 64];       /* the tick's true pose, guest bytes (big endian) */
    uint8_t  prev[(MAX_MATS + 1) * 64];
    uint8_t  prev2[(MAX_MATS + 1) * 64];     /* the pose of the tick before prev */
    uint8_t  wrote[(MAX_MATS + 1) * 64];     /* what the blend put in guest memory */
};

Slot     g_slot[MAX_CHARA];
void held_objects(Slot& sl);
extern int g_lead;                           /* the characters a tick ahead (below) */
int      g_ticked;                           /* the flagged tasks ran this frame */
int      g_cam_snap;                         /* a camera cut lands on this tick (cam_frame) */
uint32_t g_frame;
int      g_stats;
int      g_hair_every;                         /* REFINE_HAIR_EVERY=1 (test, 6d.13 Q2): step flag every frame */
int      g_blend = 1;                         /* REFINE_ROB_BLEND=0 (diagnostic): hooks and dumps, no blend */
int      g_dump_frames, g_dump_n;            /* frames to log after REFINE_DUMP_AT armed the dump */

struct Stats {
    uint32_t frames, ticks, blends, full, snap_first, snap_layout, snap_jump, snap_outside,
             overwritten, late, skipped;
    float    max_m, max_deg;
} g_st;

inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return _byteswap_ulong(v); }
inline float rdf(uint32_t a) { const uint32_t b = rd32(a); float f; memcpy(&f, &b, 4); return f; }
inline float bef(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); v = _byteswap_ulong(v); float f; memcpy(&f, &v, 4); return f; }
inline void  wbef(uint8_t* p, float f) { uint32_t v; memcpy(&v, &f, 4); v = _byteswap_ulong(v); memcpy(p, &v, 4); }

/* The guest bytes of a slot: n1 object bones, n2 more, then the world matrix. */
inline size_t slot_bytes(const Slot& s) { return (size_t)(s.n1 + s.n2 + 1) * 64; }

void read_pose(const Slot& s, uint8_t* out)
{
    memcpy(out, vm_base + s.m1, (size_t)s.n1 * 64);
    memcpy(out + (size_t)s.n1 * 64, vm_base + s.m2, (size_t)s.n2 * 64);
    memcpy(out + (size_t)(s.n1 + s.n2) * 64, vm_base + s.chara + 0x1F64, 64);
}

void write_pose(const Slot& s, const uint8_t* in)
{
    memcpy(vm_base + s.m1, in, (size_t)s.n1 * 64);
    memcpy(vm_base + s.m2, in + (size_t)s.n1 * 64, (size_t)s.n2 * 64);
    memcpy(vm_base + s.chara + 0x1F64, in + (size_t)(s.n1 + s.n2) * 64, 64);
}

/* Row-major 4x4, translation in [3], [7], [11]: v' = R*S*v + t. */
struct Xf { float q[4], t[3], s[3]; int ok; };

void decompose(const float* m, Xf& x)
{
    x.t[0] = m[3]; x.t[1] = m[7]; x.t[2] = m[11];
    float c[3][3];                                   /* columns of R*S */
    for (int j = 0; j < 3; j++) {
        c[j][0] = m[j]; c[j][1] = m[4 + j]; c[j][2] = m[8 + j];
        x.s[j] = sqrtf(c[j][0] * c[j][0] + c[j][1] * c[j][1] + c[j][2] * c[j][2]);
    }
    x.ok = x.s[0] > 1e-6f && x.s[1] > 1e-6f && x.s[2] > 1e-6f && m[12] == 0.0f && m[13] == 0.0f &&
           m[14] == 0.0f && m[15] == 1.0f;
    if (!x.ok) return;
    float r[3][3];                                   /* r[row][col] */
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) r[i][j] = c[j][i] / x.s[j];
    const float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                      r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                      r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    if (det < 0.5f) { x.ok = 0; return; }            /* mirrored or sheared: lerp the elements */
    const float tr = r[0][0] + r[1][1] + r[2][2];
    float w, a, b, d;
    if (tr > 0) {
        const float k = sqrtf(tr + 1.0f) * 2;
        w = 0.25f * k; a = (r[2][1] - r[1][2]) / k; b = (r[0][2] - r[2][0]) / k; d = (r[1][0] - r[0][1]) / k;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float k = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2;
        w = (r[2][1] - r[1][2]) / k; a = 0.25f * k; b = (r[0][1] + r[1][0]) / k; d = (r[0][2] + r[2][0]) / k;
    } else if (r[1][1] > r[2][2]) {
        const float k = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2;
        w = (r[0][2] - r[2][0]) / k; a = (r[0][1] + r[1][0]) / k; b = 0.25f * k; d = (r[1][2] + r[2][1]) / k;
    } else {
        const float k = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2;
        w = (r[1][0] - r[0][1]) / k; a = (r[0][2] + r[2][0]) / k; b = (r[1][2] + r[2][1]) / k; d = 0.25f * k;
    }
    const float n = sqrtf(w * w + a * a + b * b + d * d);
    x.q[0] = w / n; x.q[1] = a / n; x.q[2] = b / n; x.q[3] = d / n;
}

void compose(const Xf& x, float* m)
{
    const float w = x.q[0], a = x.q[1], b = x.q[2], d = x.q[3];
    const float r[3][3] = {
        {1 - 2 * (b * b + d * d), 2 * (a * b - d * w), 2 * (a * d + b * w)},
        {2 * (a * b + d * w), 1 - 2 * (a * a + d * d), 2 * (b * d - a * w)},
        {2 * (a * d - b * w), 2 * (b * d + a * w), 1 - 2 * (a * a + b * b)}};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[4 * i + j] = r[i][j] * x.s[j];
        m[4 * i + 3] = x.t[i];
    }
    m[12] = 0; m[13] = 0; m[14] = 0; m[15] = 1;
}

void load16(const uint8_t* p, float* m) { for (int i = 0; i < 16; i++) m[i] = bef(p + 4 * i); }

/* Largest move and turn of any bone between two poses (the world matrix included). */
int g_jump_bone_m, g_jump_bone_deg;              /* which bone gave each maximum (diagnostic) */

void pose_jump(const uint8_t* a, const uint8_t* b, int n, float& max_m, float& max_deg)
{
    max_m = 0; max_deg = 0; g_jump_bone_m = -1; g_jump_bone_deg = -1;
    for (int k = 0; k < n; k++) {
        float ma[16], mb[16];
        load16(a + 64 * k, ma); load16(b + 64 * k, mb);
        Xf xa, xb;
        decompose(ma, xa); decompose(mb, xb);
        const float dx = mb[3] - ma[3], dy = mb[7] - ma[7], dz = mb[11] - ma[11];
        const float dm = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dm > max_m) { max_m = dm; g_jump_bone_m = k; }
        if (xa.ok && xb.ok) {
            float dot = fabsf(xa.q[0] * xb.q[0] + xa.q[1] * xb.q[1] + xa.q[2] * xb.q[2] + xa.q[3] * xb.q[3]);
            if (dot > 1) dot = 1;
            const float deg = 2 * acosf(dot) * 57.29578f;
            if (deg > max_deg) { max_deg = deg; g_jump_bone_deg = k; }
        }
    }
}

void blend_pose(const uint8_t* a, const uint8_t* b, int n, float t, uint8_t* out)
{
    for (int k = 0; k < n; k++) {
        float ma[16], mb[16], mo[16];
        load16(a + 64 * k, ma); load16(b + 64 * k, mb);
        Xf xa, xb, xo;
        decompose(ma, xa); decompose(mb, xb);
        if (xa.ok && xb.ok) {
            float dot = xa.q[0] * xb.q[0] + xa.q[1] * xb.q[1] + xa.q[2] * xb.q[2] + xa.q[3] * xb.q[3];
            const float sg = dot < 0 ? -1.0f : 1.0f;
            float n2 = 0;
            for (int i = 0; i < 4; i++) { xo.q[i] = (1 - t) * xa.q[i] + t * sg * xb.q[i]; n2 += xo.q[i] * xo.q[i]; }
            n2 = sqrtf(n2);
            for (int i = 0; i < 4; i++) xo.q[i] /= n2;
            for (int i = 0; i < 3; i++) { xo.t[i] = xa.t[i] + t * (xb.t[i] - xa.t[i]); xo.s[i] = xa.s[i] + t * (xb.s[i] - xa.s[i]); }
            compose(xo, mo);
        } else {
            for (int i = 0; i < 16; i++) mo[i] = ma[i] + t * (mb[i] - ma[i]);
        }
        for (int i = 0; i < 16; i++) wbef(out + 64 * k + 4 * i, mo[i]);
    }
}

/* 2026-11-11: the pose between ticks on a quadratic B-spline of the last three tick poses, not a straight line
 * between the last two. On straight lines the speed of every bone changes at once on each tick: the body looks
 * right, but the hair and cloth simulation (every frame, driven by the skeleton) felt all the acceleration of a
 * tick in one frame. Measured with REFINE_PHYS_LOG=1 in Cat Food at 120 fps (runs hj1/hj0): the nodes' jerk on
 * tick frames was 4.3-4.4 times the other frames', with or without the anchor. The spline keeps speed
 * continuous and spreads the acceleration over the frames; it runs from the midpoint of p0, p1 to the midpoint
 * of p1, p2 (half a tick later than the straight line). u = alpha (0..1, 1 included). p0 = p1 after a cut. */
inline void bspline_w(float u, float w[3]) { w[0] = 0.5f * (1 - u) * (1 - u); w[1] = 0.5f + u - u * u; w[2] = 0.5f * u * u; }

/* 2026-11-12: with the hair and cloth stepped on the ticks and blended like the skeleton (REFINE_HAIR_EX), the
 * physics no longer feels the drawn pose, and the B-spline's cost is all that is left of it: it never reaches a
 * tick's pose (fast moves come out rounded) and it trails the newest tick by a tick and a half at the start of
 * each interval (measured against 30 fps: the character 50 ms late, hair_lag.py). So the drawn pose is now a
 * cubic through the tick poses: from p1 to p2, leaving p1 with the slope (p2 - p0) / 2 and reaching p2 with
 * the slope p2 - p1 (the next tick is not known yet). It passes through every tick's pose and trails the
 * newest by one tick at most, none at the end. REFINE_ROB_CURVE=0: the B-spline. */
/* 2026-11-21, tried and left out: leaving p1 with the slope the last interval arrived with (p1 - p0), which makes
 * the velocity continuous across ticks. Measured with tools/hair_smooth.py (Cat Food 42-50 s at 120 fps) it changed
 * nothing that rises above the noise between two runs of the same build, so the cubic below stays. */
int g_curve = 1;
inline void curve_w(float u, float w[3])
{
    if (!g_curve) { bspline_w(u, w); return; }
    const float u2 = u * u, u3 = u2 * u, h = (u3 - 2 * u2 + u) * 0.5f;
    w[0] = -h;
    w[1] = (2 * u3 - 3 * u2 + 1) - (u3 - u2);
    w[2] = (-2 * u3 + 3 * u2) + h + (u3 - u2);
}

void blend_pose3(const uint8_t* a, const uint8_t* b, const uint8_t* c, int n, float u, uint8_t* out)
{
    float w[3]; curve_w(u, w);
    for (int k = 0; k < n; k++) {
        float ma[16], mb[16], mc[16], mo[16];
        load16(a + 64 * k, ma); load16(b + 64 * k, mb); load16(c + 64 * k, mc);
        Xf xa, xb, xc, xo;
        decompose(ma, xa); decompose(mb, xb); decompose(mc, xc);
        if (xa.ok && xb.ok && xc.ok) {
            const float da = xa.q[0] * xb.q[0] + xa.q[1] * xb.q[1] + xa.q[2] * xb.q[2] + xa.q[3] * xb.q[3];
            const float dc = xc.q[0] * xb.q[0] + xc.q[1] * xb.q[1] + xc.q[2] * xb.q[2] + xc.q[3] * xb.q[3];
            const float sa = da < 0 ? -1.0f : 1.0f, sc = dc < 0 ? -1.0f : 1.0f;
            float n2 = 0;
            for (int i = 0; i < 4; i++) { xo.q[i] = w[0] * sa * xa.q[i] + w[1] * xb.q[i] + w[2] * sc * xc.q[i]; n2 += xo.q[i] * xo.q[i]; }
            n2 = sqrtf(n2);
            for (int i = 0; i < 4; i++) xo.q[i] /= n2;
            for (int i = 0; i < 3; i++) {
                xo.t[i] = w[0] * xa.t[i] + w[1] * xb.t[i] + w[2] * xc.t[i];
                xo.s[i] = w[0] * xa.s[i] + w[1] * xb.s[i] + w[2] * xc.s[i];
            }
            compose(xo, mo);
        } else {
            for (int i = 0; i < 16; i++) mo[i] = w[0] * ma[i] + w[1] * mb[i] + w[2] * mc[i];
        }
        for (int i = 0; i < 16; i++) wbef(out + 64 * k + 4 * i, mo[i]);
    }
}

/* Put back the tick's pose where the blend still is. Anything else in memory now was
 * written by the game after the blend (or the character went away): it stays. */
void restore_all(int late)
{
    for (Slot& s : g_slot) {
        if (!s.used || !s.pending) continue;
        s.pending = 0;
        const size_t nb = slot_bytes(s);
        uint8_t now[(MAX_MATS + 1) * 64];
        read_pose(s, now);
        if (memcmp(now, s.wrote, nb) != 0) { g_st.overwritten++; s.has_cur = 0; continue; }
        write_pose(s, s.cur);
        if (late) g_st.late++;
    }
}

Slot* slot_for(uint32_t chara)
{
    Slot* free_s = nullptr;
    for (Slot& s : g_slot) {
        if (s.used && s.chara == chara) return &s;
        if (s.used && g_frame - s.seen > 4) s.used = 0;   /* a character gone for a while */
        if (!s.used && !free_s) free_s = &s;
    }
    if (!free_s) return nullptr;
    memset(free_s, 0, offsetof(Slot, cur));
    free_s->used = 1; free_s->chara = chara;
    return free_s;
}

/* REFINE_FIND_FRAME=t (diagnostic, 6d.13 Q1): on the ticks of the half second from song t,
 * print the floats of the character (and one pointer level down from its first 0x400 bytes)
 * that grew by about 1, 2 or 0.5 since the last tick: candidates for its motion frame. */
float    g_find_at = -1;
float    g_probe_at = -1;
std::unordered_map<uint32_t, float> g_find_prev;

void find_frame(uint32_t chara)
{
    const float song = rdf(0x12CBE48);
    if (g_find_at < 0 || song < g_find_at || song > g_find_at + 0.5f) return;
    uint32_t bases[257]; int nb = 0;
    bases[nb++] = chara;
    for (uint32_t o = 0; o < 0x400 && nb < 257; o += 4) {
        const uint32_t p = rd32(chara + o);
        if (p >= 0x10000 && p < 0x0F000000 && !(p & 3)) bases[nb++] = p;
    }
    int printed = 0;
    for (int b = 0; b < nb; b++)
        for (uint32_t o = 0; o < (b ? 0x1000u : 0x4000u); o += 4) {
            const uint32_t a = bases[b] + o;
            const float v = rdf(a);
            if (!(v >= 1.0f && v <= 20000.0f)) continue;
            auto it = g_find_prev.find(a);
            if (it != g_find_prev.end() && printed < 60) {
                const float d = v - it->second;
                if ((d > 0.45f && d < 0.55f) || (d > 0.9f && d < 1.1f) || (d > 1.9f && d < 2.1f)) {
                    fprintf(stderr, "[find-frame] song %.3f chara %08X base %d addr %08X %.3f -> %.3f d=%.3f\n", song, chara,
                            b, a, it->second, v, d);
                    printed++;
                }
            }
            g_find_prev[a] = v;
        }
}

/* REFINE_HAIR_SCAN=t (diagnostic, 6d.13 Q2): for 0.4 s from song t, which words of the hair and
 * cloth objects (the ones 0x50D414 reaches, vtables 0xBB3828 and 0xBB38D0) change before
 * TaskRobDisp::Ctrl on a tick, inside it on a tick, and inside it on a frame without a tick. */
const uint32_t SCAN_BYTES = 0x1000;
const int      SCAN_MAX = 256;
float    g_scan_at = -1;
int      g_scan_state;                               /* 0 waiting, 1 running, 2 printed */
uint32_t g_scan_obj[SCAN_MAX], g_scan_vt[SCAN_MAX];  /* regions: the objects (two halves) and what they point to */
int      g_scan_n;
uint8_t  g_scan_post[SCAN_MAX][SCAN_BYTES], g_scan_pre[SCAN_MAX][SCAN_BYTES];
uint16_t g_scan_cnt[SCAN_MAX][SCAN_BYTES / 4][3];
int      g_scan_have_post, g_scan_frames[2], g_frame_ticked;

void scan_add(uint32_t a, uint32_t tag)
{
    for (int i = 0; i < g_scan_n; i++) if (g_scan_obj[i] == a) return;
    if (g_scan_n < SCAN_MAX) { g_scan_obj[g_scan_n] = a; g_scan_vt[g_scan_n] = tag; g_scan_n++; }
}

void scan_collect(uint32_t chara)
{
    const uint32_t eq = rd32(chara + 0x28);
    if (!eq) return;
    const uint32_t i0 = rd32(eq + 0x8C), i1 = rd32(eq + 0x90);
    uint32_t objs[32]; int no = 0;
    for (uint32_t i = i0; i < i1 && i < 64; i++) {
        const uint32_t ob = rd32(eq + 8) + i * 0x144, sent = rd32(ob + 0x9C);
        uint32_t nd = sent ? rd32(sent) : 0;
        for (int k = 0; nd && nd != sent && k < 16 && no < 32; k++, nd = rd32(nd)) {
            const uint32_t o2 = rd32(nd + 8), vt = o2 ? rd32(o2) : 0;
            if (vt == 0xBB3828 || vt == 0xBB38D0) objs[no++] = o2;
        }
    }
    for (int i = 0; i < no; i++) { scan_add(objs[i], rd32(objs[i])); scan_add(objs[i] + 0x1000, rd32(objs[i]) | 1); }
    for (int i = 0; i < no; i++)                     /* one level down: heap pointers in the first 0x200 bytes */
        for (uint32_t o = 4; o < 0x200; o += 4) {
            const uint32_t q = rd32(objs[i] + o);
            if (q >= 0x10000000 && q < 0x1F000000 && !(q & 3) && (q < objs[i] || q >= objs[i] + 0x2000))
                scan_add(q, objs[i] + o);            /* tag: where the pointer was */
        }
}

void scan_compare(const uint8_t* a, const uint8_t* b, int o, int cls)
{
    for (uint32_t w = 0; w < SCAN_BYTES / 4; w++)
        if (memcmp(a + 4 * w, b + 4 * w, 4)) g_scan_cnt[o][w][cls]++;
}

/* pre = 1 just before TaskRobDisp::Ctrl, 0 at the frame's end. */
void scan_step(int pre, uint32_t chara)
{
    const float song = rdf(0x12CBE48);
    if (g_scan_state == 0 && pre && song >= g_scan_at) { scan_collect(chara); g_scan_state = 1; g_scan_have_post = 0; }
    if (g_scan_state != 1) return;
    if (pre && song > g_scan_at + 0.4f) {
        g_scan_state = 2;
        fprintf(stderr, "[hair-scan] %d objects, frames with tick %d, without %d; per word: before Ctrl on a tick / in Ctrl on a tick / in Ctrl without tick\n",
                g_scan_n, g_scan_frames[1], g_scan_frames[0]);
        for (int o = 0; o < g_scan_n; o++) {
            fprintf(stderr, "[hair-scan] region %08X tag %08X:", g_scan_obj[o], g_scan_vt[o]);
            int printed = 0;
            for (uint32_t w = 0; w < SCAN_BYTES / 4 && printed < 400; ) {   /* runs of words with the same counts */
                const uint16_t* c = g_scan_cnt[o][w];
                uint32_t e = w + 1;
                while (e < SCAN_BYTES / 4 && !memcmp(g_scan_cnt[o][e], c, 6)) e++;
                if (c[0] || c[1]) { fprintf(stderr, " %X-%X:%u/%u/%u", 4 * w, 4 * e - 4, c[0], c[1], c[2]); printed++; }
                w = e;
            }
            fprintf(stderr, "\n");
        }
        return;
    }
    for (int o = 0; o < g_scan_n; o++) {
        uint8_t now[SCAN_BYTES];
        memcpy(now, vm_base + g_scan_obj[o], SCAN_BYTES);
        if (pre) {
            if (g_scan_have_post && g_ticked) scan_compare(g_scan_post[o], now, o, 0);
            memcpy(g_scan_pre[o], now, SCAN_BYTES);
        } else {
            scan_compare(g_scan_pre[o], now, o, g_ticked ? 1 : 2);
            memcpy(g_scan_post[o], now, SCAN_BYTES);
        }
    }
    if (!pre) { g_scan_have_post = 1; g_scan_frames[g_ticked ? 1 : 0]++; }
}

uint32_t g_body_snap = ~0u;
uint32_t g_tick_no;                                  /* ticks seen by hook_robdisp_ctrl, for staleness checks */
void chara_frame(uint32_t chara, float alpha)
{
    if (g_ticked && g_find_at >= 0) find_frame(chara);
    if (g_probe_at >= 0) {                           /* 6d.13 Q2: what 0x50D414 reads from equip+0x134, per frame */
        const float song = rdf(0x12CBE48);
        const uint32_t eq = rd32(chara + 0x28);
        const uint32_t i0 = eq ? rd32(eq + 0x8C) : 0, i1 = eq ? rd32(eq + 0x90) : 0;
        const uint32_t obj = eq && i1 > i0 ? rd32(eq + 8) + i0 * 0x144 : 0, x = obj ? rd32(obj + 0x134) : 0;
        if (song >= g_probe_at && song <= g_probe_at + 0.04f) fprintf(stderr, "[hair-probe] eq %08X objs %u..%u at %08X x %08X\n", eq, i0, i1, obj, x);
        if (eq && song >= g_probe_at && song <= g_probe_at + 0.04f)
            for (uint32_t i = i0; i < i1 && i < 64; i++) {      /* the list 0x50D414 calls through slot +0xC */
                const uint32_t ob = rd32(eq + 8) + i * 0x144, sent = rd32(ob + 0x9C);
                uint32_t nd = sent ? rd32(sent) : 0;
                for (int k = 0; nd && nd != sent && k < 8; k++, nd = rd32(nd)) {
                    const uint32_t o2 = rd32(nd + 8), vt = o2 ? rd32(o2) : 0;
                    fprintf(stderr, "[hair-probe] obj %u %08X node obj %08X vtable %08X fnC %08X fn10 %08X\n", i, ob, o2, vt,
                            vt ? rd32(rd32(vt + 0xC)) : 0, vt ? rd32(rd32(vt + 0x10)) : 0);
                }
            }
        if (x && song >= g_probe_at && song <= g_probe_at + 0.3f) {
            fprintf(stderr, "[hair-probe] song %.3f chara %08X eq %08X x %08X ticked=%d alpha=%.2f |", song, chara, eq, x,
                    g_ticked, alpha);
            for (uint32_t o = 0x8F0; o < 0x920; o += 4) fprintf(stderr, " %X:%08X", o, rd32(x + o));
            fprintf(stderr, "\n");
        }
    }
    if (g_ticked && g_stats) {                       /* 6d.13 Q1: the character's PV frame (+0x4AC, 60ths) against the song */
        static float next_log = 0;
        const float song = rdf(0x12CBE48), f = rdf(chara + 0x4AC);
        if (song >= next_log || song < next_log - 2) {
            next_log = floorf(song) + 1;
            fprintf(stderr, "[chara-frame] song %.3f chara %08X frame %.3f anim %.3f d=%+.4f s f-anim=%+.3f\n", song, chara, f,
                    rdf(0xC23F78), f / 60.0f - song, f - rdf(0xC23F78));
        }
    }
    const uint32_t bd = rd32(chara + 0x24);
    if (!bd) return;
    const uint32_t m1 = rd32(bd + 0x1C), e1 = rd32(bd + 0x20), m2 = rd32(bd + 0x2C), e2 = rd32(bd + 0x30);
    const uint32_t n1 = (e1 - m1) / 64, n2 = (e2 - m2) / 64;
    /* The layout read in 6d.10: n1 = object bone count (+4), n1 + n2 = total (+8). */
    if (!m1 || !m2 || e1 < m1 || e2 < m2 || (e1 - m1) % 64 || (e2 - m2) % 64 || n1 + n2 > (uint32_t)MAX_MATS ||
        n1 != rd32(bd + 4) || n1 + n2 != rd32(bd + 8)) {
        g_st.skipped++;
        return;
    }
    Slot* s = slot_for(chara);
    if (!s) { g_st.skipped++; return; }
    s->seen = g_frame;
    if (s->bd != bd || s->m1 != m1 || s->n1 != n1 || s->m2 != m2 || s->n2 != n2) {
        s->bd = bd; s->m1 = m1; s->n1 = n1; s->m2 = m2; s->n2 = n2;
        if (s->has_cur) g_st.snap_layout++;
        s->has_cur = 0; s->valid = 0;
    }
    const size_t nb = slot_bytes(*s);
    const int nm = (int)(n1 + n2 + 1);
    uint8_t now[(MAX_MATS + 1) * 64];
    int jumped = 0;
    read_pose(*s, now);
    if (!s->has_cur) {
        memcpy(s->cur, now, nb); s->has_cur = 1; s->valid = 0; g_st.snap_first++;
    } else if (memcmp(now, s->cur, nb) != 0) {
        if (g_ticked) {
            memcpy(s->prev2, s->prev, nb); s->valid2 = s->valid;
            memcpy(s->prev, s->cur, nb);
            memcpy(s->cur, now, nb);
            float jm, jd;
            pose_jump(s->prev, s->cur, nm, jm, jd);
            if (jm > g_st.max_m) g_st.max_m = jm;
            if (jd > g_st.max_deg) g_st.max_deg = jd;
            s->valid = jm <= ROB_JUMP_M && jd <= ROB_JUMP_DEG;
            jumped = !s->valid;
            if (g_stats && (jm > 0.25f || jd > 40.0f))     /* near or over a threshold: which bone */
                fprintf(stderr, "[rob-interp] tick song %.3f s: %.3f m (bone %d) %.1f deg (bone %d) of %d%s\n",
                        rdf(0x12CBE48), jm, g_jump_bone_m, jd, g_jump_bone_deg, nm, s->valid ? "" : " JUMP");
            if (!s->valid) {
                g_st.snap_jump++;
                if (g_stats && g_st.snap_jump <= 200)   /* song clock (0x12CBE48, s), as screen_census uses */
                    fprintf(stderr, "[rob-interp] jump chara %08X song %.3f s frame %u: %.3f m %.1f deg\n",
                            chara, rdf(0x12CBE48), g_frame, jm, jd);
            }
        } else {                                     /* written outside a tick: take it as is */
            memcpy(s->cur, now, nb); s->valid = 0; g_st.snap_outside++;
            if (g_stats && g_st.snap_outside <= 200)
                fprintf(stderr, "[rob-interp] outside chara %08X song %.3f s frame %u\n", chara, rdf(0x12CBE48), g_frame);
        }
    } else if (g_ticked) {
        memcpy(s->prev2, s->prev, nb); s->valid2 = s->valid;
        memcpy(s->prev, s->cur, nb);                 /* a tick that left the pose as it was */
        s->valid = 1;
    }
    const int lead = g_lead && port_in_song() == 1;
    if (g_ticked && g_cam_snap && !lead) s->valid = 0;   /* a camera cut on this tick: no blend across it */
    if (g_ticked) {
        if (s->hold) s->valid = 0;                   /* the cut's own tick */
        s->hold = lead && jumped && !s->hold && g_blend;
        if (s->hold) held_objects(*s);
    }
    if (s->hold) {                        /* the pose of after the cut is early: the one before stays */
        memcpy(s->wrote, s->prev, nb);
        write_pose(*s, s->wrote);
        s->pending = 1; s->valid2 = 0;
        g_st.blends++;
        return;
    }
    if (g_ticked && !s->valid) g_body_snap = g_frame;    /* the hair does as the body (ox_tick) */
    if (!g_blend || !s->valid) { g_st.full++; s->valid2 = 0; return; }
    blend_pose3(s->valid2 ? s->prev2 : s->prev, s->prev, s->cur, nm, alpha, s->wrote);
    write_pose(*s, s->wrote);
    s->pending = 1;
    g_st.blends++;
}

/* A cut with the characters a tick ahead (2026-11-22). The tick before a cut brings the pose of after it, a tick early:
 * drawn at once, the character was in the new place for up to 33 ms of the old shot, and the hair and cloth, which the
 * game puts back to rest on the tick of the cut itself, took the whole jump as a shove first (bangs swinging x1.3-2.8 of
 * the original, run li120_1 against li30_0; everywhere else they were the same to 0.01 mm). So on such a tick the pose
 * before it stays, for the eye and for the hair and cloth, until the next one, which is the cut's own (cut_tick): the
 * game puts them back to rest there as always, and that tick is drawn unblended, as after any cut. */
int held_block(uint32_t o)                           /* a block (osage, expression) of a character held on this tick */
{
    const uint32_t ob = rd32(o + 0x34);
    for (const Slot& sl : g_slot) if (sl.used && sl.hold && ob >= sl.ob_lo && ob < sl.ob_hi) return 1;
    return 0;
}
void held_objects(Slot& sl)
{
    const uint32_t eq = rd32(sl.chara + 0x28);
    sl.ob_lo = sl.ob_hi = 0;
    if (!eq) return;
    const uint32_t i0 = rd32(eq + 0x8C), i1 = rd32(eq + 0x90), base = rd32(eq + 8);
    if (base && i1 >= i0 && i1 <= 64) { sl.ob_lo = base + i0 * 0x144; sl.ob_hi = base + i1 * 0x144; }
}

/* Camera cuts (6d.11, P1). The camera is computed every frame, before TaskRobDisp::Ctrl,
 * on the PV's own clock; the characters change only on their 30 Hz ticks. Measured in
 * Cat Food at 120 (STATUS 6d.11): many cuts fall 1-3 frames before the tick that brings
 * the characters' new shot, so those frames drew the new camera with the old pose.
 * On the first tick at or after a cut every character takes the tick's pose unblended:
 * no frame mixes the old shot's pose into the new one (REFINE_CAM_SYNC=0 turns it off).
 * REFINE_CAM_HOLD=1 (test only) also holds the camera block drawn last until that tick;
 * it is off because the stage objects and props follow the PV clock every frame like the
 * camera, so a held camera showed them already in the new shot (6d.11, cut at 29.13 s).
 * A cut on a frame without a tick brings the tick to that frame (cut_tick, 6d.12), so the
 * 1-3 frames of new camera with the old shot's pose that 6d.11 measured are gone. */
const uint32_t CAM_BYTES   = 0xC0ACA4 - CAM_BASE;  /* view point .. view-projection (0x14C) */
const float    CAM_CUT_M   = 0.3f;                 /* view point or interest moved this much in one frame */
const int      CAM_HOLD_MAX = 9;                   /* frames: a tick at 30 fps is 8 at 240 */

uint8_t  g_cam_true[CAM_BYTES], g_cam_last[CAM_BYTES], g_cam_wrote[CAM_BYTES];
int      g_cam_have, g_cam_hold, g_cam_pending, g_cam_sync = 1, g_cam_hold_on, g_cam_cut_wait;
uint32_t g_cam_cuts, g_cam_holds, g_cam_held_frames, g_cam_snaps;
uint32_t g_cam_shot;                               /* +1 on every cut cam_frame sees (cam_delay_apply) */
float    g_dump_at = -1;

float cam_move_bytes(const uint8_t* a, const uint8_t* b)
{
    float m = 0;
    for (int k = 0; k < 2; k++) {
        float d2 = 0;
        for (int i = 0; i < 3; i++) { const float d = bef(b + 12 * k + 4 * i) - bef(a + 12 * k + 4 * i); d2 += d * d; }
        if (d2 > m) m = d2;
    }
    return sqrtf(m);
}

/* Once a frame, before the characters: decide what the camera shows. Returns 1 on a cut. */
int cam_frame(float alpha)
{
    memcpy(g_cam_true, vm_base + CAM_BASE, CAM_BYTES);
    const float mv = g_cam_have ? cam_move_bytes(g_cam_last, g_cam_true) : 0;
    /* A fast camera is not a cut (2026-11-22): Senbonzakura's ride (song 27-31 s) moves over 0.3 m every frame, 23
     * frames in a row were taken for cuts and each one brought a tick, so Miku was 0.46 s ahead of her bicycle until the
     * next shot (run a0). A cut also stands out from the moves of the frames before it; the frame right after one only
     * sets what is usual in the new shot. */
    static float usual;                              /* the camera's move of the last frames, fading */
    static int after_cut;
    const int cut = mv > CAM_CUT_M && !after_cut && mv > 3.0f * usual;
    if (cut) { after_cut = 1; usual = 0; }
    else { after_cut = 0; usual = mv > usual * 0.8f ? mv : usual * 0.8f; }
    g_cam_snap = 0;
    if (cut) g_cam_shot++;
    if (cut && !g_cam_cut_wait) {
        g_cam_cuts++;
        if (g_stats && g_cam_cuts <= 400) {   /* where the cut falls on the 30 Hz grids (6d.12) */
            const float song = rdf(0x12CBE48), anim = rdf(0xC23F78);
            fprintf(stderr, "[rob-interp] cam cut song %.3f s frame %u: %.3f m, ticked=%d alpha=%.2f%s "
                    "anim %.3f grid song %.2f anim %.2f\n", song, g_frame, mv, g_ticked, alpha,
                    g_cam_hold ? " (holding)" : "", anim, song * 29.97f - floorf(song * 29.97f),
                    anim / 2.002f - floorf(anim / 2.002f));
        }
    }
    if (g_cam_sync && g_cam_have) {
        if (g_ticked) {
            if (cut || g_cam_cut_wait) { g_cam_snap = 1; g_cam_snaps++; }
            g_cam_hold = 0; g_cam_cut_wait = 0;
        } else if (cut && !g_cam_cut_wait) {
            g_cam_cut_wait = 1;
            if (g_cam_hold_on) { g_cam_hold = 1; g_cam_holds++; }
        } else if (g_cam_cut_wait && ++g_cam_cut_wait > CAM_HOLD_MAX) {
            g_cam_hold = 0; g_cam_cut_wait = 0;          /* no tick came (the game stopped) */
        }
    }
    if (g_cam_hold) {
        memcpy(vm_base + CAM_BASE, g_cam_last, CAM_BYTES);   /* the shot drawn last frame */
        memcpy(g_cam_wrote, g_cam_last, CAM_BYTES);
        g_cam_pending = 1;
        g_cam_held_frames++;
    } else {
        memcpy(g_cam_last, g_cam_true, CAM_BYTES);
    }
    g_cam_have = 1;
    return cut;
}

/* The camera drawn at the characters' time (2026-11-21).
 *
 * Between ticks a character is drawn on the curve from the tick before to the newest one, at the phase since the
 * newest: at game time N + phase it shows its pose of N - 1 + phase, exactly one tick (33 ms) behind. The camera is
 * worked out every frame from the PV clock, so it was a tick ahead of the people it films: a shot that follows a
 * dancer framed where she was going to be, and close-ups came out off centre. At 30 fps there is no such gap.
 *
 * So the camera is drawn one tick late too. Every frame's camera block goes into a ring with its game time (the
 * sum of the frames' speeds, in ticks); the block written for drawing is the one at "now - 1 tick", blended
 * between the two frames around it. Never from before the current shot: right after a cut the characters take
 * the cut tick's pose unblended (cut_tick, g_cam_snap), so the camera holds the cut's first frame until the next
 * tick, and from there both run one tick behind again. The game's own block goes back at the frame's end
 * (cam_restore). REFINE_CAM_DELAY=0 draws the camera of the moment, as before. */
const int CAM_RING = 128;                          /* a tick is 33 frames at 1000 fps */
struct CamSample { double t; uint32_t shot; uint8_t b[CAM_BYTES]; };
CamSample g_cam_ring[CAM_RING];
int      g_cam_ring_n, g_cam_ring_new;             /* samples held, index of the newest */
double   g_game_t;                                 /* game time in ticks: the frames' speeds added up */
int      g_cam_delay = 1;
uint32_t g_cam_delayed;

void cam_push(double t)
{
    CamSample* n = &g_cam_ring[g_cam_ring_new];
    if (!g_cam_ring_n || t > n->t) {               /* time moved: a new sample; otherwise the newest is refreshed */
        g_cam_ring_new = (g_cam_ring_new + 1) % CAM_RING;
        n = &g_cam_ring[g_cam_ring_new];
        if (g_cam_ring_n < CAM_RING) g_cam_ring_n++;
    }
    n->t = t; n->shot = g_cam_shot;
    memcpy(n->b, g_cam_true, CAM_BYTES);
}

/* A word that reads as an ordinary float is blended; anything else (a flag, a count) is taken from the nearer one. */
inline int cam_word_is_float(uint32_t w)
{
    const uint32_t e = (w >> 23) & 0xFF;
    return w == 0 || w == 0x80000000u || (e != 0 && e != 0xFF);
}

void cam_delay_apply(void)
{
    if (!g_cam_delay || g_cam_hold || g_cam_ring_n < 2) return;
    const double want = g_game_t - 1.0;
    /* Walk back from the newest to the first sample at or before "want", staying in the current shot. */
    int i = g_cam_ring_new, older = -1;
    for (int k = 0; k < g_cam_ring_n; k++) {
        const int j = (g_cam_ring_new - k + CAM_RING) % CAM_RING;
        if (g_cam_ring[j].shot != g_cam_shot) break;
        if (g_cam_ring[j].t <= want) { older = j; break; }
        i = j;
    }
    uint8_t out[CAM_BYTES];
    if (older < 0) memcpy(out, g_cam_ring[i].b, CAM_BYTES);   /* the shot's first frame, or the oldest we have */
    else if (older == i) memcpy(out, g_cam_ring[i].b, CAM_BYTES);
    else {
        const CamSample& a = g_cam_ring[older];
        const CamSample& b = g_cam_ring[i];
        const double span = b.t - a.t;
        const float u = span > 0 ? (float)((want - a.t) / span) : 1.0f;
        for (uint32_t o = 0; o < CAM_BYTES; o += 4) {
            uint32_t wa, wb; memcpy(&wa, a.b + o, 4); memcpy(&wb, b.b + o, 4);
            wa = _byteswap_ulong(wa); wb = _byteswap_ulong(wb);
            uint32_t w = u < 0.5f ? wa : wb;
            if (wa != wb && cam_word_is_float(wa) && cam_word_is_float(wb)) {
                float fa, fb; memcpy(&fa, &wa, 4); memcpy(&fb, &wb, 4);
                const float f = fa + (fb - fa) * u;
                memcpy(&w, &f, 4);
            }
            w = _byteswap_ulong(w);
            memcpy(out + o, &w, 4);
        }
    }
    if (!memcmp(out, g_cam_true, CAM_BYTES)) return;
    memcpy(vm_base + CAM_BASE, out, CAM_BYTES);
    memcpy(g_cam_wrote, out, CAM_BYTES);
    g_cam_pending = 1;
    g_cam_delayed++;
}

/* REFINE_CAM_TRACE=<song second> (diagnostic, 2026-11-21): for 12 s of a song from that second, one line a frame
 * with what is about to be drawn: the song clock, the first character's skeleton centre (the mean of its bone
 * origins, put in the world by its world matrix) and that point in the camera's view space (view matrix at
 * CAM_BASE + 0x4C, row-major, translation in [3], [7], [11]), plus the eye through the same matrix as a check of the
 * convention (it must come out at 0). tools/cam_sync.py compares two runs: where the character sits in the picture
 * at each moment of the song, against the original at 30 fps. Works at 30 fps as well (nothing is blended there). */
float g_cam_trace_at = -1.0f, g_cam_trace_end = -1.0f;
/* REFINE_ROB_TRACE=<from>-<to> (song seconds, diagnostic): where every character is drawn, a line per frame and
 * character: its order in the list, the middle of its skeleton in the world, its frame and the camera's view point.
 * tools/rob_cmp.py compares two runs. */
float g_rt_from = -1.0f, g_rt_to = -1.0f;
void rob_trace(uint32_t task)
{
    const float song = rdf(0x12CBE48);
    if (song < g_rt_from || song > g_rt_to || port_in_song() != 1) return;
    const uint32_t head = rd32(task + 0x60);
    uint32_t node = head ? rd32(head) : 0;
    for (int i = 0; node && node != head && i < 16; i++, node = rd32(node)) {
        const uint32_t chara = rd32(node + 8), bd = chara ? rd32(chara + 0x24) : 0;
        if (!bd) continue;
        const uint32_t m1 = rd32(bd + 0x1C), e1 = rd32(bd + 0x20);
        if (!m1 || e1 <= m1 || (e1 - m1) % 64 || (e1 - m1) / 64 > (uint32_t)MAX_MATS) continue;
        double c[3] = { 0, 0, 0 }; int n = 0;
        for (uint32_t a = m1; a < e1; a += 64, n++) for (int k = 0; k < 3; k++) c[k] += rdf(a + 4 * (4 * k + 3));
        float w[3];
        const uint32_t W = chara + 0x1F64;
        for (int k = 0; k < 3; k++) c[k] /= n;
        for (int k = 0; k < 3; k++)
            w[k] = (float)(rdf(W + 16 * k) * c[0] + rdf(W + 16 * k + 4) * c[1] + rdf(W + 16 * k + 8) * c[2]) + rdf(W + 16 * k + 12);
        fprintf(stderr, "[rob] %.4f %d %.4f %.4f %.4f %.3f %.3f %.3f %.3f%c", song, i, w[0], w[1], w[2], rdf(chara + 0x4AC),
                rdf(CAM_BASE), rdf(CAM_BASE + 4), rdf(CAM_BASE + 8), 10);
    }
}

void cam_trace(uint32_t task)
{
    const float song = rdf(0x12CBE48);
    if (g_cam_trace_at >= 0 && song >= g_cam_trace_at && port_in_song() == 1) { g_cam_trace_end = g_cam_trace_at + 12.0f; g_cam_trace_at = -1.0f; }
    if (!(song < g_cam_trace_end)) return;
    const uint32_t head = rd32(task + 0x60);
    const uint32_t node = head ? rd32(head) : 0;
    if (!node || node == head) return;
    const uint32_t chara = rd32(node + 8), bd = chara ? rd32(chara + 0x24) : 0;
    if (!bd) return;
    const uint32_t m1 = rd32(bd + 0x1C), e1 = rd32(bd + 0x20), m2 = rd32(bd + 0x2C), e2 = rd32(bd + 0x30);
    if (!m1 || !m2 || e1 <= m1 || e2 < m2 || (e1 - m1) % 64 || (e2 - m2) % 64) return;
    double c[3] = { 0, 0, 0 }; int n = 0;
    for (uint32_t a = m1; a < e1 && n < MAX_MATS; a += 64, n++) for (int i = 0; i < 3; i++) c[i] += rdf(a + 4 * (4 * i + 3));
    for (uint32_t a = m2; a < e2 && n < MAX_MATS; a += 64, n++) for (int i = 0; i < 3; i++) c[i] += rdf(a + 4 * (4 * i + 3));
    if (!n) return;
    float w[3], v[3], e[3];
    const uint32_t W = chara + 0x1F64, V = CAM_BASE + 0x4C;
    for (int i = 0; i < 3; i++) c[i] /= n;
    for (int i = 0; i < 3; i++)
        w[i] = (float)(rdf(W + 16 * i) * c[0] + rdf(W + 16 * i + 4) * c[1] + rdf(W + 16 * i + 8) * c[2]) + rdf(W + 16 * i + 12);
    for (int i = 0; i < 3; i++) {
        v[i] = rdf(V + 16 * i) * w[0] + rdf(V + 16 * i + 4) * w[1] + rdf(V + 16 * i + 8) * w[2] + rdf(V + 16 * i + 12);
        e[i] = rdf(V + 16 * i) * rdf(CAM_BASE) + rdf(V + 16 * i + 4) * rdf(CAM_BASE + 4) + rdf(V + 16 * i + 8) * rdf(CAM_BASE + 8) + rdf(V + 16 * i + 12);
    }
    fprintf(stderr, "[cam-trace] song %.4f world %.4f %.4f %.4f view %.4f %.4f %.4f eye %.4f %.4f %.4f shot %u\n",
            song, w[0], w[1], w[2], v[0], v[1], v[2], e[0], e[1], e[2], g_cam_shot);
}

/* At the frame's end: the camera's own state back, word by word where the hold still is. */
void cam_restore(void)
{
    if (!g_cam_pending) return;
    g_cam_pending = 0;
    for (uint32_t i = 0; i < CAM_BYTES; i += 4)
        if (!memcmp(vm_base + CAM_BASE + i, g_cam_wrote + i, 4)) memcpy(vm_base + CAM_BASE + i, g_cam_true + i, 4);
}

/* Hair and cloth blend (6d.15, Q2). The osage objects of a character (equipment objects
 * eq+0x8C..0x90 of 0x144 bytes, nodes of vtable 0xBB3828 / 0xBB38D0 in the list at obj+0x9C,
 * and the heap blocks they point to) change their simulation state only inside
 * TaskRobDisp::Ctrl of a tick frame (obj+0x12C set); on every other frame Ctrl rebuilds the node
 * matrices from that frozen state and the (blended) skeleton, through 0x50D414.
 * On a tick: snapshot the regions before Ctrl, diff after it. The float words that changed are
 * the state (prev, cur). On later frames, before Ctrl, the blend of the two goes in memory and
 * Ctrl rebuilds the matrices; on the tick frame itself, 0x50D414 is called again with the flag
 * clear. At the frame's end the real state goes back. Same alpha and same jump rules as the
 * skeleton (the character's slot must be valid). Off by default; REFINE_HAIR_BLEND=1 turns it on. */
const uint32_t HAIR_WIN = 0x1000;
const int      HAIR_MAX_REG = 256;
struct HW { uint32_t addr; uint32_t prev, cur, wrote; };
struct Hair {
    uint32_t chara, seen, eq, eq8;
    int used, valid, pending;
    std::vector<uint32_t> regions, objs;
    std::vector<uint8_t> pre;
    std::vector<HW> words;
};
Hair g_hair[MAX_CHARA];
int  g_hair_on = 0;                   /* off by default (2026-11-09): the author saw it worse, hair stiff; REFINE_HAIR_BLEND=1 */
struct { uint32_t ticks, blends, rejected, stale, dropped, big, words_last, words_max, frames; float max_delta; double ms_sum, ms_tick_sum, ms_max; } g_hs;

inline float bitsf(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
inline uint32_t fbits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
inline void pw(uint32_t a, uint32_t v) { const uint32_t be = _byteswap_ulong(v); memcpy(vm_base + a, &be, 4); }
inline int hair_ok(float f) { return f == 0.0f || (isfinite(f) && fabsf(f) >= 1e-6f && fabsf(f) <= 1e6f); }

Hair* hair_for(uint32_t chara)
{
    Hair* fr = nullptr;
    for (Hair& h : g_hair) {
        if (h.used && h.chara == chara) return &h;
        if (h.used && g_frame - h.seen > 4) { h.used = 0; h.words.clear(); }
        if (!h.used && !fr) fr = &h;
    }
    if (!fr) return nullptr;
    fr->used = 1; fr->chara = chara; fr->valid = 0; fr->pending = 0; fr->words.clear();
    return fr;
}

void hair_add(Hair& h, uint32_t a)
{
    for (uint32_t r : h.regions) if (r == a) return;
    if ((int)h.regions.size() < HAIR_MAX_REG) h.regions.push_back(a);
}

void hair_collect(Hair& h)
{
    h.regions.clear(); h.objs.clear();
    const uint32_t eq = rd32(h.chara + 0x28);
    h.eq = eq; h.eq8 = eq ? rd32(eq + 8) : 0;
    if (!eq) return;
    const uint32_t i0 = rd32(eq + 0x8C), i1 = rd32(eq + 0x90);
    uint32_t nodes[32]; int nn = 0;
    for (uint32_t i = i0; i < i1 && i < 64; i++) {
        const uint32_t ob = rd32(eq + 8) + i * 0x144, sent = rd32(ob + 0x9C);
        uint32_t nd = sent ? rd32(sent) : 0;
        int found = 0;
        for (int k = 0; nd && nd != sent && k < 16 && nn < 32; k++, nd = rd32(nd)) {
            const uint32_t o2 = rd32(nd + 8), vt = o2 ? rd32(o2) : 0;
            if (vt == 0xBB3828 || vt == 0xBB38D0) { nodes[nn++] = o2; found = 1; }
        }
        if (found) h.objs.push_back(ob);
    }
    for (int i = 0; i < nn; i++) { hair_add(h, nodes[i]); hair_add(h, nodes[i] + HAIR_WIN); }
    for (int i = 0; i < nn; i++)
        for (uint32_t o = 4; o < 0x200; o += 4) {
            const uint32_t q = rd32(nodes[i] + o);
            if (q >= 0x10000000 && q < 0x1F000000 && !(q & 3) && (q < nodes[i] || q >= nodes[i] + 0x2000)) hair_add(h, q);
        }
}

void hair_snap(const Hair& h, std::vector<uint8_t>& buf)
{
    buf.resize(h.regions.size() * HAIR_WIN);
    for (size_t r = 0; r < h.regions.size(); r++) memcpy(&buf[r * HAIR_WIN], vm_base + h.regions[r], HAIR_WIN);
}

void hair_put(Hair& h, float alpha)
{
    for (HW& w : h.words) {
        const float p = bitsf(w.prev), c = bitsf(w.cur);
        w.wrote = fbits(p + (c - p) * alpha);
        pw(w.addr, w.wrote);
    }
    h.pending = 1;
}

/* A word the game rewrote after the blend is a derived one (a node matrix), not simulation state. */
void hair_classify(Hair& h)
{
    size_t k = 0;
    for (size_t i = 0; i < h.words.size(); i++)
        if (rd32(h.words[i].addr) == h.words[i].wrote) h.words[k++] = h.words[i];
        else g_hs.dropped++;
    h.words.resize(k);
}

/* Before TaskRobDisp::Ctrl. */
void hair_pre(uint32_t head, int tick, float alpha)
{
    if (!g_hair_on) return;
    for (uint32_t nd = head ? rd32(head) : 0, i = 0; nd && nd != head && i < 16; i++, nd = rd32(nd)) {
        const uint32_t chara = rd32(nd + 8);
        if (!chara || (rd32(chara + 0x320) & 1)) continue;
        Hair* h = hair_for(chara);
        if (!h) continue;
        h->seen = g_frame;
        if (tick) {
            hair_collect(*h);
            hair_snap(*h, h->pre);
            h->words.clear(); h->valid = 0;
        } else if (h->valid && !h->words.empty() && alpha < 1.0f) {
            if (rd32(chara + 0x28) != h->eq || !h->eq || rd32(h->eq + 8) != h->eq8) { h->valid = 0; g_hs.stale++; continue; }
            hair_put(*h, alpha);                              /* Ctrl rewrites the derived words; hair_post drops them */
            g_hs.blends++;
        }
    }
}

/* After TaskRobDisp::Ctrl. */
void hair_post(ppu_context* ctx, uint32_t head, int tick, float alpha)
{
    if (!g_hair_on) return;
    static std::vector<uint8_t> post;
    for (uint32_t nd = head ? rd32(head) : 0, i = 0; nd && nd != head && i < 16; i++, nd = rd32(nd)) {
        const uint32_t chara = rd32(nd + 8);
        if (!chara || (rd32(chara + 0x320) & 1)) continue;
        Hair* h = hair_for(chara);
        if (h && !tick && h->pending && h->seen == g_frame) { hair_classify(*h); continue; }
        if (!tick) continue;
        if (!h || h->seen != g_frame || h->regions.empty() || h->pre.size() != h->regions.size() * HAIR_WIN) continue;
        hair_snap(*h, post);
        h->words.clear();
        float mx = 0.0f;
        for (size_t r = 0; r < h->regions.size(); r++) {
            const uint8_t* a = &h->pre[r * HAIR_WIN];
            const uint8_t* b = &post[r * HAIR_WIN];
            for (uint32_t w = 0; w < HAIR_WIN / 4; w++) {
                if (!memcmp(a + 4 * w, b + 4 * w, 4)) continue;
                const uint32_t ad = h->regions[r] + 4 * w;
                bool dup = false;
                for (const HW& x : h->words) if (x.addr == ad) { dup = true; break; }
                if (dup) continue;
                uint32_t pa, cb2;
                memcpy(&pa, a + 4 * w, 4); memcpy(&cb2, b + 4 * w, 4);
                const uint32_t pb = _byteswap_ulong(pa), cb = _byteswap_ulong(cb2);
                const float p = bitsf(pb), c = bitsf(cb);
                if (!hair_ok(p) || !hair_ok(c)) continue;
                if (fabsf(c - p) > ROB_JUMP_M) { g_hs.big++; if (fabsf(c - p) > g_hs.max_delta) g_hs.max_delta = fabsf(c - p); continue; }
                h->words.push_back({ad, pb, cb, cb});
                if (fabsf(c - p) > mx) mx = fabsf(c - p);
            }
        }
        g_hs.ticks++;
        g_hs.words_last = (uint32_t)h->words.size();
        if (g_hs.words_last > g_hs.words_max) g_hs.words_max = g_hs.words_last;
        Slot* s = nullptr;
        for (Slot& x : g_slot) if (x.used && x.chara == chara) s = &x;
        h->valid = s && s->valid && mx <= ROB_JUMP_M && !h->words.empty();
        if (!h->valid) { g_hs.rejected++; h->words.clear(); continue; }
        if (alpha >= 1.0f) continue;
        hair_put(*h, alpha);                        /* the tick frame itself: matrices again from the blend */
        static ppu_context save;
        memcpy(&save, ctx, sizeof save);
        for (uint32_t ob : h->objs) {
            vm_base[ob + 0x12C] = 0;
            ctx->gpr[3] = ob;
            func_0050D414(ctx);
        }
        memcpy(ctx, &save, sizeof save);
        hair_classify(*h);
        g_hs.blends++;
    }
}

/* End of the frame: the real state goes back where the blend still is. */
void hair_restore(void)
{
    for (Hair& h : g_hair) {
        if (!h.used || !h.pending) continue;
        h.pending = 0;
        for (const HW& w : h.words) if (rd32(w.addr) == w.wrote) pw(w.addr, w.cur);
    }
}

int g_menu_rob = 1;  /* REFINE_MENU_ROB=0: the menus' characters at the 30 Hz ticks only, as before 2026-11-12 */
int g_active = 1;   /* 0161: off while the game runs at 30 fps, the hooks then only call the original */

/* Q2 (2026-11-09): which osage path runs on tick and non-tick frames, and with what step.
 * 0x50D414 calls, per node of the list obj+0x9C, slot +0x14 (0xBB38D0: 0x50DAAC, which steps
 * with 0x3BC0C() * eq+0x8FC) when obj+0x12C is clear, and slot +0x18 (0x50D230 -> 0x3D3C40)
 * when it is set. REFINE_PHYS_LOG=1 counts them, once a second. Diagnostic only. */
int      g_pl_on, g_in_ctrl, g_ctrl_tick;
struct { uint32_t frames, ticks, sim_t, sim_n, skip_t, skip_n, set_t, set_n, c8_t, c8_n, out; float dt_min, dt_max; } g_pl;
inline void pl_count(uint32_t& t, uint32_t& n) { if (!g_in_ctrl) g_pl.out++; else if (g_ctrl_tick) t++; else n++; }
extern int g_hair_ex; void ox_show(uint32_t o); void ox_restore(void);
void hook_osage_step(ppu_context* ctx)               /* 0x50DAAC, slot +0x14 of 0xBB38D0 */
{
    if (g_pl_on) {
        const uint32_t o = (uint32_t)ctx->gpr[3];
        if (vm_base[o + 0x39]) pl_count(g_pl.skip_t, g_pl.skip_n);
        else {
            pl_count(g_pl.sim_t, g_pl.sim_n);
            const float dt = rdf(SPEED_BASE + 0x14) * rdf(rd32(rd32(o + 0x34) + 0x134) + 0x8FC);
            if (dt < g_pl.dt_min) g_pl.dt_min = dt;
            if (dt > g_pl.dt_max) g_pl.dt_max = dt;
        }
    }
    const uint32_t oo = (uint32_t)ctx->gpr[3];
    func_0050DAAC(ctx);
    if (g_hair_ex && g_active && g_in_ctrl) ox_show(oo);
}
void hook_osage_set(ppu_context* ctx)                /* 0x50D230, slot +0x18 of 0xBB38D0 */
{
    if (g_pl_on) pl_count(g_pl.set_t, g_pl.set_n);
    const uint32_t oo = (uint32_t)ctx->gpr[3];
    func_0050D230(ctx);
    if (g_hair_ex && g_active && g_in_ctrl) ox_show(oo);
}
/* Expression bones (ExExpressionBlock, 2026-11-22): sleeves, collars, skirt and shoe bones that a formula turns from
 * the body's own joint values. 0x50C670 works the formula out (0x50B970, nine floats at node + 0xC: position, rotation,
 * scale) and hangs the result from the parent bone (0x50C404). The parent is drawn blended between ticks but the joint
 * values are the tick's, up to a tick ahead of it: the bone followed its parent between ticks and went back on each
 * one, a saw on every piece these bones move (tools/draw_census.py, Cat Food: 13 of the body's bones turned back by 4
 * frames' worth of motion on each tick). So the nine floats of the last three ticks go on the body's own curve, with
 * its phase, and the bone is hung again. The game sets the floats afresh every frame (0x50B290): nothing to restore.
 * REFINE_EXPR_BLEND=0 turns it off. */
int g_expr_on = 1;
extern float g_alpha_now;
struct Expr { uint32_t seen, tick_no; int has, valid, valid2; float p2[9], p1[9], c[9]; };
std::unordered_map<uint32_t, Expr> g_expr;
void expr_blend(ppu_context* ctx, uint32_t o)
{
    if (g_expr.size() > 1024) g_expr.clear();
    const uint32_t loc = rd32(o + 4) + 0xC;
    Expr& x = g_expr[o];
    if (x.has && (g_frame - x.seen > 8 * 16 || g_tick_no - x.tick_no > 1)) x.has = 0;
    x.seen = g_frame;
    if (g_ctrl_tick || !x.has) {
        float now[9];
        for (int i = 0; i < 9; i++) now[i] = rdf(loc + 4 * i);
        if (x.has && x.tick_no == g_tick_no) { memcpy(x.c, now, sizeof now); }   /* worked out again in the same tick */
        else {
            memcpy(x.p2, x.p1, sizeof now); memcpy(x.p1, x.c, sizeof now); memcpy(x.c, now, sizeof now);
            x.valid2 = x.has && x.valid;
            x.valid = x.has && g_body_snap != g_frame && (!g_cam_snap || (g_lead && port_in_song() == 1)) && !held_block(o);
            x.has = 1; x.tick_no = g_tick_no;
        }
    }
    if (held_block(o) && x.has) {                    /* as its character: the tick before */
        for (int i = 0; i < 9; i++) pw(loc + 4 * i, fbits(x.p1[i]));
        ctx->gpr[3] = o;
        func_0050C404(ctx);
        return;
    }
    if (!x.valid) return;
    float w[3];
    curve_w(g_alpha_now > 1.0f ? 1.0f : g_alpha_now, w);
    const float* p0 = x.valid2 ? x.p2 : x.p1;
    for (int i = 0; i < 9; i++) {
        float a = p0[i], b = x.p1[i];
        const float c = x.c[i];
        if (i >= 3 && i < 6) {                       /* angles: the short way round */
            b = c + remainderf(b - c, 6.28318531f);
            a = b + remainderf(a - b, 6.28318531f);
        }
        const float v = w[0] * a + w[1] * b + w[2] * c;
        if (!isfinite(v)) return;
        pw(loc + 4 * i, fbits(v));
    }
    ctx->gpr[3] = o;
    func_0050C404(ctx);
}
void hook_osage_b828(ppu_context* ctx)               /* 0x50C670, slot +0x14 of 0xBB3828 (+0x18 calls it too) */
{
    if (g_pl_on) pl_count(g_pl.c8_t, g_pl.c8_n);
    const uint32_t o = (uint32_t)ctx->gpr[3];
    const int works = rd32(o + 0x10) && !vm_base[o + 0x39];
    func_0050C670(ctx);
    if (works && g_expr_on && g_blend && g_active && g_in_ctrl) expr_blend(ctx, o);
}
/* Q2c (2026-11-09): slot +0x10 of 0xBB38D0 (0x50E34C) is the staged step, mode in r4 (jump
 * table at 0x50E418): 0 -> 0x3D670C, 1 -> 0x3D47B0 (if +0x38), 2 -> 0x3D47B0 (if +0x184),
 * 3 -> 0x3D2C98 + 0x3D2FC8, 4 -> 0x3D8728, 5 -> 0x3D54DC + 0x3D2888 and +0x39 = 1, which is
 * why slot +0x14 then "skips": the step already ran, staged. dt = 0x3BC0C() * eq+0x8FC, or
 * 1.0 when the play data vector eq+0x904 (+4 begin, +8 end, 0x18-byte entries) has an entry
 * with +0xC set. This counts the modes, the play data case, and how far the nodes (osage
 * +0x40, vector +0x34, 0xEC each, position at +4) and their parent (o+0x10, position at
 * +0x30) move from one frame to the next, on tick and non-tick frames. Diagnostic only. */
struct PlPos { float x, y, z, vx, vy, vz; uint32_t frame; int has_v; };
std::unordered_map<uint32_t, PlPos> g_pl_pos;
struct { uint32_t mode[6][2], pd_t, pd_n, nodes_t, nodes_n, still_n, root_n_moved, root_n; float mv_t, mv_n, mvmax_t, mvmax_n, root_mv_n;
         uint32_t jk_t, jk_n; float jerk_t, jerk_n, jkmax_t, jkmax_n; } g_pm;
/* 2026-11-11: also the jerk, |change of the per-frame move| (second difference): equal moves on tick and other
 * frames can still turn at each tick; a 30 Hz stutter is a jerk on tick frames well over the others'. */
float g_pl_jerk = -1.0f;
float pl_move(uint32_t key, float x, float y, float z)
{
    PlPos& p = g_pl_pos[key];
    g_pl_jerk = -1.0f;
    float d = -1.0f;
    if (p.frame + 1 == g_frame) {
        const float vx = x - p.x, vy = y - p.y, vz = z - p.z;
        d = sqrtf(vx * vx + vy * vy + vz * vz);
        if (p.has_v) g_pl_jerk = sqrtf((vx - p.vx) * (vx - p.vx) + (vy - p.vy) * (vy - p.vy) + (vz - p.vz) * (vz - p.vz));
        p.vx = vx; p.vy = vy; p.vz = vz; p.has_v = 1;
    } else p.has_v = 0;
    p.x = x; p.y = y; p.z = z; p.frame = g_frame;
    return d;
}
/* Q2d (2026-11-09): the hair and cloth anchor. Before its jump table, 0x50E34C reads the
 * osage's parent (o+0x10) and its position par+0x30..0x38 (Ghidra), and hands it to every
 * stage. That position changes only on ticks (measured, Cat Food at 120: "parent moved non
 * 0/325"), so the physics followed a 30 Hz anchor. It is not the tick's translation of the
 * skeleton bone (0777a0a's guard rejected 3630/3630), so it is not assumed to be anything:
 * per parent we keep the value of the previous tick and of this tick, read at the first stage
 * call of the tick frame, and on every frame with alpha < 1 write lerp(prev, cur, alpha) with
 * the same alpha as option C (real time, from the speed step). No blend: first tick of a
 * parent, a jump over ROB_JUMP_M, a camera cut on the tick (g_cam_snap), a value that changed
 * outside a tick. The real words go back in rob_interp_frame_end unless the game rewrote them.
 * The parent matrix *(par+0x3C) is a bone of the skeleton option C already blends in memory
 * (checked in 0777a0a: in_skeleton == calls), so it is not touched here. REFINE_HAIR_ANCHOR=0
 * turns it off. Nothing at 30 fps (rob_interp is not set up, or g_active is 0). */
/* On by default since 2026-11-11: measured with REFINE_PHYS_LOG=1, Cat Food at 120 fps (runs ph0/ph1), the hair
 * and cloth nodes moved 0.0016-0.0018 m on tick frames against 0.0011-0.0012 on the others without it (the
 * stutter: a 30 Hz jump in a per-frame simulation) and 0.00116 against 0.00118 with it; peaks halved. */
int g_anchor_on = 1;
float g_alpha_now = 1.0f;                            /* this frame's alpha, set before Ctrl */
struct Anchor { uint32_t par, orig[3], wrote[3]; };
struct ParState { float prev2[3], prev[3], cur[3]; uint32_t seen, frame; int has_cur, valid, valid2; };
std::unordered_map<uint32_t, ParState> g_par;
std::vector<Anchor> g_anchors;                       /* written this frame */
struct { uint32_t calls, written, cut, first, outside, alpha1; } g_an;
void anchor_write(uint32_t o)
{
    const uint32_t par = rd32(o + 0x10);
    if (!par) return;
    ParState& st = g_par[par];
    if (st.frame == g_frame && st.seen == g_frame) return;       /* already done this frame */
    g_an.calls++;
    if (st.has_cur && g_frame - st.seen > 4) st.has_cur = 0;
    st.seen = g_frame; st.frame = g_frame;
    float v[3] = { rdf(par + 0x30), rdf(par + 0x34), rdf(par + 0x38) };
    if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) { st.has_cur = 0; st.valid = 0; return; }
    if (!st.has_cur) {
        memcpy(st.cur, v, sizeof v); st.has_cur = 1; st.valid = 0; g_an.first++;
    } else if (g_ctrl_tick) {
        memcpy(st.prev2, st.prev, sizeof v); st.valid2 = st.valid;
        memcpy(st.prev, st.cur, sizeof v); memcpy(st.cur, v, sizeof v);
        const float d = sqrtf((v[0] - st.prev[0]) * (v[0] - st.prev[0]) + (v[1] - st.prev[1]) * (v[1] - st.prev[1]) + (v[2] - st.prev[2]) * (v[2] - st.prev[2]));
        st.valid = d <= ROB_JUMP_M && !g_cam_snap;
    } else if (memcmp(st.cur, v, sizeof v) != 0) {
        memcpy(st.cur, v, sizeof v); st.valid = 0; g_an.outside++;   /* moved outside a tick: take it as is */
    }
    if (!st.valid) { g_an.cut++; return; }
    if (!(g_alpha_now < 1.0f)) g_an.alpha1++;        /* counted only: the spline ends at a midpoint, not at cur */
    float bw[3]; bspline_w(g_alpha_now > 1.0f ? 1.0f : g_alpha_now, bw);   /* the same curve as the skeleton */
    const float* p0 = st.valid2 ? st.prev2 : st.prev;
    Anchor a; a.par = par;
    for (int i = 0; i < 3; i++) {
        uint32_t ob; memcpy(&ob, &st.cur[i], 4); a.orig[i] = ob;
        const float w = bw[0] * p0[i] + bw[1] * st.prev[i] + bw[2] * st.cur[i];
        uint32_t wb; memcpy(&wb, &w, 4);
        a.wrote[i] = _byteswap_ulong(wb);               /* guest bytes, big endian */
        memcpy(vm_base + par + 0x30 + 4 * i, &a.wrote[i], 4);
    }
    g_anchors.push_back(a);
    g_an.written++;
}
void anchor_restore(void)
{
    for (const Anchor& a : g_anchors)
        for (int i = 0; i < 3; i++) {
            uint32_t now; memcpy(&now, vm_base + a.par + 0x30 + 4 * i, 4);
            if (now != a.wrote[i]) continue;             /* the game wrote it after us: it stays */
            const uint32_t be = _byteswap_ulong(a.orig[i]);
            memcpy(vm_base + a.par + 0x30 + 4 * i, &be, 4);
        }
    g_anchors.clear();
}

/* 2026-11-12, diagnostic: REFINE_HAIR_TRACE=<song second>. From then, for 360 frames, one line per osage and
 * frame after its last stage: tick, alpha, the game speed, the tip node's position (nodes at o+0x74, 0xEC each,
 * position at +4) and the parent's matrix translation. The first frame also prints the nodes' pointer words. */
/* 2026-11-12: hair and cloth as the body (REFINE_HAIR_EX, on by default). The osage chains were simulated every
 * frame with the frame's share of a tick as the step (0.25 at 120 fps). That is not the original physics: against
 * a 30 fps run of the same stretch the hair kept its swing but a third of its move was different (tools/hair_cmp.py,
 * work/n12/hair_lag.py), and the author saw it stiff and "pulled". Now the chains step as in the original, on the
 * 30 Hz ticks only and by a whole step, and what is drawn between ticks is their result blended like the skeleton:
 * each node's two matrices (node +0x38 and +0x3C point at them) on the same B-spline of the last three ticks, with
 * the same phase, written after every osage method of the frame and put back at the frame's end. No blend across
 * a tick the body did not blend (camera cut, jump) or a jump of the chain itself. REFINE_HAIR_EX=0: as before. */
int g_hair_ex = 1;
/* The step of a tick reads the skeleton (the chain's parent bone): it must be the tick's own pose, as in the
 * original, not the blend this frame draws (which trails it by up to a tick and a half: the first try stepped
 * on the blend and the chains sat a centimetre behind the head, run n12_hx120). true_pose(1) puts the tick's
 * pose where the blend is, true_pose(0) the blend back; a slot the game wrote in between is left alone. */
uint32_t g_ox_swaps, g_ox_swap_skips;
void true_pose(int on)
{
    for (Slot& sl : g_slot) {
        if (!sl.used || !sl.pending || sl.hold) continue;   /* held: the pose before is the one the hair knows */
        const size_t nb = slot_bytes(sl);
        static uint8_t now[(MAX_MATS + 1) * 64];
        read_pose(sl, now);
        if (memcmp(now, on ? sl.wrote : sl.cur, nb) != 0) { g_ox_swap_skips++; continue; }
        write_pose(sl, on ? sl.cur : sl.wrote);
        g_ox_swaps++;
    }
}
struct Ox {
    uint32_t o, seen, tick_no; int n, has, valid, valid2, pending;   /* tick_no: g_tick_no of its last step */
    uint32_t addr[128];
    std::vector<uint8_t> p2, p1, c, wrote;
};
std::unordered_map<uint32_t, Ox> g_ox;
struct { uint32_t ticks, shown, snaps; } g_oxs;
int ox_addrs(uint32_t o, uint32_t* addr)
{
    const uint32_t b = rd32(o + 0x40 + 0x34), e = rd32(o + 0x40 + 0x38);
    if (!b || e <= b || (e - b) % 0xEC || (e - b) / 0xEC > 64) return 0;
    int n = 0;
    for (uint32_t nd = b; nd < e; nd += 0xEC)
        for (uint32_t w = 0x38; w <= 0x3C; w += 4) {
            const uint32_t m = rd32(nd + w);
            if (m >= 0x10000u && m < 0x40000000u && !(m & 3)) addr[n++] = m;
        }
    return n;
}
void ox_tick(uint32_t o)
{
    Ox& x = g_ox[o];
    uint32_t addr[128];
    const int n = ox_addrs(o, addr);
    if (!n) { x.has = 0; x.valid = 0; return; }
    /* A chain that missed more than one tick restarts: its old poses are from another moment. Counted in ticks,
     * not frames: at high frame rates a tick spans many frames (16 frames was half a tick at 960 fps). */
    if (x.has && (x.n != n || memcmp(x.addr, addr, n * 4) || g_tick_no - x.tick_no > 2)) x.has = 0;
    x.o = o; x.n = n; x.seen = g_frame; x.tick_no = g_tick_no; memcpy(x.addr, addr, n * 4);
    std::vector<uint8_t> now((size_t)n * 64);
    for (int k = 0; k < n; k++) memcpy(&now[k * 64], vm_base + addr[k], 64);
    g_oxs.ticks++;
    if (!x.has) { x.c = now; x.has = 1; x.valid = 0; x.valid2 = 0; return; }
    x.p2 = x.p1; x.valid2 = x.valid && x.p2.size() == now.size();
    x.p1 = x.c; x.c = now;
    float jm, jd;
    pose_jump(x.p1.data(), x.c.data(), n, jm, jd);
    x.valid = jm <= ROB_JUMP_M && g_body_snap != g_frame && (!g_cam_snap || (g_lead && port_in_song() == 1));
    if (!x.valid) { x.valid2 = 0; g_oxs.snaps++; }
}
/* REFINE_HAIR_SHOW=0 (2026-11-14): no se dibuja nada entre ticks. La fisica va a 30 Hz, como el original, y
 * el pelo se ve tal cual sale de ella: 30 imagenes por segundo de pelo, sin mezcla ninguna. Sirve para saber
 * si lo que molesta es la mezcla (que suaviza) o la fisica a 30 Hz. */
int g_hair_show = 1;
void ox_show(uint32_t o)
{
    if (!g_hair_show) return;
    auto it = g_ox.find(o);
    if (it == g_ox.end()) return;
    Ox& x = it->second;
    if (!x.has || !x.valid || g_tick_no - x.tick_no > 1) return;   /* not stepped on the last tick: nothing to blend */
    x.seen = g_frame;
    x.wrote.resize((size_t)x.n * 64);
    blend_pose3(x.valid2 ? x.p2.data() : x.p1.data(), x.p1.data(), x.c.data(), x.n, g_alpha_now > 1.0f ? 1.0f : g_alpha_now, x.wrote.data());
    for (int k = 0; k < x.n; k++) memcpy(vm_base + x.addr[k], &x.wrote[k * 64], 64);
    x.pending = 1;
    g_oxs.shown++;
}
void ox_restore(void)
{
    for (auto& kv : g_ox) {
        Ox& x = kv.second;
        if (!x.pending) continue;
        x.pending = 0;
        for (int k = 0; k < x.n; k++)
            if (!memcmp(vm_base + x.addr[k], &x.wrote[k * 64], 64)) memcpy(vm_base + x.addr[k], &x.c[k * 64], 64);
    }
}

float g_ht_at = -1.0f; int g_ht_left;
void hair_trace(uint32_t o)
{
    const float song = rdf(0x12CBE48);
    static float end; static int first;
    /* 2026-11-14: only once a song is being played. The song list's preview moves the song clock too, and over
     * 30 fps this hook also runs in the menus (the menus' characters), so the trace armed there with the
     * preview's clock past <song s> and expired at once, without a single line. */
    if (g_ht_at >= 0 && song >= g_ht_at && port_in_song() == 1) { end = g_ht_at + 8.0f; g_ht_at = -1.0f; g_ht_left = 1; first = 64; }   /* 8 s of song */
    { static unsigned n; if (n++ % 2000 == 0) fprintf(stderr, "[hair-trace] llamada %u: song=%.3f at=%.3f left=%d in_song=%d%c", n, song, g_ht_at, g_ht_left, port_in_song(), 10); }
    if (g_ht_left <= 0) return;
    if (song >= end) { g_ht_left = 0; return; }
    const uint32_t b = rd32(o + 0x40 + 0x34), e = rd32(o + 0x40 + 0x38), par = rd32(o + 0x10);
    if (!b || e <= b || !par) return;
    const uint32_t n = (e - b) / 0xEC, tip = b + (n - 1) * 0xEC, pm = rd32(par + 0x3C);
    if (first > 0) {
        first--;
        fprintf(stderr, "[hair-trace] layout o=%08X nodes=%u par=%08X par.mat(+4)=%08X par.ex(+3C)=%08X scale(+30) %.3f %.3f %.3f\n",
                o, n, par, rd32(par + 4), pm, rdf(par + 0x30), rdf(par + 0x34), rdf(par + 0x38));
        for (const Slot& sl : g_slot) if (sl.used) fprintf(stderr, "[hair-trace] slot chara=%08X m1=%08X n1=%u m2=%08X n2=%u pending=%d valid=%d%c", sl.chara, sl.m1, sl.n1, sl.m2, sl.n2, sl.pending, sl.valid, 10);
        for (uint32_t k = 0; k < n && k < 3; k++) {
            const uint32_t nd = b + k * 0xEC;
            fprintf(stderr, "[hair-trace] node %u:", k);
            for (uint32_t w = 0; w < 0xEC; w += 4) {
                const uint32_t v = rd32(nd + w);
                if (v >= 0x10000u && v < 0x40000000u && !(v & 3)) fprintf(stderr, " +%X=%08X", w, v);
                else fprintf(stderr, " +%X=%.3f", w, rdf(nd + w));
            }
            fprintf(stderr, "\n");
        }
    }
    /* What is drawn: the last node's matrix (+0x3C). Its origin is rigid to the parent bone; the hair is where it
     * points (its x axis, the chain's direction) seen from the parent bone, times the node's length (+0). */
    const uint32_t tm = rd32(tip + 0x3C);
    float tp[3] = { rdf(tip + 4), rdf(tip + 8), rdf(tip + 0xC) };
    float loc[3] = { 0, 0, 0 };
    if (pm && tm >= 0x10000u && tm < 0x40000000u) {
        const float len = rdf(tip);
        for (int j = 0; j < 3; j++) tp[j] = rdf(tm + 16 * j + 0xC) + rdf(tm + 16 * j) * len;
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) loc[i] += rdf(pm + 16 * j + 4 * i) * rdf(tm + 16 * j) * len;
    }
    fprintf(stderr, "[hair-trace] t=%.4f f=%u o=%08X tick=%d a=%.3f sp=%.3f tip %.5f %.5f %.5f par %.5f %.5f %.5f loc %.5f %.5f %.5f\n", song, g_frame, o, g_ctrl_tick,
            g_alpha_now, rdf(SPEED_BASE + 0x14), tp[0], tp[1], tp[2],
            pm ? rdf(pm + 0xC) : 0.0f, pm ? rdf(pm + 0x1C) : 0.0f, pm ? rdf(pm + 0x2C) : 0.0f, loc[0], loc[1], loc[2]);
}

/* 2026-11-14 (el pelo del autor): la fisica por frame con el amortiguamiento corregido.
 *
 * El integrador del osage (0x3D47B0) frena la velocidad con (paso - aire) * (paso - roce), donde "paso" es el
 * del frame. A 30 fps el paso es 1 y eso vale (1-aire)(1-roce); con paso 1/4 queda cuatro veces menos freno
 * del que toca, y por eso al simular cada frame el pelo oscilaba un 13 % de mas (medido el 2026-11-12). El
 * freno correcto para un paso h es h veces el de un paso entero (a primer orden), asi que antes de cada paso
 * se escriben aire y roce tales que (h - aire')(h - roce') = K * h * (1 - aire)(1 - roce), y se restauran al
 * salir. REFINE_HAIR_DT=1 lo enciende (apagado por defecto); REFINE_HAIR_DT_K ajusta K (1,0 por defecto).
 * aire = *(objeto+0x12C) + 0x18, roce = objeto + 0x438, los dos del propio objeto osage.
 * 2026-11-21, two things this mode gets wrong (docs/tiempo_real.md, "El pelo largo"): the integrator
 * (0x3D47B0) reads the block at objeto + 0x40, so these two words are 0x40 short of its air and friction; and the
 * brake is (1 - aire)(1 - roce) with a constant 1.0, not the step. The chains it moves are the bangs and the breasts
 * (two nodes each), not the long hair, which is a3d_frames_on's. It stays as the author's option. */
int   g_hair_dt;       /* REFINE_HAIR_DT=1: fisica cada frame (mas viva, oscila un 10-15 % de mas) */
float g_hair_dt_k = 1.0f;
struct DtSave { uint32_t a_addr, r_addr, g_addr; float a_org, r_org, g_org; uint32_t sp_addr[64]; float sp_org[64]; int sp_n; };
std::unordered_map<uint32_t, std::pair<float,float>> g_dt_org;   /* objeto -> (aire, roce) de paso entero */
DtSave g_dt_cur;

int dt_apply(uint32_t o, float h)
{
    g_dt_cur.a_addr = 0;
    if (!(h > 0.0f) || h >= 0.999f) return 0;
    const uint32_t pb = rd32(o + 0x12C);
    if (!pb) return 0;
    const uint32_t aa = pb + 0x18, ra = o + 0x438;
    auto it = g_dt_org.find(o);
    if (it == g_dt_org.end()) it = g_dt_org.emplace(o, std::make_pair(rdf(aa), rdf(ra))).first;
    const float a0 = it->second.first, r0 = it->second.second;
    const float d1 = (1.0f - a0) * (1.0f - r0);
    if (!(d1 > 0.0f)) return 0;
    float v = g_hair_dt_k * h * d1;
    if (v < 0.0f) v = 0.0f;
    const float side = h - sqrtf(v);
    g_dt_cur.a_addr = aa; g_dt_cur.r_addr = ra; g_dt_cur.a_org = rdf(aa); g_dt_cur.r_org = rdf(ra);
    pw(aa, fbits(side)); pw(ra, fbits(side));
    /* NO se escalan las fuerzas por h: probado el 2026-11-14 (gravedad +0x14 del bloque y rigidez +0 de cada
     * nodo) y sale peor, x1,31 de amplitud y el triple de distancia. El integrador no se deja corregir con
     * estas constantes; para que la fisica por frame sea la del original habria que reescribirlo. */
    g_dt_cur.g_addr = 0; g_dt_cur.sp_n = 0;
    return 1;
}
void dt_undo(void)
{
    if (!g_dt_cur.a_addr) return;
    pw(g_dt_cur.a_addr, fbits(g_dt_cur.a_org));
    pw(g_dt_cur.r_addr, fbits(g_dt_cur.r_org));
    if (g_dt_cur.g_addr) pw(g_dt_cur.g_addr, fbits(g_dt_cur.g_org));
    for (int k = 0; k < g_dt_cur.sp_n; k++) pw(g_dt_cur.sp_addr[k], fbits(g_dt_cur.sp_org[k]));
    g_dt_cur.sp_n = 0; g_dt_cur.g_addr = 0; g_dt_cur.a_addr = 0;
}

void hook_osage_stage(ppu_context* ctx)              /* 0x50E34C, slot +0x10 of 0xBB38D0 */
{
    const uint32_t o = (uint32_t)ctx->gpr[3], mode = (uint32_t)ctx->gpr[4];
    if (g_hair_dt && g_active && g_in_ctrl) {    /* fisica cada frame, con el freno corregido */
        const int on = dt_apply(o, rdf(SPEED_BASE + 0x14));
        func_0050E34C(ctx);
        if (on) dt_undo();
        if (mode == 5 && (g_ht_at >= 0 || g_ht_left > 0)) hair_trace(o);
        return;
    }
    if (g_hair_ex && g_active && g_in_ctrl) {
        if (g_ctrl_tick) {                           /* the original step: a whole one, on the tick */
            uint32_t keep; memcpy(&keep, vm_base + SPEED_BASE + 0x14, 4);
            pw(SPEED_BASE + 0x14, fbits(1.0f));
            true_pose(1);
            func_0050E34C(ctx);
            true_pose(0);
            memcpy(vm_base + SPEED_BASE + 0x14, &keep, 4);
            if (mode == 5) ox_tick(o);
        } else if (mode == 5) vm_base[o + 0x39] = 1;  /* no step between ticks; as stage 5 leaves it, so slot +0x14 does not step either */
        if (mode == 5) {
            ox_show(o);
            if (g_ht_at >= 0 || g_ht_left > 0) hair_trace(o);
        }
        return;
    }
    if ((g_ht_at >= 0 || g_ht_left > 0) && g_in_ctrl && mode == 5) { func_0050E34C(ctx); hair_trace(o); return; }
    if (g_active && g_anchor_on && g_in_ctrl) anchor_write(o);
    const int log = g_pl_on && g_in_ctrl, t = g_ctrl_tick;
    if (log && mode < 6) g_pm.mode[mode][t ? 0 : 1]++;
    if (log && mode == 5) {
        const uint32_t eq = rd32(rd32(o + 0x34) + 0x134), pb = rd32(eq + 0x908), pe = rd32(eq + 0x90C);
        if (pb && (uint32_t)(pe - pb + 0x17) > 0x2E && vm_base[pb + 0xC]) (t ? g_pm.pd_t : g_pm.pd_n)++;
    }
    func_0050E34C(ctx);
    if (!log || mode != 5) return;
    const uint32_t par = rd32(o + 0x10);
    if (par) {
        const float d = pl_move(par | 1, rdf(par + 0x30), rdf(par + 0x34), rdf(par + 0x38));
        if (d >= 0 && !t) { g_pm.root_n++; if (d > 1e-5f) g_pm.root_n_moved++; g_pm.root_mv_n += d; }
    }
    const uint32_t b = rd32(o + 0x40 + 0x34), e = rd32(o + 0x40 + 0x38);
    int k = 0;
    for (uint32_t n = b + 0xEC; b && n < e && k < 64; n += 0xEC, k++) {
        const float d = pl_move(n, rdf(n + 4), rdf(n + 8), rdf(n + 0xC));
        if (d < 0) continue;
        if (g_pl_jerk >= 0.0f) {
            if (t) { g_pm.jk_t++; g_pm.jerk_t += g_pl_jerk; if (g_pl_jerk > g_pm.jkmax_t) g_pm.jkmax_t = g_pl_jerk; }
            else   { g_pm.jk_n++; g_pm.jerk_n += g_pl_jerk; if (g_pl_jerk > g_pm.jkmax_n) g_pm.jkmax_n = g_pl_jerk; }
        }
        if (t) { g_pm.nodes_t++; g_pm.mv_t += d; if (d > g_pm.mvmax_t) g_pm.mvmax_t = d; }
        else   { g_pm.nodes_n++; g_pm.mv_n += d; if (d > g_pm.mvmax_n) g_pm.mvmax_n = d; if (d <= 1e-5f) g_pm.still_n++; }
    }
}
void pl_frame(int tick)
{
    static double t0;
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    const double t = (double)q.QuadPart / (double)f.QuadPart;
    if (!t0) { t0 = t; g_pl.dt_min = 1e9f; g_pl.dt_max = -1e9f; }
    g_pl.frames++; if (tick) g_pl.ticks++;
    if (t - t0 < 1.0) return;
    fprintf(stderr, "[phys-log] song %.2f frames=%u ticks=%u step(+0x14) tick=%u non=%u skip(+0x39) tick=%u non=%u "
            "set(+0x18) tick=%u non=%u b828 tick=%u non=%u outside=%u dt %.4f..%.4f speed %.4f\n",
            rdf(0x12CBE48), g_pl.frames, g_pl.ticks, g_pl.sim_t, g_pl.sim_n, g_pl.skip_t, g_pl.skip_n,
            g_pl.set_t, g_pl.set_n, g_pl.c8_t, g_pl.c8_n, g_pl.out, g_pl.dt_min, g_pl.dt_max, rdf(SPEED_BASE + 0x14));
    fprintf(stderr, "[phys-log2] modes tick/non 0=%u/%u 1=%u/%u 2=%u/%u 3=%u/%u 4=%u/%u 5=%u/%u playdata=%u/%u "
            "node move tick avg %.5f max %.5f (%u) non avg %.5f max %.5f (%u, still %u) parent moved non %u/%u avg %.5f\n",
            g_pm.mode[0][0], g_pm.mode[0][1], g_pm.mode[1][0], g_pm.mode[1][1], g_pm.mode[2][0], g_pm.mode[2][1],
            g_pm.mode[3][0], g_pm.mode[3][1], g_pm.mode[4][0], g_pm.mode[4][1], g_pm.mode[5][0], g_pm.mode[5][1],
            g_pm.pd_t, g_pm.pd_n, g_pm.nodes_t ? g_pm.mv_t / g_pm.nodes_t : 0.0f, g_pm.mvmax_t, g_pm.nodes_t,
            g_pm.nodes_n ? g_pm.mv_n / g_pm.nodes_n : 0.0f, g_pm.mvmax_n, g_pm.nodes_n, g_pm.still_n,
            g_pm.root_n_moved, g_pm.root_n, g_pm.root_n ? g_pm.root_mv_n / g_pm.root_n : 0.0f);
    fprintf(stderr, "[phys-jerk] tick avg %.6f max %.6f (%u) non avg %.6f max %.6f (%u)\n",
            g_pm.jk_t ? g_pm.jerk_t / g_pm.jk_t : 0.0f, g_pm.jkmax_t, g_pm.jk_t, g_pm.jk_n ? g_pm.jerk_n / g_pm.jk_n : 0.0f, g_pm.jkmax_n, g_pm.jk_n);
    fprintf(stderr, "[hair-anchor] on=%d calls=%u written %u skipped(cut/jump/first-tick) %u first %u outside %u alpha1 %u\n",
            g_anchor_on, g_an.calls, g_an.written, g_an.cut, g_an.first, g_an.outside, g_an.alpha1);
    if (g_par.size() > 4096) g_par.clear();
    memset(&g_an, 0, sizeof g_an);
    memset(&g_pm, 0, sizeof g_pm);
    if (g_pl_pos.size() > 4096) g_pl_pos.clear();
    memset(&g_pl, 0, sizeof g_pl); g_pl.dt_min = 1e9f; g_pl.dt_max = -1e9f; t0 = t;
}

/* The characters one tick ahead (2026-11-21). The blend shows a pose between the last two ticks, and each tick's
 * pose is the one of the moment of that tick, so what is drawn trailed the song by one tick (33 ms) while the camera
 * and the stage props, computed every frame on the PV clock, did not (tools/cam_sync.py against 30 fps: best shift
 * +32-36 ms). So on a tick, in a song, every character's motion frame is moved one tick ahead right after
 * TaskRobPrepareAction, through the game's own setter (0x4339E0: the bone data's motion frame and rob_chara + 0x4AC,
 * in 60ths), and back once the flagged tasks are done (lead_off, from hook_robdisp_ctrl). The tasks after it then
 * make the pose of the next tick, the blend from the last tick to it lands on the song's own time, and no code
 * outside the tick sees a frame it would not have had. It has to be after TaskRobPrepareAction: that task steps the
 * frame and keeps it on the song, and a lead given before it is gone when it returns (probe run rf_probe, Cat Food
 * at 120: 1198.16 + 2.00 in, 1200.16 out, the same as without it).
 * On by default since 2026-11-22 (REFINE_ROB_LEAD=0 turns it off, and the camera is then drawn one tick late instead,
 * cam_delay_apply). What kept it off was the hair at the camera cuts: see held_block. */
const float LEAD_FRAMES = 60.0f / 29.97f;           /* one 30 Hz tick in 60ths: what +0x4AC gains a tick */
int      g_lead = 1;
uint32_t g_disp_task;                               /* TaskRobDisp: its list (+0x60) holds the characters */
uint32_t g_led[MAX_CHARA]; int g_led_n;             /* the characters moved ahead on this tick */
uint32_t g_leads;

/* The motion's frame data (bone data + 0x7C -> -> + 8, + 0x438), what 0x4339E0 sets through 0x3EB44. Besides the frame
 * (+0), that setter clears +0x28 and, when the new frame reaches the frame the motion stops at (+0x14, forward unless
 * +0x18 is 2), sets that one to -1: the motion no longer stops. A tick ahead at the end of a motion did just that, and
 * the character played on past its last frame (Rin-chan Now! at 240 fps, 36 s: frame 2167 held at 30, 2278 and under
 * the stage at 240, run r38; REFINE_ROB_LEAD=0: 2167). Ours only move the frame, so the two fields stay the game's,
 * and lead_on leaves a motion standing on its stop where it is. */
uint32_t frame_data(uint32_t chara)
{
    const uint32_t bd = rd32(chara + 0x24);
    const uint32_t a = bd ? rd32(bd + 0x7C) : 0, b = a ? rd32(a) : 0, m = b ? rd32(b + 8) : 0;
    return m ? m + 0x438 : 0;
}
void chara_set_frame(ppu_context* ctx, uint32_t chara, float f)
{
    static ppu_context save;                     /* the game's thread only */
    const uint32_t fd = frame_data(chara);
    uint8_t stop[4] = {}, flag = 0;
    if (fd) { memcpy(stop, vm_base + fd + 0x14, 4); flag = vm_base[fd + 0x28]; }
    memcpy(&save, ctx, sizeof save);
    ctx->gpr[3] = chara;
    ctx->fpr[1] = f;
    func_004339E0(ctx);
    memcpy(ctx, &save, sizeof save);
    if (fd) { memcpy(vm_base + fd + 0x14, stop, 4); vm_base[fd + 0x28] = flag; }
}

void lead_on(ppu_context* ctx)
{
    g_led_n = 0;
    if (!g_lead || !g_disp_task || !port_in_song()) return;
    const uint32_t head = rd32(g_disp_task + 0x60);
    uint32_t node = head ? rd32(head) : 0;
    for (int i = 0; node && node != head && i < 16 && g_led_n < MAX_CHARA; i++, node = rd32(node)) {
        const uint32_t chara = rd32(node + 8);
        if (!chara || (rd32(chara + 0x320) & 1) || !rd32(chara + 0x24)) continue;
        const float f = rdf(chara + 0x4AC);
        if (!(f >= 0.0f) || f > 1e7f) continue;      /* NaN, or not a frame */
        /* A motion standing on its stop frame stays there: nothing ahead. One only coming up to it is led as any other:
         * the script usually moves the stop on before it is reached (Cat Food), and cutting the lead short there
         * jerked the pose and doubled the bangs' swing (hair_cmp x1.9 against x0.99). */
        const uint32_t fd = frame_data(chara);
        if (fd && rd32(fd + 0x18) != 2 && rdf(fd + 0x14) >= 0.0f && f >= rdf(fd + 0x14) - 0.001f) continue;
        chara_set_frame(ctx, chara, f + LEAD_FRAMES);
        g_led[g_led_n++] = chara;
    }
    g_leads += g_led_n;
}

void lead_off(ppu_context* ctx)
{
    for (int i = 0; i < g_led_n; i++) chara_set_frame(ctx, g_led[i], rdf(g_led[i] + 0x4AC) - LEAD_FRAMES);
    g_led_n = 0;
}

/* The tick brought to a cut steps every motion a whole tick, and only part of one has gone by: where the game does not
 * put the motion back on the song each tick, the character gained the rest on every cut. So a motion that went its usual
 * whole step in such a tick goes the part that is due (g_cut_alpha). One that was changed, or put on the song, keeps
 * what the game gave it. Around TaskRobPrepareAction, which steps them; +0x4AC is the frame, in 60ths. */
extern float g_cut_alpha;
uint32_t g_cut_part;
struct CharaStep { uint32_t tick; float before, step; };
std::unordered_map<uint32_t, CharaStep> g_chara_step;
void cut_frames(int after, ppu_context* ctx)
{
    if (!g_disp_task) return;
    const uint32_t head = rd32(g_disp_task + 0x60);
    uint32_t node = head ? rd32(head) : 0;
    if (g_chara_step.size() > 64) g_chara_step.clear();
    for (int i = 0; node && node != head && i < 16; i++, node = rd32(node)) {
        const uint32_t chara = rd32(node + 8);
        if (!chara || !rd32(chara + 0x24)) continue;
        CharaStep& c = g_chara_step[chara];
        const float f = rdf(chara + 0x4AC);
        if (!after) { c.before = f; continue; }
        const float d = f - c.before, usual = c.step;
        c.step = d;
        if (g_cut_alpha < 0 || !(g_cut_alpha < 0.95f) || !(d > 0.1f && d < 8.0f) || fabsf(d - usual) > 0.15f * d) continue;
        chara_set_frame(ctx, chara, c.before + g_cut_alpha * d);
        c.step = usual;
        g_cut_part++;
    }
}
void hook_prepare_ctrl(ppu_context* ctx)
{
    if (!g_active) { func_00428ACC(ctx); return; }
    restore_all(1);                                  /* never let a tick start from a blend */
    ox_restore();
    anchor_restore();
    g_ticked = 1;
    cut_frames(0, ctx);
    func_00428ACC(ctx);
    cut_frames(1, ctx);
    lead_on(ctx);
}

/* The tick on the cut (6d.12, P1). A camera cut seen on a frame without a tick runs the
 * flagged Rob tasks now, through the game's own path for extra steps: 0x4C0F4 calls
 * 0x4BF40(steps, 1) for each step past the first, and that runs only the flagged tasks
 * (0x4ADB8), in their order. Then the step fraction (+0x18) goes to 0, as 0x3C254 leaves
 * it after a whole step, so the next ticks keep the 30 Hz pace from the cut on: the ticks
 * are put in phase with the PV's cuts, as at 30 fps, where every frame is a tick. Only
 * while the song clock runs (a PV), over 30 fps; REFINE_CUT_TICK=0 turns it off. The task
 * manager's current-task field (+0xC), which 0x4BF40 sets and clears around each call,
 * is put back as it was, and so is every register. */
const uint32_t TASKMGR_PTR = 0xBF2A4C;           /* TOC word: address of the task manager pointer */
int      g_cut_tick = 1;
float    g_song_last = -1;
uint32_t g_cut_ticks;

float    g_cut_alpha = -1;                           /* a cut's tick is running: how much of a tick has gone by */

void cut_tick(ppu_context* ctx)
{
    static ppu_context save;                     /* the game's thread only */
    const uint32_t tm = rd32(rd32(TASKMGR_PTR));
    const uint32_t cur = rd32(tm + 0xC);
    memcpy(&save, ctx, sizeof save);
    ctx->gpr[3] = 1;
    ctx->gpr[4] = 1;
    g_cut_alpha = rdf(SPEED_BASE + 0x18);
    func_0004BF40(ctx);
    g_cut_alpha = -1;
    memcpy(ctx, &save, sizeof save);
    const uint32_t be = _byteswap_ulong(cur);
    memcpy(vm_base + tm + 0xC, &be, 4);
    memset(vm_base + SPEED_BASE + 0x18, 0, 4);   /* 0.0f */
    g_cut_ticks++;
}

/* 2026-11-12: the characters of the menus (Home, the module and song setup rooms). There the tasks' logic waits
 * for a 30 Hz tick (g_fps_menu_hold in src/main.cpp), so TaskRobDisp::Ctrl ran on the ticks only, at speed 1.0,
 * and the character and its hair moved at 30 Hz. Now, in the menus: Ctrl takes the phase since the last tick from
 * main.cpp (rob_interp_menu) and the fractional speed, as in a song; and on the frames without a tick the task's
 * Disp (0x3FFC1C, vtable 0xBB1888 slot 5, which still runs every frame) runs Ctrl first. */
int g_menu_on; float g_menu_alpha, g_menu_speed; uint32_t g_main_seq, g_ctrl_seq, g_menu_extra;

/* The long hair (2026-11-21). It is not physics: the twin tails are an Auth3D animation hung from the hair's equipment
 * object (a map at +0x13C of each 0x144-byte object; 0x50D414 -> 0x51B04C; the body object has one too), and its
 * frame is the motion's own: 0x3FD324, inside TaskRobDisp::Ctrl, copies motion + 0x438 of every motion of the
 * bone data (list at +0x7C) into equipment + 0x904, and 0x51B04C hands it to the Auth3D object (0x5E8C4). The motion
 * steps on the 30 Hz ticks, so the hair's own animation stood still between ticks while the head it hangs from was
 * drawn blended, and jumped back at each tick: a saw of about a centimetre at the tip of a tail, 30 times a second
 * (REFINE_TAIL_TRACE, tools/tail_cmp.py, run a3d120).
 *
 * So for the length of Ctrl each motion's frame is the one of the pose this frame draws: cur - (1 - alpha) * d, d
 * being what that motion gained on the last tick (with the characters a tick ahead, REFINE_ROB_LEAD, cur + alpha * d).
 * The game's own frame goes back right after. A motion that is new, changed, looped or was set outside a tick, or a
 * character drawn unblended (a camera cut, a jump), keeps its own frame. REFINE_HAIR_A3D=0 turns it off. */
int g_a3d_on = 1;
struct MotFrame { uint32_t id, tick_no; float cur, d; };
std::unordered_map<uint32_t, MotFrame> g_mot;        /* by motion address */
struct MotSave { uint32_t addr, org; };
MotSave  g_mot_save[64]; int g_mot_n;
struct { uint32_t moved, kept; } g_a3d;

void a3d_frames_on(uint32_t head, float alpha, int tick)
{
    g_mot_n = 0;
    if (!g_a3d_on || !g_blend) return;
    if (g_mot.size() > 512) g_mot.clear();
    uint32_t node = head ? rd32(head) : 0;
    for (int i = 0; node && node != head && i < 16; i++, node = rd32(node)) {
        const uint32_t chara = rd32(node + 8), bd = chara ? rd32(chara + 0x24) : 0;
        if (!bd) continue;
        int blended = 0;
        for (const Slot& sl : g_slot) if (sl.used && sl.chara == chara && sl.pending && !sl.hold) blended = 1;
        const uint32_t sent = rd32(bd + 0x7C);
        uint32_t it = sent ? rd32(sent) : 0;
        for (int k = 0; it && it != sent && k < 16; k++, it = rd32(it)) {
            const uint32_t mot = rd32(it + 8);
            if (!mot) continue;
            const uint32_t id = rd32(mot + 0x428);
            const float f = rdf(mot + 0x438);
            MotFrame& m = g_mot[mot];
            if (m.id != id || !m.tick_no || g_tick_no - m.tick_no > 1) { m.id = id; m.cur = f; m.d = 0; }
            else if (tick) { m.d = f - m.cur; m.cur = f; }
            else if (f != m.cur) { m.cur = f; m.d = 0; }                     /* set outside a tick */
            m.tick_no = g_tick_no;
            const int lead = g_lead && port_in_song();
            const float show = f + (lead ? alpha : alpha - 1.0f) * m.d;
            if (!blended || !(m.d > 0.0f && m.d < 8.0f) || !(show >= 0.0f) || g_mot_n >= 64) { g_a3d.kept++; continue; }
            g_mot_save[g_mot_n].addr = mot + 0x438; g_mot_save[g_mot_n].org = rd32(mot + 0x438); g_mot_n++;
            pw(mot + 0x438, fbits(show));
            g_a3d.moved++;
        }
    }
}
void a3d_frames_off(void)
{
    while (g_mot_n > 0) { g_mot_n--; pw(g_mot_save[g_mot_n].addr, g_mot_save[g_mot_n].org); }
}

/* REFINE_TAIL_TRACE=<song second> (diagnostic): for 8 s of a song, one line per frame and equipment object that has an
 * Auth3D animation, after Ctrl: its Auth3D frame, its last bone (the tip of a tail in the hair's) and the head. */
float g_tail_at = -1.0f;
void tail_trace(uint32_t task, float alpha, int tick)
{
    const float song = rdf(0x12CBE48);
    if (song < g_tail_at || song > g_tail_at + 8.0f || port_in_song() != 1) return;
    const uint32_t head = rd32(task + 0x60);
    const uint32_t c = head && rd32(head) != head ? rd32(rd32(head) + 8) : 0, eq = c ? rd32(c + 0x28) : 0, bd = c ? rd32(c + 0x24) : 0;
    if (!eq || !bd) return;
    const uint32_t hm = rd32(bd + 0x1C) + 10 * 64;                          /* the head bone */
    for (uint32_t k = rd32(eq + 0x8C); k < rd32(eq + 0x90) && k < 64; k++) {
        const uint32_t ob = rd32(eq + 8) + k * 0x144, map = rd32(ob + 0x13C);
        if (!map || !rd32(ob + 0x140) || !rd32(map) || rd32(map) == map) continue;
        const uint32_t x = rd32(rd32(map) + 0x10), list = rd32(x + 0x64), e = list ? rd32(list) : 0;
        if (!e || e == list) continue;
        const uint32_t h = rd32(e + 8), a = rd32(0xBF2ADC) + (h & 0x7FFF) * 0x680;
        const uint32_t skin = rd32(x + 0x10), n = skin ? rd32(skin + 0x2C) : 0, mats = rd32(x + 0x5C);
        if (!n || !mats) continue;
        const uint32_t m = mats + (rd32(rd32(skin + 0x30) + 4 * (n - 1)) & 0x7FFF) * 0x40;
        fprintf(stderr, "[tail] t=%.4f obj=%u tick=%d a=%.3f frame=%.3f game=%.3f tip %.5f %.5f %.5f head %.5f %.5f %.5f%c", song, k, tick, alpha, rdf(a + 0x160), rdf(c + 0x4AC),
                rdf(m + 0xC), rdf(m + 0x1C), rdf(m + 0x2C), rdf(hm + 0xC), rdf(hm + 0x1C), rdf(hm + 0x2C), 10);
    }
}

/* REFINE_BLOCKS_LOG=1 (diagnostic): every 1200 frames, how many blocks of each kind the characters' equipment has
 * (list at object + 0x9C; osage, expression, cloth and the others by vtable) and how many Auth3D maps. */
int g_blocks_log;
void blocks_log(uint32_t head)
{
    std::map<uint32_t, int> vt; int a3d = 0, n = 0;
    for (uint32_t nd = head ? rd32(head) : 0, i = 0; nd && nd != head && i < 16; i++, nd = rd32(nd)) {
        const uint32_t c = rd32(nd + 8), eq = c ? rd32(c + 0x28) : 0;
        if (!eq) continue;
        n++;
        for (uint32_t k = rd32(eq + 0x8C); k < rd32(eq + 0x90) && k < 64; k++) {
            const uint32_t ob = rd32(eq + 8) + k * 0x144, list = rd32(ob + 0x9C), map = rd32(ob + 0x13C);
            if (map && rd32(ob + 0x140) && rd32(map) && rd32(map) != map) a3d++;
            for (uint32_t b = list ? rd32(list) : 0, j = 0; b && b != list && j < 256; j++, b = rd32(b)) if (rd32(b + 8)) vt[rd32(rd32(b + 8))]++;
        }
    }
    fprintf(stderr, "[blocks] %d characters, %d Auth3D;", n, a3d);
    for (auto& v : vt) fprintf(stderr, " %s(%X) %d", v.first == 0xBB38D0 ? "osage" : v.first == 0xBB3828 ? "expr" : v.first == 0xBB37F0 ? "cloth" : "other", v.first, v.second);
    fprintf(stderr, "%c", 10);
}

void hook_robdisp_ctrl(ppu_context* ctx)
{
    g_ctrl_seq = g_main_seq;
    const uint32_t task = (uint32_t)ctx->gpr[3];
    if (!g_active) {
        if (g_cam_trace_end >= 0 || g_cam_trace_at >= 0) cam_trace(task);
        if (g_rt_to >= 0) rob_trace(task);
        g_in_ctrl = 1; g_ctrl_tick = 1; func_003FFCC8(ctx); g_in_ctrl = 0; if (g_tail_at >= 0) tail_trace(task, 1.0f, 1); return;
    }
    g_frame++;
    g_st.frames++;
    if (g_ticked) g_st.ticks++;
    /* 2026-11-12: the phase is the step fraction itself (+0x18), which the game has already advanced by this
     * frame's speed (and wrapped on a tick) when this runs. Adding the speed again, as before, gave at 120 fps
     * the phases 0.46, 0.71, 0.96, 1.0 (clamped): steps of 0.46, 0.25, 0.25 and 0.04 of a tick, a 30 Hz judder
     * of the body that the hair hanging from it made plain (run n12_ht3, REFINE_HAIR_TRACE). At whole steps
     * (30 fps pace, the menus' ticks) the newest pose, as before. */
    float alpha = rdf(SPEED_BASE + 0x14) >= 1.0f ? 1.0f : rdf(SPEED_BASE + 0x18);
    if (g_menu_on) alpha = g_menu_alpha;
    if (!(alpha >= 0.0f)) alpha = 1.0f;              /* NaN as well */
    if (alpha > 1.0f) alpha = 1.0f;
    if (g_dump_at >= 0 && rdf(0x12CBE48) >= g_dump_at) {
        g_dump_at = -1;
        fprintf(stderr, "[rob-interp] dump armed at song %.3f s frame %u\n", rdf(0x12CBE48), g_frame);
        rsx_d3d12_dump_arm();
        g_dump_frames = g_dump_n;
    }
    g_game_t += g_menu_on ? g_menu_speed : rdf(SPEED_BASE + 0x14);
    const int cut = cam_frame(alpha);
    cam_push(g_game_t);
    const float song = rdf(0x12CBE48);
    if (cut && !g_ticked && g_cut_tick && g_cam_sync && g_song_last >= 0 && song != g_song_last) {
        cut_tick(ctx);                           /* sets g_ticked through hook_prepare_ctrl */
        if (g_ticked) { g_cam_snap = 1; g_cam_snaps++; g_cam_cut_wait = 0; g_cam_hold = 0; }
        if (g_stats)
            fprintf(stderr, "[rob-interp] tick on the cut song %.3f s frame %u ticked=%d\n", song, g_frame, g_ticked);
    }
    g_song_last = song;
    g_disp_task = task;
    lead_off(ctx);                                   /* the flagged tasks are done: the game's own frames back */
    const uint32_t head = rd32(task + 0x60);         /* the same list TaskRobDisp::Ctrl walks */
    uint32_t node = head ? rd32(head) : 0;
    for (int i = 0; node && node != head && i < 16; i++) {
        const uint32_t chara = rd32(node + 8);
        if (chara && !(rd32(chara + 0x320) & 1)) chara_frame(chara, alpha);
        node = rd32(node);
    }
    if (g_stats && g_dump_frames > 0) {
        g_dump_frames--;
        fprintf(stderr, "[rob-interp] frame %u song %.3f s ticked=%d alpha=%.2f hold=%d snap=%d\n",
                g_frame, rdf(0x12CBE48), g_ticked, alpha, g_cam_hold, g_cam_snap);
    }
    g_frame_ticked = g_ticked;
    cam_delay_apply();                               /* the camera at the characters' time, before anything draws */
    if (g_cam_trace_end >= 0 || g_cam_trace_at >= 0) cam_trace(task);
    if (g_rt_to >= 0) rob_trace(task);
    if (g_hair_every && !g_ticked)                   /* test (6d.13 Q2): the equipment's step flag on every frame */
        for (uint32_t nd = head ? rd32(head) : 0, i = 0; nd && nd != head && i < 16; i++, nd = rd32(nd)) {
            const uint32_t c = rd32(nd + 8), eq = c ? rd32(c + 0x28) : 0;
            if (!eq) continue;
            for (uint32_t k = rd32(eq + 0x8C); k < rd32(eq + 0x90) && k < 64; k++) vm_base[rd32(eq + 8) + k * 0x144 + 0x12C] = 1;
        }
    if (g_scan_at >= 0) { const uint32_t c0 = head && rd32(head) != head ? rd32(rd32(head) + 8) : 0; if (c0) scan_step(1, c0); }
    const int tick_now = g_ticked;
    g_ticked = 0;
    if (tick_now) g_tick_no++;
    LARGE_INTEGER q0, q1, q2, q3, qf;
    QueryPerformanceCounter(&q0);
    hair_pre(head, tick_now, alpha);
    QueryPerformanceCounter(&q1);
    if (g_pl_on) pl_frame(tick_now);
    g_in_ctrl = 1; g_ctrl_tick = tick_now; g_alpha_now = alpha;
    a3d_frames_on(head, alpha, tick_now);
    if (g_menu_on) {                                 /* the hair and cloth step by the frame's share of a tick */
        uint32_t keep; memcpy(&keep, vm_base + SPEED_BASE + 0x14, 4);
        pw(SPEED_BASE + 0x14, fbits(g_menu_speed));
        func_003FFCC8(ctx);
        memcpy(vm_base + SPEED_BASE + 0x14, &keep, 4);
    } else func_003FFCC8(ctx);
    a3d_frames_off();
    g_in_ctrl = 0;
    if (g_tail_at >= 0) tail_trace(task, alpha, tick_now);
    QueryPerformanceCounter(&q2);
    hair_post(ctx, head, tick_now, alpha);
    QueryPerformanceCounter(&q3);
    QueryPerformanceFrequency(&qf);
    {   /* the hair blend's own cost on the game thread, ms per frame (Ctrl itself excluded) */
        const double ms = 1000.0 * ((double)(q1.QuadPart - q0.QuadPart) + (double)(q3.QuadPart - q2.QuadPart)) / (double)qf.QuadPart;
        g_hs.frames++; g_hs.ms_sum += ms; if (tick_now) g_hs.ms_tick_sum += ms; if (ms > g_hs.ms_max) g_hs.ms_max = ms;
    }
    if (g_blocks_log && g_st.frames % 1200 == 0) blocks_log(head);
    if (g_hair_ex && g_st.frames % 3600 == 0)
        fprintf(stderr, "[hair-ex] ticks %u shown %u snaps %u; true-pose swaps %u skipped %u; hair animations at the drawn pose %u, at their own frame %u%c",
                g_oxs.ticks, g_oxs.shown, g_oxs.snaps, g_ox_swaps, g_ox_swap_skips, g_a3d.moved, g_a3d.kept, 10);
    if (g_stats && g_st.frames % 600 == 0) {
        fprintf(stderr, "[rob-interp] frames=%u ticks=%u blends=%u full=%u snap first=%u layout=%u jump=%u "
                "outside=%u overwritten=%u late=%u skipped=%u max_tick %.3f m %.1f deg; cam cuts=%u holds=%u "
                "held=%u snaps=%u cut_ticks=%u part steps=%u leads=%u\n[hair-blend] on=%d ticks=%u blends=%u rejected=%u stale=%u dropped=%u big=%u words_last=%u words_max=%u max_delta=%.3f cost avg %.3f ms/frame, avg on tick frames %.3f ms, max %.3f ms\n",
                g_st.frames, g_st.ticks, g_st.blends, g_st.full, g_st.snap_first, g_st.snap_layout,
                g_st.snap_jump, g_st.snap_outside, g_st.overwritten, g_st.late, g_st.skipped,
                g_st.max_m, g_st.max_deg, g_cam_cuts, g_cam_holds, g_cam_held_frames, g_cam_snaps, g_cut_ticks, g_cut_part, g_leads,
                g_hair_on, g_hs.ticks, g_hs.blends, g_hs.rejected, g_hs.stale, g_hs.dropped, g_hs.big, g_hs.words_last, g_hs.words_max, g_hs.max_delta,
                g_hs.ms_sum / (g_hs.frames ? g_hs.frames : 1), g_hs.ms_tick_sum / (g_hs.ticks ? g_hs.ticks : 1), g_hs.ms_max);
        g_st.max_m = 0; g_st.max_deg = 0;
    }
}

void hook_robdisp_disp(ppu_context* ctx)             /* 0x3FFC1C */
{
    if (g_active && g_menu_on && g_ctrl_seq != g_main_seq && g_main_seq - g_ctrl_seq < 16) {
        static ppu_context save;
        memcpy(&save, ctx, sizeof save);
        hook_robdisp_ctrl(ctx);
        memcpy(ctx, &save, sizeof save);
        if (g_menu_extra++ % 2000 == 0) fprintf(stderr, "[rob-interp] menus: character frames between ticks %u (alpha %.2f); hair ticks %u shown %u snaps %u%c", g_menu_extra, g_menu_alpha, g_oxs.ticks, g_oxs.shown, g_oxs.snaps, 10);
    }
    func_003FFC1C(ctx);
}

int g_on;

}  // namespace

/* Once a frame from src/main.cpp (fps_guest_frame), for the frame about to run: menus over 30 fps or not, the
 * phase since the last 30 Hz tick and the frame's share of a tick. */
extern "C" void rob_interp_menu(int on, float alpha, float speed)
{
    g_menu_on = on && g_menu_rob; g_menu_alpha = alpha; g_menu_speed = speed; g_main_seq++;
}

/* Called by src/main.cpp after the lifted table is registered, only over 30 fps. */
extern "C" void rob_interp_init(void)
{
    const char* e = getenv("REFINE_ROB_INTERP");
    if (e && atoi(e) == 0) {
        fprintf(stderr, "[rob-interp] off (REFINE_ROB_INTERP=0): the character steps at 30 Hz\n");
        return;
    }
    g_stats = getenv("REFINE_ROB_INTERP_STATS") != NULL;
    if (const char* c = getenv("REFINE_CAM_SYNC")) g_cam_sync = atoi(c) != 0;
    if (const char* c = getenv("REFINE_CUT_TICK")) g_cut_tick = atoi(c) != 0;
    if (const char* b = getenv("REFINE_ROB_BLEND")) g_blend = atoi(b) != 0;
    if (const char* c = getenv("REFINE_ROB_LEAD")) g_lead = atoi(c) != 0;
    if (!g_blend) g_lead = 0;
    g_cam_delay = !g_lead;                           /* the characters ahead, or the camera behind: not both */
    if (const char* c = getenv("REFINE_CAM_DELAY")) g_cam_delay = atoi(c) != 0;
    g_blocks_log = getenv("REFINE_BLOCKS_LOG") != NULL;
    if (const char* c = getenv("REFINE_EXPR_BLEND")) g_expr_on = atoi(c) != 0;
    if (const char* c = getenv("REFINE_HAIR_A3D")) g_a3d_on = atoi(c) != 0;
    if (const char* c = getenv("REFINE_TAIL_TRACE")) g_tail_at = (float)atof(c);
    if (const char* c = getenv("REFINE_ROB_TRACE")) sscanf(c, "%f-%f", &g_rt_from, &g_rt_to);
    if (const char* c = getenv("REFINE_CAM_TRACE")) g_cam_trace_at = (float)atof(c);
    if (!g_blend) g_cam_delay = 0;                   /* no blend, no lag to follow */
    if (!g_blend) g_cut_tick = 0;
    if (const char* hb = getenv("REFINE_HAIR_BLEND")) g_hair_on = atoi(hb) != 0;
    if (!g_blend) g_hair_on = 0;
    if (const char* a = getenv("REFINE_HAIR_ANCHOR")) g_anchor_on = atoi(a) != 0;
    if (!g_blend) g_anchor_on = 0;
    g_cam_hold_on = getenv("REFINE_CAM_HOLD") && atoi(getenv("REFINE_CAM_HOLD")) != 0;
    if (const char* f = getenv("REFINE_FIND_FRAME")) g_find_at = (float)atof(f);
    if (const char* f = getenv("REFINE_HAIR_PROBE")) g_probe_at = (float)atof(f);
    if (const char* f = getenv("REFINE_HAIR_SCAN")) g_scan_at = (float)atof(f);
    if (const char* f = getenv("REFINE_HAIR_EVERY")) g_hair_every = atoi(f) != 0;
    if (const char* d = getenv("REFINE_DUMP_AT")) {   /* with CELLMARK_DUMP and CELLMARK_DUMP_ARMED */
        g_dump_at = (float)atof(d);
        g_dump_n = getenv("CELLMARK_DUMP") ? atoi(getenv("CELLMARK_DUMP")) + 4 : 0;
    }
    ppu_register_function(ROBPREPARE_CTRL, hook_prepare_ctrl);
    ppu_register_function(ROBDISP_CTRL, hook_robdisp_ctrl);
    if ((g_pl_on = getenv("REFINE_PHYS_LOG") != NULL)) {
        ppu_register_function(0x50DAAC, hook_osage_step);
        ppu_register_function(0x50D230, hook_osage_set);
        ppu_register_function(0x50C670, hook_osage_b828);
    }
    if (const char* f = getenv("REFINE_HAIR_TRACE")) g_ht_at = (float)atof(f);
    if (const char* hs = getenv("REFINE_HAIR_SHOW")) g_hair_show = atoi(hs) != 0;
    if (const char* hd = getenv("REFINE_HAIR_DT")) g_hair_dt = atoi(hd) != 0;
    if (!g_blend) g_hair_dt = 0;
    if (const char* hk = getenv("REFINE_HAIR_DT_K")) g_hair_dt_k = (float)atof(hk);
    if (g_hair_dt) { g_hair_ex = 0; fprintf(stderr, "[hair-dt] fisica cada frame con el freno corregido, K=%.2f%c", g_hair_dt_k, 10); }
    /* con la fisica por frame no hay nada que mezclar entre ticks: fuera el ancla y la mezcla del modo viejo */
    if (const char* hx = getenv("REFINE_HAIR_EX")) g_hair_ex = atoi(hx) != 0;
    if (!g_blend) g_hair_ex = 0;
    if (const char* cv = getenv("REFINE_ROB_CURVE")) g_curve = atoi(cv) != 0;
    if (!g_hair_ex) g_curve = 0;                     /* the physics on the drawn pose needs the B-spline */
    if (g_hair_ex || g_hair_dt) { g_anchor_on = 0; g_hair_on = 0; }
    if (g_hair_ex && !g_pl_on) { ppu_register_function(0x50DAAC, hook_osage_step); ppu_register_function(0x50D230, hook_osage_set); }
    if (g_pl_on || g_anchor_on || g_ht_at >= 0 || g_hair_ex || g_hair_dt) ppu_register_function(0x50E34C, hook_osage_stage);
    if (g_expr_on && !g_pl_on) ppu_register_function(0x50C670, hook_osage_b828);
    if (const char* mr = getenv("REFINE_MENU_ROB")) g_menu_rob = atoi(mr) != 0;
    if (g_menu_rob) ppu_register_function(0x3FFC1C, hook_robdisp_disp);
    g_on = 1;
    fprintf(stderr, "[rob-interp] on: pose blended between 30 Hz ticks; hair anchor %s\n",
            g_anchor_on ? "on (REFINE_HAIR_ANCHOR=0 turns it off)" : "off");
}

/* 0161: the frame rate changed in the running game. Called on the game's thread, after
 * rob_interp_frame_end, so no blended pose is pending when it goes off. */
extern "C" void rob_interp_set_active(int on)
{
    if (on != g_active) g_cam_ring_n = 0;        /* the camera ring is from another stretch of time */
    g_active = on;
}

/* Called once a frame on the game's thread, after it drew (src/main.cpp, fps_guest_frame). */
extern "C" void rob_interp_frame_end(void)
{
    if (!g_on) return;
    if (g_scan_at >= 0) { g_ticked = g_frame_ticked; scan_step(0, 0); g_ticked = 0; }
    restore_all(0);
    ox_restore();
    hair_restore();
    anchor_restore();
    cam_restore();
}
