// Re:Fine installer (hito 8, part 1). One small exe: Win32 window + D3D12 + RmlUi, the same renderer
// and palette as the start screen. It carries nothing of the game; it checks the user's own copy,
// asks the user's own RPCS3 to open the encrypted EBOOT, and builds the game on this PC.
//
// Part 2: "Building the game" runs payload\build_game.ps1 on this PC: it finds or installs the tools
// (LLVM, Python, Microsoft C++ Build Tools; downloads only after the user agrees on the tools screen),
// lifts the user's own EBOOT.elf and compiles ReFine.exe. The payload carries no ReFine.exe.
//
// Test options (nothing here touches the real install):
//   --sandbox <dir>   install into <dir>\app, data into <dir>\data, shortcuts into <dir>\Desktop and
//                     <dir>\Start; no registry; Play is disabled
//   --payload <dir>   the files to install (default: <exe dir>\payload)
//   --game/--rpcs3/--save <dir>   preselect (the folder dialogs cannot be scripted)
//   --script a,b,c    click those buttons one after another (1.2 s apart, waits for the install)
//   --uninstall       remove (reads uninstall.dat from the install dir; runs from a copy in %TEMP%)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <RmlUi/Core.h>
#include "../../../src/port_menu/rml_d3d12.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" const char* ppu_vfs_root = nullptr;   // rml_d3d12.cpp: no disc here, image sources are plain paths

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------- strings
static std::string u8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string esc(const std::string& s)
{
    std::string o;
    for (char c : s) { if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else o += c; }
    return o;
}

// ---------------------------------------------------------------------------- options
struct Opts {
    std::wstring sandbox, payload, game, rpcs3, save, script, uninstallRun;
    bool uninstall = false;
} g_o;

static std::wstring g_exeDir, g_exePath;
static std::wstring g_install, g_data, g_desktop, g_start;   // chosen once at startup
static bool g_sandbox = false;

static std::wstring env(const wchar_t* n)
{
    wchar_t b[1024]; DWORD k = GetEnvironmentVariableW(n, b, 1024);
    return k && k < 1024 ? std::wstring(b, k) : std::wstring();
}
static std::wstring known(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
static void init_paths()
{
    if (g_sandbox) {
        g_install = g_o.sandbox + L"\\app"; g_data = g_o.sandbox + L"\\data";
        g_desktop = g_o.sandbox + L"\\Desktop"; g_start = g_o.sandbox + L"\\Start\\ReFine";
    } else {
        g_install = known(FOLDERID_LocalAppData) + L"\\Programs\\ReFine";
        g_data = known(FOLDERID_LocalAppData) + L"\\ReFine";   // refine.cmd and the game use this one
        g_desktop = known(FOLDERID_Desktop);
        g_start = known(FOLDERID_Programs) + L"\\ReFine";
    }
}

// ---------------------------------------------------------------------------- PARAM.SFO (title id and version only)
struct Sfo { std::string title_id, app_ver; bool ok = false; };
static Sfo read_sfo(const fs::path& p)
{
    Sfo r;
    std::ifstream f(p, std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
    if (b.size() < 20 || memcmp(b.data(), "\0PSF", 4) != 0) return r;
    auto u16 = [&](size_t o) { return (unsigned)(b[o] | b[o + 1] << 8); };
    auto u32 = [&](size_t o) { return (unsigned)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (unsigned)b[o + 3] << 24); };
    const unsigned kt = u32(8), dt = u32(12), n = u32(16);
    for (unsigned i = 0; i < n && 20 + i * 16 + 16 <= b.size(); i++) {
        const size_t e = 20 + i * 16;
        const size_t ko = kt + u16(e), dd = dt + u32(e + 12);
        if (ko >= b.size() || dd >= b.size()) continue;
        std::string key((const char*)&b[ko]);
        std::string val((const char*)&b[dd], strnlen((const char*)&b[dd], b.size() - dd));
        if (key == "TITLE_ID") r.title_id = val;
        if (key == "APP_VER") r.app_ver = val;
    }
    r.ok = !r.title_id.empty();
    return r;
}
static bool is_elf(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary); char m[4] = {};
    f.read(m, 4);
    return f.gcount() == 4 && !memcmp(m, "\x7f" "ELF", 4);
}

// ---------------------------------------------------------------------------- model
enum Screen { W_WELCOME, W_GAME, W_RPCS3, W_SAVE, W_TOOLS, W_INSTALL, W_DONE, U_ASK, U_DATA, U_DONE };
struct Model {
    Screen scr = W_WELCOME;
    fs::path game;            // folder that contains PS3_GAME
    bool gameOk = false, needDecrypt = false;
    std::string gameMsg;
    fs::path rpcs3; bool rpcs3Ok = false; std::string rpcs3Msg;
    fs::path save; bool saveOk = false; std::string saveMsg; bool savePrivate = false;
    bool privateBuild = false;
    std::wstring createdElf;
    std::vector<std::string> toolsDl;   // "name|MB|source" for each missing tool (build_game.ps1 -Check)
    bool toolsAgreed = false;
    bool deleteData = false;
} M;

static const char* kTitle = "BLUS31319";

