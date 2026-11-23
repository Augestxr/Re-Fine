/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dwmapi.h>    /* DWM_TIMING_INFO: REFINE_PACE (the function is loaded, not linked) */
#include <algorithm>   /* std::sort: PS3_FRAMESTATS percentiles */
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <sys/mman.h>   /* mmap: the guest arena, see alloc_guest_vm */
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#include <tlhelp32.h>
#include "src/port_menu/port_menu.h"   /* 6b+6c */
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

/* libs/codec/ffmpeg_runtime.c: load FFmpeg now, not at the first movie, so a
 * build that may not be loaded (GPL, nonfree) stops at start-up. */
extern "C" const struct PS3FFmpeg* ps3_ffmpeg_get(void);

/* libs/system/cellGame.c: title id, TITLE, CATEGORY and versions from the
 * game's own PARAM.SFO. Without this call cellGame keeps its BLES00000
 * placeholder, and that is the id the title builds /dev_hdd0/game/<id> from. */
void     cellGame_init_from_paramsfo(const char* sfo_path);

extern const char* ppu_vfs_root;             /* host dir the PS3 mount points map into */
void     ps3_host_quit(const char* why);     /* cellSysutil.c: REQUEST_EXITGAME, then a time limit */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
/* N1 (docs/plan_nativo.md): REFINE_RENDERER=vulkan is read by the backend itself (rsx_d3d12_backend.c,
 * 0180): it records as always and hands each frame to the Vulkan executor. */
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* Re:Fine 5c.1: the port's settings file, %LOCALAPPDATA%\ReFine\refine.ini, lines of
 * `key = value`, '#' for comments. No file means the defaults. Each key becomes the
 * variable the runtime reads, and a variable already set wins. Today:
 *   audio_device = auto     (or a piece of an output's name, or its endpoint id)
 *   stick_deadzone = 0.20   (0-1, radial; a stick under it reads as centred)
 *   stick_hysteresis = 0.25 (0-1; it leaves the centre at deadzone + this)
 *   stick_rearm_ms = 34     (ms under the dead zone before another flick; 0 at fps_target=30)
 *   render_height = 720     (360-2160, the height of the picture drawn; docs/decisions.md, 6a.1)
 *   fps_target = 60         (30 to 1000, display or unlimited; real frames; 60 by default, docs/mejoras.md R2)
 *   realtime = on           (off: the mode before, the menus' logic on 30 Hz ticks; docs/tiempo_real.md)
 *   vsync = on              (off: D3D12_VSYNC=0). The port menu (src/port_menu) writes this file and
 *                           also keeps show_fps, display_mode, master_volume, vibration, menu_scale.
 * The menu that writes this file is milestone 6 (R1, R11, docs/mejoras.md). */
static char g_game_folder[900];   /* refine.ini game_folder (6b+6c p4): the disc folder with PS3_GAME */
static void load_port_settings()
{
#ifdef _WIN32
    const char* base = getenv("LOCALAPPDATA");
    const char* over = getenv("REFINE_INI");   /* 6b+6c: tools/run.ps1 -Profile test gives its own file */
    if (!base && !over) return;
    char path[MAX_PATH];
    if (over) snprintf(path, sizeof path, "%s", over);
    else snprintf(path, sizeof path, "%s\\ReFine\\refine.ini", base);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char key[64], value[256];
        if (line[0] == '#' || sscanf(line, " %63[^= \t] = %255[^\r\n]", key, value) != 2) continue;
        if (!strcmp(key, "game_folder")) snprintf(g_game_folder, sizeof g_game_folder, "%s", value);
        if (!strcmp(key, "audio_device") && !getenv("REFINE_AUDIO_DEVICE"))
            _putenv_s("REFINE_AUDIO_DEVICE", value);
        if (!strcmp(key, "stick_deadzone") && !getenv("REFINE_STICK_DEADZONE"))
            _putenv_s("REFINE_STICK_DEADZONE", value);
        if (!strcmp(key, "stick_hysteresis") && !getenv("REFINE_STICK_HYSTERESIS"))
            _putenv_s("REFINE_STICK_HYSTERESIS", value);
        if (!strcmp(key, "stick_rearm_ms") && strcmp(value, "auto") && !getenv("REFINE_STICK_REARM_MS"))
            _putenv_s("REFINE_STICK_REARM_MS", value);
        if (!strcmp(key, "render_height") && !getenv("REFINE_RENDER_HEIGHT"))
            _putenv_s("REFINE_RENDER_HEIGHT", value);
        if (!strcmp(key, "fps_target") && !getenv("REFINE_FPS_TARGET"))
            _putenv_s("REFINE_FPS_TARGET", value);
        if (!strcmp(key, "menu_fps") && !getenv("REFINE_MENU_FPS"))
            _putenv_s("REFINE_MENU_FPS", value);
        /* 2026-11-14: realtime = off takes the game back to the mode before (the menus' logic on 30 Hz ticks
         * and the renderer drawing between them). On by default; docs/tiempo_real.md. */
        if (!strcmp(key, "realtime") && !getenv("REFINE_REALTIME"))
            _putenv_s("REFINE_REALTIME", strcmp(value, "off") && strcmp(value, "0") ? "1" : "0");
        /* 0188: renderer = vulkan (the default) or d3d12, read by the backend at its init. */
        if (!strcmp(key, "renderer") && !getenv("REFINE_RENDERER") && (!strcmp(value, "vulkan") || !strcmp(value, "d3d12")))
            _putenv_s("REFINE_RENDERER", value);
        /* 6b+6c: the port menu's VSync (Off = D3D12_VSYNC=0, read once when the swap chain is made). */
        if (!strcmp(key, "vsync") && !getenv("D3D12_VSYNC"))
            _putenv_s("D3D12_VSYNC", strcmp(value, "off") ? "1" : "0");
    }
    fclose(f);
#endif
}

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
/* Frames the guest actually finished: its flips (cellGcm_flip_request_count,
 * flip markers the FIFO drain reached). Re:Fine 4d.3: this used to count calls
 * to present_guest_frame, and the loop below can present the same flip by two
 * paths (cellGcm_take_flip_pending and the flip count changing), so the
 * watchdog's frames= read ~2 per flip. The presents made before the guest's
 * first flip, so a fresh window is not left blank through a long boot, carry
 * no guest frame and are not counted either. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return cellGcm_flip_request_count();
}

/* REFINE_PACE=<from>-<to> (diagnostic, seconds since boot): the frame pacing, one line per event, printed at <to>
 * for tools/pace.py. g: the game starts a frame (time, the step it takes in 30 Hz ticks, 30 Hz tick or not);
 * p: a present (time before the call and after it returned). Times in ms from <from>. Off unless it is set. */
struct PaceEv { char kind; double t, v; int tick; };
static PaceEv* g_pace;                      /* NULL: off */
static volatile LONG g_pace_n;
static volatile int  g_pace_on;
static const LONG    PACE_MAX = 32768;
static double qpc_seconds();

/* The screen's refresh, as the desktop compositor sees it (the primary monitor's): the period and the moment of one
 * refresh, in seconds of qpc_seconds. 0 when there is nothing to ask (then the caller keeps its own clock). Windows
 * only: src/platform one day. The function is loaded, not linked. */
static int screen_beat(double* period, double* at)
{
    typedef HRESULT (WINAPI *timing_fn)(HWND, DWM_TIMING_INFO*);
    static timing_fn fn; static int tried;
    if (!tried) {
        tried = 1;
        if (HMODULE d = LoadLibraryW(L"dwmapi.dll")) fn = (timing_fn)(void*)GetProcAddress(d, "DwmGetCompositionTimingInfo");
    }
    DWM_TIMING_INFO ti; memset(&ti, 0, sizeof ti); ti.cbSize = sizeof ti;
    if (!fn || FAILED(fn(NULL, &ti)) || !ti.qpcRefreshPeriod) return 0;
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    *period = (double)ti.qpcRefreshPeriod / (double)f.QuadPart;
    *at = qpc_seconds() - (double)(c.QuadPart - (LONGLONG)ti.qpcVBlank) / (double)f.QuadPart;
    return 1;
}
/* vblank_clock writes these, the other two threads read them: a frame on the screen's beat. */
static volatile double g_beat_frame;                 /* its length in 30 Hz ticks; 0 = the port's own clock */
static volatile double g_beat_s, g_screen_per, g_screen_at;   /* its length in seconds, the screen's refresh and one of them */
static volatile double g_hold_err;          /* frame_clock: how much longer than wanted the presents wait, seconds */

static void pace_ev(char kind, double t, double v, int tick)
{
    if (!g_pace_on) return;
    const LONG i = InterlockedIncrement(&g_pace_n) - 1;
    if (i < PACE_MAX) { g_pace[i].kind = kind; g_pace[i].t = t; g_pace[i].v = v; g_pace[i].tick = tick; }
}

static void present_guest_frame(void)
{
    const double t = qpc_seconds();
    rsx_backend_present();
    pace_ev('p', t, qpc_seconds(), 0);
}

/* Re:Fine, diagnostic only: PS3_WATCHDOG_S=<n> prints, every n seconds, the time
 * since boot, the frames presented and the last direct-call lr of every guest
 * thread (ppu_report_guest_lrs, runtime/ppu/ppu_loader.cpp). It tells a hang
 * from a slow boot without the cost of PS3_HLE_TRACE. Off unless the variable
 * is set; it never touches guest state. */
extern "C" void ppu_report_guest_lrs(void);
void ps3_sampler_start(void);   /* C++ linkage, as runtime/ppu/tests/boot_main.cpp declares it */
static ULONGLONG g_boot_ms;

static DWORD WINAPI watchdog(LPVOID arg)
{
    DWORD period_ms = (DWORD)(uintptr_t)arg;
    for (;;) {
        Sleep(period_ms);
        fprintf(stderr, "[WATCHDOG] t=%.1f s frames=%ld\n",
                (GetTickCount64() - g_boot_ms) / 1000.0, (long)ppu_boot_frames_presented());
        ppu_report_guest_lrs();
    }
}

/* Re:Fine 5d.2, diagnostic only: AUDIO_STALL=1. Every 5 ms this thread notes the CPU
 * cycles of the threads that make the sound and how idle the machine is, touching no
 * thread. When the mixer says the title's writer came back from being late, it prints
 * those samples and the syscalls of the mixer threads (ppu_sc_ring_dump). Off unless
 * the variable is set. */
extern "C" void* g_audio_stall_event;
extern "C" void ppu_sc_ring_dump(long long now_qpc, double back_ms);

#define STALL_THR 12
#define STALL_SAMPLES 40
struct stall_thread { HANDLE h; DWORD id; char name[40]; };
static stall_thread g_st[STALL_THR];
static int g_st_n;
static struct { long long q; ULONG64 c[STALL_THR]; ULONGLONG idle, total; } g_ss[STALL_SAMPLES];

static void stall_threads(void)
{
    static const char* const want[] = { "ppu _", "spu ", "ppu main", "vdec", "dmux", "adec", "audio mix", "sdl pad" };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok && g_st_n < STALL_THR; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        int known = 0;
        for (int i = 0; i < g_st_n; i++) known |= g_st[i].id == te.th32ThreadID;
        if (known) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        PWSTR d = NULL;
        char name[40] = "";
        if (SUCCEEDED(GetThreadDescription(h, &d)) && d) { snprintf(name, sizeof name, "%ls", d); LocalFree(d); }
        int hit = 0;
        for (const char* w : want) hit |= strncmp(name, w, strlen(w)) == 0;
        if (!hit) { CloseHandle(h); continue; }
        g_st[g_st_n].h = h; g_st[g_st_n].id = te.th32ThreadID;
        memcpy(g_st[g_st_n].name, name, sizeof name);
        fprintf(stderr, "[AUDIO_STALL] column %d: %s prio=%d\n", g_st_n, name, GetThreadPriority(h));
        g_st_n++;
    }
    CloseHandle(snap);
}

static ULONGLONG ft64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static DWORD WINAPI audio_stall(LPVOID)
{
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    unsigned n = 0;
    for (;;) {
        const DWORD r = WaitForSingleObject((HANDLE)g_audio_stall_event, 5);
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        if (!(n % 400)) stall_threads();
        auto& s = g_ss[n % STALL_SAMPLES];
        s.q = q.QuadPart;
        for (int i = 0; i < g_st_n; i++) QueryThreadCycleTime(g_st[i].h, &s.c[i]);
        FILETIME id, k, u;
        GetSystemTimes(&id, &k, &u);
        s.idle = ft64(id); s.total = ft64(k) + ft64(u);   /* kernel time includes idle */
        n++;
        if (r != WAIT_OBJECT_0 || n < STALL_SAMPLES) continue;
        fprintf(stderr, "[AUDIO_STALL] samples, ms before now, %% machine busy, Mcycles per column:\n");
        for (unsigned i = n - STALL_SAMPLES + 1; i < n; i++) {
            auto& a = g_ss[(i - 1) % STALL_SAMPLES];
            auto& b = g_ss[i % STALL_SAMPLES];
            char line[256]; int o = 0;
            for (int t = 0; t < g_st_n; t++)
                o += snprintf(line + o, sizeof line - o, " %.1f", (b.c[t] - a.c[t]) / 1e6);
            const double tot = (double)(b.total - a.total);
            fprintf(stderr, "[AUDIO_STALL] %7.1f busy %3.0f%%:%s\n", (q.QuadPart - b.q) * 1000.0 / f.QuadPart,
                    tot > 0 ? 100.0 * (1.0 - (double)(b.idle - a.idle) / tot) : 0.0, line);
        }
        ppu_sc_ring_dump(q.QuadPart, 1000.0 / f.QuadPart);
        fflush(stderr);
    }
}

