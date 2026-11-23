/* rpcs3_watch.c - Lee desde fuera, solo lectura, el estado del juego en RPCS3 (5c.3).
 *
 * Herramienta de medida: con ReadProcessMemory localiza la memoria guest buscando la tabla de
 * ventanas del juicio (esta en 0x12D09A4 en este EBOOT) y muestrea el reloj de la cancion y el
 * bloque de audio, en el mismo formato que usan tools/song_sync.py y tools/audio_cut.py en Re:Fine.
 * No escribe en el proceso.
 *
 * Compilar:  clang-cl /O2 tools\rpcs3_watch.c /Fe:work\5c\rpcs3_watch.exe
 * Uso:       rpcs3_watch.exe <pid> <prefijo> <segundos desde que empieza la cancion> [s1,s2,...]
 *            Con la lista (6d.12), al pasar el reloj de la cancion por cada segundo manda F12 (la
 *            captura del propio RPCS3) con PostMessage a la ventana de juego del proceso.
 * Salida:    <prefijo>.watch  (como MEM_WATCH: indices 0 reloj y 1 bloque)
 *            <prefijo>.f32 y <prefijo>.f32.qpc  (bloques leidos del puerto, como AUDIO_WAV)
 *            en stdout, las ternas (bloque, posicion) halladas en el monton del juego.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define EA_WINDOWS 0x012D09A4u
#define EA_CLOCK   0x012CBE48u
#define EA_TAG     0x012D3228u
#define EA_PORTCFG 0x012D3788u

static HANDLE proc;
static uint8_t* base;

static int rd(uint32_t ea, void* out, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(proc, base + ea, out, n, &got) && got == n;
}
static uint32_t be32(const uint8_t* p) { return p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint32_t rd32(uint32_t ea) { uint8_t b[4] = {0}; rd(ea, b, 4); return be32(b); }

static int find_base(void)
{
    static const float win[8] = { 0.03f, -0.03f, 0.07f, -0.07f, 0.1f, -0.1f, 0.13f, -0.13f };
    uint8_t pat[32];
    for (int i = 0; i < 8; i++) {
        uint32_t v; memcpy(&v, &win[i], 4);
        pat[i * 4] = v >> 24; pat[i * 4 + 1] = v >> 16; pat[i * 4 + 2] = v >> 8; pat[i * 4 + 3] = v;
    }
    uint8_t* at = 0; MEMORY_BASIC_INFORMATION mi;
    static uint8_t page[0x10000];
    while (VirtualQueryEx(proc, at, &mi, sizeof mi)) {
        uint8_t* b = (uint8_t*)mi.BaseAddress;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            ((uintptr_t)b + mi.RegionSize > ((uintptr_t)b & ~(uintptr_t)0xFFFFFF) + EA_WINDOWS)) {
            /* La tabla solo puede estar en una pagina cuyo offset sea el de EA_WINDOWS */
            for (uint8_t* c = (uint8_t*)((uintptr_t)b & ~(uintptr_t)0xFFFFFF); c < b + mi.RegionSize; c += 0x1000000) {
                uint8_t* page_at = c + (EA_WINDOWS & 0xFFFF0000u & 0xFFFFFF);
                SIZE_T got = 0;
                if (page_at < b) continue;
                if (ReadProcessMemory(proc, page_at, page, sizeof page, &got) && got == sizeof page &&
                    !memcmp(page + (EA_WINDOWS & 0xFFFF), pat, 32)) {
                    base = page_at + (EA_WINDOWS & 0xFFFF) - EA_WINDOWS;
                    return 1;
                }
            }
        }
        at = b + mi.RegionSize;
        if (!mi.RegionSize) break;
    }
    return 0;
}

static void find_triples(uint64_t around)
{
    static uint8_t heap[0x4000000];
    for (uint32_t ea = 0x30000000u; ea < 0x34000000u; ea += 0x10000) {
        if (!rd(ea, heap + (ea - 0x30000000u), 0x10000)) memset(heap + (ea - 0x30000000u), 0, 0x10000);
    }
    for (size_t p = 0; p + 0xF0 < sizeof heap; p += 8) {
        uint64_t t = be64(heap + p);
        if (t + 64 < around || t > around + 64) continue;
        int ok = 1; uint64_t bt = 0; double bp = 0;
        for (int k = 0; k < 10 && ok; k++) {
            uint64_t tk = be64(heap + p + k * 0x18), st = be64(heap + p + k * 0x18 + 8), ps = be64(heap + p + k * 0x18 + 16);
            double d; memcpy(&d, &ps, 8);
            if (tk + 64 < around || tk > around + 64 || st < 1000000ull || !(d >= 0 && d < 1000)) ok = 0;
            else if (tk > bt) { bt = tk; bp = d; }
        }
        if (ok) { printf("terna 0x%08zX bloque %llu posicion %.5f\n", 0x30000000u + p, (unsigned long long)bt, bp); fflush(stdout); return; }
    }
    printf("sin ternas cerca de %llu\n", (unsigned long long)around); fflush(stdout);
}

