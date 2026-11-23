/* audio_loopback.c - Graba lo que el motor de audio de Windows saca por cada salida activa.
 *
 * Es una herramienta de medida propia, no parte del port: WASAPI en modo loopback da las
 * muestras tal como salen del mezclador de Windows, con el QPC del primer frame de cada
 * paquete. Con eso se mide el corte de un cambio de dispositivo (5c.1) y lo que tarda un
 * bloque en llegar a la salida (5c.3). No mide el camino del driver al altavoz.
 *
 * Compilar:  clang-cl /O2 tools\audio_loopback.c /Fe:work\5c\audio_loopback.exe ole32.lib
 * Uso:       audio_loopback.exe <prefijo> <segundos>
 * Salida:    <prefijo>_<n>.f32 (float, canales del dispositivo), <prefijo>_<n>.tsv (qpc_s y
 *            frames de cada paquete), y una linea por salida en stdout.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>

DEFINE_GUID(MY_CLSID_Enum, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(MY_IID_Enum, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(MY_IID_Client, 0x1CB9AD4C, 0xDBFA, 0x4c32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(MY_IID_Capture, 0xC8ADBD64, 0xE71E, 0x48a0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);

typedef struct { IMMDevice* dev; int n; double secs; const char* prefix; } Job;

static unsigned __stdcall capture(void* arg)
{
    Job* j = (Job*)arg;
    IAudioClient* c = NULL; IAudioCaptureClient* cap = NULL; WAVEFORMATEX* fmt = NULL;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(IMMDevice_Activate(j->dev, &MY_IID_Client, CLSCTX_ALL, NULL, (void**)&c))) return 1;
    IAudioClient_GetMixFormat(c, &fmt);
    if (FAILED(IAudioClient_Initialize(c, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                       2000000, 0, fmt, NULL))) return 1;
    IAudioClient_GetService(c, &MY_IID_Capture, (void**)&cap);
    char path[512];
    snprintf(path, sizeof path, "%s_%d.f32", j->prefix, j->n);
    FILE* f = fopen(path, "wb");
    snprintf(path, sizeof path, "%s_%d.tsv", j->prefix, j->n);
    FILE* t = fopen(path, "w");
    fprintf(t, "# rate %u channels %u bits %u\n", (unsigned)fmt->nSamplesPerSec, fmt->nChannels, fmt->wBitsPerSample);
    IAudioClient_Start(c);
    ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < (ULONGLONG)(j->secs * 1000)) {
        UINT32 pk = 0;
        IAudioCaptureClient_GetNextPacketSize(cap, &pk);
        if (!pk) { Sleep(2); continue; }
        BYTE* data; UINT32 frames; DWORD flags; UINT64 pos, qpc;
        if (FAILED(IAudioCaptureClient_GetBuffer(cap, &data, &frames, &flags, &pos, &qpc))) break;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            static const float zero[4096];
            size_t left = frames * fmt->nBlockAlign;
            while (left) { size_t n = left < sizeof zero ? left : sizeof zero; fwrite(zero, 1, n, f); left -= n; }
        } else {
            fwrite(data, fmt->nBlockAlign, frames, f);
        }
        fprintf(t, "%.7f\t%u\t%u\n", qpc / 1e7, frames, (unsigned)flags);
        IAudioCaptureClient_ReleaseBuffer(cap, frames);
    }
    IAudioClient_Stop(c);
    fclose(f); fclose(t);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "uso: audio_loopback <prefijo> <segundos>\n"); return 2; }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = NULL; IMMDeviceCollection* all = NULL; UINT n = 0;
    if (FAILED(CoCreateInstance(&MY_CLSID_Enum, NULL, CLSCTX_ALL, &MY_IID_Enum, (void**)&en))) return 1;
    IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &all);
    IMMDeviceCollection_GetCount(all, &n);
    static Job jobs[16]; static HANDLE th[16];
    if (n > 16) n = 16;
    for (UINT i = 0; i < n; i++) {
        jobs[i].n = (int)i; jobs[i].secs = atof(argv[2]); jobs[i].prefix = argv[1];
        IMMDeviceCollection_Item(all, i, &jobs[i].dev);
        static const PROPERTYKEY friendly =
            {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14};
        IPropertyStore* st = NULL; PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(jobs[i].dev, STGM_READ, &st)) &&
            SUCCEEDED(IPropertyStore_GetValue(st, &friendly, &v)) && v.vt == VT_LPWSTR)
            printf("%u\t%ls\n", i, v.pwszVal);
        th[i] = (HANDLE)_beginthreadex(NULL, 0, capture, &jobs[i], 0, NULL);
    }
    WaitForMultipleObjects(n, th, TRUE, INFINITE);
    return 0;
}