/* Re:Fine, diagnostic only: PS3_FRAMESTATS=<from>-<to> (seconds since boot).
 * Over that window it measures what docs/perf.md tracks for frame pacing and
 * prints one [FRAMESTATS] summary at <to>:
 *   - flips: changes of cellGcm_flip_request_count() (flip markers the FIFO
 *     drain reached), polled every 1 ms, with the interval between them
 *     (mean, p50, p99, max). The drain runs on frame_clock, so an interval is
 *     only as fine as that loop's cadence (~4-5 ms), not the 1 ms poll;
 *   - vblank ticks issued by this runner (cellGcmTickVBlank calls);
 *   - guest callbacks delivered, per OPD (the game's vblank handler is one);
 *   - CPU time of the main thread (the guest's main PPU thread, ppu_run).
 * Off unless the variable is set; it never touches guest state. */
static volatile LONG g_vblank_ticks = 0;
static HANDLE        g_main_thread  = NULL;

struct cb_count { volatile LONG opd; volatile LONG n; };
static cb_count g_cb_counts[8];

static void count_guest_callback(uint32_t opd)
{
    for (cb_count& c : g_cb_counts) {
        LONG cur = c.opd;
        if (cur == 0) cur = InterlockedCompareExchange(&c.opd, (LONG)opd, 0);
        if (cur == 0 || (uint32_t)cur == opd) { InterlockedIncrement(&c.n); return; }
    }
}

