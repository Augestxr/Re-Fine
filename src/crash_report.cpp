// Re:Fine: what a crash leaves behind. Until now a host crash ended the process with nothing in the logs
// (2026-11-11: two of the author's runs died with the last lines of stderr only). This filter, installed by
// main() before the game starts, writes one line per fact to stderr (exception, address as module+RVA,
// the thread, a short host backtrace as RVAs) and a small minidump to %LOCALAPPDATA%\ReFine\crash\.
// RVAs resolve against build\ReFine.map (lld-link /MAP) with tools\crash_symbols.py.
// The guest VM's own faults never get here: vm_commit_veh (main.cpp) handles them first.
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>

static LPTOP_LEVEL_EXCEPTION_FILTER s_prev;
static volatile LONG s_once;

static void module_rva(const void* a, char* out, size_t n)
{
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, sizeof name);
        const char* b = strrchr(name, '\\');
        snprintf(out, n, "%s+0x%llX", b ? b + 1 : name, (unsigned long long)((const char*)a - (const char*)m));
    } else snprintf(out, n, "%p", a);
}

static void write_dump(EXCEPTION_POINTERS* ep)
{
    wchar_t dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) return;
    wcscat(dir, L"\\ReFine");
    CreateDirectoryW(dir, nullptr);
    wcscat(dir, L"\\crash");
    CreateDirectoryW(dir, nullptr);
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t path[MAX_PATH + 64];
    swprintf(path, MAX_PATH + 64, L"%s\\ReFine_%04d%02d%02d_%02d%02d%02d.dmp", dir, t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), ep, FALSE};
    // Threads, their stacks and the exception; not the 4 GB guest space (it would be huge and holds the game).
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                      (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), &mei, nullptr, nullptr);
    CloseHandle(f);
    fprintf(stderr, "[crash] minidump %s: %ls\n", ok ? "written" : "FAILED", path);
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS* ep)
{
    if (InterlockedExchange(&s_once, 1)) return s_prev ? s_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    char where[MAX_PATH + 32];
    module_rva(er->ExceptionAddress, where, sizeof where);
    char tname[64] = "";
    {   // the thread's name, when it has one (ps3_thread_name / SetThreadDescription)
        typedef HRESULT(WINAPI * GetDesc)(HANDLE, PWSTR*);
        if (auto gd = (GetDesc)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")) {
            PWSTR w = nullptr;
            if (SUCCEEDED(gd(GetCurrentThread(), &w)) && w) { WideCharToMultiByte(CP_UTF8, 0, w, -1, tname, sizeof tname, nullptr, nullptr); LocalFree(w); }
        }
    }
    fprintf(stderr, "[crash] exception 0x%08lX at %s, thread %lu '%s'\n", er->ExceptionCode, where, GetCurrentThreadId(), tname);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, "[crash] %s of address 0x%llX\n", er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "execute" : "read",
                (unsigned long long)er->ExceptionInformation[1]);
    // The faulting thread's stack from its own context (the filter runs on it).
    {
        CONTEXT c = *ep->ContextRecord;
        STACKFRAME64 sf = {};
        sf.AddrPC.Offset = c.Rip; sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
        HANDLE p = GetCurrentProcess();
        SymInitialize(p, nullptr, TRUE);
        for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, p, GetCurrentThread(), &sf, &c, nullptr,
                                                SymFunctionTableAccess64, SymGetModuleBase64, nullptr); i++) {
            if (!sf.AddrPC.Offset) break;
            module_rva((const void*)sf.AddrPC.Offset, where, sizeof where);
            fprintf(stderr, "[crash]   #%02d %s\n", i, where);
        }
    }
    fflush(stderr);
    write_dump(ep);
    fflush(stderr);
    return s_prev ? s_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

extern "C" void refine_crash_report_install(void)
{
    s_prev = SetUnhandledExceptionFilter(on_crash);
}