// Result: is `dir` (or its parent / its PS3_GAME) the right disc folder?
static void check_game(const fs::path& chosen)
{
    M.gameOk = false; M.needDecrypt = false; M.gameMsg.clear();
    fs::path base = chosen;
    std::error_code ec;
    if (fs::exists(base / "PS3_GAME" / "PARAM.SFO", ec)) {}
    else if (fs::exists(base / "PARAM.SFO", ec) && fs::exists(base / "USRDIR", ec)) base = base.parent_path();   // the user picked PS3_GAME itself
    else { M.gameMsg = "This folder has no PS3_GAME inside."; M.game = chosen; return; }
    M.game = base;
    const Sfo s = read_sfo(base / "PS3_GAME" / "PARAM.SFO");
    if (!s.ok) { M.gameMsg = "PARAM.SFO could not be read."; return; }
    if (s.title_id != kTitle) { M.gameMsg = "This is " + s.title_id + ". Re:Fine needs BLUS31319 (USA)."; return; }
    if (s.app_ver != "01.00") { M.gameMsg = "This is version " + s.app_ver + ". Re:Fine needs 01.00, the disc without updates."; return; }
    const fs::path usr = base / "PS3_GAME" / "USRDIR";
    if (fs::exists(usr / "EBOOT.elf", ec) && is_elf(usr / "EBOOT.elf")) { M.gameOk = true; M.gameMsg = "\xE2\x9C\x93 BLUS31319 v01.00"; return; }
    if (fs::exists(usr / "EBOOT.BIN", ec)) { M.gameOk = true; M.needDecrypt = true; M.gameMsg = "\xE2\x9C\x93 BLUS31319 v01.00. The game file is encrypted: the next step opens it."; return; }
    M.gameMsg = "EBOOT.BIN is missing from PS3_GAME\\USRDIR.";
}
static void check_rpcs3(const fs::path& dir)
{
    std::error_code ec;
    M.rpcs3 = dir;
    M.rpcs3Ok = fs::exists(dir / "rpcs3.exe", ec);
    M.rpcs3Msg = M.rpcs3Ok ? "\xE2\x9C\x93 RPCS3 found" : "rpcs3.exe is not in this folder.";
}
static void check_save(const fs::path& dir)
{
    std::error_code ec;
    M.save = dir; M.saveOk = false;
    const std::string n = u8(dir.filename().wstring());
    if (n.rfind("NPUB31241", 0) != 0) { M.saveMsg = "This folder is not named NPUB31241_...; it is not a save of this game."; return; }
    if (!fs::exists(dir / "PARAM.SFO", ec)) { M.saveMsg = "PARAM.SFO is missing: it is not a PS3 save."; return; }
    M.saveOk = true; M.saveMsg = "\xE2\x9C\x93 " + n;
}
// RPCS3's own save folder, found from its install folder (dev_hdd0\home\<user>\savedata).
static fs::path find_rpcs3_save(const fs::path& rpcs3)
{
    std::error_code ec;
    const fs::path home = rpcs3 / "dev_hdd0" / "home";
    if (!fs::exists(home, ec)) return {};
    for (auto& u : fs::directory_iterator(home, ec)) {
        const fs::path sd = u.path() / "savedata";
        if (!fs::exists(sd, ec)) continue;
        for (auto& s : fs::directory_iterator(sd, ec))
            if (s.is_directory() && u8(s.path().filename().wstring()).rfind("NPUB31241", 0) == 0) return s.path();
    }
    return {};
}

// ---------------------------------------------------------------------------- the install (worker thread)
struct Work {
    std::atomic<int> step{0};          // 0..3 = the step running
    std::atomic<float> frac{0};        // 0..1 overall
    std::atomic<int> state{0};         // 0 idle, 1 running, 2 ok, -1 failed
    std::mutex mu; std::string text, error;
    void say(const std::string& t) { std::lock_guard<std::mutex> l(mu); text = t; }
    void fail(const std::string& e) { std::lock_guard<std::mutex> l(mu); error = e; state = -1; }
} W;