static uint64_t thread_cpu_100ns(HANDLE t)
{
    FILETIME c, e, k, u;
    if (!t || !GetThreadTimes(t, &c, &e, &k, &u)) return 0;
    return (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
           (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

static uint32_t guest_read32(uint32_t ea)
{
    const uint8_t* q = vm_base + ea;
    return ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static DWORD WINAPI pace_log(LPVOID arg)
{
    double from_s = 0, to_s = 0;
    if (sscanf((const char*)arg, "%lf-%lf", &from_s, &to_s) != 2 || to_s <= from_s) return 0;
    g_pace = (PaceEv*)calloc(PACE_MAX, sizeof *g_pace);
    if (!g_pace) return 0;
    while (GetTickCount64() < g_boot_ms + (ULONGLONG)(from_s * 1000)) Sleep(50);
    const double t0 = qpc_seconds();
    g_pace_on = 1;
    while (GetTickCount64() < g_boot_ms + (ULONGLONG)(to_s * 1000)) {
        double per, at;                     /* tools/pace.py puts the pictures on the screen's own beat */
        if (screen_beat(&per, &at)) pace_ev('v', at, per, 0);
        Sleep(250);
    }
    g_pace_on = 0;
    Sleep(50);                              /* a writer still inside pace_ev */
    const LONG n = g_pace_n < PACE_MAX ? g_pace_n : PACE_MAX;
    for (LONG i = 0; i < n; i++) {
        const PaceEv& e = g_pace[i];
        if (e.kind == 'g') fprintf(stderr, "[pace] g %.4f %.5f %d\n", (e.t - t0) * 1e3, e.v, e.tick);
        else if (e.kind == 'v') fprintf(stderr, "[pace] v %.4f %.6f%c", (e.t - t0) * 1e3, e.v * 1e3, 10);
        else fprintf(stderr, "[pace] p %.4f %.4f\n", (e.t - t0) * 1e3, (e.v - t0) * 1e3);
    }
    fprintf(stderr, "[pace] end %ld\n", (long)n);
    return 0;
}

static DWORD WINAPI framestats(LPVOID arg)
{
    double from_s = 0, to_s = 0;
    if (sscanf((const char*)arg, "%lf-%lf", &from_s, &to_s) != 2 || to_s <= from_s) {
        fprintf(stderr, "[FRAMESTATS] bad PS3_FRAMESTATS='%s' (want <from>-<to>)\n", (const char*)arg);
        return 0;
    }
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    LARGE_INTEGER qpf, now;
    QueryPerformanceFrequency(&qpf);
    const ULONGLONG from_ms = g_boot_ms + (ULONGLONG)(from_s * 1000);
    const ULONGLONG to_ms   = g_boot_ms + (ULONGLONG)(to_s * 1000);
    while (GetTickCount64() < from_ms) Sleep(50);

    static double intervals[100000];
    size_t n_int = 0;
    unsigned flips = 0;
    unsigned last = cellGcm_flip_request_count();
    LONG cb0[8];
    for (int i = 0; i < 8; i++) cb0[i] = g_cb_counts[i].n;
    const LONG ticks0 = g_vblank_ticks;
    const uint64_t cpu0 = thread_cpu_100ns(g_main_thread);
    QueryPerformanceCounter(&now);
    const long long t0 = now.QuadPart;
    long long last_flip_qpc = 0;

    while (GetTickCount64() < to_ms) {
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -10000;   /* 1 ms */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
        unsigned fc = cellGcm_flip_request_count();
        if (fc == last) continue;
        QueryPerformanceCounter(&now);
        for (unsigned d = fc - last; d; d--) {
            if (last_flip_qpc && n_int < sizeof intervals / sizeof intervals[0]) {
                const double iv = (now.QuadPart - last_flip_qpc) * 1000.0 / qpf.QuadPart;
                intervals[n_int++] = iv;
                if (iv > 45.0)                        /* 6d.13 Q7: when a long frame falls (boot seconds) */
                    fprintf(stderr, "[FRAMESTATS] long frame %.2f ms at %.3f s (flip %u)\n", iv,
                            (GetTickCount64() - g_boot_ms) / 1000.0, flips);
            }
            last_flip_qpc = now.QuadPart;
            flips++;
        }
        last = fc;
    }
    QueryPerformanceCounter(&now);
    const double secs = (now.QuadPart - t0) / (double)qpf.QuadPart;
    const uint64_t cpu1 = thread_cpu_100ns(g_main_thread);
    const LONG ticks1 = g_vblank_ticks;

    double mean = 0, p50 = 0, p99 = 0, mx = 0;
    if (n_int) {
        std::sort(intervals, intervals + n_int);
        for (size_t i = 0; i < n_int; i++) mean += intervals[i];
        mean /= n_int;
        p50 = intervals[n_int / 2];
        p99 = intervals[(std::min)(n_int - 1, (size_t)(n_int * 0.99))];
        mx  = intervals[n_int - 1];
    }
    fprintf(stderr, "[FRAMESTATS] window %.1f-%.1f s (%.2f s measured)\n", from_s, to_s, secs);
    fprintf(stderr, "[FRAMESTATS] flips=%u (%.2f/s) interval ms: mean=%.2f p50=%.2f p99=%.2f max=%.2f (n=%zu)\n",
            flips, flips / secs, mean, p50, p99, mx, n_int);
    fprintf(stderr, "[FRAMESTATS] vblank_ticks=%ld (%.2f/s)\n",
            (long)(ticks1 - ticks0), (ticks1 - ticks0) / secs);
    for (int i = 0; i < 8; i++) {
        const uint32_t opd = (uint32_t)g_cb_counts[i].opd;
        if (!opd) break;
        const LONG n = g_cb_counts[i].n - cb0[i];
        fprintf(stderr, "[FRAMESTATS] guest callback opd=0x%08X code=0x%08X calls=%ld (%.2f/s)\n",
                opd, guest_read32(opd), (long)n, n / secs);
    }
    fprintf(stderr, "[FRAMESTATS] main thread CPU %.1f %% of one core\n",
            (cpu1 - cpu0) / 1e7 / secs * 100.0);
    if (timer) CloseHandle(timer);
    return 0;
}

/* Re:Fine 6.0.1, diagnostic only: FRAME_PROF=<from>-<to> (seconds since boot). One sample
 * per guest flip, and at <to> a [FRAMEPROF] table of mean, p50, p99 and max in ms:
 *   guest      from cellGcmResetFlipStatus, where the guest leaves its flip wait, to
 *              the next cellGcmGetVBlankCount of that thread, where it enters it
 *   guest_cpu  the CPU that thread really used in that span (QueryThreadCycleTime)
 *   vbl_wait   the rest of the frame: the guest waiting for vblank and flip
 *   drain      FIFO walk on frame_clock, net of any render_frame inside it
 *   gpu_wait, record, submit, present, next   the phases of render_frame
 *   gpu        GPU time of the frame's command list (D3D12 timestamps)
 *   busy       drain + record + submit, what frame_clock works per flip
 * Off unless the variable is set; it never touches guest state. */
extern "C" {
extern void (*g_frame_prof_guest)(int);
extern int g_frame_prof_on;
extern struct { double wait, cmd, exec, present, next, gpu; unsigned long long draws, renders; } g_frame_prof;
extern unsigned long long g_fp_parts[6];
}
enum { FP_GUEST, FP_GCPU, FP_VWAIT, FP_DRAIN, FP_GWAIT, FP_REC, FP_SUBMIT, FP_PRESENT, FP_NEXT, FP_GPU,
       FP_BUSY, FP_DRAWS, FP_N };
static const char* const fp_names[FP_N] = { "guest", "guest_cpu", "vbl_wait", "drain", "gpu_wait", "record",
    "submit", "present", "next", "gpu", "busy", "draws" };
#define FP_MAX 8192
static double qpc_seconds();
static float* g_fp_s[FP_N];
static volatile LONG g_fp_n[FP_N];
static volatile int g_fp_active;
static double g_fp_drain_s;   /* frame_clock only */
static double g_fp_ms_per_cycle;

static void fp_push(int col, double v)
{
    if (!g_fp_active) return;
    const LONG i = g_fp_n[col];
    if (i < FP_MAX) { g_fp_s[col][i] = (float)v; g_fp_n[col] = i + 1; }
}

static void fp_guest(int what)
{
    static DWORD thread;
    static double t_left, t_entered;
    static ULONG64 c_left;
    static int running;
    if (what == 0) {
        thread = GetCurrentThreadId();
        const double now = qpc_seconds();
        if (t_entered > 0) fp_push(FP_VWAIT, (now - t_entered) * 1e3);
        t_left = now; running = 1;
        QueryThreadCycleTime(GetCurrentThread(), &c_left);
    } else if (running && GetCurrentThreadId() == thread) {
        ULONG64 c; QueryThreadCycleTime(GetCurrentThread(), &c);
        t_entered = qpc_seconds();
        running = 0;
        fp_push(FP_GUEST, (t_entered - t_left) * 1e3);
        fp_push(FP_GCPU, (double)(c - c_left) * g_fp_ms_per_cycle);
    }
}

/* frame_clock: a flip went through. Everything since the last one is one sample. */
static void fp_flip(void)
{
    static double last[6]; static unsigned long long last_draws;
    const double now[6] = { g_frame_prof.wait, g_frame_prof.cmd, g_frame_prof.exec, g_frame_prof.present,
                            g_frame_prof.next, g_frame_prof.gpu };
    double d[6];
    for (int i = 0; i < 6; i++) { d[i] = (now[i] - last[i]) * 1e3; last[i] = now[i]; }
    fp_push(FP_DRAIN, g_fp_drain_s * 1e3);
    fp_push(FP_GWAIT, d[0]); fp_push(FP_REC, d[1]); fp_push(FP_SUBMIT, d[2]);
    fp_push(FP_PRESENT, d[3]); fp_push(FP_NEXT, d[4]); fp_push(FP_GPU, d[5]);
    fp_push(FP_BUSY, g_fp_drain_s * 1e3 + d[1] + d[2]);
    fp_push(FP_DRAWS, (double)(g_frame_prof.draws - last_draws));
    last_draws = g_frame_prof.draws;
    g_fp_drain_s = 0;
}

static DWORD WINAPI frame_prof(LPVOID arg)
{
    double from_s = 0, to_s = 0;
    if (sscanf((const char*)arg, "%lf-%lf", &from_s, &to_s) != 2 || to_s <= from_s) return 0;
    const ULONGLONG from_ms = g_boot_ms + (ULONGLONG)(from_s * 1000);
    const ULONGLONG to_ms   = g_boot_ms + (ULONGLONG)(to_s * 1000);
    /* The cycles of QueryThreadCycleTime tick at the TSC: calibrate them against QPC. */
    const double q0 = qpc_seconds(); const ULONG64 c0 = __rdtsc();
    while (GetTickCount64() < from_ms) Sleep(50);
    g_fp_ms_per_cycle = (qpc_seconds() - q0) * 1e3 / (double)(__rdtsc() - c0);
    unsigned long long parts0[6];
    memcpy(parts0, g_fp_parts, sizeof parts0);
    g_fp_active = 1;
    while (GetTickCount64() < to_ms) Sleep(50);
    g_fp_active = 0;
    const LONG flips = g_fp_n[FP_DRAIN] > 0 ? g_fp_n[FP_DRAIN] : 1;
    fprintf(stderr, "[FRAMEPROF] window %.0f-%.0f s, TSC %.3f GHz\n", from_s, to_s, 1e-6 / g_fp_ms_per_cycle);
    fprintf(stderr, "[FRAMEPROF] drain parts, ms per flip: draws %.2f (arrays %.2f; vp_record_cb %.2f, of it constants %.2f; texs %.2f; vs hash %.2f)\n",
            (double)(g_fp_parts[0] - parts0[0]) * g_fp_ms_per_cycle / flips,
            (double)(g_fp_parts[4] - parts0[4]) * g_fp_ms_per_cycle / flips,
            (double)(g_fp_parts[1] - parts0[1]) * g_fp_ms_per_cycle / flips,
            (double)(g_fp_parts[2] - parts0[2]) * g_fp_ms_per_cycle / flips,
            (double)(g_fp_parts[3] - parts0[3]) * g_fp_ms_per_cycle / flips,
            (double)(g_fp_parts[5] - parts0[5]) * g_fp_ms_per_cycle / flips);
    fprintf(stderr, "[FRAMEPROF] %-10s %6s %8s %8s %8s %8s\n", "ms", "n", "mean", "p50", "p99", "max");
    for (int c = 0; c < FP_N; c++) {
        const LONG n = g_fp_n[c];
        if (n <= 0) continue;
        std::sort(g_fp_s[c], g_fp_s[c] + n);
        double sum = 0;
        for (LONG i = 0; i < n; i++) sum += g_fp_s[c][i];
        fprintf(stderr, "[FRAMEPROF] %-10s %6ld %8.2f %8.2f %8.2f %8.2f\n", fp_names[c], (long)n, sum / n,
                g_fp_s[c][n / 2], g_fp_s[c][(std::min)((LONG)(n - 1), (LONG)(n * 0.99))], g_fp_s[c][n - 1]);
    }
    return 0;
}

/* Re:Fine 5c.2, diagnostic only. They read guest memory and change nothing.
 * MEM_DUMP=<prefix>:<sec>;<sec>;... writes the committed guest memory below
 * 0x30000000 and from 0xD0000000 up to <prefix>_<n>.bin at those seconds since boot:
 * a QPC in seconds as a double, then (address, size, bytes) records.
 * MEM_WATCH=<file>|<from>-<to>|<hex addr>[;<hex addr>...] samples 8 bytes at each address
 * (*<hex addr>-<hex off>: at the pointer stored there, minus the offset)
 * every 500 us in that window and writes a line per change: QPC seconds, the index
 * of the address, the bytes as hex, the flip count and the vblank count. */
static double qpc_seconds()
{
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    return (double)q.QuadPart / (double)f.QuadPart;
}

static void sleep_until_boot_second(double boot, double t)
{
    for (double left; (left = t - (qpc_seconds() - boot)) > 0; )
        Sleep((DWORD)(left > 0.5 ? 400 : left * 1000 + 1));
}

static DWORD WINAPI mem_dump(LPVOID arg)
{
    const double boot = qpc_seconds();
    char prefix[400] = "";
    const char* e = (const char*)arg;
    const char* colon = strrchr(e, ':');
    if (!colon) return 1;
    snprintf(prefix, sizeof prefix, "%.*s", (int)(colon - e), e);
    e = colon + 1;
    for (int n = 0; *e; n++) {
        double t = strtod(e, (char**)&e);
        while (*e == ';') e++;
        sleep_until_boot_second(boot, t);
        char path[440];
        snprintf(path, sizeof path, "%s_%d.bin", prefix, n);
        FILE* f = fopen(path, "wb");
        if (!f) continue;
        const double q = qpc_seconds();
        fwrite(&q, 8, 1, f);
        uint64_t at = 0;
        MEMORY_BASIC_INFORMATION mi;
        while (at < VM_SIZE && VirtualQuery(vm_base + at, &mi, sizeof mi)) {
            uint64_t size = mi.RegionSize;
            if (at + size > VM_SIZE) size = VM_SIZE - at;
            if (mi.State == MEM_COMMIT && (at < 0x30000000ull || at >= 0xD0000000ull)) {
                const uint32_t h[2] = { (uint32_t)at, (uint32_t)size };
                fwrite(h, 4, 2, f);
                fwrite(vm_base + at, 1, size, f);
            }
            at += size;
        }
        fclose(f);
        fprintf(stderr, "[MEM_DUMP] %s at %.3f s, took %.3f s\n", path, q - boot, qpc_seconds() - q);
    }
    return 0;
}

static DWORD WINAPI mem_watch(LPVOID arg)
{
    const double boot = qpc_seconds();
    char path[400]; double from = 0, to = 0; int used = 0;
    if (sscanf((const char*)arg, "%399[^|]|%lf-%lf|%n", path, &from, &to, &used) != 3) return 1;
    uint32_t addr[32], back[32] = {0}; uint64_t last[32] = {0}; int ind[32] = {0}, n = 0;
    for (const char* e = (const char*)arg + used; *e && n < 32; n++) {
        if (*e == '*') { ind[n] = 1; e++; }
        addr[n] = (uint32_t)strtoul(e, (char**)&e, 16);
        if (*e == '-') back[n] = (uint32_t)strtoul(e + 1, (char**)&e, 16);
        while (*e == ';') e++;
    }
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    sleep_until_boot_second(boot, from);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    FILE* f = fopen(path, "w");
    if (!f) return 1;
    for (;;) {
        const double t = qpc_seconds();
        if (t - boot > to) break;
        for (int i = 0; i < n; i++) {
            uint64_t v;
            uint32_t at = addr[i];
            if (ind[i]) { memcpy(&at, vm_base + addr[i], 4); at = _byteswap_ulong(at) - back[i]; }
            if (at < 0x10000) continue;
            memcpy(&v, vm_base + at, 8);
            if (v != last[i]) {
                last[i] = v;
                fprintf(f, "%.6f %d %016llx %u %ld\n", t, i, (unsigned long long)_byteswap_uint64(v),
                        cellGcm_flip_request_count(), (long)g_vblank_ticks);
            }
        }
        LARGE_INTEGER due; due.QuadPart = -5000;   /* 500 us */
        SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(timer, 10);
    }
    fclose(f);
    return 0;
}

/* Re:Fine 5c.2, diagnostic only. MEM_BP=<from>-<to>|<hex addr> puts a hardware
 * breakpoint on 4 guest bytes in every thread for that window and reports which
 * translated functions touched them, with how many accesses. */
#define BP_SITES 64
static struct { volatile LONG64 rip; volatile LONG n; } g_bp_sites[BP_SITES];

static uint64_t g_bp_lo, g_bp_hi;

static LONG CALLBACK mem_bp_handler(PEXCEPTION_POINTERS e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    /* With memory access out of line the fault lands in a helper at the start of the
     * translated code: the site is the first return address past it on the stack. */
    LONG64 rip = (LONG64)e->ContextRecord->Rip;
    const uint64_t* sp = (const uint64_t*)e->ContextRecord->Rsp;
    for (int k = 0; k < 48; k++) {
        if (sp[k] > g_bp_lo + 0x20000 && sp[k] < g_bp_hi) { rip = (LONG64)sp[k]; break; }
    }
    for (int i = 0; i < BP_SITES; i++) {
        const LONG64 was = InterlockedCompareExchange64(&g_bp_sites[i].rip, rip, 0);
        if (was == 0 || was == rip) { InterlockedIncrement(&g_bp_sites[i].n); break; }
    }
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void mem_bp_arm(DWORD64 addr, DWORD64 dr7)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (snap == INVALID_HANDLE_VALUE) return;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT c; memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) { c.Dr0 = addr; c.Dr7 = dr7; c.Dr6 = 0; SetThreadContext(t, &c); }
        ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(snap);
}

static DWORD WINAPI mem_bp(LPVOID arg)
{
    const double boot = qpc_seconds();
    double from = 0, to = 0; unsigned addr = 0;
    if (sscanf((const char*)arg, "%lf-%lf|%x", &from, &to, &addr) != 3) return 1;
    sleep_until_boot_second(boot, from);
    g_bp_lo = ~0ull;
    for (uint64_t k = 0; k < function_table_count; k++) {
        const uint64_t f = (uint64_t)(uintptr_t)function_table[k].func;
        if (f < g_bp_lo) g_bp_lo = f;
        if (f > g_bp_hi) g_bp_hi = f;
    }
    g_bp_hi += 0x10000;
    AddVectoredExceptionHandler(1, mem_bp_handler);
    /* L0, read/write, 4 bytes. Armed again every second for threads born meanwhile. */
    const DWORD64 dr7 = 1 | (3ull << 16) | (3ull << 18);
    for (double t = from; t < to; t += 1.0) {
        mem_bp_arm((DWORD64)(vm_base + addr), dr7);
        sleep_until_boot_second(boot, t + 1.0);
    }
    mem_bp_arm(0, 0);
    for (int i = 0; i < BP_SITES && g_bp_sites[i].n; i++) {
        const uintptr_t rip = (uintptr_t)g_bp_sites[i].rip;
        const char* name = "?"; uintptr_t best = 0;
        for (uint64_t k = 0; k < function_table_count; k++) {
            const uintptr_t f = (uintptr_t)function_table[k].func;
            if (f <= rip && f > best) { best = f; name = function_table[k].name; }
        }
        fprintf(stderr, "[MEM_BP] 0x%08X: %ld accesses at %s+0x%llx rva=0x%llx\n", addr, (long)g_bp_sites[i].n,
                name, (unsigned long long)(rip - best), (unsigned long long)(rip - (uintptr_t)GetModuleHandleA(NULL)));
    }
    return 0;
}

/* Re:Fine 5c.3, diagnostic only. PAD_BOT=<ms>[;<step>;<period>] plays a song by the game's own
 * live note list and presses so that the note has <ms> left when the press is seen
 * (positive: early), to measure the judgment against time. With <step> and <period> it
 * adds <step> ms per note and starts over every <period> notes, sweeping. Everything comes from guest memory, no file of the game
 * is read: 0x12CE588 is the judgment object of this EBOOT (STATUS 5c.3), +0x14 the list
 * head, and a note has +4 next, +0xC type, +0x10 seconds left and +0x118 its result. */
extern "C" volatile char g_pad_bot_keys[12];
#define BOT_JUDGE      0x012CE588u
#define BOT_FRAME_S    0.03337

static float guest_read_f32(uint32_t ea)
{
    uint32_t v = guest_read32(ea);
    float f; memcpy(&f, &v, 4);
    return f;
}

/* Re:Fine 6d, diagnostic only: the per-screen census of tools/screen_census.ps1. It reads
 * the window and holds keys the way PAD_KEYS does; it changes nothing else.
 * SHOTS=<dir>|<at>:<keys>:<n>:<ms>:<name>;... At <at> seconds since boot it holds <keys>
 * (the PAD_KEYS letters, '.' for none) for 120 ms and, from that same instant, writes <n>
 * images of the window, one every <ms>, to <dir>\<name>_<k>.bmp (480x270, 24 bits). The
 * press and the images share one clock, so a screen that answers the press can be compared
 * at the same real time across two fps_target. cellPad reads these keys only while
 * PAD_KEYS is set, to any value. +<s> instead of <at> (6d.14): <s> seconds after the press of
 * the step before, for screens where the song clock stops (the pause menu). */
#ifdef _WIN32
static BOOL CALLBACK shots_find(HWND h, LPARAM out)
{
    DWORD pid = 0; char title[16] = "";
    GetWindowThreadProcessId(h, &pid);
    GetWindowTextA(h, title, sizeof title);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !strncmp(title, "Re:Fine", 7)) {
        *(HWND*)out = h; return FALSE;
    }
    return TRUE;
}

static int shots_take(const char* path)
{
    enum { W = 480, H = 270 };
    static HWND win; static HDC dc_big, dc_small; static HBITMAP bmp_big, bmp_small; static void* bits;
    static int big_w, big_h;
    if (!win) EnumWindows(shots_find, (LPARAM)&win);
    if (!win) return 0;
    RECT r; GetClientRect(win, &r);
    if (r.right <= 0 || r.bottom <= 0) return 0;
    HDC screen = GetDC(NULL);
    if (!dc_small) {
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
        dc_small = CreateCompatibleDC(screen);
        bmp_small = CreateDIBSection(dc_small, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        SelectObject(dc_small, bmp_small);
        SetStretchBltMode(dc_small, HALFTONE);
    }
    if (big_w != r.right || big_h != r.bottom) {
        if (!dc_big) dc_big = CreateCompatibleDC(screen);
        if (bmp_big) DeleteObject(bmp_big);
        bmp_big = CreateCompatibleBitmap(screen, r.right, r.bottom);
        SelectObject(dc_big, bmp_big);
        big_w = r.right; big_h = r.bottom;
    }
    ReleaseDC(NULL, screen);
    const BOOL ok = PrintWindow(win, dc_big, 3);   /* PW_CLIENTONLY | PW_RENDERFULLCONTENT */
    StretchBlt(dc_small, 0, 0, W, H, dc_big, 0, 0, big_w, big_h, SRCCOPY);
    GdiFlush();
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const uint32_t img = W * H * 3;
    uint8_t hdr[54] = { 'B', 'M' };
    const uint32_t v[] = { 54 + img, 0, 54, 40, (uint32_t)W, (uint32_t)H };
    memcpy(hdr + 2, &v[0], 4); memcpy(hdr + 10, &v[2], 4); memcpy(hdr + 14, &v[3], 4);
    memcpy(hdr + 18, &v[4], 4); memcpy(hdr + 22, &v[5], 4);
    hdr[26] = 1; hdr[28] = 24; memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f); fwrite(bits, 1, img, f); fclose(f);
    return ok ? 1 : 0;
}

extern "C" void rsx_d3d12_dump_arm(void);    /* patches/0156 */

static DWORD WINAPI shots(LPVOID arg)
{
    const double boot = qpc_seconds();
    char dir[300] = "";
    const char* e = (const char*)arg;
    const char* bar = strchr(e, '|');
    if (!bar || bar - e >= (ptrdiff_t)sizeof dir) return 1;
    memcpy(dir, e, bar - e);
    for (e = bar + 1; *e; ) {
        double at = 0, ms = 0; int n = 0, used = 0; char keys[12] = "", name[32] = "";
        unsigned watch = 0;
        static double last_t0;                   /* the press of the step before (6d.14) */
        const int rel = *e == '+';
        if (rel) e++;
        /* f<hex>@<value> instead of <at>: wait until the guest f32 at <hex> reaches <value>
         * (the song clock 0x12CBE48, seconds of song: a PV compared at the same song time). */
        const int got = *e == 'f'
            ? sscanf(e, "f%x@%lf:%11[^:]:%d:%lf:%31[^;]%n", &watch, &at, keys, &n, &ms, name, &used) - 1
            : sscanf(e, "%lf:%11[^:]:%d:%lf:%31[^;]%n", &at, keys, &n, &ms, name, &used);
        if (got != 5) { fprintf(stderr, "[SHOTS] bad step at '%s'\n", e); return 1; }
        e += used; while (*e == ';') e++;
        if (watch) {
            while (guest_read_f32(watch) < (float)at) Sleep(1);
            fprintf(stderr, "[SHOTS] %s: f32 at 0x%X reached %.3f at %.3f s\n", name, watch, at, qpc_seconds() - boot);
        } else {
            sleep_until_boot_second(boot, rel ? last_t0 - boot + at : at);
        }
        const double t0 = qpc_seconds();
        last_t0 = t0;
        double release = 0;
        if (!strcmp(keys, "*")) {                /* 6d.14: arm CELLMARK_DUMP (with CELLMARK_DUMP_ARMED) */
            rsx_d3d12_dump_arm();
            fprintf(stderr, "[SHOTS] %s: dump armed at %.3f s\n", name, t0 - boot);
        } else if (strcmp(keys, ".")) { memcpy((void*)g_pad_bot_keys, keys, sizeof keys); release = t0 + 0.120; }
        for (int k = 0; k < n || release; ) {
            const double next = k < n ? t0 + k * ms / 1000.0 : 1e300;
            const double until = release && release < next ? release : next;
            sleep_until_boot_second(boot, until - boot);
            if (release && qpc_seconds() >= release) { g_pad_bot_keys[0] = 0; release = 0; continue; }
            char path[400];
            snprintf(path, sizeof path, "%s\\%s_%02d.bmp", dir, name, k);
            const double q = qpc_seconds();
            if (getenv("SHOTS_TRACE")) fprintf(stderr, "[SHOTS] %s %d taking\n", name, k);
            const int ok = shots_take(path);
            fprintf(stderr, "[SHOTS] %s %d +%.3f s took %.1f ms ok=%d\n", name, k, q - t0, (qpc_seconds() - q) * 1e3, ok);
            k++;
        }
    }
    return 0;
}
#endif

static const char* bot_keys_for(uint32_t type, int* flick)
{
    static const char* const single[4] = { "W", "D", "S", "A" };
    static const char* const both[4]   = { "WI", "DL", "SK", "AJ" };
    if (type < 4) return single[type];
    if (type < 8) return both[type - 4];
    if (type < 12) return single[type - 8];
    *flick ^= 1;
    return *flick ? "Q" : "E";
}

/* Re:Fine 6b+6c p3, diagnostic only: real keys. REAL_BOT=<ms> is PAD_BOT with SendInput (the
 * keyboard as the author uses it: GetAsyncKeyState, the window's own messages), and
 * REAL_KEYS=<s>:<keys>:<ms>;... holds <keys> for <ms> at <s> seconds since boot. Letters are
 * PAD_KEYS's (u d l r arrows, n Enter, b Backspace, s Space, e Esc) plus t Tab and f F1. Both
 * bring the game's window to the front first: input from SendInput goes to the foreground
 * window, so these move the user's real keyboard state. Never set unless asked. */
static WORD real_vk(char c)
{
    switch (c) {
    case 'u': return VK_UP; case 'd': return VK_DOWN; case 'l': return VK_LEFT; case 'r': return VK_RIGHT;
    case 'n': return VK_RETURN; case 'b': return VK_BACK; case 's': return VK_SPACE; case 'e': return VK_ESCAPE;
    case 't': return VK_TAB; case 'f': return VK_F1;
    default: return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? (WORD)c : 0;
    }
}

static BOOL CALLBACK real_find(HWND h, LPARAM lp)
{
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { *(HWND*)lp = h; return FALSE; }
    return TRUE;
}

static void real_front()
{
    HWND h = NULL; EnumWindows(real_find, (LPARAM)&h);
    if (!h || GetForegroundWindow() == h) return;
    {   /* who had it: a REFUSED or a stolen front names the other process */
        HWND f = GetForegroundWindow(); DWORD pid = 0; GetWindowThreadProcessId(f, &pid);
        wchar_t exe[MAX_PATH] = L"?"; DWORD n = MAX_PATH; char cls[64] = "";
        if (HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) { QueryFullProcessImageNameW(ph, 0, exe, &n); CloseHandle(ph); }
        GetClassNameA(f, cls, sizeof cls);
        const wchar_t* b = wcsrchr(exe, L'\\');
        fprintf(stderr, "[real] front was pid %lu %ls class '%s'\n", (unsigned long)pid, b ? b + 1 : exe, cls);
    }
    /* Joined to the input of the thread in front, SetForegroundWindow is not refused. */
    const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), NULL), me = GetCurrentThreadId();
    AttachThreadInput(me, fg, TRUE);
    SetForegroundWindow(h); BringWindowToTop(h); SetFocus(h);
    AttachThreadInput(me, fg, FALSE);
    fprintf(stderr, "[real] window to the front: %s\n", GetForegroundWindow() == h ? "ok" : "REFUSED");
}

