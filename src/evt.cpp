/* evt.cpp - Re:Fine, diagnostic only: the speed test (docs/tiempo_real.md, tools/speed_test.ps1).
 *
 * REFINE_EVT=1 prints, with the real time in ms since boot, every change of what the game is doing:
 *   state  a/b/c/d   the game's state block (*0xBF296C: +0 mode, +4 sub mode, +0xC phase, +0x1C sub state)
 *   task+ / task-    a task entering or leaving the task list (0xBF2A4C), by its RTTI name
 *   aet+ / aet= / aet-   a 2D animation (AET object of the manager task, vtable 0xBA8310) made, ended
 *                    (bit 0 of its flags: it reached its last frame) or removed; its signature is its
 *                    first and last frame and its scene's name
 *   sub              a screen task's own state changing (+0x198), "<task> <from>><to>"
 *   speed            the game's speed going to 0 and back (0xC22F50)
 *   key              a key of the route pressed
 *   motion           once a second: guest frames, frames whose drawn vertices and constants differ from the
 *                    frame before (the backend's hash, g_port_frame_hash) and 30 Hz ticks
 *   count            REFINE_EVT_SCAN=1, over 30 fps: a word of a task's object (or of a block it points at) that
 *                    goes up or down by exactly 1 a frame for 12 frames, as an integer or as a whole float:
 *                    a frame counter, which runs fast at more fps. Printed once per task and offset.
 *   rate             REFINE_EVT_SCAN=1: a float of a task that moves by the SAME amount every frame -- an
 *                    accumulator that does not take the game speed, so it runs fast at more fps.
 * The same run at two frame rates must give the same events at the same times: what comes sooner or later
 * counts frames, not time (tools/speed_cmp.py).
 *
 * REFINE_ROUTE=<file> drives the run by those events instead of by the clock, so that loading times do not
 * move the presses. One step per line:
 *   wait <text> [seconds]   until an event line contains <text> (60 s by default, then the route stops)
 *   sleep <ms>
 *   press <letters> <ms>    the PAD_KEYS letters (libs/input/cellPad.c); needs PAD_BOT or PAD_KEYS set
 *   until <text> <letters> <ms>   press <letters> (100 ms) every <ms> until an event line contains <text> (120 s at most)
 *   taps <letters> <count> <period_ms>   <count> presses of <letters>, one every <period_ms>, queued in the
 *                    pad layer the way a real key's press is (never missed by the poll). At the end it prints
 *                    an event with how many the game read, so 1000 presses must be 1000 edges at any fps.
 *   fps <n>          cambia fps_target en caliente, como el menu del port
 *   menu             opens or closes the port's menu, as F1 does
 *   speed            an event with the game's speed (0 = the song is paused)
 *   mark <name>             an event of its own, to cut the comparison in sections
 *   close                   WM_CLOSE to the game's window
 * It only reads guest memory and holds keys; it changes nothing in the game. */

#include "ppu_recomp.h"
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

extern "C" volatile char g_pad_bot_keys[12];
extern "C" void cellPad_script_tap(const char* letters);
extern "C" void port_set_fps_target(int v);   /* src/main.cpp: cambio de fps en caliente (0161) */                      /* libs/input/cellPad.c, 0191 */
extern "C" void cellPad_tap_stats(unsigned*, unsigned*, unsigned*, unsigned*);
extern "C" volatile unsigned long long g_port_frame_hash;   /* libs/video, 0190: this frame's drawn data */
extern "C" void port_menu_toggle(void);       /* src/port_menu */
extern "C" int g_fps_not_tick;
extern "C" int port_in_song(void);            /* src/main.cpp */
extern "C" int g_fps_high;      /* la velocidad del juego es nuestra y fps > 30 (src/main.cpp) */