static uint64_t tree_size(const fs::path& p)
{
    uint64_t t = 0; std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(p, ec)) if (e.is_regular_file(ec)) t += e.file_size(ec);
    return t;
}
// Copy a tree; frac advances from f0 to f1 with the bytes done.
static void copy_tree(const fs::path& from, const fs::path& to, float f0, float f1, const std::vector<std::string>& skip = {})
{
    const uint64_t total = tree_size(from) ? tree_size(from) : 1; uint64_t done = 0;
    for (auto& e : fs::recursive_directory_iterator(from)) {
        const fs::path rel = fs::relative(e.path(), from), dst = to / rel;
        const std::string top = u8(rel.begin()->wstring());
        if (std::find(skip.begin(), skip.end(), top) != skip.end()) continue;
        if (e.is_directory()) { fs::create_directories(dst); continue; }
        fs::create_directories(dst.parent_path());
        fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing);
        done += e.file_size();
        W.frac = f0 + (f1 - f0) * (float)((double)done / (double)total);
    }
}
static bool run_rpcs3_decrypt(const fs::path& rpcs3, const fs::path& bin, std::string& err)
{
    std::wstring cmd = L"\"" + (rpcs3 / "rpcs3.exe").wstring() + L"\" --headless --decrypt \"" + bin.wstring() + L"\"";
    STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, rpcs3.c_str(), &si, &pi)) { err = "RPCS3 could not be started."; return false; }
    const DWORD r = WaitForSingleObject(pi.hProcess, 180000);   // a blocking wait, not a poll
    if (r != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 1); err = "RPCS3 did not finish in 3 minutes."; }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return r == WAIT_OBJECT_0;
}
// Runs a PowerShell script with its stdout piped; every line goes to onLine (blocking reads, no polling).
static int run_script(const fs::path& script, const std::wstring& args, const std::function<void(const std::string&)>& onLine)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE}; HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof si}; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" " + args;
    std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(rd); CloseHandle(wr); return -1; }
    CloseHandle(wr);
    std::string buf; char tmp[4096]; DWORD n;
    while (ReadFile(rd, tmp, sizeof tmp, &n, nullptr) && n) {
        buf.append(tmp, n);
        size_t p;
        while ((p = buf.find('\n')) != std::string::npos) {
            std::string l = buf.substr(0, p); buf.erase(0, p + 1);
            while (!l.empty() && (l.back() == '\r')) l.pop_back();
            if (!l.empty()) onLine(l);
        }
    }
    if (!buf.empty()) onLine(buf);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    return (int)code;
}
static fs::path payload_dir() { return g_o.payload.empty() ? fs::path(g_exeDir) / "payload" : fs::path(g_o.payload); }
// What is missing on this PC (nothing = nothing to download).
// Private build: the payload carries the finished ReFine.exe, so nothing is downloaded or compiled.
static bool prebuilt() { std::error_code ec; return fs::exists(payload_dir() / "ReFine.exe", ec); }
static void check_tools()
{
    M.toolsDl.clear(); M.toolsAgreed = false;
    if (prebuilt()) return;
    run_script(payload_dir() / "build_game.ps1", L"-Check", [&](const std::string& l) { if (l.rfind("dl=", 0) == 0) M.toolsDl.push_back(l.substr(3)); });
}
static void make_link(const fs::path& lnk, const fs::path& target, const std::wstring& args, const fs::path& wd, const fs::path& icon, int show)
{
    ComPtr<IShellLinkW> l;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l)))) throw std::runtime_error("shortcut");
    l->SetPath(target.c_str()); l->SetArguments(args.c_str()); l->SetWorkingDirectory(wd.c_str());
    l->SetIconLocation(icon.c_str(), 0); l->SetShowCmd(show);
    ComPtr<IPersistFile> pf; l.As(&pf);
    fs::create_directories(lnk.parent_path());
    if (FAILED(pf->Save(lnk.c_str(), TRUE))) throw std::runtime_error("shortcut");
}
static const wchar_t* kRegKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ReFine";