static void real_send(const char* keys, bool up)
{
    /* The mouse: ^ v wheel up/down (on the press only), < > * left, right, middle button, over the
     * middle of the game's window. */
    for (const char* k = keys; *k; k++) {
        if (!strchr("^v<>*", *k)) continue;
        HWND h = NULL; EnumWindows(real_find, (LPARAM)&h);
        RECT rc; POINT c = {0, 0};
        if (h && GetClientRect(h, &rc)) { c.x = rc.right / 2; c.y = rc.bottom / 2; ClientToScreen(h, &c); SetCursorPos(c.x, c.y); }
        INPUT m = {}; m.type = INPUT_MOUSE;
        switch (*k) {
        case '^': case 'v': if (up) continue; m.mi.dwFlags = MOUSEEVENTF_WHEEL; m.mi.mouseData = (DWORD)(*k == '^' ? WHEEL_DELTA : -WHEEL_DELTA); break;
        case '<': m.mi.dwFlags = up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN; break;
        case '>': m.mi.dwFlags = up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN; break;
        default:  m.mi.dwFlags = up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN; break;
        }
        SendInput(1, &m, sizeof(INPUT));
    }
    INPUT in[12] = {}; UINT n = 0;
    for (const char* k = keys; *k && n < 12; k++) {
        const WORD vk = real_vk(*k);
        if (!vk) continue;
        in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk;
        in[n].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in[n].ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) |
            (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ? KEYEVENTF_EXTENDEDKEY : 0);
        n++;
    }
    if (n) SendInput(n, in, sizeof(INPUT));
}

static double g_real_t0;
static DWORD WINAPI real_keys(LPVOID arg)
{
    struct Ev { double at, ms; char keys[12]; };
    Ev ev[256]; int n = 0;
    for (const char* p = (const char*)arg; p && *p && n < 256;) {
        Ev e = {}; int used = 0;
        if (sscanf(p, "%lf:%11[^:]:%lf%n", &e.at, e.keys, &e.ms, &used) != 3) break;
        ev[n++] = e; p += used; if (*p == ',' || *p == ';') p++;
    }
    for (int i = 0; i < n; i++) {
        const double wait = ev[i].at - (qpc_seconds() - g_real_t0);
        if (wait > 0) Sleep((DWORD)(wait * 1000));
        real_front();
        real_send(ev[i].keys, false);
        fprintf(stderr, "[real] t=%.3f down '%s' for %.0f ms\n", qpc_seconds() - g_real_t0, ev[i].keys, ev[i].ms);
        Sleep((DWORD)ev[i].ms);
        real_send(ev[i].keys, true);
    }
    return 0;
}
static bool g_bot_real;

static DWORD WINAPI pad_bot(LPVOID arg)
{
    double aim = 0, every = 0; int period = 1;
    const char* fps_env = getenv("REFINE_FPS_TARGET");
    const double frame_s = BOT_FRAME_S * 30.0 / (fps_env && atoi(fps_env) > 30 ? atoi(fps_env) : 30);
    sscanf((const char*)arg, "%lf;%lf;%d", &aim, &every, &period);
    aim /= 1000.0; every /= 1000.0;
    if (period < 1) period = 1;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    struct { uint32_t node, type; int pressed; float r_press, r_last; double t_press, want; } seen[8];
    memset(seen, 0, sizeof seen);
    int flick = 0, serial = 0;
    uint32_t counts[12] = {0};
    double release_at = 0;
    for (;;) {
        LARGE_INTEGER due; due.QuadPart = -5000;   /* 500 us */
        SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(timer, 10);
        const double now = qpc_seconds();
        if (release_at && now >= release_at) {
            if (g_bot_real) real_send((const char*)g_pad_bot_keys, true);
            g_pad_bot_keys[0] = 0; release_at = 0;
        }
        char keys[12] = ""; size_t nk = 0;
        uint32_t p = guest_read32(BOT_JUDGE + 0x14);
        int active[8] = {0};
        for (int hops = 0; p && hops < 16; hops++, p = guest_read32(p + 4)) {
            int slot = -1;
            for (int i = 0; i < 8; i++) if (seen[i].node == p) slot = i;
            const float r = guest_read_f32(p + 0x10);
            const uint32_t type = guest_read32(p + 0xC);
            const uint32_t res = guest_read32(p + 0x118);
            if (slot < 0) {
                for (int i = 0; i < 8 && slot < 0; i++) if (!seen[i].node) slot = i;
                if (slot < 0) continue;
                memset(&seen[slot], 0, sizeof seen[slot]);
                seen[slot].node = p;
                seen[slot].want = aim + every * (serial++ % period);
            }
            if (r > seen[slot].r_last + 0.15f && seen[slot].type) {
                seen[slot].pressed = 0;
                seen[slot].want = aim + every * (serial++ % period);
            }
            active[slot] = 1;
            seen[slot].r_last = r;
            seen[slot].type = type + 1;
            (void)res;
            if (!seen[slot].pressed && (double)r - frame_s <= seen[slot].want) {
                const char* k = bot_keys_for(type, &flick);
                if (nk + strlen(k) < sizeof keys) { memcpy(keys + nk, k, strlen(k)); nk += strlen(k); }
                seen[slot].pressed = 1;
                seen[slot].r_press = r;
                seen[slot].t_press = now;
                fprintf(stderr, "[BOT] press t=%.4f type=%u want=%.4f r=%.4f\n", now, type, seen[slot].want, r);
            }
        }
        for (int i = 0; i < 8; i++) if (!active[i]) seen[i].node = 0;
        /* The counters of the result screen, by grade. */
        for (int i = 0; i < 12; i++) {
            const uint32_t c = guest_read32(0x012CCA00u + i * 4);
            if (c != counts[i]) {
                counts[i] = c;
                fprintf(stderr, "[BOT] count t=%.4f +0x%02X=%u\n", now, i * 4, c);
            }
        }
        if (nk) {
            keys[nk] = 0;
            if (g_bot_real) { if (release_at) real_send((const char*)g_pad_bot_keys, true); real_front(); real_send(keys, false); }
            memcpy((void*)g_pad_bot_keys, keys, nk + 1);   /* REAL_BOT: only what to release; PAD_BOT is unset */
            release_at = now + 0.05;
        }
    }
    return 0;
}