namespace {

int g_on = -1;
int g_quiet;       /* REFINE_EVT=2: solo lo que busca pasos por frame (count, rate), sin tareas ni 2D */
double g_t0;
FILE* g_out;

uint32_t rd(uint32_t a) { return _byteswap_ulong(*(const uint32_t*)(vm_base + a)); }
float rdf(uint32_t a) { uint32_t w = rd(a); float f; memcpy(&f, &w, 4); return f; }
bool ptr_ok(uint32_t a) { return a >= 0x10000u && a < 0x40000000u; }

double now_s(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

/* ---- the route -------------------------------------------------------------------------- */
struct Step { int op; std::string arg; double n; std::string keys; double n2; };   /* 0 wait, 1 sleep, 2 press, 3 mark, 4 close, 5 until, 6 taps */
std::vector<Step> g_route;
size_t g_pc;
double g_step_from = -1, g_release = 0;
bool g_wait_hit;

void emit(double t, const char* kind, const char* sig)
{
    fprintf(g_out, "[ev] %.1f %s %s%c", (t - g_t0) * 1e3, kind, sig, 10);
    if (g_pc < g_route.size() && (g_route[g_pc].op == 0 || g_route[g_pc].op == 5) && !g_wait_hit) {
        char line[400]; snprintf(line, sizeof line, "%s %s", kind, sig);
        if (strstr(line, g_route[g_pc].arg.c_str())) g_wait_hit = true;
    }
}

void route_load(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[route] cannot open %s%c", path, 10); return; }
    char ln[400];
    while (fgets(ln, sizeof ln, f)) {
        char op[16] = "", a[300] = ""; double n = 0;
        size_t L = strlen(ln); while (L && (ln[L - 1] == '\n' || ln[L - 1] == '\r' || ln[L - 1] == ' ')) ln[--L] = 0;
        if (!L || ln[0] == '#') continue;
        if (sscanf(ln, "%15s", op) != 1) continue;
        const char* rest = ln + strlen(op); while (*rest == ' ') rest++;
        Step s{ -1, "", 0, "", 0 }; char k[16] = "";
        if (!strcmp(op, "wait")) {
            s.op = 0; s.n = 60;
            /* a trailing number is the timeout */
            const char* sp = strrchr(rest, ' ');
            char* end = nullptr;
            if (sp) { const double v = strtod(sp + 1, &end); if (end && !*end && end != sp + 1) { s.n = v; s.arg.assign(rest, sp - rest); } }
            if (s.arg.empty()) s.arg = rest;
        }
        else if (!strcmp(op, "sleep")) { s.op = 1; s.n = atof(rest); }
        else if (!strcmp(op, "press") && sscanf(rest, "%299s %lf", a, &n) == 2) { s.op = 2; s.arg = a; s.n = n; }
        else if (!strcmp(op, "until") && sscanf(rest, "%299s %15s %lf", a, k, &n) == 3) { s.op = 5; s.arg = a; s.keys = k; s.n = n; for (auto& c : s.arg) if (c == '_') c = ' '; }
        else if (!strcmp(op, "taps") && sscanf(rest, "%299s %lf %lf", a, &n, &s.n2) == 3) { s.op = 6; s.arg = a; s.n = n; }
        else if (!strcmp(op, "fps")) { s.op = 7; s.n = atof(rest); }
        else if (!strcmp(op, "menu")) s.op = 8;
        else if (!strcmp(op, "speed")) s.op = 9;
        else if (!strcmp(op, "mark")) { s.op = 3; s.arg = rest; }
        else if (!strcmp(op, "close")) s.op = 4;
        if (s.op < 0) { fprintf(stderr, "[route] bad line: %s%c", ln, 10); continue; }
        g_route.push_back(s);
    }
    fclose(f);
    fprintf(stderr, "[route] %zu steps from %s%c", g_route.size(), path, 10);
}

BOOL CALLBACK find_win(HWND h, LPARAM out)
{
    DWORD pid = 0; char title[16] = "";
    GetWindowThreadProcessId(h, &pid);
    GetWindowTextA(h, title, sizeof title);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !strncmp(title, "Re:Fine", 7)) { *(HWND*)out = h; return FALSE; }
    return TRUE;
}

void route_frame(double t)
{
    if (g_release && t >= g_release) { g_pad_bot_keys[0] = 0; g_release = 0; }
    while (g_pc < g_route.size()) {
        const Step& s = g_route[g_pc];
        if (g_step_from < 0) g_step_from = t;
        if (s.op == 0) {
            if (g_wait_hit) { g_wait_hit = false; }
            else if (t - g_step_from > s.n) {
                char b[340]; snprintf(b, sizeof b, "timeout waiting for '%s'", s.arg.c_str());
                emit(t, "route", b);
                g_pc = g_route.size(); g_route.push_back(Step{ 4, "", 0, "", 0 });   /* give up: close */
                g_step_from = -1; continue;
            }
            else return;
        }
        else if (s.op == 5) {
            static double next_press;
            if (g_wait_hit) { g_wait_hit = false; next_press = 0; }
            else if (t - g_step_from > 120) { emit(t, "route", "timeout in until"); g_pc = g_route.size(); g_route.push_back(Step{ 4, "", 0, "", 0 }); g_step_from = -1; next_press = 0; continue; }
            else {
                if (!g_release && t >= next_press) {
                    memset((void*)g_pad_bot_keys, 0, sizeof g_pad_bot_keys);
                    memcpy((void*)g_pad_bot_keys, s.keys.c_str(), s.keys.size() < 11 ? s.keys.size() : 11);
                    g_release = t + 0.1; next_press = t + s.n / 1e3;
                    emit(t, "key", s.keys.c_str());
                }
                return;
            }
        }
        else if (s.op == 6) {
            static int sent; static double next; static unsigned q0, g0, h0, d0;
            if (!sent) { cellPad_tap_stats(&q0, &g0, &h0, &d0); next = t; }
            if (sent < (int)s.n) {
                if (t < next) return;
                cellPad_script_tap(s.arg.c_str());
                next += s.n2 / 1e3;
                sent++;
                return;
            }
            if (t - next < 0.5) return;        /* half a second for the last ones to be read */
            unsigned q, g, h, d;
            cellPad_tap_stats(&q, &g, &h, &d);
            char b[200];
            snprintf(b, sizeof b, "%s x%d every %.0f ms: queued %u, given %u, held %u, dropped %u",
                     s.arg.c_str(), sent, s.n2, q - q0, g - g0, h - h0, d - d0);
            emit(t, "taps", b);
            sent = 0;
        }
        else if (s.op == 1) { if (t - g_step_from < s.n / 1e3) return; }
        else if (s.op == 2) {
            if (g_release) return;               /* the press before is still down */
            memset((void*)g_pad_bot_keys, 0, sizeof g_pad_bot_keys);
            memcpy((void*)g_pad_bot_keys, s.arg.c_str(), s.arg.size() < 11 ? s.arg.size() : 11);
            g_release = t + s.n / 1e3;
            emit(t, "key", s.arg.c_str());
        }
        else if (s.op == 7) { char b[48]; snprintf(b, sizeof b, "%d", (int)s.n); emit(t, "fps", b); port_set_fps_target((int)s.n); }
        else if (s.op == 8) { emit(t, "menu", "toggle"); port_menu_toggle(); }
        else if (s.op == 9) {
            uint32_t w = rd(0xC22F50); float f; memcpy(&f, &w, 4);
            char b[32]; snprintf(b, sizeof b, "%.3f", f); emit(t, "gamespeed", b);
        }
        else if (s.op == 3) emit(t, "mark", s.arg.c_str());
        else if (s.op == 4) {
            HWND w = nullptr; EnumWindows(find_win, (LPARAM)&w);
            emit(t, "route", "close");
            fflush(g_out);
            if (w) PostMessageA(w, WM_CLOSE, 0, 0);
        }
        g_pc++; g_step_from = -1;
    }
}

/* ---- what the game is doing ---------------------------------------------------------------- */
std::string cstr(uint32_t a, int max = 48)
{
    std::string s;
    if (!ptr_ok(a)) return s;
    for (int i = 0; i < max; i++) { const char c = (char)vm_base[a + i]; if (c < 0x20 || c > 0x7E) break; s += c; }
    return s;
}

std::string task_name(uint32_t task)
{
    const uint32_t vt = rd(task);
    char b[32];
    if (!ptr_ok(vt)) { snprintf(b, sizeof b, "vt%08X", vt); return b; }
    const uint32_t ti = rd(vt - 4);
    std::string n = ptr_ok(ti) ? cstr(rd(ti + 4), 240) : "";
    if (n.size() > 44) n = n.substr(n.size() - 44);   /* the long ones differ at the end */
    if (n.empty()) { snprintf(b, sizeof b, "vt%08X", vt); return b; }
    return n;
}

struct Aet { uint32_t flags; float f0, f1, frame; std::string sig; bool seen; };
std::map<uint32_t, Aet> g_aet;                 /* by node address */
std::map<std::string, int> g_tasks;            /* name -> count */
uint32_t g_aet_mgr;
int g_sub = 1;                                 /* REFINE_EVT_SUB=0: no "sub" lines */
int g_aet_dump = 0;
int g_aet_frames = 0;                          /* REFINE_EVT_AETF=1: once a second while the game is stopped, every
                                                  object's frame (+0x74), flags and rate: does the 2D move? */

void tasks_frame(double t)
{
    const uint32_t work = rd(rd(0xBF2A4C));
    if (!ptr_ok(work)) return;
    const uint32_t head = rd(work + 4);
    if (!ptr_ok(head)) return;
    std::map<std::string, int> cur;
    uint32_t node = rd(head);
    g_aet_mgr = 0;
    for (int n = 0; node != head && n < 400; n++) {
        if (!ptr_ok(node)) return;
        const uint32_t task = rd(node + 8);
        if (ptr_ok(task)) {
            if (rd(task) == 0xBA8310u) g_aet_mgr = task;
            const std::string nm = task_name(task);
            cur[nm]++;
            /* the screens' own state (the Sel*, Adv* and result tasks switch on +0x198; SelPv on +0xA1C) */
            if (g_sub) {
                static std::map<uint32_t, uint32_t> last;
                const uint32_t st = rd(task + (rd(task) == 0xBB2C98u ? 0xA1C : 0x198));
                auto it = last.find(task);
                if (it == last.end()) last[task] = st;
                else if (it->second != st) {
                    if (st < 64 && it->second < 64) { char b[120]; snprintf(b, sizeof b, "%s %u>%u", nm.c_str(), it->second, st); emit(t, "sub", b); }
                    it->second = st;
                }
            }
        }
        node = rd(node);
    }
    if (!g_quiet) {
        for (auto& kv : cur) { const int was = g_tasks.count(kv.first) ? g_tasks[kv.first] : 0; for (int i = was; i < kv.second; i++) emit(t, "task+", kv.first.c_str()); }
        for (auto& kv : g_tasks) { const int is = cur.count(kv.first) ? cur[kv.first] : 0; for (int i = is; i < kv.second; i++) emit(t, "task-", kv.first.c_str()); }
    }
    g_tasks.swap(cur);
}

/* The manager's objects: a std::map at +0x60 (head node; a node: +0 left, +4 parent, +8 right, +0xC key,
 * +0x10 the object, +0x1D5 nil). The object (0x1416C): +8 its scene (+0xC frames per second), +0x14 first
 * frame, +0x18 last, +0x1C flags, +0x60 rate, +0x74 the frame it is at. */
void aet_walk(uint32_t node, uint32_t head, int depth, std::vector<uint32_t>& out)
{
    if (!ptr_ok(node) || node == head || depth > 40 || out.size() > 2000) return;
    if (vm_base[node + 0x1D5]) return;
    aet_walk(rd(node), head, depth + 1, out);
    out.push_back(node);
    aet_walk(rd(node + 8), head, depth + 1, out);
}

/* REFINE_HUD_TRACE=<file>:<from>-<to> (seconds of song, or with
 * m<from>-<to> seconds of the run, for the menus): every 2D object of every frame in that stretch, whole, for
 * tools/hud_cmp.py: a frame is u32 'HUD1', u32 n, f64 time, f32 song clock, u32 tick, f32 speed, u32 0, and n times
 * { u32 key, u32 node, 0x1C4 bytes of the object as the game holds them }. */
void hud_trace(double t)
{
    static int state, menus; static FILE* f; static float from, to;
    if (state == 0) {
        const char* e = getenv("REFINE_HUD_TRACE");
        const char* c = e ? strrchr(e, ':') : NULL;
        state = -1;
        menus = c && c[1] == 'm';
        if (c && sscanf(c + 1 + menus, "%f-%f", &from, &to) == 2) {
            f = fopen(std::string(e, c - e).c_str(), "wb");
            if (f) { setvbuf(f, NULL, _IOFBF, 8u << 20); state = 1; }
        }
    }
    if (state < 1) return;
    const float song = menus ? (float)(t - g_t0) : rdf(0x12CBE48);
    if (song > to + 0.5f && (menus || port_in_song())) { fclose(f); state = -1; fprintf(stderr, "[hud-trace] done%c", 10); return; }
    if (song < from || song > to || (port_in_song() != 1 && !menus) || !g_aet_mgr) return;
    const uint32_t head = rd(g_aet_mgr + 0x60);
    if (!ptr_ok(head)) return;
    std::vector<uint32_t> nodes;
    aet_walk(rd(head + 4), head, 0, nodes);
    struct { uint32_t magic, n; double t; float song; uint32_t tick; float speed; uint32_t zero; } h =
        { 0x31445548u, (uint32_t)nodes.size(), t, song, (uint32_t)!g_fps_not_tick, rdf(0xC22F50), 0 };
    fwrite(&h, sizeof h, 1, f);
    for (uint32_t n : nodes) {
        const uint32_t k[2] = { rd(n + 0xC), n };
        fwrite(k, sizeof k, 1, f);
        fwrite(vm_base + n + 0x10, 0x1C4, 1, f);
    }
}

void aet_frame(double t)
{
    if (!g_aet_mgr) return;
    const uint32_t head = rd(g_aet_mgr + 0x60);
    if (!ptr_ok(head)) return;
    std::vector<uint32_t> nodes;
    aet_walk(rd(head + 4), head, 0, nodes);
    for (auto& kv : g_aet) kv.second.seen = false;
    for (uint32_t n : nodes) {
        const uint32_t o = n + 0x10;
        const uint32_t flags = rd(o + 0x1C);
        auto it = g_aet.find(n);
        const float f0 = rdf(o + 0x14), f1 = rdf(o + 0x18);
        if (it == g_aet.end() || it->second.f0 != f0 || it->second.f1 != f1) {
            if (it != g_aet.end()) emit(t, "aet-", it->second.sig.c_str());
            Aet a; a.flags = flags; a.f0 = f0; a.f1 = f1; a.frame = rdf(o + 0x74); a.seen = true;
            /* the signature: a name the object points at, if any, and its frame range */
            std::string name;
            for (int k = 0; k < 0x60 && name.empty(); k += 4) {
                const uint32_t p = rd(o + k);
                if (!ptr_ok(p)) continue;
                std::string s = cstr(p, 40);
                if (s.size() >= 4) { name = s; break; }
                const uint32_t q = rd(p);
                if (ptr_ok(q)) { s = cstr(q, 40); if (s.size() >= 4) name = s; }
            }
            char b[200]; snprintf(b, sizeof b, "%s[%g-%g]%s", name.c_str(), f0, f1, (flags & 0x10000) ? "loop" : "");
            a.sig = b;
            if (g_aet_dump > 0) { g_aet_dump--;
                fprintf(g_out, "[evt-aet] node %08X key %08X:", n, rd(n + 0xC));
                for (int k = 0; k < 0x80; k += 4) fprintf(g_out, " %08X", rd(o + k));
                fputc(10, g_out); }
            g_aet[n] = a;
            emit(t, "aet+", b);
            continue;
        }
        Aet& a = it->second;
        a.seen = true;
        if ((flags & 1) && !(a.flags & 1)) emit(t, "aet=", a.sig.c_str());
        a.flags = flags; a.frame = rdf(o + 0x74);
    }
    for (auto it = g_aet.begin(); it != g_aet.end(); ) {
        if (!it->second.seen) { emit(t, "aet-", it->second.sig.c_str()); it = g_aet.erase(it); }
        else ++it;
    }
    static double last_dump;
    if (g_aet_frames && rdf(0xC22F50) == 0.0f && t - last_dump >= 1.0) {   /* t in seconds */
        last_dump = t;
        for (uint32_t n : nodes) {
            const uint32_t o = n + 0x10;
            fprintf(g_out, "[evt-aetf] %.2f %08X frame %.3f [%g-%g] flags %08X rate %.3f scene fps %.3f\n", t, n, rdf(o + 0x74),
                    rdf(o + 0x14), rdf(o + 0x18), rd(o + 0x1C), rdf(o + 0x60), ptr_ok(rd(o + 8)) ? rdf(rd(o + 8) + 0xC) : -1.0f);
        }
    }
}

void state_frame(double t)
{
    static uint32_t last[4] = { ~0u, ~0u, ~0u, ~0u };
    const uint32_t st = rd(0xBF296C);
    if (!ptr_ok(st)) return;
    const uint32_t cur[4] = { rd(st), rd(st + 4), rd(st + 0xC), rd(st + 0x1C) };
    if (memcmp(cur, last, sizeof cur)) {
        memcpy(last, cur, sizeof cur);
        char b[64]; snprintf(b, sizeof b, "%u/%u/%u/%u", cur[0], cur[1], cur[2], cur[3]);
        emit(t, "state", b);
    }
    static int stopped = -1;
    const int s = rdf(0xC22F50) == 0.0f;
    if (s != stopped) { stopped = s; emit(t, "speed", s ? "0" : "go"); }
}

/* ---- the frame counter scan ------------------------------------------------------------------ */
int g_scan = 0;
struct ScanBlk { std::vector<uint32_t> prev; std::vector<int8_t> run_i, run_f, run_d; std::vector<float> dlt; uint32_t seen; };
std::map<std::string, ScanBlk> g_blk;          /* by label */
std::map<std::string, int> g_reported;
uint32_t g_scan_frame;

bool readable(uint32_t a, uint32_t n)
{
    static std::map<uint32_t, bool> page; static uint32_t stamp;
    if (stamp != g_scan_frame / 64) { stamp = g_scan_frame / 64; page.clear(); }
    for (uint32_t p = a >> 12; p <= (a + n - 1) >> 12; p++) {
        auto it = page.find(p);
        if (it == page.end()) {
            MEMORY_BASIC_INFORMATION mi;
            const bool ok = VirtualQuery(vm_base + ((uint64_t)p << 12), &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            it = page.emplace(p, ok).first;
        }
        if (!it->second) return false;
    }
    return true;
}

void scan_block(double t, const std::string& label, uint32_t a, uint32_t words)
{
    if (!readable(a, words * 4)) return;
    ScanBlk& b = g_blk[label];
    if (b.prev.size() != words || b.seen + 1 != g_scan_frame) {
        b.prev.assign(words, 0); b.run_i.assign(words, 0); b.run_f.assign(words, 0);
        b.run_d.assign(words, 0); b.dlt.assign(words, 0.0f);
        for (uint32_t i = 0; i < words; i++) b.prev[i] = rd(a + 4 * i);
        b.seen = g_scan_frame; return;
    }
    b.seen = g_scan_frame;
    for (uint32_t i = 0; i < words; i++) {
        const uint32_t w = rd(a + 4 * i), o = b.prev[i];
        b.prev[i] = w;
        if (w == o) { b.run_i[i] = b.run_f[i] = b.run_d[i] = 0; continue; }
        const int32_t di = (int32_t)(w - o);
        float fw, fo; memcpy(&fw, &w, 4); memcpy(&fo, &o, 4);
        const float df = fw - fo;
        const bool whole = fw == (float)(int)fw && fo == (float)(int)fo && fw > -1e6f && fw < 1e6f;
        b.run_i[i] = (di == 1 || di == -1) && w < 0x1000000u ? (int8_t)(b.run_i[i] + 1) : 0;
        b.run_f[i] = whole && (df == 1.0f || df == -1.0f) ? (int8_t)(b.run_f[i] + 1) : 0;
        /* 2026-11-14: a float that moves by the SAME amount every frame is an accumulator that does not take
         * the game speed (the scrolling text of the menus, the cubes): at 120 fps it runs four times as fast.
         * The ones that do take the speed move by speed * k, and the speed of a frame is never twice the same
         * (it is its real time), so they do not come out here. */
        const float af = df < 0 ? -df : df;
        const int sane = fo > -1e7f && fo < 1e7f && fw > -1e7f && fw < 1e7f && af > 1e-6f && af < 1e5f;
        const float pd = b.dlt[i], apd = pd < 0 ? -pd : pd;
        b.run_d[i] = (sane && apd > 0 && (df > 0) == (pd > 0) && (af > apd ? af - apd : apd - af) <= 0.002f * af)
                     ? (int8_t)(b.run_d[i] + 1) : 0;
        b.dlt[i] = sane ? df : 0.0f;
        if (b.run_i[i] == 12 || b.run_f[i] == 12) {
            char k[300]; snprintf(k, sizeof k, "%s+0x%X %s", label.c_str(), i * 4, b.run_i[i] == 12 ? "int" : "float");
            if (!g_reported[k]++) emit(t, "count", k);
            b.run_i[i] = b.run_f[i] = 0;
        }
        if (b.run_d[i] == 12) {
            char k[320];
            if (!strncmp(label.c_str(), "mem", 3)) snprintf(k, sizeof k, "%08X paso %+.4f por frame (era %.4f)", a + i * 4, df, fo);
            else snprintf(k, sizeof k, "%s+0x%X paso %+.4f por frame", label.c_str(), i * 4, df);
            if (!g_reported[k]++) { char b2[380]; snprintf(b2, sizeof b2, "%s (velocidad %.3f)", k, rdf(0xC22E84)); emit(t, "rate", b2); }
            b.run_d[i] = 0;
        }
    }
}

/* 2026-11-14: the 3D animation system of the menus and stages (auth_3d). Its objects are not inside a task,
 * so the scan above never saw them: the manager is at *0xBF2ADC, its live ids between +0xBC and +0xC0, and an
 * object is manager + (id & 0x7FFF) * 0x680 with its own time at +0x98 (0x709D8 advances it). */
void auth3d_scan(double t)
{
    const uint32_t mgr = rd(0xBF2ADC);
    { static int once; if (!once++) fprintf(g_out, "[auth3d] manager=%08X%c", mgr, 10); }
    if (!ptr_ok(mgr)) return;
    const uint32_t b = rd(mgr + 0xBC), e = rd(mgr + 0xC0);
    if (!ptr_ok(b) || e < b || e - b > 0x4000) return;
    int n = 0;
    for (uint32_t p = b; p < e && n < 64; p += 4, n++) {
        const uint32_t id = rd(p);
        if (id & 0x80000000u || (id & 0x7FFF) >= 0x400) continue;
        const uint32_t o = mgr + (id & 0x7FFF) * 0x680;
        if (!ptr_ok(o) || rd(o + 0xCC) != id) continue;
        char lb[40]; snprintf(lb, sizeof lb, "auth3d#%u", id & 0x7FFF);
        { static unsigned seen; if (seen++ % 4000 == 0) fprintf(g_out, "[auth3d] objetos vivos: id=%08X obj=%08X (visto %u)%c", id, o, seen, 10); }
        scan_block(t, lb, o, 0x1A0);              /* el objeto entero (0x680 bytes) */
    }
}

/* 2026-11-14: the sweep. REFINE_EVT_SWEEP=<MB from>-<MB to> walks the game's memory a window at a time
 * (0x40000 bytes, 24 frames each, which at 120 fps is a fifth of a second) looking for the same thing as the
 * scan above: a float that moves by the SAME amount every frame, which is a step that does not take the game
 * speed and so runs fast at more fps. It prints the absolute address, and PPU_WWATCH=<address> then says who
 * writes it. It is the way to find these without knowing whose structure they are in. */
uint32_t g_sw_lo, g_sw_hi, g_sw_at;
int g_sw_left;
void sweep_frame(double t)
{
    if (!g_sw_hi) return;
    if (g_sw_left <= 0) {                      /* next window */
        g_sw_at = g_sw_at + 0x40000 >= g_sw_hi ? g_sw_lo : g_sw_at + 0x40000;
        g_sw_left = 26;
        char b[64]; snprintf(b, sizeof b, "ventana %08X", g_sw_at);
        if (g_sw_at == g_sw_lo) emit(t, "sweep", "vuelta completa");
        (void)b;
    }
    g_sw_left--;
    char lb[32]; snprintf(lb, sizeof lb, "mem%08X", g_sw_at);
    scan_block(t, lb, g_sw_at, 0x10000);       /* 64K palabras = 256 KB */
}

void scan_frame(double t)
{
    if (!g_scan || rdf(0xC22F50) >= 0.999f) return;      /* at speed 1 every counter is right */
    g_scan_frame++;
    const uint32_t work = rd(rd(0xBF2A4C));
    if (!ptr_ok(work)) return;
    const uint32_t head = rd(work + 4);
    if (!ptr_ok(head)) return;
    uint32_t node = rd(head);
    std::map<std::string, int> nth;
    for (int n = 0; node != head && n < 400; n++) {
        if (!ptr_ok(node)) return;
        const uint32_t task = rd(node + 8);
        if (ptr_ok(task)) {
            std::string name = task_name(task);
            const int k = nth[name]++;
            { char b[24]; snprintf(b, sizeof b, "(vt%X)", rd(task)); name += b; }
            if (k) { char b[8]; snprintf(b, sizeof b, "#%d", k); name += b; }
            const uint32_t W = 0x400;                 /* 4 KB of the task's object */
            scan_block(t, name, task, W);
            if (g_scan > 1 && readable(task, W * 4))  /* =2: and 1 KB of what it points at */
                for (uint32_t i = 1; i < W; i++) {
                    const uint32_t p = rd(task + 4 * i);
                    if (p < 0x10000000u || p >= 0x40000000u || (p & 3) || (p >= task && p < task + W * 4)) continue;
                    char b[32]; snprintf(b, sizeof b, "+0x%X->%08X", i * 4, p);
                    scan_block(t, name + b, p, 0x100);
                }
        }
        node = rd(node);
    }
    auth3d_scan(t);
    sweep_frame(t);
}

void motion_frame(double t)
{
    static double from, steps; static unsigned frames, moved, ticks, high0; static unsigned long long last;
    const unsigned long long h = g_port_frame_hash;
    frames++; if (h != last) moved++; last = h;
    if (!g_fps_not_tick) ticks++;
    steps += rdf(0xC22E84);                    /* the speed the game is stepping with, in 1/29.97 s */
    if (!g_fps_high) high0++;                  /* frames en que el juego lleva su propia velocidad */
    if (t - from >= 1.0) {
        /* pace: game time / real time. 1.000 = the original speed, over 1 the game runs fast */
        if (from > 0) { char b[96]; snprintf(b, sizeof b, "frames=%u moved=%u ticks=%u pace=%.3f high0=%u", frames, moved, ticks, steps / 29.97 / (t - from), high0); emit(t, "motion", b); }
        from = t; frames = moved = ticks = high0 = 0; steps = 0;
    }
}

}  // namespace

extern "C" void evt_frame(void)
{
    if (g_on < 0) {
        const char* e = getenv("REFINE_EVT"); const char* r = getenv("REFINE_ROUTE");
        g_on = (e && *e != '0') || r;
        g_quiet = e && *e == '2';
        if (!g_on) return;
        g_t0 = now_s(); g_out = stderr;
        if (const char* d = getenv("REFINE_EVT_AETDUMP")) g_aet_dump = atoi(d);
        if (const char* d = getenv("REFINE_EVT_AETF")) g_aet_frames = atoi(d);
        if (const char* d = getenv("REFINE_EVT_SCAN")) g_scan = atoi(d);
        if (const char* d = getenv("REFINE_EVT_SWEEP")) { unsigned a0 = 0, b0 = 0;
            if (sscanf(d, "%u-%u", &a0, &b0) == 2 && b0 > a0) { g_sw_lo = a0 << 20; g_sw_hi = b0 << 20; g_sw_at = g_sw_lo; g_scan = g_scan ? g_scan : 1;
                fprintf(stderr, "[sweep] de %08X a %08X, 256 KB cada 26 frames%c", g_sw_lo, g_sw_hi, 10); } }
        if (const char* d = getenv("REFINE_EVT_SUB")) g_sub = atoi(d);
        if (r) route_load(r);
    }
    if (!g_on) return;
    const double t = now_s();
    hud_trace(t);
    if (!g_quiet) { state_frame(t); tasks_frame(t); aet_frame(t); }
    else { tasks_frame(t); }                 /* hace falta para el escaneo por tarea */
    motion_frame(t);
    scan_frame(t);
    route_frame(t);
}