static BOOL CALLBACK find_game(HWND h, LPARAM out)
{
    DWORD pid = 0; wchar_t t[8] = L"";
    GetWindowThreadProcessId(h, &pid);
    GetWindowTextW(h, t, 8);
    if (pid == GetProcessId(proc) && IsWindowVisible(h) && !wcsncmp(t, L"FPS:", 4)) { *(HWND*)out = h; return FALSE; }
    return TRUE;
}

static void shot(float song)
{
    HWND h = NULL;
    EnumWindows(find_game, (LPARAM)&h);
    if (!h) { printf("captura %.3f s: sin ventana\n", song); fflush(stdout); return; }
    PostMessageW(h, WM_KEYDOWN, VK_F12, 1 | (0x58 << 16));
    Sleep(60);
    PostMessageW(h, WM_KEYUP, VK_F12, (LPARAM)(0xC0000001u | (0x58u << 16)));
    printf("captura en %.3f s de cancion\n", song); fflush(stdout);
}

int main(int argc, char** argv)
{
    float at[64]; int nat = 0, iat = 0;
    if (argc > 4) for (char* s = argv[4]; *s && nat < 64; ) { at[nat++] = (float)strtod(s, &s); while (*s == ',') s++; }
    if (argc < 4) { fprintf(stderr, "uso: rpcs3_watch <pid> <prefijo> <segundos>\n"); return 2; }
    proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, (DWORD)atoi(argv[1]));
    if (!proc) { fprintf(stderr, "no abre el proceso\n"); return 1; }
    const double secs = atof(argv[3]);
    while (!find_base()) {
        DWORD c = 0;
        if (GetExitCodeProcess(proc, &c) && c != STILL_ACTIVE) return 1;
        Sleep(1000);
    }
    printf("base guest %p; ventanas iguales a Re:Fine\n", base); fflush(stdout);
    char path[512];
    snprintf(path, sizeof path, "%s.watch", argv[2]);
    FILE* w = fopen(path, "w");
    snprintf(path, sizeof path, "%s.f32", argv[2]);
    FILE* pcm = fopen(path, "wb");
    snprintf(path, sizeof path, "%s.f32.qpc", argv[2]);
    FILE* q = fopen(path, "wb");

    uint8_t cfg[32]; rd(EA_PORTCFG, cfg, 32);
    const uint32_t nch = (uint32_t)be64(cfg + 8), nblk = (uint32_t)be64(cfg + 16), port = be32(cfg + 28);
    printf("puerto 0x%08X, %u canales, %u bloques\n", port, nch, nblk); fflush(stdout);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    uint32_t last_clock = 0; uint64_t last_tag = 0, pending = 0; LONGLONG pending_q = 0;
    double t0 = 0; int triple_n = 0;
    uint64_t first = 0, emitted = 0;
    static uint8_t blk[256 * 8 * 4];
    static float out[512];
    for (;;) {
        LARGE_INTEGER n; QueryPerformanceCounter(&n);
        const double t = n.QuadPart / (double)f.QuadPart;
        uint8_t b[8] = {0};
        rd(EA_CLOCK, b, 4);
        uint32_t c = be32(b);
        rd(EA_TAG, b, 8);
        uint64_t tag = be64(b);
        if (pending) {
            /* El juego ya ha escrito el bloque: se copia y se marca con cuando aparecio el bloque. */
            rd(port + (uint32_t)(pending % nblk) * 256 * nch * 4, blk, 256 * nch * 4);
            for (int i = 0; i < 256; i++) {
                for (int ch = 0; ch < 2; ch++) {
                    uint32_t v = be32(blk + (i * nch + ch) * 4);
                    memcpy(&out[i * 2 + ch], &v, 4);
                }
            }
            if (!first) first = pending;
            while (first + emitted < pending) {   /* un hueco: se conserva el indice */
                fwrite(out, 4, 512, pcm); fwrite(&pending_q, 8, 1, q); emitted++;
            }
            fwrite(out, 4, 512, pcm); fwrite(&pending_q, 8, 1, q); emitted++;
            pending = 0;
        }
        if (tag != last_tag) {
            last_tag = tag;
            pending = tag; pending_q = n.QuadPart;
            fprintf(w, "%.6f 1 %016llx 0 0\n", t, (unsigned long long)tag);
        }
        if (c != last_clock) {
            last_clock = c;
            float fv; memcpy(&fv, &c, 4);
            fprintf(w, "%.6f 0 %08x00000000 0 0\n", t, c);
            if (!t0 && fv > 0 && fv < 2) {
                t0 = t;
                snprintf(path, sizeof path, "%s.clock", argv[2]);
                FILE* c0 = fopen(path, "w"); fputs("x", c0); fclose(c0);
            }
            if (t0 && iat < nat && fv >= at[iat]) { shot(fv); iat++; }
            if (t0 && triple_n < 3 && fv > 3.0f + 4.0f * triple_n) { find_triples(tag); triple_n++; }
        }
        if (t0 && t - t0 > secs) break;
        DWORD code = 0;
        if (GetExitCodeProcess(proc, &code) && code != STILL_ACTIVE) break;
        LARGE_INTEGER due; due.QuadPart = -5000;
        SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(timer, 10);
    }
    fclose(w); fclose(pcm); fclose(q);
    return 0;
}