/* Re:Fine 4d.2: the vblank clock. The game's clock is the vblank at 59.94 Hz
 * (NTSC 60000/1001), independent of the monitor's refresh and of present
 * (docs/decisions.md): Project DIVA F counts vblanks in its handler (0x588310)
 * and its frame loop waits for two of them.
 *
 * Adapted from JonathanDC64/ps3recomp ca23b8bd (MIT, runtime/ppu/tests/
 * boot_main.cpp vblank_clock; THIRD_PARTY_NOTICES.md). Like libgcm's interrupt
 * thread, this thread runs the game's vblank and flip handlers itself, on a
 * guest stack of its own (the runtime gives every host thread its own callback
 * stack slot since ps3recomp v0.18.0, #489); the HLE-boundary pump stays as the
 * fallback for a tick it could not deliver because another thread was
 * mid-pump. Deadlines are absolute, t0 + n * period, computed from n each time,
 * so rounding never accumulates into drift. It sleeps on a high-resolution
 * waitable timer and re-checks on wake (a timer can fire a little early);
 * there is no spin. */
extern "C" void     ppu_gcm_pump(void);
extern "C" void     cellGcmTickPace(void);

#define VBLANK_HZ_NUM 60000ll   /* 59.94 Hz = 60000 / 1001 */
#define VBLANK_HZ_DEN 1001ll

/* Re:Fine 6d: frames per second asked for. F waits two vblanks per frame, so the clock
 * beats at 2 * fps_target: 59.94 Hz at 30, today's pace. The game's vblank handler keeps
 * 59.94 Hz at any target; only cellGcmGetVBlankCount and the flip completion go faster. */
/* Re:Fine 0161: it can change in the running game (port_set_fps_target, from the port's
 * menu). Every user reads it again each frame or beat, and the game's thread sees the
 * change on its next frame (fps_guest_frame). */
/* Re:Fine 0163: fps_target() is the rate in force now. The user's choice is g_fps_user; out of a
 * song or PV (the song clock does not move) the menus run at 30 unless "Menu frame rate" is
 * "Same as game". Why: the 2D of the menus (AET lists) runs faster than the original at more fps and the
 * state that drives it is not found (STATUS, B; hito 7). The game's thread decides song / menu once a frame
 * (fps_guest_frame); the change goes through the same path as a live change (0161). */
static volatile int g_fps_cur = 0, g_fps_user = 0;
static volatile int g_menu_fps_same = 1;    /* refine.ini menu_fps = same (default) or 30, REFINE_MENU_FPS */
static volatile int g_fps_in_song = 0;
/* For the port's menu (2026-11-11): one answer to "is a song being played" for everything that asks (the quick
 * bar's pause, pause on focus lost, Discord, Star vibration). With the known EBOOT it is the game's pv_game
 * task (below), not the song clock, which the song select's preview also moves. */
extern "C" int port_in_song(void) { return g_fps_in_song; }

/* 2026-11-11: a song is being played (or a PV watched): the game's own pv_game task is alive. Its list
 * (0xBF2A4C -> work, +4 head; a node +0 next, +8 the task; a task +0 its vtable) is what tools/task_list.py
 * reads; in the song select only SelFreeplay is there, also while the preview moves the song clock (which
 * the clock rule below took for a song, so the song select ran at the full frame rate). -1: not this EBOOT. */
static int fps_pv_game_alive(void)
{
    /* 2026-11-11 (the author's video, run ms2): the song select also makes a pv_game task, for the stage it shows
     * behind the list, and was taken for a song (no 30 Hz ticks there, no drawing between them). A song is
     * pv_game without the song select's task, SelFreeplay (vtable 0xBB0C98). */
    const uint32_t kTasks = 0x00BF2A4Cu, kPvGameVt = 0x00BB3B98u, kSelFreeplayVt = 0x00BB0C98u;
    auto rd = [](uint32_t a) { return _byteswap_ulong(*(const uint32_t*)(vm_base + a)); };
    const uint32_t work = rd(rd(kTasks));
    if (work < 0x10000u || work >= 0x40000000u) return -1;
    const uint32_t head = rd(work + 4);
    if (head < 0x10000u || head >= 0x40000000u) return -1;
    uint32_t node = rd(head);
    int pv = 0, sel = 0;
    for (int n = 0; node != head && n < 300; n++) {
        if (node < 0x10000u || node >= 0x40000000u) return -1;
        const uint32_t task = rd(node + 8);
        if (task >= 0x10000u && task < 0x40000000u) {
            const uint32_t vt = rd(task);
            if (vt == kPvGameVt) pv = 1;
            else if (vt == kSelFreeplayVt) sel = 1;
        }
        node = rd(node);
    }
    static int last = -1;
    if ((pv | sel << 1) != last) { last = pv | sel << 1; fprintf(stderr, "[fps] tasks: pv_game=%d song select=%d (%.1f s)\n", pv, sel, (double)clock() / CLOCKS_PER_SEC); }
    return pv && !sel;
}
/* 2026-11-21: any frame rate from FPS_MIN to FPS_MAX, or none at all ("unlimited", stored as 0). Unlimited still
 * needs a pace for the vblank clock and the flip, so it runs them at FPS_MAX and the frames come as fast as the game
 * and the GPU make them (with V-Sync on, at the monitor's rate). Everything that steps by time reads the real length
 * of each frame, so nothing depends on the number chosen; the cap keeps the speed step well above its floor (1/256). */
#define FPS_MIN 30
#define FPS_MAX 1000
#define FPS_UNLIMITED 0
#define FPS_DISPLAY 1                          /* the refresh rate of the main display */
static int fps_parse(const char* s)            /* -1 when it is not a frame rate */
{
    if (!s || !*s) return -1;
    if (!_stricmp(s, "unlimited") || !_stricmp(s, "uncapped") || !strcmp(s, "0")) return FPS_UNLIMITED;
    if (!_stricmp(s, "display") || !_stricmp(s, "monitor")) return FPS_DISPLAY;
    char* end = nullptr;
    const long v = strtol(s, &end, 10);
    if (end == s || v < FPS_MIN) return -1;
    return v > FPS_MAX ? FPS_MAX : (int)v;
}
/* 2026-11-21: "display" paces the game at the main display's refresh rate. With V-Sync on, a frame rate that is not
 * the display's (60 on a 144 Hz screen, 120 on 165 Hz) shows some frames for one refresh and others for two, and the
 * motion judders however even the game's own steps are; at the display's rate every frame gets one refresh.
 * ASSUMPTION: the game's window is on the main display (a second monitor with another rate is not looked at). */
static int fps_display_rate(void)
{
    static int rate;
    if (!rate) {
        DEVMODEW dm = {};
        dm.dmSize = sizeof dm;
        const int hz = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 0;
        rate = hz < FPS_MIN ? 60 : hz > FPS_MAX ? FPS_MAX : hz;   /* 0 and 1 mean "the hardware default" */
        fprintf(stderr, "[fps] main display: %d Hz\n", hz);
    }
    return rate;
}
static int fps_pace(int user)
{
    if (user == FPS_UNLIMITED) return FPS_MAX;
    if (user == FPS_DISPLAY) return fps_display_rate();
    return user;
}
static int g_fps_user_read;
static int fps_user(void)                      /* the user's choice: FPS_MIN..FPS_MAX, or FPS_UNLIMITED */
{
    if (!g_fps_user_read) {
        g_fps_user_read = 1;
        /* 60 by default since 2026-11-14 (the author: the port is for modern PCs; 30 is still offered). */
        const char* e = getenv("REFINE_FPS_TARGET");
        const int v = e ? fps_parse(e) : 60;
        g_fps_user = v >= 0 ? v : 60;
        if (v < 0) fprintf(stderr, "[fps] fps_target=%s is not a frame rate (%d-%d or unlimited), using 60\n", e, FPS_MIN, FPS_MAX);
        /* 0175 (2026-11-11): "same" by default, now that the menus keep their speed (30 Hz ticks) and are
         * drawn between ticks. menu_fps = 30 still draws them at 30. */
        const char* m = getenv("REFINE_MENU_FPS");
        g_menu_fps_same = !(m && !strcmp(m, "30"));
    }
    return g_fps_user;
}
extern "C" void pad_set_fps_high(int high);   /* libs/input/cellPad.c, 0161 */
extern "C" void rob_interp_set_active(int on);
static int fps_eff(void) { return (g_menu_fps_same || g_fps_in_song) ? fps_pace(fps_user()) : 30; }
static void fps_update_eff(void)
{
    const int e = fps_eff();
    if (e != g_fps_cur) { g_fps_cur = e; pad_set_fps_high(e > 30); }
}
static int fps_target(void)
{
    if (!g_fps_cur) { fps_user(); g_fps_cur = fps_eff(); }
    return g_fps_cur;
}
extern "C" void port_set_fps_target(int v)
{
    if (v != FPS_UNLIMITED && v != FPS_DISPLAY && (v < FPS_MIN || v > FPS_MAX)) return;
    if (v == fps_user()) return;
    fprintf(stderr, "[fps] fps_target %d -> %d in the running game (0 = unlimited, 1 = display)\n", fps_user(), v);
    g_fps_user = v;
    fps_update_eff();
}
extern "C" int port_fps_user(void) { return fps_user(); }
extern "C" void port_set_menu_fps(int same)
{
    fps_user();
    if (same == g_menu_fps_same) return;
    fprintf(stderr, "[fps] menu frame rate: %s\n", same ? "same as game" : "30 (original speed)");
    g_menu_fps_same = same;
    fps_update_eff();
}

/* Re:Fine 6d: the game's own speed. Every frame 0x3C300 copies the speed at 0xC22F50 (1.0,
 * base 0xC22E70 + 0xE0) to +0x14, and what steps "per frame" reads it through 0x3BC0C:
 * fades, sprites, characters, and the motion clock as speed * 60 / 29.97. At 2 * 30 fps
 * a speed of 0.5 gives the same per second, and nearly all tasks still run every frame
 * (0x4BF40: only the ones flagged by 0x4143F0 tick on speed). It is written here, on the
 * game's thread, once a frame: 0x3C178 sets 1.0 at boot, and what the game saves and
 * restores (0x131A0C, 0x131908) comes back in our range. It is each frame's real time in
 * 1/29.97 s steps, with what the clamp left over carried to the next frame: a monitor under
 * fps_target, or a drop in the frame rate, does not change how fast the game goes. Under
 * 29.97 fps the speed stays 1.0 and the game slows down, as the original does.
 * ASSUMPTION: the speeds set by 0x7D0748 and 0x85B718, not seen, are 1.0. No game code
 * changes here; docs/mejoras.md, R2. */
extern "C" void (*g_fps_guest_frame)(void);
extern "C" int g_fps_not_tick;         /* config/insn_patches.json: this frame is no 30 Hz tick */
int g_fps_not_tick = 0;
extern "C" int g_fps_high;             /* config/insn_patches.json: fps_target over 30 */
int g_fps_high = 0;
/* config/insn_patches.json (6d.14): the game is stopped (speed 0: pause menu, song loading)
 * and this frame is no 30 Hz tick. Then the AET layers that step a whole step a frame
 * (flag 0x400000) wait for the tick, as at 30 fps. Never set at fps_target=30. */
extern "C" int g_fps_aet_hold;
int g_fps_aet_hold = 0;
/* config/insn_patches.json: outside a song over 30 fps, this frame is no 30 Hz tick: the tasks' whole logic
 * step (0x4C0F4, called at 0x3C3C8 and 0x3CA74) is skipped, so the menus run at the original speed whatever
 * the frame rate. Until 2026-11-12 only its Ctrl pass was (0x4C2CC), and Init ran ahead of Ctrl: a song chosen
 * over 30 fps often started with no song (white screen), see the note in the JSON. */
extern "C" int g_fps_menu_hold;
int g_fps_menu_hold = 0;
/* config/insn_patches.json: real time (REFINE_REALTIME=1) and this frame is no 30 Hz tick. What the game
 * does once a frame and would be four times as fast at 120 fps -- applying each task's requested phase
 * -- the game's own screen flow (0x3E714) -- waits for the tick with it. Never set with REFINE_REALTIME=0 or
 * at 30 fps. g_state_hold is that or the menus' hold of the mode before, the guard of the call at 0x3CA2C. */
extern "C" int g_rt_not_tick, g_state_hold;
int g_rt_not_tick = 0, g_state_hold = 0;
extern "C" volatile long g_port_pad_hold;   /* libs/input/cellPad.c, 0173 */
extern "C" volatile long g_port_pad_hold_sys;   /* libs/input/cellPad.c, 0193: START and SELECT only */
extern "C" volatile long  g_port_draw_interp; /* libs/video/rsx_d3d12_backend.c, 0175 */
extern "C" volatile float g_port_draw_alpha;
extern "C" volatile long  g_port_draw_tick;
extern "C" volatile long  g_port_draw_ticks;
extern "C" volatile long  g_port_draw_seq;
extern "C" void judge_frame(void);            /* src/judge.cpp */
extern "C" void rob_interp_init(void);        /* src/rob_interp.cpp: option C, the pose between ticks */
extern "C" void rob_interp_frame_end(void);
extern "C" void rob_interp_menu(int on, float alpha, float speed);
extern "C" void cellPad_poll(void);          /* libs/input/cellPad.c */
extern "C" void evt_frame(void);             /* src/evt.cpp: the speed test (REFINE_EVT, REFINE_ROUTE) */
static int g_fps_rob_interp;                  /* fps_speed_init found the BLUS31319 EBOOT */
static float g_fps_speed;              /* 30 / the frame rate being reached */