static void install_thread()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        const fs::path payload = g_o.payload.empty() ? fs::path(g_exeDir) / "payload" : fs::path(g_o.payload);
        const fs::path usr = M.game / "PS3_GAME" / "USRDIR";
        fs::path elf = usr / "EBOOT.elf";
        // 0 - check
        W.step = 0; W.say("Checking your files"); W.frac = 0.02f;
        check_game(M.game);
        if (!M.gameOk) { W.fail(M.gameMsg); return; }
        const bool pre = prebuilt();
        if (pre ? !fs::exists(payload / "ui" / "menu.rml") : (!fs::exists(payload / "build_game.ps1") || !fs::exists(payload / "src" / "CMakeLists.txt"))) { W.fail("The installer files are incomplete (sources are missing)."); return; }
        W.frac = 0.08f;
        // 1 - prepare: open the game file, import the save
        W.step = 1; W.say("Preparing");
        fs::create_directories(g_data); fs::create_directories(g_install);
        if (M.needDecrypt) {
            W.say("Opening the game file with your RPCS3");
            const fs::path tmp = fs::path(g_data) / "decrypt";
            fs::create_directories(tmp);
            fs::copy_file(usr / "EBOOT.BIN", tmp / "EBOOT.BIN", fs::copy_options::overwrite_existing);
            std::error_code ec; fs::remove(tmp / "EBOOT.elf", ec);
            std::string err;
            if (!run_rpcs3_decrypt(M.rpcs3, tmp / "EBOOT.BIN", err)) { W.fail(err); return; }
            if (!fs::exists(tmp / "EBOOT.elf") || !is_elf(tmp / "EBOOT.elf")) { W.fail("RPCS3 did not produce EBOOT.elf. Open the game once in RPCS3 and try again."); return; }
            elf = tmp / "EBOOT.elf";   // never written into the game folder (it may be read-only)
        }
        W.frac = 0.25f;
        if (M.saveOk) {
            W.say("Importing your save");
            const fs::path sd = fs::path(g_data) / "hdd0" / "home" / "00000001" / "savedata";
            fs::create_directories(sd);
            const fs::path dst = sd / M.save.filename();
            std::error_code ec;
            if (fs::exists(dst, ec)) {   // never overwrite a save: the old one is kept next to it
                SYSTEMTIME t; GetLocalTime(&t); wchar_t b[64];
                swprintf(b, 64, L".old-%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                fs::rename(dst, fs::path(dst.wstring() + b));
            }
            copy_tree(M.save, dst, 0.25f, 0.30f);
        }
        W.frac = 0.30f;
        // 2 - the game itself
        W.step = 2; W.say(pre ? "Preparing the game" : "Building the game");
        if (pre) {
            fs::create_directories(g_data);
            fs::copy_file(elf, fs::path(g_data) / "EBOOT.elf", fs::copy_options::overwrite_existing);   // the game reads its own code file at start
            std::error_code ec; fs::remove_all(fs::path(g_data) / "decrypt", ec);
            W.frac = 0.90f;
        } else {
            fs::create_directories(fs::path(g_data) / "logs");
            std::ofstream blog(fs::path(g_data) / "logs" / "install_build.log", std::ios::binary);
            std::string err;
            std::wstring a = L"-Src \"" + (payload / "src").wstring() + L"\" -Root \"" + (fs::path(g_data) / "build").wstring() + L"\" -Elf \"" + elf.wstring()
                + L"\" -Out \"" + g_install + L"\" -Tools \"" + (fs::path(g_data) / "tools").wstring() + L"\"" + (M.toolsAgreed ? L" -Download" : L"");
            const int rc = run_script(payload / "build_game.ps1", a, [&](const std::string& l) {
                blog << l << "\n"; blog.flush();
                if (l.rfind("@@ ", 0) == 0) {
                    const double f = atof(l.c_str() + 3); const size_t sp = l.find(' ', 3);
                    W.frac = 0.30f + 0.60f * (float)f; if (sp != std::string::npos) W.say(l.substr(sp + 1));
                } else if (l.rfind("ERR: ", 0) == 0) err = l.substr(5);
            });
            if (rc != 0) { W.fail(err.empty() ? "The game could not be built (code " + std::to_string(rc) + "). See logs\\install_build.log in " + u8(g_data) + "." : err); return; }
            fs::copy_file(elf, fs::path(g_data) / "EBOOT.elf", fs::copy_options::overwrite_existing);   // the game reads its own code file at start
            std::error_code ec; fs::remove_all(fs::path(g_data) / "decrypt", ec);
        }
        copy_tree(payload, g_install, 0.90f, 0.91f, {"src", "build_game.ps1", "partida"});
        W.say("Copying the uninstaller");
        fs::copy_file(g_exePath, fs::path(g_install) / "ReFineSetup.exe", fs::copy_options::overwrite_existing);
        copy_tree(fs::path(g_exeDir) / "ui", fs::path(g_install) / "ui", 0.91f, 0.93f);
        // 3 - shortcuts, game location, uninstall data
        W.step = 3; W.say("Creating shortcuts");
        { std::ofstream f(fs::path(g_data) / "ruta_juego.txt", std::ios::binary); f << u8(M.game.wstring()) << "\r\n"; }
        if (!fs::exists(fs::path(g_data) / "refine.ini") && fs::exists(payload / "refine.ini"))
            fs::copy_file(payload / "refine.ini", fs::path(g_data) / "refine.ini");
        const fs::path cmd = fs::path(g_install) / "refine.cmd", icon = fs::path(g_install) / "ReFine.exe";
        make_link(fs::path(g_desktop) / "ReFine.lnk", cmd, L"", g_install, icon, SW_SHOWMINNOACTIVE);
        make_link(fs::path(g_start) / "ReFine.lnk", cmd, L"", g_install, icon, SW_SHOWMINNOACTIVE);
        make_link(fs::path(g_start) / "Uninstall ReFine.lnk", fs::path(g_install) / "ReFineSetup.exe", L"--uninstall" + std::wstring(g_sandbox ? L" --sandbox \"" + g_o.sandbox + L"\"" : L""), g_install, icon, SW_SHOWNORMAL);
        {
            std::ofstream f(fs::path(g_install) / "uninstall.dat", std::ios::binary);
            f << "install=" << u8(g_install) << "\ndata=" << u8(g_data) << "\ndesktop=" << u8(g_desktop) << "\nstart=" << u8(g_start)
              << "\nelf=" << u8(M.createdElf) << "\nregistry=" << (g_sandbox ? 0 : 1) << "\n";
        }
        if (!g_sandbox) {
            HKEY k;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
                auto set = [&](const wchar_t* n, const std::wstring& v) { RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2)); };
                set(L"DisplayName", L"Re:Fine"); set(L"DisplayVersion", L"1.0.0-rc1"); set(L"Publisher", L"Re:Fine");
                set(L"InstallLocation", g_install); set(L"DisplayIcon", (fs::path(g_install) / "ReFine.exe").wstring());
                set(L"UninstallString", L"\"" + (fs::path(g_install) / "ReFineSetup.exe").wstring() + L"\" --uninstall");
                DWORD one = 1; RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, 4);
                RegCloseKey(k);
            }
        }
        W.frac = 1.0f; W.say("Done");
        W.state = 2;
    } catch (const std::exception& e) {
        W.fail(std::string("Something went wrong: ") + e.what());
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------------- uninstall
static std::string manifest_get(const std::vector<std::string>& lines, const char* key)
{
    const std::string k = std::string(key) + "=";
    for (auto& l : lines) if (l.rfind(k, 0) == 0) { std::string v = l.substr(k.size()); while (!v.empty() && (v.back() == '\r')) v.pop_back(); return v; }
    return {};
}
static std::vector<std::string> g_manifest;
static void load_manifest(const fs::path& installDir)
{
    std::ifstream f(installDir / "uninstall.dat"); std::string l;
    while (std::getline(f, l)) g_manifest.push_back(l);
}
static std::string do_uninstall(bool deleteData)
{
    std::error_code ec;
    const fs::path install = wide(manifest_get(g_manifest, "install")), data = wide(manifest_get(g_manifest, "data"));
    const fs::path desktop = wide(manifest_get(g_manifest, "desktop")), start = wide(manifest_get(g_manifest, "start"));
    const fs::path elf = wide(manifest_get(g_manifest, "elf"));
    fs::remove(desktop / "ReFine.lnk", ec);
    fs::remove_all(start, ec);
    if (!elf.empty()) fs::remove(elf, ec);   // the EBOOT.elf the installer made in the user's folder
    fs::remove_all(install, ec);
    if (ec) return "Some files could not be removed: " + ec.message();
    if (manifest_get(g_manifest, "registry") == "1") RegDeleteKeyW(HKEY_CURRENT_USER, kRegKey);
    if (deleteData) {
        // Only what Re:Fine made: never a folder that is not named ReFine (a mistyped manifest must not delete anything else)
        if (data.filename() == "ReFine" || data.filename() == "data") fs::remove_all(data, ec);
    }
    return {};
}

// ---------------------------------------------------------------------------- D3D12 host
static HWND g_hwnd;
static const UINT kW = 1280, kH = 720;
struct Gfx {
    ComPtr<ID3D12Device> dev; ComPtr<ID3D12CommandQueue> q; ComPtr<IDXGISwapChain3> sc;
    ComPtr<ID3D12DescriptorHeap> rtvHeap; ComPtr<ID3D12Resource> bb[2];
    ComPtr<ID3D12CommandAllocator> al[2]; ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence; HANDLE ev = nullptr; UINT64 fv[2] = {}, last = 0; UINT rtvSize = 0;
} G;

static bool gfx_init()
{
    ComPtr<IDXGIFactory4> fac;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) return false;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&G.dev)))) return false;
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(G.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&G.q)))) return false;
    DXGI_SWAP_CHAIN_DESC1 sd = {}; sd.Width = kW; sd.Height = kH; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(fac->CreateSwapChainForHwnd(G.q.Get(), g_hwnd, &sd, nullptr, nullptr, &sc1))) return false;
    fac->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&G.sc);
    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.NumDescriptors = 2; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(G.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&G.rtvHeap)))) return false;
    G.rtvSize = G.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = G.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; i++) {
        G.sc->GetBuffer(i, IID_PPV_ARGS(&G.bb[i]));
        G.dev->CreateRenderTargetView(G.bb[i].Get(), nullptr, h);
        h.ptr += G.rtvSize;
        if (FAILED(G.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&G.al[i])))) return false;
    }
    if (FAILED(G.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, G.al[0].Get(), nullptr, IID_PPV_ARGS(&G.cl)))) return false;
    G.cl->Close();
    if (FAILED(G.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&G.fence)))) return false;
    G.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}