static int   g_fps_aet_log = -1;
static float g_fps_aet_last = -1;
static double g_fps_boot;

/* Hito 7 (2026-11-11), diagnostic only: REFINE_SAMPLE=<from>-<to> (seconds since boot) samples where the
 * game's main thread (the one that runs the frame) is, every millisecond, and at <to> prints the 60 busiest
 * addresses as "[SAMPLE] <count> ReFine.exe+0x<rva>" (tools/sample_report.py names and sums them by function
 * with build/ReFine.map). It suspends that thread for a few microseconds per sample: only for measuring. */
static DWORD g_sample_tid;
static DWORD WINAPI sample_thread(LPVOID arg)
{
    const char* e = (const char*)arg;
    double from = 0, to = 0;
    if (sscanf(e, "%lf-%lf", &from, &to) != 2 || to <= from) return 0;
    const double boot = qpc_seconds();
    while (!g_sample_tid) Sleep(10);
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_sample_tid);
    if (!th) return 0;
    sleep_until_boot_second(boot, from);
    std::unordered_map<uint64_t, unsigned> hist;
    const uint64_t base = (uint64_t)GetModuleHandleW(nullptr);
    unsigned n = 0, other = 0;
    while (qpc_seconds() - boot < to) {
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &c)) {
                if (c.Rip >= base && c.Rip < base + 0x10000000ull) hist[c.Rip - base]++; else other++;
                n++;
            }
            ResumeThread(th);
        }
        Sleep(1);
    }
    std::vector<std::pair<unsigned, uint64_t>> v;
    for (auto& kv : hist) v.push_back({kv.second, kv.first});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    fprintf(stderr, "[SAMPLE] %u samples of thread %lu, %u outside ReFine.exe (system DLLs)\n", n, g_sample_tid, other);
    for (const auto& kv : v) fprintf(stderr, "[SAMPLE] %u ReFine.exe+0x%llX\n", kv.first, (unsigned long long)kv.second);
    CloseHandle(th);
    return 0;
}

/* 2026-11-12: the menus' 2D by real time. The AET objects (the 2D animations of every menu) step in the Basic
 * pass of their manager's task (vtable 0xBA8310, slot 6: 0x172CC, which calls 0x1416C for each object), a pass
 * that still runs every frame while the menus' logic waits for its 30 Hz tick (g_fps_menu_hold). There each
 * object advances by its own rate times the game speed (0x3BC0C: 0xC22E70 + 0x14), which in the menus is 1.0,
 * a whole step, for the tasks' logic. So for that pass only the speed is this frame's real fraction of a step
 * (30 / the frame rate being reached, the same that adds up to one tick): the animations advance every frame
 * at the original speed and the game's own code evaluates them at the fractional frame. Before, they were held
 * between ticks and the renderer blended draws it paired by guessing (0175), which made sprites of the title
 * jump. Reached only through the vtable, so it hooks with ppu_register_function. REFINE_AET_TIME=0: as before. */
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
static int g_fps_menu_time, g_fps_aet_time = 1;
extern "C" void aet_before(uint32_t mgr);     /* src/aet_time.cpp: objects end and loop on the original's steps */
extern "C" void aet_after(void);
extern "C" void aet_time_init(void);
extern "C" void aet_log(void);
static void hook_aet_basic(ppu_context* ctx)
{
    uint32_t keep; memcpy(&keep, vm_base + 0xC22E84, 4);
    if (g_fps_menu_time) {
        uint32_t w; memcpy(&w, &g_fps_speed, 4); w = _byteswap_ulong(w);
        memcpy(vm_base + 0xC22E84, &w, 4);
    }
    aet_before((uint32_t)ctx->gpr[3]);
    func_000172CC(ctx);
    aet_after();
    if (g_fps_menu_time) memcpy(vm_base + 0xC22E84, &keep, 4);
}

extern "C" void realtime_init(void);         /* src/realtime.cpp */
extern "C" void realtime_frame(int active, float speed);
static int fps_realtime(void)
{
    static int realtime = -1;
    /* On by default since 2026-11-14 (the author's call: every screen at the real frame rate). REFINE_REALTIME=0
     * goes back to the mode before (the menus' logic on 30 Hz ticks). At fps_target=30 neither does anything. */
    if (realtime < 0) { const char* e = getenv("REFINE_REALTIME"); realtime = !(e && *e == '0'); fprintf(stderr, "[fps] real time in the menus: %s%c", realtime ? "on (REFINE_REALTIME=0 turns it off)" : "off (logic on 30 Hz ticks)", 10); }
    return realtime;
}

/* Even steps (2026-11-21). A frame's step was the time measured between two frame starts on the game's thread. Those
 * starts land on the steps of the system timers (about 1 ms: 8.0, 9.0 or 10.0 ms for the 8.34 of 120 fps, run pf120),
 * while the screen shows the pictures at even intervals, so everything moved unevenly. While the frames really come
 * at the rate asked for (the mean of the last ones within 4 % of it), the step is now a whole number of frames of
 * that rate, and what the clock says over or under stays in "due" for the next ones: the sum is still the real time,
 * never more than half a frame away from it. At any other rate (unlimited, or a machine that does not reach the
 * target) the step is the measured time, as before. REFINE_STEADY=0 turns it off. */
static double steady_step(double dt, double due, double frame, double measured)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("REFINE_STEADY"); on = !(e && *e == '0'); }
    if (!on || fps_user() == FPS_UNLIMITED) return measured;
    static double mean; static int good;
    if (g_beat_frame > 0.0) frame = g_beat_frame;        /* the beat follows the screen (vblank_clock) */
    const double f = dt * 29.97 / frame;                 /* this frame's length, in frames of the target */
    if (f > 4.0) { mean = 1.0; good = 0; return measured; }   /* a load or a stall: not a rate */
    mean = mean > 0.0 ? mean + (f - mean) / 64.0 : f;
    if (mean > 0.96 && mean < 1.04) { if (good < 64) good++; } else good = 0;
    if (good < 64) return measured;
    const double k = floor(due / frame + 0.5);
    return k >= 1.0 && k * frame <= 1.0 ? k * frame : measured;
}

static void fps_guest_frame(void)
{
    if (!g_sample_tid) {
        g_sample_tid = GetCurrentThreadId();
        if (const char* e = getenv("REFINE_SAMPLE")) CreateThread(nullptr, 0, sample_thread, (LPVOID)e, 0, nullptr);
    }
    if (g_fps_aet_log < 0) { g_fps_aet_log = getenv("REFINE_AET_LOG") != NULL; g_fps_boot = qpc_seconds(); }
    rob_interp_frame_end();                   /* the tick's true pose back before any logic */
    /* 0185: the keyboard and the pad are polled every frame, whether or not the game reads the pad in it (in
     * the menus its logic, and with it its pad read, runs on the 30 Hz ticks only); what a poll sees down is
     * kept for the game's next read (s_btn_latch). */
    cellPad_poll();
    static double last, acc, due;
    static int last_fps;
    {   /* 0163: song or PV = the song clock (0x12CBE48) moved. It stays "in song" while the game is stopped
         * (pause menu, speed 0) and ends 1 s after the clock stops with the game running. */
        static float clk_last; static double clk_moved; static int clk_init;
        uint32_t cb = _byteswap_ulong(*(uint32_t*)(vm_base + 0x12CBE48)); float clk; memcpy(&clk, &cb, 4);
        uint32_t sb = _byteswap_ulong(*(uint32_t*)(vm_base + 0xC22F50)); float spd; memcpy(&spd, &sb, 4);
        const double t = qpc_seconds();
        const int pv = g_fps_rob_interp ? fps_pv_game_alive() : -1;
        if (!clk_init) { clk_init = 1; clk_last = clk; }
        else if (pv >= 0) {   /* the known EBOOT: the task decides, the clock only says it moved */
            if (clk != clk_last) clk_last = clk;
            if (pv != g_fps_in_song) { g_fps_in_song = pv; fprintf(stderr, "[fps] %s\n", pv ? "song: full frame rate" : "menus"); }
        }
        else if (clk != clk_last) { clk_last = clk; clk_moved = t; g_fps_in_song = 1; }
        else if (g_fps_in_song && spd != 0.0f && t - clk_moved > 1.0) g_fps_in_song = 0;
        fps_update_eff();
    }
    const int fps = fps_target();
    int trans = 0;                            /* the frame rate changed since the last frame (0161) */
    if (fps != last_fps) {
        if (last_fps) { trans = 1; due = 0; }
        last_fps = fps;
        rob_interp_set_active(fps > 30);
    }
    const int realtime = fps_realtime();
    const double now = qpc_seconds(), low = 30.0 / fps;
    if (last > 0) {
        due += (now - last) * 29.97;
        /* Real time (2026-11-14): this frame's step is its own real time, with no floor. The floor of 30/fps
         * below made the game run fast whenever the real frames came in a little shorter than the target
         * (which the limiter does): each frame advanced 30/fps of a step while real time advanced less, due
         * pinned at -30/fps and the pace stayed off for good (the menus of the speed test ran 5 % short,
         * 450 ms in 9 s). Only a minimum that keeps the speed away from 0 (which the game reads as stopped). */
        const double floor_rt = 1.0 / 256.0;
        /* At 30 fps every frame is a whole step, as in the original, whatever the clock says. */
        double step = realtime && fps > 30 ? (due < floor_rt ? floor_rt : due > 1.0 ? 1.0 : due)
                               : (due < low ? low : due > 1.0 ? 1.0 : due);
        if (realtime && fps > 30) step = steady_step(now - last, due, low, step);
        due -= step;
        /* A frame of a whole step is a load or a stall: at 30 fps the game goes on one step and loses the rest. Kept for
         * the next frames, the rest (up to a step) was played fast: the screens that open after a load came in up to
         * 20 % quicker (mode select's cards, 322 ms at 30 fps and 270 at 240; 2026-11-22). */
        if (realtime && fps > 30 && step >= 1.0) due = 0.0;
        if (due > 1.0) due = 1.0;
        if (due < -low) due = -low;
        g_fps_speed = (float)step;
    }
    last = now;
    float v;
    uint32_t bits = _byteswap_ulong(*(uint32_t*)(vm_base + 0xC22F50));
    memcpy(&v, &bits, 4);
    /* Ours = the very value we left there last frame (in real time it can be any (0, 1], so the range test
     * below is not enough). On the frame after a change, the speed we left was for the old rate. */
    static float wrote;
    const int ours = v > 0.0f && v <= 1.0f && (v == wrote || trans || v >= 30.0f / fps - 0.001f);
    /* At speed 0 (the game stopped, as while loading a song) the g_fps_high patch steps
     * back: the flagged AET layers are what still moves. */
    g_fps_high = ours && fps > 30;            /* the patches are the original code at 30 */
    /* Menus over 30 fps (2026-11-11): the tasks' logic runs only on 30 Hz ticks (g_fps_menu_hold), each tick a
     * whole step at speed 1.0, as the original at 30 fps; the frames between ticks only draw. Only with the
     * known EBOOT (the patch and the task list are its addresses). */
    /* Real time (docs/tiempo_real.md, 2026-11-13): the whole game steps every frame by the real time of the
     * frame, menus included; nothing waits for a 30 Hz tick but what counts frames (config/insn_patches.json).
     * REFINE_REALTIME=0: the mode before, the menus' logic on 30 Hz ticks. */
    realtime_frame(realtime && g_fps_rob_interp && fps > 30, g_fps_speed);
    const int menu_time = !realtime && g_fps_rob_interp && fps > 30 && !g_fps_in_song && ours;
    const float want = menu_time ? 1.0f : g_fps_speed;
    if (ours && v != want) {
        memcpy(&bits, &want, 4);
        *(uint32_t*)(vm_base + 0xC22F50) = _byteswap_ulong(bits);
        wrote = want;
    } else if (!ours) wrote = 0.0f;
    acc += g_fps_speed;
    pace_ev('g', now, g_fps_speed, acc >= 0.9999);
    if (acc >= 0.9999) { acc -= 1.0; g_fps_not_tick = 0; } else { g_fps_not_tick = 1; }
    /* 2026-11-12: in the menus the AET objects step every frame by real time (hook_aet_basic above), so they
     * are no longer held between ticks; with the game stopped (speed 0) the flagged layers still wait. */
    g_fps_menu_time = menu_time && g_fps_aet_time;
    g_fps_aet_hold = (v == 0.0f || (menu_time && !g_fps_aet_time)) && g_fps_not_tick;
    g_fps_menu_hold = menu_time && g_fps_not_tick;
    /* Only while the speed is ours (as the mode before did with menu_time): with the game stopped (speed 0,
     * a load or the opening movie) the game's own screen flow is what moves, and holding it to the ticks left
     * the movie ignoring START (work/run/c*, d*, e*). */
    g_rt_not_tick = realtime && g_fps_rob_interp && fps > 30 && ours && g_fps_not_tick;
    g_state_hold = g_fps_menu_hold || g_rt_not_tick;
    /* 0173: in a frame whose logic does not run, the game's pad reads repeat the last packet. In real time
     * (2026-11-14) the same holds outside a song: the menus' flow (0x3E714, g_state_hold) and the screens'
     * tasks step on the ticks, and they work out "pressed now" from the reads of the frame before -- with the
     * pad read every frame, the up->down edge fell on a frame that no tick-gated step ever looked at, and the
     * opening movie ignored START for its whole 100 s (work/run/c*-j*). In a song nothing is held: there the
     * logic runs every frame and the judge reads the press as soon as it arrives. */
    static int pad_hold_on = -1;   /* REFINE_PAD_HOLD=0 (diagnostico): sin repetir el paquete entre ticks */
    if (pad_hold_on < 0) { const char* e = getenv("REFINE_PAD_HOLD"); pad_hold_on = !(e && *e == '0'); }
    /* 2026-11-21: also while the game is stopped (speed 0: the pause menu, a song loading), in a song or not. The
     * pause menu's logic now runs on the ticks (src/realtime.cpp), so it needs the same rule; nothing judges
     * notes while the game is stopped. */
    const int stopped_not_tick = realtime && g_fps_rob_interp && fps > 30 && v == 0.0f && g_fps_not_tick;
    g_port_pad_hold = pad_hold_on && (g_fps_menu_hold || (g_rt_not_tick && !g_fps_in_song) || stopped_not_tick);
    /* 0193: in a song that runs, between ticks only START and SELECT are held: the pause logic reads them on the
     * ticks, and the notes are judged every frame. */
    g_port_pad_hold_sys = pad_hold_on && g_rt_not_tick && g_fps_in_song;
    /* 0175: the backend draws the menus between their last two ticks, at the phase since the last one (acc,
     * in ticks, after this frame's tick decision). REFINE_MENU_INTERP=0: drawn at the ticks only. */
    static int menu_interp = -1;
    /* 2026-11-12: off by default. It pairs the draws of two ticks by guess, and a wrong pair is a sprite drawn
     * somewhere else for a frame (the author's video: 16 such frames in 11 s of the song list; tools/win_burst.ps1 +
     * video_glitch.py: 5 in 30 s with it, 0 without). The AET objects already step every frame (hook_aet_basic)
     * and the characters too (rob_interp_menu); what the game's code moves stays at the ticks. =1 turns it on. */
    if (menu_interp < 0) { const char* e = getenv("REFINE_MENU_INTERP"); menu_interp = e && *e == '1'; }
    if (g_fps_rob_interp) rob_interp_menu(menu_time, (float)(acc < 0.0 ? 0.0 : acc > 1.0 ? 1.0 : acc), g_fps_speed);
    g_port_draw_seq++;
    g_port_draw_tick = !g_fps_not_tick;
    if (!g_fps_not_tick) g_port_draw_ticks++;
    g_port_draw_alpha = (float)(acc < 0.0 ? 0.0 : acc > 1.0 ? 1.0 : acc);
    g_port_draw_interp = menu_time && menu_interp;
    /* The frame rate the game measures for itself (0xC22E98, +0x28; 30 at 30 fps). One step of the game is
     * 1/30 s whatever the real frame rate, so in real time it is 30 as well: left at the real rate (110-125 at
     * 120 fps) the opening movie never took START (work/run/i1 against i0, 2026-11-14). */
    if (menu_time || (realtime && ours)) {
        const float thirty = 30.0f; uint32_t w; memcpy(&w, &thirty, 4);
        *(uint32_t*)(vm_base + 0xC22E98) = _byteswap_ulong(w);
    }
    {   /* REFINE_SEL_LOG=1 (diagnostic, 2026-11-12): the song the game is about to play (block at *0xBFAC34: +0x10 the PV, +0x1C the difficulty), whenever it changes */
        static int on = -1; static uint32_t lp = ~0u, ld = ~0u;
        if (on < 0) on = getenv("REFINE_SEL_LOG") != NULL;
        if (on) {
            const uint32_t blk = _byteswap_ulong(*(uint32_t*)(vm_base + 0xBFAC34));
            if (blk >= 0x10000u && blk < 0x40000000u) {
                const uint32_t pv = _byteswap_ulong(*(uint32_t*)(vm_base + blk + 0x10)), df = _byteswap_ulong(*(uint32_t*)(vm_base + blk + 0x1C));
                if (pv != lp || df != ld) { lp = pv; ld = df; if (getenv("REFINE_SEL_DUMP")) for (uint32_t o = 0; o < 0x200; o += 32) { fprintf(stderr, "[sel-dump] +%03X", o); for (uint32_t k = 0; k < 32; k += 4) fprintf(stderr, " %08X", _byteswap_ulong(*(uint32_t*)(vm_base + blk + o + k))); fputc(10, stderr); } fprintf(stderr, "[sel] t=%.3f pv=%d diff=%d tick=%d in_song=%d fps=%d%c", now - g_fps_boot, (int)pv, (int)df, !g_fps_not_tick, (int)g_fps_in_song, fps, 10); }
            }
        }
    }
    {   /* REFINE_FORCE_PV=<number> (diagnostic, test runs only): whatever song the list has chosen, that PV is the one
         * the game loads (the block of REFINE_SEL_LOG). To measure every song without a save that has them. */
        static int pv = -1;
        if (pv < 0) { const char* e = getenv("REFINE_FORCE_PV"); pv = e ? atoi(e) : 0; }
        if (pv && !g_fps_in_song) {
            const uint32_t blk = _byteswap_ulong(*(uint32_t*)(vm_base + 0xBFAC34));
            if (blk >= 0x10000u && blk < 0x40000000u) {
                uint32_t* at = (uint32_t*)(vm_base + blk + 0x10);
                const uint32_t now_pv = _byteswap_ulong(*at);
                if (now_pv >= 600 && now_pv < 700 && (int)now_pv != pv) *at = _byteswap_ulong((uint32_t)pv);
            }
        }
    }
    if (g_fps_aet_log) {   /* REFINE_AET_LOG=1, 0162: once a second, the game's speed block (0xC22E70: +0x14 speed, +0x20 flag, +0x24 / +0x28 for 0x3BC0C) */
        static double t_log; static int n_log;
        n_log++;
        if (now - t_log >= 1.0) {
            fprintf(stderr, "[fps] t=%.1f eff=%d user=%d in_song=%d frames=%d in %.2f s speed=%.4f\n", now - g_fps_boot,
                    fps_target(), fps_user(), (int)g_fps_in_song, n_log, now - t_log, g_fps_speed);
            n_log = 0;
            t_log = now;
            aet_log();
            auto rd = [](uint32_t a) { uint32_t w = _byteswap_ulong(*(uint32_t*)(vm_base + a)); float f; memcpy(&f, &w, 4); return f; };
            fprintf(stderr, "[aet] t=%.1f s speed=%.4f flag20=%u +24=%.5f +28=%.5f src(E0)=%.4f flagE4=%u\n", now - g_fps_boot,
                    rd(0xC22E84), (unsigned)*(uint8_t*)(vm_base + 0xC22E90), rd(0xC22E94), rd(0xC22E98), rd(0xC22F50),
                    (unsigned)*(uint8_t*)(vm_base + 0xC22F54));
        }
    }
    if (g_fps_aet_log && (v == 0.0f) != (g_fps_aet_last == 0.0f)) {   /* REFINE_AET_LOG=1: when the game stops and goes */
        fprintf(stderr, "[fps] game speed %.4f -> %.4f at %.3f s\n", g_fps_aet_last, v, now - g_fps_boot);
        g_fps_aet_last = v;
    }
    judge_frame();
    evt_frame();
}

static void fps_speed_init(void)
{
    uint32_t refresh, thirty;
    memcpy(&refresh, vm_base + 0xC00014, 4);
    memcpy(&thirty, vm_base + 0xBF2880, 4);
    if (_byteswap_ulong(thirty) != 0x41F00000 || _byteswap_ulong(refresh) != 0x426FC28F) {
        fprintf(stderr, "[fps] not the BLUS31319 EBOOT: fps_target only sets the pace\n");
        return;
    }
    g_fps_speed = 30.0f / fps_target();
    g_fps_guest_frame = fps_guest_frame;
    g_fps_rob_interp = 1;
    fprintf(stderr, "[fps] fps_target=%d: speed %f\n", fps_target(), g_fps_speed);
}

/* Re:Fine 6d.4: over 30 fps a flip completes as soon as the drain has shown it, not on the
 * next beat: waiting for the beat cost up to a beat per frame, on top of the wait for two
 * of them, and capped 180 at ~150. vblank_clock still delivers it. NULL at fps_target=30. */
static HANDLE g_flip_done;

static DWORD WINAPI vblank_clock(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (!timer) fprintf(stderr, "[vblank] no high-resolution timer (error %lu); using Sleep\n",
                        GetLastError());

    LARGE_INTEGER qpf, now;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&now);
    long long t0 = now.QuadPart;
    long long n  = 1;
    long long fps = fps_target();         /* the beat is 59.94 Hz * fps / 30; 0161: it can change */
    long long period = qpf.QuadPart * VBLANK_HZ_DEN * 30 / (VBLANK_HZ_NUM * fps);
    long long beats = 0;                  /* beats since t0, for the 59.94 Hz division */
    /* On the screen's beat (2026-11-21). With V-Sync the pictures go up on the screen's refreshes; this clock ran at
     * 59.94 Hz / 30 * fps with whatever phase it started with, and a frame was presented 1 to 7 ms after it started,
     * so the pictures crossed the refreshes and stayed up one more or one less than their neighbours (60 fps on a
     * 180 Hz screen: 35 % of them, run pkb60). When the frame rate is a divisor of the refresh (within 0.4 %) and a
     * frame has 10 ms or more, it lasts exactly that many refreshes, frame_clock presents it on its own refresh
     * (hold_present), and this clock moves its phase (30 us a frame at most) so that wait stays short. Any other rate,
     * no V-Sync, 30 fps, or no compositor to ask: the clock of before. REFINE_SCREEN_BEAT=0 turns it off. */
    const char* sb = getenv("REFINE_SCREEN_BEAT"); const char* vs = getenv("D3D12_VSYNC");
    const int follow = !(sb && *sb == '0') && !(vs && *vs == '0');
    double scr_per = 0, scr_at = 0, asked = -1, lock = 0;   /* lock: seconds of a frame; 0 = free */
    long long nudged = 0;
    for (;;) {
        double want = lock;
        if (follow && qpc_seconds() - asked > 0.1) {
            asked = qpc_seconds();
            want = 0;
            if (fps_target() > 30 && screen_beat(&scr_per, &scr_at)) {
                const double frame = (double)VBLANK_HZ_DEN * 60.0 / ((double)VBLANK_HZ_NUM * fps_target());
                const double m = floor(frame / scr_per + 0.5);
                if (m >= 1.0 && m * scr_per >= 0.010 && fabs(m * scr_per / frame - 1.0) < 0.004) want = m * scr_per;
            }
            /* The period the compositor reports moves by parts per million: not a new schedule. */
            if (lock > 0 && want > 0 && fabs(want / lock - 1.0) < 1e-3) want = lock;
            /* One reading of the refresh is up to 1.5 ms off (run pva): they are followed slowly, phase and rate. */
            if (want > 0) {
                if (g_screen_per > 0 && fabs(scr_per / g_screen_per - 1.0) < 1e-3) {
                    const double n = floor((scr_at - g_screen_at) / g_screen_per + 0.5);
                    const double e = scr_at - g_screen_at - n * g_screen_per;
                    if (n > 0) g_screen_per += 0.002 * e / n;
                    g_screen_at += n * g_screen_per + 0.08 * e;
                } else { g_screen_per = scr_per; g_screen_at = scr_at; }
            }
        }
        if (fps != fps_target() || want != lock) {   /* a new schedule from now: no burst, no gap */
            fps = fps_target();
            lock = fps > 30 ? want : 0;
            period = lock > 0 ? (long long)(lock / 2.0 * (double)qpf.QuadPart)
                              : qpf.QuadPart * VBLANK_HZ_DEN * 30 / (VBLANK_HZ_NUM * fps);
            QueryPerformanceCounter(&now);
            t0 = now.QuadPart; n = 1; beats = 0; nudged = 0;
            g_hold_err = 0; g_beat_s = lock; g_beat_frame = lock * 29.97;
            fprintf(stderr, "[vblank] %lld fps, %s%c", fps, lock > 0 ? "on the screen's beat" : "own clock", 10);
        }
        if (lock > 0 && beats / 2 != nudged) {       /* once a frame: presents that wait too long start later */
            nudged = beats / 2;
            double nudge = g_hold_err * 0.02;
            if (nudge > 30e-6) nudge = 30e-6; else if (nudge < -30e-6) nudge = -30e-6;
            t0 += (long long)(nudge * (double)qpf.QuadPart);
        }
        /* Re:Fine 5c.2, diagnostic: VBLANK_SLOW=<factor> stretches the tick. */
        static const double slow = getenv("VBLANK_SLOW") ? atof(getenv("VBLANK_SLOW")) : 1.0;
        const long long deadline = t0 + (lock > 0 ? (long long)(n * (lock / 2.0) * (double)qpf.QuadPart)
            : (long long)(slow * n * qpf.QuadPart * VBLANK_HZ_DEN * 30 / (VBLANK_HZ_NUM * fps)));
        QueryPerformanceCounter(&now);
        const long long wait = deadline - now.QuadPart;
        if (wait > 0) {
            const long long us = wait * 1000000 / qpf.QuadPart;
            if (timer) {
                LARGE_INTEGER due; due.QuadPart = -(us * 10 > 0 ? us * 10 : 1);
                SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
                const HANDLE wake[2] = { timer, g_flip_done };
                if (WaitForMultipleObjects(fps > 30 ? 2 : 1, wake, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
                    cellGcmTickFlip();
                    ppu_gcm_pump();
                }
            } else {
                Sleep((DWORD)(us / 1000));
            }
            continue;
        }
        /* Far behind (the machine stalled): restart the schedule rather than
         * burst the missed vblanks. */
        if (-wait > period * 8) { t0 = now.QuadPart; n = 0; }
        n++;
        beats++;
        if (beats * 30 / fps != (beats - 1) * 30 / fps) {
            cellGcmTickVBlank();
            InterlockedIncrement(&g_vblank_ticks);
        } else {
            cellGcmTickPace();
        }
        cellGcmTickFlip();
        ppu_gcm_pump();
    }
    return 0;
}