static void gfx_wait(UINT64 v)
{
    if (G.fence->GetCompletedValue() < v) { G.fence->SetEventOnCompletion(v, G.ev); WaitForSingleObject(G.ev, INFINITE); }
}
static RmlD3D12 g_render;
static Rml::Context* g_ctx;
static Rml::ElementDocument* g_doc;

static void gfx_frame()
{
    const UINT fi = G.sc->GetCurrentBackBufferIndex();
    gfx_wait(G.fv[fi]);
    G.al[fi]->Reset();
    G.cl->Reset(G.al[fi].Get(), nullptr);
    D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = G.bb[fi].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    G.cl->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE h = G.rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)fi * G.rtvSize;
    const float clear[4] = {0.043f, 0.047f, 0.063f, 1};
    G.cl->ClearRenderTargetView(h, clear, 0, nullptr);
    g_render.BeginFrame(G.cl.Get(), h.ptr, kW, kH, fi);
    g_ctx->Render();
    g_render.EndFrame();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    G.cl->ResourceBarrier(1, &b);
    G.cl->Close();
    ID3D12CommandList* l[] = {G.cl.Get()};
    G.q->ExecuteCommandLists(1, l);
    G.sc->Present(1, 0);
    G.fv[fi] = ++G.last;
    G.q->Signal(G.fence.Get(), G.fv[fi]);
}

// ---------------------------------------------------------------------------- RmlUi glue
class Sys final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
    bool LogMessage(Rml::Log::Type t, const Rml::String& m) override { if (t <= Rml::Log::LT_WARNING) fprintf(stderr, "[rml] %s\n", m.c_str()); return true; }
} g_sys;

static Rml::Element* E(const char* id) { return g_doc->GetElementById(id); }
// Setting the same text again every frame rebuilds the element and blanks it: only write on change.
static std::map<std::string, std::string> g_txt;
static void text(const char* id, const std::string& s)
{
    auto it = g_txt.find(id);
    if (it != g_txt.end() && it->second == s) return;
    if (auto* e = E(id)) { e->SetInnerRML(esc(s)); g_txt[id] = s; }
}
static void cls(const char* id, const char* c, bool on) { if (auto* e = E(id)) e->SetClass(c, on); }
static void status(const char* id, const std::string& s, int tone)   // 1 ok, -1 bad, 0 neutral
{
    text(id, s);
    if (auto* e = E(id)) { e->SetClass("ok", tone > 0); e->SetClass("bad", tone < 0); }
}
static std::string short_path(const fs::path& p)
{
    std::string s = u8(p.wstring());
    return s.size() > 60 ? "..." + s.substr(s.size() - 57) : s;
}

static std::vector<Screen> flow()
{
    std::vector<Screen> f = {W_WELCOME, W_GAME};
    if (M.needDecrypt) f.push_back(W_RPCS3);
    f.push_back(W_SAVE); if (!M.toolsDl.empty()) f.push_back(W_TOOLS); f.push_back(W_INSTALL); f.push_back(W_DONE);
    return f;
}
static Screen step_of(int d)
{
    auto f = flow(); size_t i = 0;
    for (; i < f.size(); i++) if (f[i] == M.scr) break;
    i = (size_t)std::max(0, std::min((int)f.size() - 1, (int)i + d));
    return f[i];
}

static void show(Screen s)
{
    M.scr = s;
    static const char* ids[] = {"s_welcome", "s_game", "s_rpcs3", "s_save", "s_tools", "s_install", "s_done", "s_unask", "s_undata", "s_done"};
    for (int i = 0; i < 10; i++) cls(ids[i], "shown", !strcmp(ids[i], ids[(int)s]));
    auto btn = [&](const char* id, const char* label, bool vis, bool enabled, const char* extra = nullptr) {
        if (auto* e = E(id)) {
            e->SetInnerRML(label ? esc(label) : "");
            e->SetClass("hidden", !vis); e->SetClass("off", vis && !enabled);
            if (extra) e->SetClass("danger", !strcmp(extra, "danger")); else e->SetClass("danger", false);
        }
    };
    if (s >= U_ASK) { text("stepno", ""); }
    else { auto f = flow(); size_t i = 0; for (; i < f.size(); i++) if (f[i] == s) break; text("stepno", "Step " + std::to_string(i + 1) + " of " + std::to_string(f.size())); }
    switch (s) {
    case W_WELCOME: btn("btn_back", "", false, false); btn("btn_next", "Next", true, true); btn("btn_alt", "Close", true, true); break;
    case W_GAME: {
        btn("btn_back", "Back", true, true); btn("btn_next", "Next", true, M.gameOk); btn("btn_alt", "", false, false);
        text("game_path", M.game.empty() ? "No folder chosen" : short_path(M.game)); cls("game_path", "set", !M.game.empty());
        status("game_status", M.gameMsg, M.gameOk ? 1 : (M.gameMsg.empty() ? 0 : -1)); break; }
    case W_RPCS3:
        btn("btn_back", "Back", true, true); btn("btn_next", "Next", true, M.rpcs3Ok); btn("btn_alt", "", false, false);
        text("rpcs3_path", M.rpcs3.empty() ? "No folder chosen" : short_path(M.rpcs3)); cls("rpcs3_path", "set", !M.rpcs3.empty());
        status("rpcs3_status", M.rpcs3Msg, M.rpcs3Ok ? 1 : (M.rpcs3Msg.empty() ? 0 : -1)); break;
    case W_SAVE:
        btn("btn_back", "Back", true, true); btn("btn_next", M.saveOk ? "Next" : "Skip", true, true); btn("btn_alt", "", false, false);
        text("save_path", M.save.empty() ? "No save selected" : short_path(M.save)); cls("save_path", "set", !M.save.empty());
        status("save_status", M.saveMsg, M.saveOk ? 1 : (M.saveMsg.empty() ? 0 : -1));
        text("save_note", M.savePrivate ? "This installer includes a save. It will be installed unless you change it." : "Skip this step to start a new game. An existing save is never overwritten.");
        break;
    case W_TOOLS: {
        btn("btn_back", "Back", true, true); btn("btn_next", "Download and continue", true, true); btn("btn_alt", "", false, false);
        std::string l;
        for (auto& d : M.toolsDl) {
            const size_t a = d.find('|'), b = d.find('|', a + 1);
            const std::string mb = d.substr(a + 1, b - a - 1);
            l += esc(d.substr(0, a) + " - " + (mb == "3000" ? "about 3 GB" : mb + " MB") + " from " + d.substr(b + 1)) + "<br/>";
        }
        if (auto* e = E("tools_list")) e->SetInnerRML(l);
        break; }
    case W_INSTALL: btn("btn_back", "", false, false); btn("btn_next", "", false, false); btn("btn_alt", "", false, false); break;
    case W_DONE:
        btn("btn_back", "", false, false); btn("btn_next", g_sandbox ? "Play (off in test)" : "Play", true, !g_sandbox); btn("btn_alt", "Close", true, true);
        text("done_title", "Ready to play");
        text("done_text", "Re:Fine is installed. Use the shortcut on your desktop or in the Start menu, or press Play.");
        break;
    case U_ASK: btn("btn_back", "", false, false); btn("btn_next", "Uninstall", true, true, "danger"); btn("btn_alt", "Cancel", true, true); break;
    case U_DATA:
        btn("btn_back", "", false, false); btn("btn_next", "Delete them too", true, true, "danger"); btn("btn_alt", "Keep my saves", true, true);
        text("data_path", manifest_get(g_manifest, "data")); break;
    case U_DONE:
        btn("btn_back", "", false, false); btn("btn_next", "", false, false); btn("btn_alt", "Close", true, true);
        text("done_title", "Re:Fine was removed"); break;
    }
}

static bool g_quit = false;
static void pick_folder(const wchar_t* title, fs::path& out, bool& chosen)
{
    ComPtr<IFileOpenDialog> d;
    chosen = false;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(title);
    if (FAILED(d->Show(g_hwnd))) return;
    ComPtr<IShellItem> it; PWSTR p = nullptr;
    if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; chosen = true; }
    CoTaskMemFree(p);
}

static void start_install()
{
    show(W_INSTALL);
    text("inst_title", "Installing"); status("inst_status", "", 0);
    W.step = 0; W.frac = 0; W.state = 1;
    std::thread(install_thread).detach();
}