/* A finished frame waits for its refresh (vblank_clock, "On the screen's beat"): the moments are one frame apart and
 * 35 % into a refresh, away from its edges (the GPU still has work to do, and a timer may wake 1 ms late). A frame that
 * is already past its moment goes up at once, the next one counts from there, and the spare time asked for grows a
 * little; it shrinks again when nothing is late for a while. */
static void hold_present(HANDLE timer)
{
    static double last, spare = 0.0025, mean; static unsigned good;
    const double frame = g_beat_s, per = g_screen_per;
    if (!(frame > 0.0) || !(per > 0.0) || !timer) { last = 0; return; }
    const double now = qpc_seconds(), at = g_screen_at + 0.35 * per;
    double due = last > 0 ? last + frame : now;
    due = at + floor((due - at) / per + 0.5) * per;        /* on the refreshes, whatever the clocks drifted */
    const double wait = due - now;
    if (last <= 0 || wait < 0 || wait > frame) {           /* the first, late, or a schedule of another time */
        if (last > 0 && wait < 0) { good = 0; if (spare < 0.3 * frame) spare += 0.0003; }
        last = at + floor((now - at) / per) * per;
        if (wait > frame) mean = spare;
    } else {
        if (++good >= 1024) { good = 0; if (spare > 0.0025) spare -= 0.0003; }
        last = due;
        LARGE_INTEGER d; d.QuadPart = -(long long)(wait * 1e7);
        if (d.QuadPart < 0 && SetWaitableTimer(timer, &d, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    }
    mean += ((wait < 0 ? 0 : wait > frame ? spare : wait) - mean) / 32.0;
    g_hold_err = mean - spare;
}

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned  last_flip = 0;
    unsigned  flip_signalled = 0;

    /* Re:Fine 6d.1: asleep on a high-resolution timer, no spin. 4 ms at fps_target=30, the
     * Sleep(4) of before; at more fps, an eighth of a frame. */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (!timer) fprintf(stderr, "[frame] no high-resolution timer (error %lu); using Sleep\n",
                        GetLastError());

    /* Re:Fine 4d.2: the vblank ticks come from vblank_clock below, not from this
     * loop. What stays here is the drain and the present. */
    for (;;) {
        /* 0161: live. An eighth of a frame, but never under 0.5 ms: at 1000 fps that would be 8000 wakeups a
         * second for a drain that the flip event (g_flip_done) already makes prompt. */
        const int fps_now = fps_target();
        const int pass_us = fps_now == 30 ? 4000 : (1000000 / fps_now / 8 < 500 ? 500 : 1000000 / fps_now / 8);
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -(long long)pass_us * 10;
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(pass_us >= 1000 ? pass_us / 1000 : 1);
        }

        /* Drain at this cadence: titles fence every render pass on a label the
         * drain writes, and one wave of those at 16 ms apiece paces the guest
         * into single-figure frame rates. The real RSX writes them in
         * microseconds. A pending flip is presented BEFORE draining further:
         * the flip fires at a get==put frame boundary on the guest thread, so
         * the batch held right now is exactly the completed frame. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                if (cellGcm_flip_request_count() != last_flip) hold_present(timer);   /* not already shown below */
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
                if (g_frame_prof_on) fp_flip();
            }
            if (g_frame_prof_on) {
                const double t0 = qpc_seconds(), rf0 = g_frame_prof.wait + g_frame_prof.cmd + g_frame_prof.exec +
                                                       g_frame_prof.present + g_frame_prof.next;
                cellGcm_rsx_process_fifo();
                g_fp_drain_s += qpc_seconds() - t0 - (g_frame_prof.wait + g_frame_prof.cmd + g_frame_prof.exec +
                                                      g_frame_prof.present + g_frame_prof.next - rf0);
            } else {
                cellGcm_rsx_process_fifo();
            }

            /* Window closed: the console's Quit Game. The drain and the present go on,
             * because the title still needs the RSX until it leaves by itself. */
            if (rsx_backend_pump() != 0)
                ps3_host_quit("window closed");
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                hold_present(timer);
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                rsx_backend_present();
            }
            if (g_flip_done && fc != flip_signalled) {
                flip_signalled = fc;
                if (fps_target() > 30) SetEvent(g_flip_done);
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * PARAM.SFO
 * -----------------------------------------------------------------------*/
/* Hand cellGame the title's PARAM.SFO. Same lookup as the toolkit's
 * runtime/ppu/tests/boot_main.cpp: the file sits under PS3_GAME/ for a
 * disc-style root (this project's tools/run.ps1), one level up for a root at
 * USRDIR, or beside the root itself. The first that exists wins; if none does,
 * cellGame logs it and keeps its defaults. */
static void init_title_from_paramsfo(void)
{
    static const char* const kSfoLayouts[] = {
        "%s/PS3_GAME/PARAM.SFO",   /* vfs root = disc root      */
        "%s/../PARAM.SFO",         /* vfs root = .../USRDIR     */
        "%s/PARAM.SFO",            /* vfs root = the game dir   */
    };
    char sfo[1100];
    int found = 0;
    for (size_t i = 0; i < sizeof kSfoLayouts / sizeof kSfoLayouts[0]; i++) {
        snprintf(sfo, sizeof sfo, kSfoLayouts[i], ppu_vfs_root);
        FILE* f = fopen(sfo, "rb");
        if (f) { fclose(f); found = 1; break; }
    }
    if (!found)
        snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
    cellGame_init_from_paramsfo(sfo);
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is mapped up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left.
     *
     * mmap, not calloc: vm_commit() hands vm_base + addr to mprotect, which
     * takes page-aligned addresses only. glibc serves an allocation this big
     * from its own mmap chunk and returns a pointer 16 bytes past the page
     * start, so every commit failed with EINVAL and sys_memory_allocate
     * answered ENOMEM to every call. mmap's base is page-aligned and zeroed. */
    void* p = mmap(NULL, 0xE0000000u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
    ppu_vm_size = 0xE0000000u;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    count_guest_callback(opd);
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

extern "C" void refine_crash_report_install(void);   /* src/crash_report.cpp */
int main(int argc, char** argv)
{
    refine_crash_report_install();
    if (getenv("REFINE_CRASH_TEST")) *(volatile int*)(uintptr_t)8 = 1;   /* diagnostic only: proves the crash report */
    load_port_settings();
    {   /* The frame rate as a number for the runtime libraries that read REFINE_FPS_TARGET at their init (cellPad:
         * "over 30?"). Without this they saw nothing at the default 60 and "unlimited" as 0, and the stick re-arm
         * of high frame rates stayed off until the rate was changed in the menu. */
        char pace[16];
        snprintf(pace, sizeof pace, "%d", fps_pace(fps_user()));
        _putenv_s("REFINE_FPS_TARGET", pace);
    }
    /* 6b+6c p4: without an ELF on the command line (or with one that is not there), the disc folder
     * the player chose in the start screen: <folder>/PS3_GAME/USRDIR/EBOOT.elf. */
    static char elf_from_ini[1100];
    if (g_game_folder[0] && (argc < 2 || GetFileAttributesA(argv[1]) == INVALID_FILE_ATTRIBUTES)) {
        snprintf(elf_from_ini, sizeof elf_from_ini, "%s/PS3_GAME/USRDIR/EBOOT.elf", g_game_folder);
        if (GetFileAttributesA(elf_from_ini) != INVALID_FILE_ATTRIBUTES) {
            static char* av[3]; av[0] = argv[0]; av[1] = elf_from_ini; av[2] = NULL;
            argv = av; argc = 2;
            fprintf(stderr, "[boot] game_folder from refine.ini: %s\n", elf_from_ini);
        }
    }
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    /* Re:Fine 4c.5: quiet unless asked. With stderr redirected, ps3_log_verbose()
     * turns on one line per event, every thread through one FILE lock: the audio
     * writer stalled up to 3.7 ms inside sys_event_port_send and the mix played
     * 27-102 silent blocks per movie, against 0 with PS3_VERBOSE=0. */
    if (!getenv("PS3_VERBOSE")) _putenv_s("PS3_VERBOSE", "0");
    printf("=== ps3recomp game runner ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }
    fps_speed_init();       /* 0161: always, the rate can change in the running game; it is the original at 30 */
    g_flip_done = CreateEventW(NULL, FALSE, FALSE, NULL);

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);
    init_title_from_paramsfo();
    ps3_ffmpeg_get();

    ppu_recomp_register();   /* lifted function table -> address map */
    if (g_fps_rob_interp) rob_interp_init();   /* 6d option C: hooks two Rob task methods */
    if (g_fps_rob_interp && fps_realtime()) realtime_init();
    if (g_fps_rob_interp) {                    /* 2026-11-12: the menus' AET objects by real time */
        const char* e = getenv("REFINE_AET_TIME"); g_fps_aet_time = !(e && *e == '0');
        if (g_fps_aet_time) { ppu_register_function(0x172CC, hook_aet_basic); aet_time_init(); }
    }
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    if (getenv("FRAME_PROF")) {
        for (int c = 0; c < FP_N; c++) g_fp_s[c] = (float*)malloc(FP_MAX * sizeof(float));
        g_frame_prof_on = 1;
        g_frame_prof_guest = fp_guest;
    }
    /* 6b+6c: the port's settings menu (F1), hooks set before the frame clock runs. */
    { const char* rh = getenv("REFINE_RENDER_HEIGHT");
      port_menu_start(rh ? atoi(rh) : 720, fps_user()); }
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
    CreateThread(NULL, 1u * 1024 * 1024, vblank_clock, NULL, 0, NULL);

    /* PS3_SAMPLE=<ms>: the runtime's guest-code sampler (runtime/ppu/ppu_loader.cpp,
     * per host thread, guest vs host time). A no-op unless the variable is set. */
    ps3_sampler_start();

    g_boot_ms = GetTickCount64();
    if (const char* w = getenv("PS3_WATCHDOG_S")) {
        int s = atoi(w);
        if (s > 0) CreateThread(NULL, 256u * 1024, watchdog, (LPVOID)(uintptr_t)(s * 1000), 0, NULL);
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &g_main_thread, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    if (const char* md = getenv("MEM_DUMP"))
        CreateThread(NULL, 256u * 1024, mem_dump, (LPVOID)md, 0, NULL);
    if (const char* mb = getenv("MEM_BP"))
        CreateThread(NULL, 256u * 1024, mem_bp, (LPVOID)mb, 0, NULL);
    if (const char* pb = getenv("PAD_BOT"))
        CreateThread(NULL, 256u * 1024, pad_bot, (LPVOID)pb, 0, NULL);
    g_real_t0 = qpc_seconds();
    if (const char* rb = getenv("REAL_BOT")) {
        g_bot_real = true;
        CreateThread(NULL, 256u * 1024, pad_bot, (LPVOID)rb, 0, NULL);
    }
    if (const char* rk = getenv("REAL_KEYS"))
        CreateThread(NULL, 256u * 1024, real_keys, (LPVOID)rk, 0, NULL);
    if (const char* mw = getenv("MEM_WATCH"))
        CreateThread(NULL, 256u * 1024, mem_watch, (LPVOID)mw, 0, NULL);
#ifdef _WIN32
    if (const char* sh = getenv("SHOTS"))
        CreateThread(NULL, 256u * 1024, shots, (LPVOID)sh, 0, NULL);
#endif
    if (const char* fp = getenv("FRAME_PROF"))
        CreateThread(NULL, 256u * 1024, frame_prof, (LPVOID)fp, 0, NULL);
    if (const char* pc = getenv("REFINE_PACE"))
        CreateThread(NULL, 256u * 1024, pace_log, (LPVOID)pc, 0, NULL);
    if (const char* fs = getenv("PS3_FRAMESTATS"))
        CreateThread(NULL, 256u * 1024, framestats, (LPVOID)fs, 0, NULL);
    if (getenv("AUDIO_STALL")) {
        g_audio_stall_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 256u * 1024, audio_stall, NULL, 0, NULL);
    }

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