static void on_click(const std::string& id)
{
    auto enabled = [&](const char* b) { auto* e = E(b); return e && !e->IsClassSet("off") && !e->IsClassSet("hidden"); };
    if (!enabled(id.c_str()) && id.rfind("btn_", 0) == 0 && id != "btn_game" && id != "btn_rpcs3" && id != "btn_save" && id != "btn_web") return;
    bool ch; fs::path p;
    if (id == "btn_game") { pick_folder(L"Choose the folder that contains PS3_GAME", p, ch); if (ch) check_game(p); show(M.scr); return; }
    if (id == "btn_rpcs3") { pick_folder(L"Choose the RPCS3 folder", p, ch); if (ch) check_rpcs3(p); show(M.scr); return; }
    if (id == "btn_save") { pick_folder(L"Choose the NPUB31241_SYSTEM00 folder", p, ch); if (ch) { check_save(p); M.savePrivate = false; } show(M.scr); return; }
    if (id == "btn_web") { ShellExecuteW(nullptr, L"open", L"https://rpcs3.net", nullptr, nullptr, SW_SHOWNORMAL); return; }
    if (id == "btn_back") { show(step_of(-1)); return; }
    if (id == "btn_alt") {
        if (M.scr == U_ASK) { g_quit = true; return; }
        if (M.scr == U_DATA) { M.deleteData = false; const std::string e = do_uninstall(false); text("done_text", e.empty() ? "Your saves and settings were kept." : e); show(U_DONE); return; }
        g_quit = true; return;   // Close
    }
    if (id == "btn_next") {
        switch (M.scr) {
        case W_WELCOME: show(W_GAME); break;
        case W_GAME:
            if (M.needDecrypt && !M.rpcs3Ok) {   // RPCS3 only needs asking once: try the default places first? no -- the user picks it
            }
            if (M.gameOk) check_tools();
            show(step_of(1));
            if (M.scr == W_SAVE && M.rpcs3Ok && !M.saveOk) { const fs::path s = find_rpcs3_save(M.rpcs3); if (!s.empty()) { check_save(s); show(W_SAVE); } }
            break;
        case W_RPCS3: show(W_SAVE); if (M.rpcs3Ok && !M.saveOk) { const fs::path s = find_rpcs3_save(M.rpcs3); if (!s.empty()) { check_save(s); show(W_SAVE); } } break;
        case W_SAVE: if (M.toolsDl.empty()) start_install(); else show(W_TOOLS); break;
        case W_TOOLS: M.toolsAgreed = true; start_install(); break;
        case W_DONE: if (!g_sandbox) { ShellExecuteW(nullptr, L"open", (fs::path(g_install) / "refine.cmd").c_str(), nullptr, g_install.c_str(), SW_SHOWMINNOACTIVE); g_quit = true; } break;
        case U_ASK: show(U_DATA); break;
        case U_DATA: { M.deleteData = true; const std::string e = do_uninstall(true); text("done_text", e.empty() ? "Everything was deleted." : e); show(U_DONE); break; }
        default: break;
        }
    }
}

// ---------------------------------------------------------------------------- window
static std::string g_pendingClick;
static bool g_clickPending = false;
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEMOVE: if (g_ctx) g_ctx->ProcessMouseMove((short)LOWORD(l), (short)HIWORD(l), 0); return 0;
    case WM_LBUTTONDOWN: if (g_ctx) g_ctx->ProcessMouseButtonDown(0, 0); return 0;
    case WM_LBUTTONUP: if (g_ctx) g_ctx->ProcessMouseButtonUp(0, 0); g_clickPending = true; return 0;
    case WM_KEYDOWN:
        if (w == VK_RETURN) { g_pendingClick = "btn_next"; return 0; }
        if (w == VK_ESCAPE) { g_pendingClick = M.scr == W_WELCOME || M.scr == W_DONE || M.scr == U_ASK || M.scr == U_DONE ? "btn_alt" : "btn_back"; return 0; }
        break;
    case WM_CLOSE: if (M.scr == W_INSTALL && W.state == 1) return 0; g_quit = true; return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static std::wstring arg_value(int& i, int n, wchar_t** a) { return i + 1 < n ? a[++i] : L""; }

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int)
{
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int n; wchar_t** a = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; i < n; i++) {
        const std::wstring s = a[i];
        if (s == L"--sandbox") g_o.sandbox = arg_value(i, n, a);
        else if (s == L"--payload") g_o.payload = arg_value(i, n, a);
        else if (s == L"--game") g_o.game = arg_value(i, n, a);
        else if (s == L"--rpcs3") g_o.rpcs3 = arg_value(i, n, a);
        else if (s == L"--save") g_o.save = arg_value(i, n, a);
        else if (s == L"--script") g_o.script = arg_value(i, n, a);
        else if (s == L"--uninstall") g_o.uninstall = true;
        else if (s == L"--uninstall-run") g_o.uninstallRun = arg_value(i, n, a);
    }
    g_sandbox = !g_o.sandbox.empty();
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exePath = exe; g_exeDir = fs::path(exe).parent_path().wstring();
    init_paths();

    // Uninstall step 1: run from a copy in %TEMP%, so the install folder can be deleted whole.
    if (g_o.uninstall && g_o.uninstallRun.empty()) {
        const fs::path inst = fs::path(g_exeDir);
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        const fs::path t = fs::path(tmp) / (L"ReFineUninstall-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::create_directories(t, ec);
        fs::copy_file(g_exePath, t / L"ReFineSetup.exe", fs::copy_options::overwrite_existing, ec);
        fs::copy(inst / L"ui", t / L"ui", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        std::wstring cmd = L"\"" + (t / L"ReFineSetup.exe").wstring() + L"\" --uninstall --uninstall-run \"" + inst.wstring() + L"\"";
        if (g_sandbox) cmd += L" --sandbox \"" + g_o.sandbox + L"\"";
        if (!g_o.script.empty()) cmd += L" --script \"" + g_o.script + L"\"";
        STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
        if (CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, 0, nullptr, t.c_str(), &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        return 0;
    }
    if (g_o.uninstall) load_manifest(g_o.uninstallRun);

    WNDCLASSW wc = {}; wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.lpszClassName = L"ReFineSetup"; wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r = {0, 0, (LONG)kW, (LONG)kH};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowW(L"ReFineSetup", g_o.uninstall ? L"Re:Fine - Uninstall" : L"Re:Fine - Setup", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);
    if (!gfx_init() || !g_render.Init(G.dev.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 2)) {
        MessageBoxW(nullptr, L"This PC needs a graphics card with DirectX 12.", L"Re:Fine", MB_OK | MB_ICONERROR);
        return 1;
    }
    Rml::SetSystemInterface(&g_sys);
    Rml::SetRenderInterface(&g_render);
    Rml::Initialise();
    const std::string ui = u8(g_exeDir) + "/ui/";
    for (const char* f : {"Inter-Regular.ttf", "Inter-SemiBold.ttf", "BarlowCondensed-SemiBold.ttf", "BarlowCondensed-Bold.ttf"})
        Rml::LoadFontFace(ui + "fonts/" + f);
    g_ctx = Rml::CreateContext("setup", Rml::Vector2i((int)kW, (int)kH));
    g_ctx->SetDensityIndependentPixelRatio((float)kH / 1080.0f);
    g_doc = g_ctx->LoadDocument(ui + "setup.rml");
    if (!g_doc) { MessageBoxW(nullptr, L"The installer files are incomplete (ui folder).", L"Re:Fine", MB_OK | MB_ICONERROR); return 1; }
    g_doc->Show();
    std::error_code ec;
    if (fs::exists(fs::path(g_exeDir) / "ui" / "art" / "bg.png", ec)) { cls("bg", "art", true); M.privateBuild = true; }   // private build only

    // Private build: a save may come with the installer (payload\partida\NPUB31241_*).
    {
        const fs::path payload = g_o.payload.empty() ? fs::path(g_exeDir) / "payload" : fs::path(g_o.payload);
        if (fs::exists(payload / "partida", ec))
            for (auto& s : fs::directory_iterator(payload / "partida", ec))
                if (s.is_directory() && u8(s.path().filename().wstring()).rfind("NPUB31241", 0) == 0) { check_save(s.path()); M.savePrivate = M.saveOk; }
    }
    if (!g_o.game.empty()) check_game(g_o.game);
    if (!g_o.rpcs3.empty()) check_rpcs3(g_o.rpcs3);
    if (!g_o.save.empty()) { check_save(g_o.save); M.savePrivate = false; }

    show(g_o.uninstall ? U_ASK : W_WELCOME);
    ShowWindow(g_hwnd, SW_SHOW);

    // the script: "btn_next,btn_next,..." one every 1.2 s, never while the install runs
    std::vector<std::string> script; { std::string s = u8(g_o.script), t; for (char c : s) { if (c == ',') { script.push_back(t); t.clear(); } else t += c; } if (!t.empty()) script.push_back(t); }
    size_t si = 0; double nextAt = g_sys.GetElapsedTime() + 1.5;
    bool wasRunning = false;

    MSG msg;
    while (!g_quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) g_quit = true; TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (g_quit) break;
        if (IsIconic(g_hwnd)) { MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT); continue; }
        g_ctx->Update();
        if (g_clickPending) {
            g_clickPending = false;
            for (Rml::Element* e = g_ctx->GetHoverElement(); e; e = e->GetParentNode()) {
                const std::string id = e->GetId();
                if (id.rfind("btn_", 0) == 0) { on_click(id); break; }
            }
        }
        if (!g_pendingClick.empty()) { const std::string id = g_pendingClick; g_pendingClick.clear(); on_click(id); }
        if (si < script.size() && W.state != 1 && g_sys.GetElapsedTime() >= nextAt) { on_click(script[si++]); nextAt = g_sys.GetElapsedTime() + 1.2; }
        if (M.scr == W_INSTALL) {
            const int st = W.state;
            for (int i = 0; i < 4; i++) {
                char sid[8], mid[8]; snprintf(sid, sizeof sid, "st%d", i); snprintf(mid, sizeof mid, "m%d", i);
                const bool done = st == 2 || i < W.step, cur = st == 1 && i == W.step;
                cls(sid, "done", done); cls(sid, "cur", cur);
                text(mid, done ? "\xE2\x9C\x93" : cur ? "\xE2\x80\xA2" : "");
            }
            if (auto* f = E("pfill")) f->SetProperty("width", std::to_string((int)(W.frac * 100.0f)) + "%");
            { std::lock_guard<std::mutex> l(W.mu); if (st == 1) status("inst_status", W.text, 0); }
            if (st == 2) { show(W_DONE); wasRunning = false; }
            else if (st == -1) {
                std::string e; { std::lock_guard<std::mutex> l(W.mu); e = W.error; }
                status("inst_status", e, -1); text("inst_title", "Installation stopped");
                if (!wasRunning) {
                    if (auto* b = E("btn_back")) { b->SetInnerRML("Back"); b->SetClass("hidden", false); b->SetClass("off", false); }
                    if (auto* b = E("btn_alt")) { b->SetInnerRML("Close"); b->SetClass("hidden", false); b->SetClass("off", false); }
                    wasRunning = true; fprintf(stderr, "[setup] install failed: %s\n", e.c_str()); }
            }
        }
        gfx_frame();
    }
    if (W.state == 1) { gfx_wait(G.last); }   // closing is blocked while installing (WM_CLOSE); nothing to join
    gfx_wait(G.last);
    return 0;
}
