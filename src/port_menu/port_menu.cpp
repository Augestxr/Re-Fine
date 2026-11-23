// Re:Fine 6b+6c, part 2: the port's quick bar and settings panel, RmlUi over the runtime's D3D12 backend.
//
// Our own structure and styles (ui/*.rml, *.rcss), after the layout ideas described in docs/plan_f_f2nd.md;
// nothing of Dusklight's files is used. Colours of F's Miku teal in ui/theme.rcss.
//
// Threads:
//   boot thread    Rml::Initialise, fonts, the document and a dry render (glyphs and geometry), and the
//                  D3D12 pipeline once the runtime hands over the device (g_port_overlay_device_ready): the
//                  first open costs nothing more than an open. Started by port_menu_start().
//   window thread  F1, the mouse and the display mode (g_port_overlay_wndproc).
//   guest thread   cellPadGetData: g_port_overlay_watch sees every read (long SELECT, the song clock),
//                  g_port_overlay_pad holds the pad while anything is open (keyboard and controllers
//                  alike: cellPad merges both) and collects the presses.
//   render thread  everything else of RmlUi, in g_port_overlay_draw, once per presented frame, only while
//                  something is shown (bar, panel, or the FPS counter if it is on).
// With everything closed and the FPS counter off, the runtime does not call the draw hook at all.
#include "port_menu.h"
#include "rml_d3d12.h"
#include "rml_vk.h"
#include "discord.h"
#include "extras.h"
#include "../mods.h"
#include <deque>

#include <RmlUi/Core.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cmath>
#include <algorithm>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <cstdio>
#include <ctime>
#include <mutex>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "port_overlay.h"
#include "rsx_vk_exec.h"   // g_rsx_vk_ui (0186)

extern "C" uint8_t* vm_base;
extern "C" void ps3_host_quit(const char* why);   // cellSysutil.c: REQUEST_EXITGAME, waits for the save
extern "C" int ps3_savedata_busy(void);           // cellSaveData.c

#ifndef REFINE_VERSION
#define REFINE_VERSION "1.0.0-rc1"
#endif
// The Application ID of the author's Discord app "Re:Fine" (public, not a secret; given by the author 2026-11-11).
#define kDiscordAppId "1570140093350551674"

namespace {

// cellPad.h bits (the mask is DIGITAL2 << 8 | DIGITAL1).
enum : uint32_t {
    PAD_SELECT = 1u << 0, PAD_START = 1u << 3, PAD_UP = 1u << 4, PAD_RIGHT = 1u << 5, PAD_DOWN = 1u << 6,
    PAD_LEFT = 1u << 7, PAD_L1 = 1u << 10, PAD_R1 = 1u << 11, PAD_TRIANGLE = 1u << 12, PAD_CIRCLE = 1u << 13,
    PAD_CROSS = 1u << 14, PAD_SQUARE = 1u << 15,
};
constexpr uint32_t kGameSongClock = 0x12CBE48;   // f32, seconds of song (STATUS 6d, screen_census)
constexpr uint32_t kGameSpeed = 0xC22F50;        // f32, 0 while the game is stopped (pause menu, loading)
constexpr double kSelectHold = 0.5;              // long SELECT for the bar (F may read a short one)
constexpr double kRepeatDelay = 0.40, kRepeatStep = 0.075;

double now_s()
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
float guest_f32(uint32_t ea)
{
    const uint32_t b = _byteswap_ulong(*(const uint32_t*)(vm_base + ea));
    float f; memcpy(&f, &b, 4);
    return f;
}

uint32_t guest_u32(uint32_t ea) { return _byteswap_ulong(*(const uint32_t*)(vm_base + ea)); }

std::string g_dir;                    // the exe's folder, with a trailing backslash
std::wstring g_data_dir;              // %LOCALAPPDATA%\ReFine
std::unordered_map<std::string, std::string> g_lang, g_ini;
double g_boot_s;

std::string tr(const std::string& k)
{
    auto it = g_lang.find(k);
    return it != g_lang.end() ? it->second : k;
}
std::string esc(const std::string& s)
{
    std::string o;
    for (char c : s) o += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
    return o;
}
std::string narrow(const std::wstring& w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------------------------------
// refine.ini: read at boot, written per change with a temporary file and a rename (atomic on NTFS).
// Lines we do not know are kept as they are.
// ---------------------------------------------------------------------------------------------------
// REFINE_INI overrides the file (tools/run.ps1 -Profile test: never the author's own settings).
std::wstring ini_path()
{
    if (const wchar_t* o = _wgetenv(L"REFINE_INI")) return o;
    return g_data_dir + L"\\refine.ini";
}

void ini_load()
{
    FILE* f = _wfopen(ini_path().c_str(), L"rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char key[64], value[256];
        if (line[0] == '#' || sscanf(line, " %63[^= \t] = %255[^\r\n]", key, value) != 2) continue;
        g_ini[key] = value;
    }
    fclose(f);
}

// A key not in the file yet goes at the end of its [section], or before the first section without one.
bool ini_write(const std::string& key, const std::string& value, const char* section = nullptr)
{
    static std::mutex m;   // the window thread (remember window) and the menu thread both write
    std::lock_guard<std::mutex> lk(m);
    g_ini[key] = value;
    std::vector<std::string> lines;
    bool found = false;
    if (FILE* f = _wfopen(ini_path().c_str(), L"rb")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            char k[64];
            if (!s.empty() && s[0] != '#' && s[0] != '[' && sscanf(s.c_str(), " %63[^= 	]", k) == 1 && key == k) {
                s = key + " = " + value;
                found = true;
            }
            lines.push_back(s);
        }
        fclose(f);
    }
    if (!found) {
        const std::string want = section ? "[" + std::string(section) + "]" : "";
        size_t at = lines.size();
        bool in = !section;
        for (size_t i = 0; i < lines.size(); i++) {
            const bool head = !lines[i].empty() && lines[i][0] == '[';
            if (head && in) { at = i; break; }          // the end of our section (or the first header)
            if (head && lines[i] == want) in = true;
        }
        if (section && !in) {                           // no such section yet
            if (!lines.empty() && !lines.back().empty()) lines.push_back("");
            lines.push_back(want);
            at = lines.size();
        }
        while (at > 0 && lines[at - 1].empty() && at < lines.size()) at--;   // before the blank line ahead of a header
        lines.insert(lines.begin() + (long long)at, key + " = " + value);
    }
    CreateDirectoryW(g_data_dir.c_str(), nullptr);
    const std::wstring tmp = ini_path() + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) { fprintf(stderr, "[menu] refine.ini: cannot write the temporary file\n"); return false; }
    for (auto& l : lines) fprintf(f, "%s\r\n", l.c_str());
    const bool ok = fflush(f) == 0 && fclose(f) == 0;
    if (!ok || !MoveFileExW(tmp.c_str(), ini_path().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fprintf(stderr, "[menu] refine.ini: rename failed (%lu)\n", GetLastError());
        return false;
    }
    fprintf(stderr, "[menu] refine.ini: %s = %s\n", key.c_str(), value.c_str());
    return true;
}

// ---------------------------------------------------------------------------------------------------
// The settings. A row that does nothing yet is `soon`: shown greyed with "Coming soon", never live.
// ---------------------------------------------------------------------------------------------------
enum Kind { CHOICE, TOGGLE, BUTTON, INFO, BIND };   // BIND: a key or controller binding (6f p1)
enum Tab { T_VIDEO, T_AUDIO, T_INPUT, T_GAMEPLAY, T_INTERFACE, T_DATA, T_MODS, T_COUNT };
const char* const kTabKeys[T_COUNT] = {"video", "audio", "input", "gameplay", "interface", "data", "mods"};

struct Row {
    int tab; const char* sect; const char* id; Kind kind; const char* ini;
    std::vector<std::string> vals, labels;   // ini values and what is shown ("#key" = from the language)
    int orig;                                // index of the PS3 original, -1 for none
    bool restart, soon;
    int cur = 0, boot = 0;
    std::string info;                        // INFO rows
    int act = -1;                            // BIND rows: PORT_BIND_*
    bool pad = false;                        // BIND rows: controller (else keyboard)
    std::string title, desc;                 // set: shown instead of row.<id> / desc.<id> (a mod's own text)
};
std::vector<Row> g_rows;
// Mods (6e, src/mods.h): what mods\ held at boot. A mod's row id and refine.ini key are "mod.<folder>",
// kept here so the Row's const char* stay valid.
std::vector<ModInfo> g_mods;
std::deque<std::string> g_mod_ids;

void add(int tab, const char* sect, const char* id, Kind k, const char* ini, std::vector<std::string> vals,
         std::vector<std::string> labels, int orig, bool restart, bool soon)
{
    Row r{tab, sect, id, k, ini, std::move(vals), std::move(labels), orig, restart, soon};
    g_rows.push_back(std::move(r));
}
int find_row(const char* id)
{
    for (size_t i = 0; i < g_rows.size(); i++)
        if (!strcmp(g_rows[i].id, id)) return (int)i;
    return -1;
}
Row& row(const char* id) { return g_rows[find_row(id)]; }
std::string val(const char* id) { const Row& r = row(id); return r.vals.empty() ? "" : r.vals[r.cur]; }
std::string label_of(const Row& r, int i)
{
    const std::string& l = r.labels[i];
    return l.size() > 1 && l[0] == '#' ? tr(l.substr(1)) : l;
}

// ---------------------------------------------------------------------------------------------------
// 6f p1: bindings, live (0171). refine.ini [bindings]: kb_<action> = <key>[,<key>] or none (two keys per
// action), pad_<action> = <source> or none (one per action). Without the section, the layouts of today
// (0142, 0145). A key belongs to one action: taking it elsewhere takes it from where it was. A controller
// source too: taking it swaps the two actions, so no PS3 button is left without one.
// ---------------------------------------------------------------------------------------------------
const char* const kActKeys[PORT_BIND_COUNT] = {"cross", "circle", "square", "triangle", "up", "down", "left", "right",
    "l1", "r1", "l2", "r2", "l3", "r3", "start", "select", "ls_left", "ls_right", "rs_left", "rs_right"};
const char* const kKbIds[PORT_BIND_COUNT] = {"kb_cross", "kb_circle", "kb_square", "kb_triangle", "kb_up", "kb_down",
    "kb_left", "kb_right", "kb_l1", "kb_r1", "kb_l2", "kb_r2", "kb_l3", "kb_r3", "kb_start", "kb_select", "kb_ls_left",
    "kb_ls_right", "kb_rs_left", "kb_rs_right"};
const char* const kPadIds[PORT_BIND_PAD_COUNT] = {"pad_cross", "pad_circle", "pad_square", "pad_triangle", "pad_up",
    "pad_down", "pad_left", "pad_right", "pad_l1", "pad_r1", "pad_l2", "pad_r2", "pad_l3", "pad_r3", "pad_start", "pad_select"};
const int kKbDefault[PORT_BIND_COUNT][PORT_BIND_KEYS] = {
    {'S', VK_RETURN}, {'D', VK_BACK}, {'A', 0}, {'W', 0}, {'I', VK_UP}, {'K', VK_DOWN}, {'J', VK_LEFT}, {'L', VK_RIGHT},
    {'1', 0}, {'2', 0}, {'3', 0}, {'4', 0}, {0, 0}, {0, 0}, {VK_SPACE, VK_ESCAPE}, {VK_TAB, 0},
    {'Q', 0}, {'E', 0}, {'U', 0}, {'O', 0}};
// Controller sources: SDL_GamepadButton (SDL3's order), then the triggers (port_overlay.h).
enum { SRC_SOUTH, SRC_EAST, SRC_WEST, SRC_NORTH, SRC_BACK, SRC_GUIDE, SRC_START, SRC_LSTICK, SRC_RSTICK, SRC_LSHOULDER,
       SRC_RSHOULDER, SRC_DUP, SRC_DDOWN, SRC_DLEFT, SRC_DRIGHT, SRC_MISC1, SRC_RPADDLE1, SRC_LPADDLE1, SRC_RPADDLE2,
       SRC_LPADDLE2, SRC_TOUCHPAD, SRC_MISC2, SRC_MISC3, SRC_MISC4, SRC_MISC5, SRC_MISC6, SRC_COUNT_ = PORT_SRC_RT + 1 };
static_assert(SRC_MISC6 + 1 == PORT_SRC_LT, "SDL3's button order");
const int kPadDefault[PORT_BIND_PAD_COUNT] = {SRC_SOUTH, SRC_EAST, SRC_WEST, SRC_NORTH, SRC_DUP, SRC_DDOWN, SRC_DLEFT,
    SRC_DRIGHT, SRC_LSHOULDER, SRC_RSHOULDER, PORT_SRC_LT, PORT_SRC_RT, SRC_LSTICK, SRC_RSTICK, SRC_START, SRC_BACK};
// ini name, then what the menu shows (no icons: both families of names).
const char* const kSrc[SRC_COUNT_][2] = {
    {"south", "Cross / A"}, {"east", "Circle / B"}, {"west", "Square / X"}, {"north", "Triangle / Y"},
    {"back", "Select / Back"}, {"guide", "Guide"}, {"start", "Start / Options"}, {"leftstick", "L3"}, {"rightstick", "R3"},
    {"leftshoulder", "L1 / LB"}, {"rightshoulder", "R1 / RB"}, {"dpup", "D-pad up"}, {"dpdown", "D-pad down"},
    {"dpleft", "D-pad left"}, {"dpright", "D-pad right"}, {"misc1", "Share / Capture"}, {"rightpaddle1", "Paddle R1"},
    {"leftpaddle1", "Paddle L1"}, {"rightpaddle2", "Paddle R2"}, {"leftpaddle2", "Paddle L2"}, {"touchpad", "Touchpad"},
    {"misc2", "Misc 2"}, {"misc3", "Misc 3"}, {"misc4", "Misc 4"}, {"misc5", "Misc 5"}, {"misc6", "Misc 6"},
    {"lefttrigger", "L2 / LT"}, {"righttrigger", "R2 / RT"}};
// Never bound: Guide (the system's) and the touchpad (the quick bar, 0159).
constexpr unsigned kSrcReserved = (1u << SRC_GUIDE) | (1u << SRC_TOUCHPAD);

int g_kb[PORT_BIND_COUNT][PORT_BIND_KEYS];
int g_pb[PORT_BIND_PAD_COUNT];

// Key names for refine.ini, the same on any keyboard layout. A key without a name is written as 0x<vk>.
const struct { int vk; const char* name; } kKeyNames[] = {
    {VK_RETURN, "Enter"}, {VK_BACK, "Backspace"}, {VK_SPACE, "Space"}, {VK_ESCAPE, "Esc"}, {VK_TAB, "Tab"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"}, {VK_LSHIFT, "LShift"},
    {VK_RSHIFT, "RShift"}, {VK_LCONTROL, "LCtrl"}, {VK_RCONTROL, "RCtrl"}, {VK_LMENU, "LAlt"}, {VK_RMENU, "RAlt"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"},
    {VK_NEXT, "PageDown"}, {VK_CAPITAL, "CapsLock"}, {VK_MULTIPLY, "Num*"}, {VK_ADD, "Num+"}, {VK_SUBTRACT, "Num-"},
    {VK_DECIMAL, "Num."}, {VK_DIVIDE, "Num/"}, {VK_OEM_1, "Oem1"}, {VK_OEM_PLUS, "OemPlus"}, {VK_OEM_COMMA, "OemComma"},
    {VK_OEM_MINUS, "OemMinus"}, {VK_OEM_PERIOD, "OemPeriod"}, {VK_OEM_2, "Oem2"}, {VK_OEM_3, "Oem3"}, {VK_OEM_4, "Oem4"},
    {VK_OEM_5, "Oem5"}, {VK_OEM_6, "Oem6"}, {VK_OEM_7, "Oem7"}, {VK_OEM_102, "Oem102"}};

std::string key_name(int vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num" + std::to_string(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    for (const auto& k : kKeyNames) if (k.vk == vk) return k.name;
    char b[8]; snprintf(b, sizeof b, "0x%02X", vk & 0xFF);
    return b;
}
// What the menu shows: the ini name, except the punctuation keys, which show their character on this layout.
std::string key_label(int vk)
{
    if (!vk) return "-";
    if ((vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_7) || vk == VK_OEM_102) {
        const UINT c = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0xFFFF;
        if (c > 32 && c < 127) return std::string(1, (char)c);
    }
    return key_name(vk);
}
int key_from_name(const std::string& n)
{
    if (n.empty() || n == "none") return 0;
    if (n.size() == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return n[0];
    if (n.size() > 2 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) return (int)strtol(n.c_str() + 2, nullptr, 16) & 0xFF;
    for (int vk = 1; vk < 256; vk++) if (key_name(vk) == n) return vk;
    return -1;
}
// F1 is the menu's; the Windows keys and Print Screen belong to the system; the generic Shift/Ctrl/Alt
// arrive as their left or right key (wndproc).
bool key_bindable(int vk)
{
    return vk > 0 && vk < 256 && vk != VK_F1 && vk != VK_LWIN && vk != VK_RWIN && vk != VK_APPS && vk != VK_SNAPSHOT &&
           vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && (vk < VK_LBUTTON || vk > VK_XBUTTON2) && vk != VK_CANCEL;
}

std::string kb_value(int a)
{
    std::string v;
    for (int s = 0; s < PORT_BIND_KEYS; s++)
        if (g_kb[a][s]) v += (v.empty() ? "" : ",") + key_name(g_kb[a][s]);
    return v.empty() ? "none" : v;
}
std::string pad_value(int a) { return g_pb[a] < 0 ? "none" : kSrc[g_pb[a]][0]; }

void binds_load()
{
    memcpy(g_kb, kKbDefault, sizeof g_kb);
    memcpy(g_pb, kPadDefault, sizeof g_pb);
    for (int a = 0; a < PORT_BIND_COUNT; a++) {
        auto it = g_ini.find(kKbIds[a]);
        if (it == g_ini.end()) continue;
        int k[PORT_BIND_KEYS] = {0, 0}, n = 0;
        bool ok = true;
        const std::string v = it->second + ",";
        for (size_t at = 0, c; ok && (c = v.find(',', at)) != std::string::npos; at = c + 1) {
            std::string name = v.substr(at, c - at);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            while (!name.empty() && name[0] == ' ') name.erase(0, 1);
            const int vk = key_from_name(name);
            if (vk < 0 || (vk && !key_bindable(vk))) ok = false;
            else if (vk && n < PORT_BIND_KEYS) k[n++] = vk;
        }
        if (ok) memcpy(g_kb[a], k, sizeof k);
        else fprintf(stderr, "[menu] refine.ini: %s = %s not understood, default kept\n", kKbIds[a], it->second.c_str());
    }
    for (int a = 0; a < PORT_BIND_PAD_COUNT; a++) {
        auto it = g_ini.find(kPadIds[a]);
        if (it == g_ini.end()) continue;
        int src = it->second == "none" ? -1 : -2;
        for (int s = 0; s < SRC_COUNT_ && src == -2; s++) if (it->second == kSrc[s][0] && !(kSrcReserved >> s & 1)) src = s;
        if (src != -2) g_pb[a] = src;
        else fprintf(stderr, "[menu] refine.ini: %s = %s not understood, default kept\n", kPadIds[a], it->second.c_str());
    }
    // A key or a source written twice (by hand) keeps only its first action, as the menu would have left it.
    for (int a = 0; a < PORT_BIND_COUNT; a++)
        for (int s = 0; s < PORT_BIND_KEYS; s++) {
            bool dup = false;
            for (int b = 0; b <= a && g_kb[a][s] && !dup; b++)
                for (int t = 0; t < PORT_BIND_KEYS && !dup; t++)
                    if ((b < a || t < s) && g_kb[b][t] == g_kb[a][s]) dup = true;
            if (dup) g_kb[a][s] = 0;
        }
    for (int a = 0; a < PORT_BIND_PAD_COUNT; a++)
        for (int b = 0; b < a; b++) if (g_pb[a] >= 0 && g_pb[b] == g_pb[a]) g_pb[a] = -1;
}
void binds_apply()
{
    pad_set_key_binds(g_kb);
    pad_set_pad_binds(g_pb);
}

std::string row_title(const Row& r)
{
    if (!r.title.empty()) return r.title;
    if (r.kind == BIND) return tr(std::string("act.") + kActKeys[r.act]);
    return tr(std::string("row.") + r.id);
}

// Waiting for a key or a button (render thread). The key comes from the window thread (s_listen_vk); the
// controller is read as pressed, before any binding (pad_raw_sources). While waiting, and after it until
// everything is up, the menu takes no presses: the key or button that was bound does not also move the menu.
int g_listen = -1;                 // the row waiting
int g_slot;                        // keyboard rows: 0 the first key, 1 the second
int g_listen_left;                 // whole seconds left, shown
double g_listen_at, g_bind_note_until;
bool g_listen_armed, g_listen_mute;
unsigned g_listen_prev;
std::string g_bind_note;
volatile long s_listen_kb;         // 1 waiting for a key, 2 for a button (Esc cancels); window thread reads it
volatile long s_listen_vk, s_listen_esc, s_clear_req;
constexpr double kListenTime = 5.0;

// 6f p2: the outputs Windows has right now (0172). The one chosen before stays in the list when it is not
// plugged in, marked, so the choice is not lost; cellAudio uses the Windows default until it comes back.
Row& row(const char* id);
int g_star_vib;   // 6f p2 (R12): 0 off, 1 low, 2 medium, 3 high
void audio_rows_fill()
{
    Row& r = row("audio_device");
    const std::string keep = !r.vals.empty() ? r.vals[r.cur] : g_ini.count("audio_device") ? g_ini["audio_device"] : "auto";
    constexpr int kStride = 128, kMax = 32;
    std::vector<char> buf(kStride * kMax, 0);
    const int n = (std::min)(port_audio_outputs(buf.data(), kStride, kMax), kMax);
    r.vals = {"auto"};
    r.labels = {"#val.default_device"};
    for (int i = 0; i < n; i++) {
        const std::string name = &buf[i * kStride];
        if (name.empty() || std::find(r.vals.begin(), r.vals.end(), name) != r.vals.end()) continue;
        r.vals.push_back(name);
        r.labels.push_back(name);
    }
    int at = -1;
    for (size_t i = 0; i < r.vals.size(); i++) if (r.vals[i] == keep) at = (int)i;
    if (at < 0) { r.vals.push_back(keep); r.labels.push_back(keep + " " + tr("val.not_found")); at = (int)r.vals.size() - 1; }
    r.cur = r.boot = at;
}

// ---------------------------------------------------------------------------------------------------
// Plan 6h S1: the second game. refine.ini f2nd_folder = the user's Project DIVA F 2nd disc folder (the one
// with PS3_GAME); menu_game = f | f2nd, whose menu opens. F 2nd runs in its own exe (two exes, plan_6h.md §1),
// ReFine-F2nd.exe next to this one, which does not exist yet (P3): until it does, "f2nd" cannot be chosen and
// the start screen says "port pending". Nothing is read from F 2nd's disc but its PARAM.SFO.
// ---------------------------------------------------------------------------------------------------
extern "C" int cellGame_sfo_string(const char* sfo_path, const char* key, char* out, int out_size);   // 0194
extern "C" const char* cellGame_title_id_str(void);
extern "C" const char* cellGame_app_ver_str(void);
struct OtherGame {
    std::string folder, title_id, status;   // status: what the menu shows next to the folder
    bool disc = false;                       // the folder holds an F 2nd disc
    bool runnable = false;                   // ... and ReFine-F2nd.exe is installed
};
OtherGame g_f2nd;
// BLES02029 (Europe) is the disc seen here. ASSUMPTION: BLUS31446 (America) and BLJM61079 (Japan) are the
// other discs' IDs (PS3 catalogue); untested.
const char* const kF2ndIds[] = {"BLES02029", "BLUS31446", "BLJM61079"};

std::string f2nd_exe() { return g_dir + "ReFine-F2nd.exe"; }

void f2nd_probe()
{
    g_f2nd = OtherGame{};
    g_f2nd.folder = g_ini.count("f2nd_folder") ? g_ini["f2nd_folder"] : "";
    if (g_f2nd.folder.empty()) { g_f2nd.status = tr("games.not_set"); return; }
    char id[64] = "";
    bool sfo = false;
    for (const char* sub : {"\\PS3_GAME\\PARAM.SFO", "\\PARAM.SFO"})
        if (cellGame_sfo_string((g_f2nd.folder + sub).c_str(), "TITLE_ID", id, sizeof id) == 0) { sfo = true; break; }
    g_f2nd.title_id = id;
    for (const char* k : kF2ndIds) if (!strcmp(id, k)) g_f2nd.disc = true;
    g_f2nd.runnable = g_f2nd.disc && GetFileAttributesA(f2nd_exe().c_str()) != INVALID_FILE_ATTRIBUTES;
    g_f2nd.status = !sfo ? tr("games.no_sfo") : !g_f2nd.disc ? std::string(id) + " " + tr("games.not_f2nd")
                  : std::string(id) + " " + tr(g_f2nd.runnable ? "games.ready" : "games.found");
    fprintf(stderr, "[launcher] games: F %s; F 2nd folder '%s': %s (disc %s, exe %s)\n", cellGame_title_id_str(),
            g_f2nd.folder.c_str(), g_f2nd.status.c_str(), g_f2nd.disc ? "yes" : "no", g_f2nd.runnable ? "yes" : "no");
}

// The folder row: the status in the row (a long path would run over the label), the path in its description.
void f2nd_rows_fill()
{
    Row& r = row("f2nd_folder");
    r.info = g_f2nd.status;
    r.desc = g_f2nd.folder.empty() ? "" : tr("desc.f2nd_folder") + " " + tr("games.folder") + " " + g_f2nd.folder;
}

void build_rows(int render_height, int fps_target)
{
    const std::vector<std::string> onoff = {"on", "off"}, onoff_l = {"#val.on", "#val.off"};
    const std::vector<std::string> offon = {"off", "on"}, offon_l = {"#val.off", "#val.on"};
    add(T_VIDEO, "display", "display_mode", CHOICE, "display_mode", {"windowed", "borderless", "exclusive"},
        {"#val.windowed", "#val.borderless", "#val.exclusive"}, 0, false, false);
    add(T_VIDEO, "display", "remember_window", TOGGLE, "remember_window", offon, offon_l, 0, false, false);
    add(T_VIDEO, "display", "vsync", TOGGLE, "vsync", onoff, onoff_l, 0, true, false);
    add(T_VIDEO, "display", "renderer", CHOICE, "renderer", {"vulkan", "d3d12"}, {"#val.vulkan", "#val.d3d12"}, 0, true, false);
    add(T_VIDEO, "display", "show_fps", TOGGLE, "show_fps", offon, offon_l, 0, false, false);
    add(T_VIDEO, "display", "pause_focus", TOGGLE, "pause_focus", offon, offon_l, 0, false, false);
    add(T_VIDEO, "display", "screenshot_key", TOGGLE, "screenshot_key", offon, offon_l, 0, false, false);
    add(T_VIDEO, "resolution", "render_height", CHOICE, "render_height", {"720", "900", "1080", "1440", "2160"},
        {"720p", "900p", "1080p", "1440p", "2160p (4K)"}, 0, false, false);
    add(T_VIDEO, "resolution", "aniso", CHOICE, "anisotropic", {"off", "2", "4", "8", "16"}, {"Off", "2x", "4x", "8x", "16x"}, 0, false, false);
    add(T_VIDEO, "resolution", "sharpen", CHOICE, "sharpening", {"off", "low", "medium", "high"}, {"Off", "Low", "Medium", "High"}, 0, false, false);
    add(T_VIDEO, "resolution", "resampling", CHOICE, "resampling", {"bilinear", "area"}, {"Bilinear", "Area"}, 0, false, false);
    // Common monitor rates, the display's own rate and "unlimited"; any other rate from refine.ini (30-1000) is
    // added below. The values are what refine.ini keeps (main.cpp, fps_parse).
    add(T_VIDEO, "framerate", "fps_target", CHOICE, "fps_target",
        {"display", "30", "60", "75", "90", "120", "144", "165", "180", "240", "360", "unlimited"},
        {"#val.match_display", "30 FPS", "60 FPS", "75 FPS", "90 FPS", "120 FPS", "144 FPS", "165 FPS", "180 FPS", "240 FPS",
         "360 FPS", "#val.unlimited"},
        2, false, false);   // the default is 60 (main.cpp); "restore" used to put back 30, the first value
    add(T_VIDEO, "framerate", "menu_fps", CHOICE, "menu_fps", {"30", "same"},
        {"30", "Same as game"}, 1, false, false);   // 0175: same by default
    add(T_VIDEO, "framerate", "best", BUTTON, nullptr, {}, {}, -1, false, false);
    add(T_VIDEO, "framerate", "restore", BUTTON, nullptr, {}, {}, -1, false, false);

    std::vector<std::string> vol, vol_l;
    for (int v = 0; v <= 100; v += 10) { vol.push_back(std::to_string(v)); vol_l.push_back(std::to_string(v) + " %"); }
    add(T_AUDIO, "output", "master_volume", CHOICE, "master_volume", vol, vol_l, -1, false, false);   // 100 % by default
    add(T_AUDIO, "output", "audio_device", CHOICE, "audio_device", {}, {}, -1, false, false);   // filled by audio_rows_fill
    add(T_AUDIO, "output", "mute_background", TOGGLE, "mute_background", offon, offon_l, 0, false, false);
    add(T_AUDIO, "output", "audio_offset", INFO, nullptr, {}, {}, -1, false, true);

    add(T_INPUT, "controller", "controller", INFO, nullptr, {}, {}, -1, false, false);
    add(T_INPUT, "controller", "vibration", TOGGLE, "vibration", onoff, onoff_l, 0, false, false);
    add(T_INPUT, "controller", "touch_stars", TOGGLE, "touch_stars", onoff, onoff_l, 0, false, false);   // 0176: asked by the author, on
    add(T_INPUT, "controller", "star_vibration", CHOICE, "star_vibration", {"off", "low", "medium", "high"},
        {"#val.off", "#val.low", "#val.medium", "#val.high"}, 0, false, false);   // R12: new, off by default
    {   // Stick (0148, 0155, 0166): 5-50 % in steps of 5; the original values are 20 %, 25 % and automatic.
        std::vector<std::string> pv, pl;
        for (int v = 5; v <= 50; v += 5) { char b[8]; snprintf(b, sizeof b, "0.%02d", v); pv.push_back(b); pl.push_back(std::to_string(v) + " %"); }
        add(T_INPUT, "controller", "stick_deadzone", CHOICE, "stick_deadzone", pv, pl, 3, false, false);
        add(T_INPUT, "controller", "stick_hysteresis", CHOICE, "stick_hysteresis", pv, pl, 4, false, false);
        add(T_INPUT, "controller", "stick_rearm", CHOICE, "stick_rearm_ms", {"auto", "0", "10", "20", "34", "50", "75", "100"},
            {"#val.auto", "0 ms", "10 ms", "20 ms", "34 ms", "50 ms", "75 ms", "100 ms"}, 0, false, false);
        add(T_INPUT, "controller", "stick_reset", BUTTON, nullptr, {}, {}, -1, false, false);
    }
    for (int a = 0; a < PORT_BIND_PAD_COUNT; a++) {
        add(T_INPUT, "pad_buttons", kPadIds[a], BIND, nullptr, {}, {}, -1, false, false);
        g_rows.back().act = a; g_rows.back().pad = true;
    }
    add(T_INPUT, "pad_buttons", "pad_reset", BUTTON, nullptr, {}, {}, -1, false, false);
    for (int a = 0; a < PORT_BIND_COUNT; a++) {
        add(T_INPUT, "keyboard", kKbIds[a], BIND, nullptr, {}, {}, -1, false, false);
        g_rows.back().act = a;
    }
    add(T_INPUT, "keyboard", "kb_reset", BUTTON, nullptr, {}, {}, -1, false, false);
    // The PS3 had no mouse: "PS3 original" is off for both.
    add(T_INPUT, "mouse", "mouse_menus", TOGGLE, "mouse_menus", onoff, onoff_l, 1, false, false);
    add(T_INPUT, "mouse", "mouse_songs", TOGGLE, "mouse_songs", offon, offon_l, 0, false, false);

    add(T_GAMEPLAY, "timing", "timing_display", TOGGLE, "timing_display", offon, offon_l, 0, false, false);
    {   // src/judge.cpp: -100 to +100 ms, 0 as the game
        std::vector<std::string> jv, jl;
        for (int ms = -100; ms <= 100; ms += 5) { char b[16]; snprintf(b, sizeof b, "%+d ms", ms); jv.push_back(std::to_string(ms)); jl.push_back(ms ? b : "0 ms"); }
        add(T_GAMEPLAY, "timing", "judge_offset", CHOICE, "judge_offset_ms", jv, jl, 20, false, false);
    }
    add(T_GAMEPLAY, "extras", "skip_intro", TOGGLE, "skip_intro", offon, offon_l, 0, false, false);
    add(T_GAMEPLAY, "extras", "unlock_all", TOGGLE, nullptr, offon, offon_l, 0, false, true);

    std::vector<std::string> sc, sc_l;
    for (int v = 90; v <= 130; v += 10) { sc.push_back(std::to_string(v)); sc_l.push_back(std::to_string(v) + " %"); }
    add(T_INTERFACE, "general", "language", CHOICE, "language", {"en"}, {"English"}, -1, false, false);
    add(T_INTERFACE, "general", "show_launcher", TOGGLE, "show_launcher", onoff, onoff_l, -1, false, false);
    add(T_INTERFACE, "general", "menu_scale", CHOICE, "menu_scale", sc, sc_l, -1, false, false);
    // the author 2026-11-11: optional and ON by default (a choice of his, not the PS3's: no "original").
    add(T_INTERFACE, "general", "discord", TOGGLE, "discord", onoff, onoff_l, -1, false, false);
    // Plan 6h S1: the second game. "menu_game" is greyed ("Coming soon") until F 2nd's exe is installed.
    f2nd_probe();
    add(T_INTERFACE, "games", "menu_game", CHOICE, "menu_game", {"f", "f2nd"}, {"Project DIVA F", "Project DIVA F 2nd"}, -1, true, !g_f2nd.runnable);
    add(T_INTERFACE, "games", "f2nd_folder", INFO, nullptr, {}, {}, -1, false, false);
    add(T_INTERFACE, "games", "choose_f2nd", BUTTON, nullptr, {}, {}, -1, false, false);

    add(T_DATA, "savedata", "data_folder", INFO, nullptr, {}, {}, -1, false, false);
    add(T_DATA, "savedata", "open_folder", BUTTON, nullptr, {}, {}, -1, false, false);
    add(T_DATA, "savedata", "backups", TOGGLE, "save_backups", offon, offon_l, 0, true, false);

    // Mods: off when first seen (new things off by default); a change applies after a restart.
    add(T_MODS, "mods_folder", "mods_folder", INFO, nullptr, {}, {}, -1, false, false);
    add(T_MODS, "mods_folder", "open_mods", BUTTON, nullptr, {}, {}, -1, false, false);
    g_mods = mods_scan(mods_dir(g_dir));
    int shown = 0;
    for (const ModInfo& m : g_mods) {
        if (!m.for_this_game) { fprintf(stderr, "[mods] %s is for %s, not this game: not listed\n", m.folder.c_str(), m.game.c_str()); continue; }
        g_mod_ids.push_back("mod." + m.folder);
        add(T_MODS, "mods_installed", g_mod_ids.back().c_str(), TOGGLE, g_mod_ids.back().c_str(), offon, offon_l, -1, true, false);
        Row& r = g_rows.back();
        r.title = m.title;
        std::string by;
        if (!m.author.empty()) by += tr("mods.by") + " " + m.author;
        if (!m.version.empty()) by += (by.empty() ? "" : " \xC2\xB7 ") + std::string("v") + m.version;
        r.desc = (m.desc.empty() ? "" : m.desc + "  ") + (by.empty() ? "" : by + ". ") + std::to_string(m.files) + " " + tr("mods.files") +
                 " " + tr("mods.priority") + " " + std::to_string(m.priority) + ".";
        shown++;
    }
    if (!shown) add(T_MODS, "mods_installed", "mods_none", INFO, nullptr, {}, {}, -1, false, false);

    // Current values: refine.ini, except render_height and fps_target, which come from what this session
    // really runs with (an environment variable wins over the file at boot, main.cpp).
    for (Row& r : g_rows) {
        if (!r.ini) continue;
        std::string v;
        if (!strcmp(r.id, "render_height")) v = std::to_string(render_height);
        else if (!strcmp(r.id, "fps_target")) v = fps_target == 0 ? "unlimited" : fps_target == 1 ? "display" : std::to_string(fps_target);
        else if (!strcmp(r.id, "vsync")) { const char* e = getenv("D3D12_VSYNC"); v = e && *e == '0' ? "off" : "on"; }
        else if (!strcmp(r.id, "renderer")) v = rsx_renderer_is_vulkan() ? "vulkan" : "d3d12";
        else if (g_ini.count(r.ini)) v = g_ini[r.ini];
        int at = -1;
        for (size_t i = 0; i < r.vals.size(); i++) if (r.vals[i] == v) at = (int)i;
        if (at < 0 && !v.empty() && !strcmp(r.id, "render_height")) {   // a height outside the list (360-2160)
            r.vals.push_back(v); r.labels.push_back(v + "p"); at = (int)r.vals.size() - 1;
        }
        if (at < 0 && !v.empty() && !strcmp(r.id, "fps_target")) {      // a rate outside the list (30-1000)
            r.vals.insert(r.vals.end() - 1, v); r.labels.insert(r.labels.end() - 1, v + " FPS"); at = (int)r.vals.size() - 2;
        }
        r.cur = r.boot = at >= 0 ? at : !strcmp(r.id, "mouse_menus") ? 0 : r.orig >= 0 ? r.orig : !strcmp(r.id, "menu_scale") ? 1 : !strcmp(r.id, "master_volume") ? 10 : 0;
    }
    audio_rows_fill();
    row("data_folder").info = narrow(g_data_dir);
    row("mods_folder").info = mods_dir(g_dir);
    f2nd_rows_fill();
    if (!g_f2nd.runnable) row("menu_game").cur = row("menu_game").boot = 0;   // F 2nd cannot open: the row shows F
    if (find_row("mods_none") >= 0) row("mods_none").info = "-";
}

// The mods that are ON in refine.ini, in g_mods' order (priority, then folder): the first one with a file wins.
void mods_boot()
{
    std::vector<const ModInfo*> on;
    for (const ModInfo& m : g_mods) {
        const int i = find_row(("mod." + m.folder).c_str());
        if (i >= 0 && g_rows[i].vals[g_rows[i].boot] == "on") on.push_back(&m);
    }
    mods_activate(on);
}

// ---------------------------------------------------------------------------------------------------
// State shared across threads.
// ---------------------------------------------------------------------------------------------------
volatile long s_ready;                // boot thread: CPU side done (1) or failed (-1)
HANDLE s_ready_event, s_device_event;
ID3D12Device* s_device; uint32_t s_rtv_format, s_frame_count;
volatile long s_gpu_ready;            // the pipeline was built on the boot thread
volatile long s_toggle;               // F1, touchpad or long SELECT, not yet seen by the render thread
volatile long s_edges;                // pad buttons newly pressed since the render thread looked
volatile long s_last[4];              // last mask per port (edges and auto-repeat)
volatile long s_closing;              // closed: the gate waits for every button to be up
volatile double s_closed_at;          // when it closed: the gate never outlives it by more than kGateMax
constexpr double kGateMax = 1.0;
volatile long s_mouse_x = -1, s_mouse_y = -1, s_mouse_moved, s_clicks, s_wheel;
volatile long s_shown;                // anything shown: the window thread consumes the mouse
// The mouse in the game's own menus (menu closed): wheel = up/down, left = Cross, right = Circle,
// middle = Triangle. Each one is a press of kMousePress then a release of kMouseGap, one after another,
// so every notch of the wheel is its own press. Pending per button: window thread adds, guest thread takes.
constexpr uint32_t kMouseBtn[5] = {PAD_UP, PAD_DOWN, PAD_CROSS, PAD_CIRCLE, PAD_TRIANGLE};
constexpr double kMousePress = 0.07, kMouseGap = 0.05, kCursorIdle = 3.0;
volatile long s_mouse_pend[5];
volatile long s_inject_start, s_inject_mouse;   // the two parts of g_port_overlay_inject
double s_mouse_until, s_mouse_gap_until;        // guest thread only
volatile double s_mouse_seen_at;                // last move, for hiding the cursor
volatile long s_cursor_hidden;
bool g_mouse_menus = true, g_mouse_songs = false;
bool g_pause_focus, g_remember_window;   // 6b+6c p6 C
volatile long s_focus_lost;              // window thread: the app lost the focus; guest thread takes it
HWND s_hwnd;
int s_big_w, s_big_h;   // PORT_MENU_BIGWINDOW=<w>x<h>
double s_select_since, s_song_seen_at;
float s_song_last;
bool s_select_fired;

RmlD3D12 g_render;
// N1 (0186): with the Vulkan renderer the menu draws through the executor (rml_vk); nothing of D3D12 is made.
RmlVk g_render_vk;
bool g_vk;   // set when the backend says which renderer draws (device_ready, or the first draw)
// 0188: RmlUi keeps one render interface from Rml::Initialise on, and the backend decides the renderer later
// (Vulkan, or Direct3D 12 when Vulkan does not start): this one hands every call to the renderer in use. Nothing
// reaches it before that is known (the boot thread waits for the device before its first render).
struct RmlEither final : Rml::RenderInterface {
    Rml::RenderInterface& r() { return g_vk ? (Rml::RenderInterface&)g_render_vk : (Rml::RenderInterface&)g_render; }
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> v, Rml::Span<const int> i) override { return r().CompileGeometry(v, i); }
    void RenderGeometry(Rml::CompiledGeometryHandle g, Rml::Vector2f t, Rml::TextureHandle x) override { r().RenderGeometry(g, t, x); }
    void ReleaseGeometry(Rml::CompiledGeometryHandle g) override { r().ReleaseGeometry(g); }
    Rml::TextureHandle LoadTexture(Rml::Vector2i& d, const Rml::String& s) override { return r().LoadTexture(d, s); }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> s, Rml::Vector2i d) override { return r().GenerateTexture(s, d); }
    void ReleaseTexture(Rml::TextureHandle t) override { r().ReleaseTexture(t); }
    void EnableScissorRegion(bool e) override { r().EnableScissorRegion(e); }
    void SetScissorRegion(Rml::Rectanglei g) override { r().SetScissorRegion(g); }
    void SetTransform(const Rml::Matrix4f* m) override { r().SetTransform(m); }
    Rml::CompiledShaderHandle CompileShader(const Rml::String& n, const Rml::Dictionary& p) override { return r().CompileShader(n, p); }
    void RenderShader(Rml::CompiledShaderHandle s, Rml::CompiledGeometryHandle g, Rml::Vector2f t, Rml::TextureHandle x) override { r().RenderShader(s, g, t, x); }
    void ReleaseShader(Rml::CompiledShaderHandle s) override { r().ReleaseShader(s); }
} g_render_either;
bool g_gpu_ok, g_gpu_failed;
Rml::Context* g_ctx;
Rml::ElementDocument* g_doc;

enum Mode { M_CLOSED, M_BAR, M_PANEL, M_CONFIRM, M_LAUNCHER };
enum Confirm { C_QUIT, C_RESTART, C_BEST };
Mode g_mode = M_CLOSED;
Confirm g_confirm;
int g_bar_sel, g_tab, g_sel = -1, g_conf_sel;   // g_sel: index in g_rows of the row with focus
bool g_show_fps, g_quitting;
// 6g: options that are the port's own, all off by default.
bool g_skip_intro, g_shot_key, g_mute_bg, g_timing;
volatile long s_active = 1;            // the window is in front (WM_ACTIVATEAPP)
volatile long s_tm_seq;                // a note was judged: the next three are its own (guest thread writes, draw reads)
volatile double g_tm_ms, g_tm_avg, g_tm_until;
volatile long s_note;                  // a message to show: 1 screenshot saved, 2 screenshot failed
double g_note_until, g_shot_test = -1; // PORT_MENU_SHOT=<s>, diagnostic: the screenshot key at that second
// 6b+6c p3: the start screen (before the game's logos). g_launcher_on from the hook to PLAY.
bool g_launcher_on, g_launcher_done;
double g_launch_t = -1;                // PLAY pressed at (now_s()): the start screen plays its way out first
constexpr double kLaunchSecs = 0.82;   // the "launching" motion in menu.rcss ends in black at 0.80 s
int g_lsel;                       // 0 PLAY, 1 SETTINGS, 2 QUIT
double g_toast_until;
std::string g_toast_pad;
float g_scale = 1.0f;
double g_pause_check_at, g_rep_next[4];
volatile double g_inject_until;   // render thread sets it, guest thread (watch) lets go of the START
std::vector<double> g_present_times;
int g_clock_min = -1;

enum { B_SETTINGS, B_ACHIEVEMENTS, B_SAVES, B_QUIT, B_COUNT };
const char* const kBarKeys[B_COUNT] = {"settings", "achievements", "saves", "quit"};
const bool kBarSoon[B_COUNT] = {false, true, true, false};

// Diagnostic only: PORT_MENU_AT=<open s>-<close s> since boot opens the quick bar (and with
// PORT_MENU_PANEL=<tab> the panel on that tab) for unattended runs. PORT_MENU_TRACE=1 prints the panel's
// opacity on the frames of the first 0.3 s.
double g_auto_open = -1, g_auto_close = -1, g_opened_at;
int g_auto_panel = -1;
double g_restart_test = -1;   // PORT_MENU_RESTART_TEST
double g_focus_test = -1;     // PORT_MENU_FOCUS_TEST
std::vector<std::pair<double, int>> g_auto_tabs;   // PORT_MENU_TABS=<tab>@<s>,...: that tab at that second
bool g_trace;

class System final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override { return now_s() - g_boot_s; }
    int TranslateString(Rml::String& out, const Rml::String& in) override
    {
        if (in.size() > 1 && in[0] == '#') {
            auto it = g_lang.find(in.substr(1));
            if (it != g_lang.end()) { out = it->second; return 1; }
        }
        out = in;
        return 0;
    }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        if (type <= Rml::Log::LT_WARNING) fprintf(stderr, "[menu] RmlUi: %s\n", message.c_str());
        return true;
    }
};
System g_system;

bool load_lang(const std::string& path)
{
    FILE* f = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"rb");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == ';') continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string v) {
            const size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
        };
        g_lang[trim(s.substr(0, eq))] = trim(s.substr(eq + 1));
    }
    fclose(f);
    return true;
}

Rml::Element* el(const char* id) { return g_doc->GetElementById(id); }
Rml::Element* el(const std::string& id) { return g_doc->GetElementById(id); }
void set_text(const char* id, const std::string& rml) { if (Rml::Element* e = el(id)) e->SetInnerRML(rml); }
void set_class(const char* id, const char* c, bool on) { if (Rml::Element* e = el(id)) e->SetClass(c, on); }

// ---------------------------------------------------------------------------------------------------
// Redesign 2026-11-11: motion. A screen goes away with a short fade ("closing", 0.18 s in menu.rcss) and only
// then loses "shown"; one that comes back before that keeps going. The ink (focus indicator) of each screen
// slides to the focused element: it is placed from the last frame's layout, so a new screen's first frame
// puts it in place without sliding ("live" turns the transition on from the next frame).
// ---------------------------------------------------------------------------------------------------
struct Hide { std::string id; double at; };
std::vector<Hide> g_hides;
double now_s();
void show_el(const char* id)
{
    for (size_t k = 0; k < g_hides.size();) { if (g_hides[k].id == id) g_hides.erase(g_hides.begin() + (long long)k); else k++; }
    set_class(id, "closing", false);
    set_class(id, "shown", true);
}
void hide_el(const char* id, double secs = 0.18)
{
    Rml::Element* e = el(id);
    if (!e || !e->IsClassSet("shown")) return;
    for (const Hide& h : g_hides) if (h.id == id) return;
    e->SetClass("closing", true);
    g_hides.push_back({id, now_s() + secs});
}
bool hides_tick(double now)   // true while something is still fading out
{
    for (size_t k = 0; k < g_hides.size();) {
        if (now >= g_hides[k].at) {
            set_class(g_hides[k].id.c_str(), "shown", false);
            set_class(g_hides[k].id.c_str(), "closing", false);
            g_hides.erase(g_hides.begin() + (long long)k);
        } else k++;
    }
    return !g_hides.empty();
}
void restart_class(const char* id, const char* cls);
struct Ink { float a = -1, b = -1; int live = 0; };
// Ink <ink_id> to <target>: horizontal (left and width) or vertical (top and height).
void place_ink(const char* ink_id, Rml::Element* target, bool vertical, Ink& st)
{
    Rml::Element* ink = el(ink_id);
    if (!ink || !target) return;
    const float a = vertical ? target->GetOffsetTop() : target->GetOffsetLeft();
    const float b = vertical ? target->GetOffsetHeight() : target->GetOffsetWidth();
    if (b <= 0) { st.live = 0; ink->SetClass("live", false); return; }   // not laid out (hidden): starts again
    if (a != st.a || b != st.b) {
        st.a = a; st.b = b;
        // A transform, not top/left: drawn at sub-pixel positions while it eases (menu.rcss, P0 2026-11-11).
        ink->SetProperty("transform", std::string(vertical ? "translateY(" : "translateX(") + std::to_string(a) + "px)");
        ink->SetProperty(vertical ? "height" : "width", std::to_string(b) + "px");
    }
    if (st.live < 2 && ++st.live == 2) ink->SetClass("live", true);
}

// ---------------------------------------------------------------------------------------------------
// Settings applied in place, and the ones read at boot.
// ---------------------------------------------------------------------------------------------------
constexpr UINT WM_PORT_DISPLAY = WM_APP + 0x5F1;   // +0 display mode, +1 big window, +2 hide cursor, +3 save the window, +4 restore it
extern "C" { extern volatile int g_port_exclusive, g_port_exclusive_state, g_port_gpu_prof; }   // 0167

extern "C" void rsx_d3d12_set_render_height(unsigned h);
extern "C" unsigned rsx_d3d12_take_render_height_failure(unsigned* now_height);   // 0162: a height that could not be made
double g_excl_err_until = -1;
double g_rh_err_until = -1;   // until when the render_height row says the change failed   // 0161: applied before the next frame is recorded
extern "C" void port_set_judge_offset(int ms);             // src/judge.cpp
extern "C" float port_judge_offset(void);
extern "C" void port_set_menu_fps(int same);
extern "C" int port_in_song(void);                          // src/main.cpp: the game's pv_game task is alive               // src/main.cpp, 0163
extern "C" void port_set_fps_target(int v);                // src/main.cpp: the next frame of the game

extern "C" void pad_set_stick(float dz, float hy, int rearm_ms);     // libs/input/cellPad.c, 0166
extern "C" void pad_view_stick(float* x, float* y);
float stick_value(const char* id) { return (float)atof(val(id).c_str()); }
void apply_stick()
{
    const std::string rm = val("stick_rearm");
    pad_set_stick(stick_value("stick_deadzone"), stick_value("stick_hysteresis"), rm == "auto" ? -1 : atoi(rm.c_str()));
}

void volume_apply()
{
    g_port_master_volume = g_mute_bg && !s_active ? 0.0f : (float)atoi(val("master_volume").c_str()) / 100.0f;
}

void apply_live(const Row& r)
{
    if (!strncmp(r.id, "stick_", 6)) { if (strcmp(r.id, "stick_reset")) apply_stick(); }
    else if (!strcmp(r.id, "master_volume")) volume_apply();
    else if (!strcmp(r.id, "mute_background")) { g_mute_bg = r.vals[r.cur] == "on"; volume_apply(); }
    else if (!strcmp(r.id, "screenshot_key")) g_shot_key = r.vals[r.cur] == "on";
    else if (!strcmp(r.id, "judge_offset")) port_set_judge_offset(atoi(r.vals[r.cur].c_str()));
    else if (!strcmp(r.id, "skip_intro")) g_skip_intro = r.vals[r.cur] == "on";
    else if (!strcmp(r.id, "timing_display")) g_timing = r.vals[r.cur] == "on";
    else if (!strcmp(r.id, "vibration")) g_port_rumble_off = r.vals[r.cur] == "off";
    else if (!strcmp(r.id, "touch_stars")) g_port_touch_stars = r.vals[r.cur] == "on";
    else if (!strcmp(r.id, "star_vibration")) g_star_vib = r.cur;
    else if (!strcmp(r.id, "discord")) discord_enable(r.vals[r.cur] == "on");
    else if (!strcmp(r.id, "audio_device")) port_audio_set_device(r.vals[r.cur].c_str());
    else if (!strcmp(r.id, "show_fps")) g_show_fps = r.vals[r.cur] == "on";
    else if (!strcmp(r.id, "mouse_menus")) g_mouse_menus = r.vals[r.cur] == "on";
    else if (!strcmp(r.id, "mouse_songs")) g_mouse_songs = r.vals[r.cur] == "on";
    else if (!strcmp(r.id, "menu_scale")) g_scale = (float)atoi(r.vals[r.cur].c_str()) / 100.0f;
    else if (!strcmp(r.id, "aniso")) g_port_aniso = r.vals[r.cur] == "off" ? 0 : atoi(r.vals[r.cur].c_str());
    else if (!strcmp(r.id, "resampling")) g_port_area = r.vals[r.cur] == "area";
    else if (!strcmp(r.id, "sharpen")) g_port_sharpen = r.vals[r.cur] == "off" ? 0.0f : r.vals[r.cur] == "low" ? 0.3f : r.vals[r.cur] == "medium" ? 0.6f : 1.0f;
    else if (!strcmp(r.id, "render_height")) rsx_d3d12_set_render_height((unsigned)atoi(r.vals[r.cur].c_str()));
    else if (!strcmp(r.id, "fps_target")) {                  // main.cpp: 0 = unlimited, 1 = the display's rate
        const std::string& f = r.vals[r.cur];
        port_set_fps_target(f == "unlimited" ? 0 : f == "display" ? 1 : atoi(f.c_str()));
    }
    else if (!strcmp(r.id, "menu_fps")) port_set_menu_fps(r.vals[r.cur] == "same");
    else if (!strcmp(r.id, "display_mode") && s_hwnd) PostMessageW(s_hwnd, WM_PORT_DISPLAY, r.cur, 0);
    else if (!strcmp(r.id, "pause_focus")) g_pause_focus = r.vals[r.cur] == "on";
    else if (!strcmp(r.id, "remember_window")) {
        g_remember_window = r.vals[r.cur] == "on";
        if (g_remember_window && s_hwnd) PostMessageW(s_hwnd, WM_PORT_DISPLAY + 3, 0, 0);   // save where it is now
    }
}

// Window thread. Borderless: a popup over the monitor the window is on; exclusive: the same, then the
// swap chain goes fullscreen (0167, done by the renderer between frames); windowed: its old frame back.
int s_display_mode;   // 0 windowed, 1 borderless, 2 exclusive
void set_display_mode(HWND h, int mode)
{
    static LONG_PTR s_style;
    static RECT s_rect;
    if (mode == s_display_mode) return;
    const int was = s_display_mode;
    if (was == 2) g_port_exclusive = 0;   // leave exclusive first
    if (mode != 0) {
        if (was == 0) {
            s_style = GetWindowLongPtrW(h, GWL_STYLE);
            GetWindowRect(h, &s_rect);
        }
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(h, GWL_STYLE, (s_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        if (mode == 2) g_port_exclusive = 1;
    } else {
        SetWindowLongPtrW(h, GWL_STYLE, s_style);
        SetWindowPos(h, nullptr, s_rect.left, s_rect.top, s_rect.right - s_rect.left, s_rect.bottom - s_rect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    s_display_mode = mode;
    fprintf(stderr, "[menu] display mode %s\n", mode == 2 ? "exclusive" : mode == 1 ? "borderless" : "windowed");
}

// Window thread. Remember window size: where the window is, in refine.ini (window_x/y/w/h), only in
// windowed mode and never minimised.
void save_window(HWND h)
{
    if (!g_remember_window || s_display_mode != 0 || IsIconic(h)) return;
    RECT r;
    if (!GetWindowRect(h, &r)) return;
    ini_write("window_x", std::to_string(r.left)); ini_write("window_y", std::to_string(r.top));
    ini_write("window_w", std::to_string(r.right - r.left)); ini_write("window_h", std::to_string(r.bottom - r.top));
    fprintf(stderr, "[menu] window saved %ldx%ld at %ld,%ld\n", r.right - r.left, r.bottom - r.top, r.left, r.top);
}
// At boot: back to the saved place, if it is still on a monitor and a sensible size.
void restore_window(HWND h)
{
    if (!g_remember_window || !g_ini.count("window_w") || !g_ini.count("window_h") || !g_ini.count("window_x") || !g_ini.count("window_y")) return;
    RECT r;
    r.left = atoi(g_ini["window_x"].c_str()); r.top = atoi(g_ini["window_y"].c_str());
    r.right = r.left + atoi(g_ini["window_w"].c_str()); r.bottom = r.top + atoi(g_ini["window_h"].c_str());
    if (r.right - r.left < 320 || r.bottom - r.top < 180 || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
        fprintf(stderr, "[menu] saved window place not usable: ignored\n");
        return;
    }
    SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    fprintf(stderr, "[menu] window restored %ldx%ld at %ld,%ld\n", r.right - r.left, r.bottom - r.top, r.left, r.top);
}

bool pending_restart()
{
    for (const Row& r : g_rows) if (r.restart && r.cur != r.boot) return true;
    return false;
}

std::wstring g_relaunch;
void relaunch_at_exit()
{
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    std::wstring cmd = g_relaunch;
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

// ---------------------------------------------------------------------------------------------------
// The document.
// ---------------------------------------------------------------------------------------------------
std::string value_rml(const Row& r)
{
    std::string v;
    if (r.kind == INFO) v = esc(r.info);
    else if (r.kind == BIND) {
        const int i = (int)(&r - g_rows.data());
        if (i == g_listen)
            v = "<span class=\"listen\">" + esc(tr(r.pad ? "bind.press_button" : "bind.press_key")) + " (" + std::to_string(g_listen_left) + ")</span>";
        else if (r.pad) v = esc(g_pb[r.act] < 0 ? "-" : kSrc[g_pb[r.act]][1]);
        else
            for (int k = 0; k < PORT_BIND_KEYS; k++)
                v += "<span class=\"slot" + std::string(k == g_slot ? " sel" : "") + "\">" + esc(key_label(g_kb[r.act][k])) + "</span>";
    }
    else if (r.kind == CHOICE && r.vals.size() > 1) v = "‹ " + esc(label_of(r, r.cur)) + " ›";
    else if (r.kind != BUTTON) v = esc(label_of(r, r.cur));
    if (r.soon) v = (v.empty() ? "" : v + "  ") + "<span class=\"soon\">" + tr("soon") + "</span>";
    return v;
}

void refresh_row(int i)
{
    Row& r = g_rows[i];
    char id[16]; snprintf(id, sizeof id, "v%d", i);
    set_text(id, value_rml(r));
    snprintf(id, sizeof id, "r%d", i);
    set_class(id, "changed", r.restart && r.cur != r.boot);
}

void refresh_desc()
{
    if (g_sel < 0) return;
    const Row& r = g_rows[g_sel];
    set_text("desc_title", esc(row_title(r)));
    set_text("desc_text", esc(!r.desc.empty() ? r.desc + (strncmp(r.id, "mod.", 4) ? "" : " " + tr("desc.mod")) :
                              tr(r.kind == BIND ? (r.pad ? "desc.pad_bind" : "desc.kb_bind") : std::string("desc.") + r.id)));
    if (r.kind == BIND) {
        std::string d;
        if (r.pad) d = kPadDefault[r.act] < 0 ? "-" : kSrc[kPadDefault[r.act]][1];
        else for (int k = 0; k < PORT_BIND_KEYS; k++) if (kKbDefault[r.act][k]) d += (d.empty() ? "" : ", ") + key_label(kKbDefault[r.act][k]);
        std::string o = tr("note.default");
        const size_t p = o.find("%s");
        if (p != std::string::npos) o.replace(p, 2, d.empty() ? "-" : d);
        const bool fresh = now_s() < g_bind_note_until;
        set_text("desc_note", esc(fresh ? g_bind_note : o));
        set_class("desc_note", "warn", fresh);
        set_class("sview", "shown", false);
        return;
    }
    std::string note;
    bool warn = false;
    if (!strcmp(r.id, "render_height") && now_s() < g_rh_err_until) { note = tr("note.rh_failed"); warn = true; }
    else if (!strcmp(r.id, "display_mode") && now_s() < g_excl_err_until) { note = tr("note.excl_failed"); warn = true; }
    else if (r.soon) { note = tr("soon"); warn = true; }
    else if (r.restart && r.cur != r.boot) { note = tr("note.pending"); warn = true; }
    else if (r.restart) note = tr("note.restart");
    if (!r.soon && r.orig >= 0 && r.kind != BUTTON && strncmp(r.id, "stick_", 6)) {
        std::string o = tr("note.original");
        const size_t p = o.find("%s");
        if (p != std::string::npos) o.replace(p, 2, label_of(r, r.orig));
        note = note.empty() ? o : note + "<br/>" + esc(o);
    } else note = esc(note);
    set_text("desc_note", note);
    set_class("desc_note", "warn", warn);
    set_class("sview", "shown", !strncmp(r.id, "stick_", 6));
}

// The live stick (0166): the dead zone and the leave-the-centre circles from the rows, and the left stick of
// port 0 as it is, before any of the filtering. 200 dp square, the stick's travel is the outer circle.
void update_sview()
{
    Rml::Element* v = el("sview");
    if (!v || !v->IsClassSet("shown")) return;
    const float R = 100.0f;
    const float dz = stick_value("stick_deadzone"), lv = dz + stick_value("stick_hysteresis");
    auto circle = [&](const char* id, float r) {
        if (Rml::Element* e = el(id)) {
            const float d = (r > 1 ? 1 : r) * R;
            e->SetProperty("left", std::to_string(R - d) + "dp"); e->SetProperty("top", std::to_string(R - d) + "dp");
            e->SetProperty("width", std::to_string(2 * d) + "dp"); e->SetProperty("height", std::to_string(2 * d) + "dp");
        }
    };
    circle("sv_dz", dz); circle("sv_leave", lv);
    float x = 0, y = 0;
    pad_view_stick(&x, &y);
    const float r = sqrtf(x * x + y * y);
    if (Rml::Element* d = el("sv_dot")) {
        d->SetProperty("left", std::to_string(R + x * R - 7) + "dp"); d->SetProperty("top", std::to_string(R + y * R - 7) + "dp");
        d->SetClass("out", r >= lv);
    }
    char t[48]; snprintf(t, sizeof t, "%.0f %%", r * 100.0f);
    set_text("sv_txt", t);
}

void focus_row(int i, bool scroll)
{
    char id[16];
    const int was = g_sel;
    if (g_sel >= 0) { snprintf(id, sizeof id, "r%d", g_sel); set_class(id, "focus", false); }
    g_sel = i;
    snprintf(id, sizeof id, "r%d", i);
    if (Rml::Element* e = el(id)) {
        e->SetClass("focus", true);
        if (scroll) e->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
    }
    refresh_desc();
    if (was != i && g_mode == M_PANEL) restart_class("desc", "swap");   // the description fades in anew
    if (g_trace) fprintf(stderr, "[menu] focus %s at %.3f s\n", g_rows[i].id, now_s() - g_boot_s);
}

int first_row(int tab)
{
    for (size_t i = 0; i < g_rows.size(); i++) if (g_rows[i].tab == tab) return (int)i;
    return -1;
}

void bind_stop(const char* why);
void select_tab(int t, bool cascade)
{
    bind_stop("tab changed");
    g_tab = t;
    char id[16];
    for (int k = 0; k < T_COUNT; k++) {
        snprintf(id, sizeof id, "t%d", k); set_class(id, "sel", k == t);
        snprintf(id, sizeof id, "l%d", k); set_class(id, "shown", k == t);
    }
    focus_row(first_row(t), true);
    if (!cascade) return;
    // Rows come in one after another: 20 ms apart, 0.3 s at most in all.
    int n = 0;
    for (size_t i = 0; i < g_rows.size(); i++) {
        if (g_rows[i].tab != t) continue;
        snprintf(id, sizeof id, "r%d", (int)i);
        if (Rml::Element* e = el(id)) {
            const Rml::Property from(0.0f, Rml::Unit::NUMBER);
            const float delay = n * 0.02f > 0.16f ? 0.16f : n * 0.02f;
            e->Animate("opacity", Rml::Property(1.0f, Rml::Unit::NUMBER), 0.14f, Rml::Tween{Rml::Tween::Cubic, Rml::Tween::Out},
                       1, false, delay, &from);
            n++;
        }
    }
}

void bar_focus(int b)
{
    g_bar_sel = b;
    char id[8];
    for (int k = 0; k < B_COUNT; k++) { snprintf(id, sizeof id, "b%d", k); set_class(id, "focus", k == b && g_mode == M_BAR); }
}

bool make_document()
{
    g_ctx = Rml::CreateContext("refine_menu", Rml::Vector2i(1280, 720));
    if (!g_ctx) return false;
    g_doc = g_ctx->LoadDocument(g_dir + "ui\\menu.rml");
    if (!g_doc) return false;
    std::string tabs, lists, bar = "<div id=\"bar_ink\"></div>";
    for (int t = 0; t < T_COUNT; t++) {
        tabs += "<div class=\"tab\" id=\"t" + std::to_string(t) + "\"><span class=\"tno\">" + (t < 9 ? "0" : "") + std::to_string(t + 1) + "</span>" + esc(tr(std::string("tab.") + kTabKeys[t])) + "</div>";
        lists += "<div class=\"tablist\" id=\"l" + std::to_string(t) + "\"><div class=\"pill\" id=\"pill" + std::to_string(t) + "\"></div>";
        const char* sect = nullptr;
        for (size_t i = 0; i < g_rows.size(); i++) {
            const Row& r = g_rows[i];
            if (r.tab != t) continue;
            if (!sect || strcmp(sect, r.sect)) {
                sect = r.sect;
                lists += "<div class=\"sect\">" + esc(tr(std::string("sect.") + sect)) + "</div>";
            }
            const std::string n = std::to_string(i);
            lists += "<div class=\"row" + std::string(r.soon ? " off" : "") + "\" id=\"r" + n + "\"><span class=\"rlabel\">" +
                     esc(row_title(r)) + "</span><span class=\"rval\" id=\"v" + n + "\">" + value_rml(r) +
                     "</span></div>";
        }
        lists += "</div>";
    }
    for (int b = 0; b < B_COUNT; b++) {
        bar += "<div class=\"btab" + std::string(kBarSoon[b] ? " off" : "") + "\" id=\"b" + std::to_string(b) + "\">" +
               esc(tr(std::string("bar.") + kBarKeys[b])) +
               (kBarSoon[b] ? "<span class=\"soon\">" + tr("soon") + "</span>" : "") + "</div>";
    }
    set_text("tabs", tabs);
    set_text("list", lists);
    set_text("bar_tabs", bar);
    std::string help, h = tr("menu.help") + "|";
    for (size_t at = 0, bar_at; (bar_at = h.find('|', at)) != std::string::npos; at = bar_at + 1)
        help += "<span class=\"hk\">" + esc(h.substr(at, bar_at - at)) + "</span>";
    set_text("help", help);
    return true;
}

// Boot thread, after the CPU side: once the device is there, the pipeline and a dry render (no command
// list: geometry and glyph atlases are made, the textures wait for the first real frame).
void prepare_gpu()
{
    if (WaitForSingleObject(s_device_event, 30000) != WAIT_OBJECT_0) {
        fprintf(stderr, "[menu] no device after 30 s: the menu builds on its first open\n");
        return;
    }
    const double t0 = now_s();
    // Vulkan: the executor makes its side on its first overlay pass; the textures made here wait for it.
    if (!g_vk && !g_render.Init(s_device, s_rtv_format, s_frame_count)) { fprintf(stderr, "[menu] pipeline FAILED\n"); return; }
    // At the window's size and scale, so the glyphs made now are the ones the first open draws.
    RECT rc = {0, 0, 1280, 720};
    if (s_hwnd) GetClientRect(s_hwnd, &rc);
    const int h = rc.bottom - rc.top > 0 ? rc.bottom - rc.top : 720;
    g_ctx->SetDimensions(Rml::Vector2i(rc.right - rc.left > 0 ? rc.right - rc.left : 1280, h));
    g_ctx->SetDensityIndependentPixelRatio((float)h / 1080.0f * g_scale);
    g_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    for (const char* id : {"dim", "bar", "panel", "confirm", "fps"}) set_class(id, "shown", true);
    for (int t = 0; t < T_COUNT; t++) { char id[8]; snprintf(id, sizeof id, "l%d", t); set_class(id, "shown", true); }
    g_ctx->Update();
    g_ctx->Render();   // cl_ is null: nothing recorded
    for (const char* id : {"dim", "bar", "panel", "confirm", "fps"}) set_class(id, "shown", false);
    g_doc->Hide();
    g_ctx->Update();
    fprintf(stderr, "[menu] GPU side ready off the frame in %.1f ms\n", (now_s() - t0) * 1e3);
    InterlockedExchange(&s_gpu_ready, 1);
}

DWORD WINAPI boot_thread(LPVOID)
{
    const double t0 = now_s();
    Rml::SetSystemInterface(&g_system);
    Rml::SetRenderInterface(&g_render_either);
    bool ok = !g_lang.empty() && Rml::Initialise();   // the language file: port_menu_start
    for (const char* f : {"ChakraPetch-Medium.ttf", "ChakraPetch-SemiBold.ttf", "ChakraPetch-BoldItalic.ttf", "IBMPlexMono-Medium.ttf", "DelaGothicOne-Regular.ttf"})
        ok = ok && Rml::LoadFontFace(g_dir + "ui\\fonts\\" + f);
    ok = ok && make_document();
    fprintf(stderr, "[menu] boot %s in %.1f ms\n", ok ? "ready" : "FAILED", (now_s() - t0) * 1e3);
    if (ok) prepare_gpu();
    InterlockedExchange(&s_ready, ok ? 1 : -1);
    SetEvent(s_ready_event);
    if (ok && g_show_fps) InterlockedExchange(&g_port_overlay_active, 1);
    return 0;
}

void device_ready(void* dev, uint32_t fmt, uint32_t frames)
{
    s_device = (ID3D12Device*)dev; s_rtv_format = fmt; s_frame_count = frames;
    g_vk = dev == nullptr;   // 0188: no D3D12 device = the Vulkan executor draws
    SetEvent(s_device_event);
}

// ---------------------------------------------------------------------------------------------------
// Opening, closing, and what each control does. Render thread.
// ---------------------------------------------------------------------------------------------------
void gate_on()
{
    for (int p = 0; p < 4; p++) InterlockedExchange(&s_last[p], 0xFFFF);   // held now: no press
    InterlockedExchange(&s_edges, 0);
    InterlockedExchange(&s_closing, 0);
    InterlockedExchange(&s_shown, 1);              // before the gate: the watchdog in watch() reads both
    InterlockedExchange(&g_port_overlay_gate, 1);
}

void restart_class(const char* id, const char* cls)
{
    if (Rml::Element* p = el(id)) { p->SetClass(cls, false); g_ctx->Update(); p->SetClass(cls, true); }
}
void restart_anim(const char* id) { restart_class(id, "open"); }

void open_bar()
{
    gate_on();
    g_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    show_el("bar");
    restart_anim("bar");
    g_mode = M_BAR;
    bar_focus(g_bar_sel == B_QUIT ? B_QUIT : B_SETTINGS);
    g_opened_at = now_s();
    // In a song, the game's own pause first (its START), so the song clock, the audio and the judgement
    // stop the way the game stops them. In a song = the song clock moved in the last 0.3 s and the game
    // is not stopped. ASSUMPTION: outside songs (menus, title) that clock does not move.
    const bool in_song = port_in_song() && g_opened_at - s_song_seen_at < 0.3 && guest_f32(kGameSpeed) != 0.0f;
    if (in_song) {
        g_inject_until = g_opened_at + 0.12;   // before the START: watch() reads it to let go
        InterlockedExchange(&s_inject_start, PAD_START);
        InterlockedExchange(&g_port_overlay_inject, s_inject_start | s_inject_mouse);
        g_pause_check_at = g_opened_at + 0.6;
    }
    fprintf(stderr, "[menu] bar open at %.3f s%s\n", g_opened_at - g_boot_s, in_song ? ", in a song: START for the game's pause" : "");
}

void open_panel()
{
    g_mode = M_PANEL;
    {   // a device plugged in since the last look shows up
        const int i = find_row("audio_device");
        audio_rows_fill();
        refresh_row(i);
    }
    bar_focus(g_bar_sel);
    show_el("dim");
    show_el("panel");
    restart_anim("panel");
    select_tab(g_tab, true);
    if (const char* rid = getenv("PORT_MENU_ROW")) { const int ri = find_row(rid); if (ri >= 0 && g_rows[ri].tab == g_tab) focus_row(ri, true); }   // diagnostic
    g_opened_at = now_s();
    fprintf(stderr, "[menu] panel open (tab %s)\n", kTabKeys[g_tab]);
}

void bind_stop(const char* why);
void close_all()
{
    bind_stop("menu closed");
    for (const char* id : {"dim", "bar", "panel", "confirm"}) hide_el(id);   // the document hides once they are gone (draw)
    g_mode = M_CLOSED;
    InterlockedExchange(&s_shown, 0);
    s_closed_at = now_s();
    InterlockedExchange(&s_closing, 1);   // the gate drops once every button is up (pad hook)
    fprintf(stderr, "[menu] closed at %.3f s\n", now_s() - g_boot_s);
}

void open_confirm(Confirm c)
{
    g_confirm = c;
    g_mode = M_CONFIRM;
    g_conf_sel = c == C_RESTART ? 0 : 1;   // Quit asks with No first
    set_text("confirm_text", esc(tr(c == C_QUIT ? "confirm.quit" : "confirm.restart")));
    set_text("confirm_note", c == C_RESTART ? esc(tr("confirm.restart_note")) : "");
    set_text("cyes", esc(tr("confirm.yes")));
    set_text("cno", esc(tr("confirm.no")));
    show_el("confirm");
    set_class("cyes", "focus", g_conf_sel == 0);
    set_class("cno", "focus", g_conf_sel == 1);
}

void panel_back()
{
    hide_el("panel");
    hide_el("dim");
    g_mode = g_launcher_on ? M_LAUNCHER : M_BAR;
    bar_focus(B_SETTINGS);
    if (pending_restart()) open_confirm(C_RESTART);
}

void best_answer(bool yes);
void confirm_answer(bool yes)
{
    hide_el("confirm", 0.16);
    if (g_confirm == C_BEST) { best_answer(yes); return; }
    if (g_confirm == C_QUIT) {
        if (!yes) { g_mode = M_BAR; bar_focus(B_QUIT); return; }
        // ps3_host_quit sends REQUEST_EXITGAME and only ends the process once no save is being written.
        if (ps3_savedata_busy()) {
            set_text("confirm_note", esc(tr("confirm.saving")));
            show_el("confirm");
        }
        g_quitting = true;
        ps3_host_quit("Quit from the port's quick bar");
        return;
    }
    g_mode = g_launcher_on ? M_LAUNCHER : M_BAR;
    bar_focus(B_SETTINGS);
    if (yes && g_launcher_on) {
        // The game has not started: there is nothing to save or to ask to exit. Same command line.
        g_relaunch = GetCommandLineW();
        fprintf(stderr, "[menu] restart from the start screen\n");
        relaunch_at_exit();
        ExitProcess(0);
    }
    if (yes) {
        g_relaunch = GetCommandLineW();
        atexit(relaunch_at_exit);
        g_quitting = true;
        ps3_host_quit("Restart from the port's settings");
    }
}

void refresh_binds();
void change(int dir)
{
    if (g_sel < 0) return;
    Row& r = g_rows[g_sel];
    if (r.kind == BIND) {
        if (!r.pad) { g_slot = (g_slot + PORT_BIND_KEYS + dir) % PORT_BIND_KEYS; refresh_binds(); }
        return;
    }
    if (r.soon || (r.kind != CHOICE && r.kind != TOGGLE) || r.vals.size() < 2) return;
    const int n = (int)r.vals.size();
    r.cur = r.kind == TOGGLE ? 1 - r.cur : (r.cur + dir + n) % n;
    if (r.ini) ini_write(r.ini, r.vals[r.cur]);
    if (!r.restart) apply_live(r);
    refresh_row(g_sel);
    refresh_desc();
    char vid[16]; snprintf(vid, sizeof vid, "v%d", g_sel);
    restart_class(vid, "bump");   // the new value gives a small pop
}


// ---------------------------------------------------------------------------------------------------
// 6b+6c p6 E: "Choose best for my PC". Reads the GPU and its VRAM (DXGI), the logical cores and the
// monitor's refresh rate, measures about 5 s of the menu (GPU time of the frame from the renderer's
// timestamps, and the frame intervals) and proposes a render height and a frame rate that leave at
// least 30 % of the frame budget free. Never applied without the answer to Apply / Cancel.
// ---------------------------------------------------------------------------------------------------
extern "C" {
extern int g_frame_prof_on;
extern struct { double wait, cmd, exec, present, next, gpu; unsigned long long draws, renders; } g_frame_prof;
}
struct BestInputs { std::string gpu; double gpu_ms; double vram_mb; int cores; double hz; int cur_h; double ft_mean, ft_p99; };
struct BestProposal { int h, fps; double gpu_p99, budget; };
constexpr double kBestMargin = 0.70;   // the GPU may use 70 % of the frame: 30 % free
constexpr int kBestFps[6] = {30, 60, 120, 144, 180, 240};
constexpr int kBestH[5] = {720, 900, 1080, 1440, 2160};

// Pure: the same inputs always give the same proposal (PORT_MENU_BEST_TABLE=1 prints a table of them).
// ASSUMPTION: the song scene costs 1.10 ms mean / 1.47 ms p99 of GPU where the menu costs 0.62 (RTX 3060,
// 720p, docs/perf.md 2026-11-07) and grows with the height as h^1.2 (720p 1.10 ms -> 4K 4.07 ms mean,
// 6a.4). One GPU only: on another one the ratios are a guess, which is why the user answers Apply / Cancel.
// ASSUMPTION: fewer than 6 logical cores cannot hold more than 60 fps, fewer than 8 more than 120.
BestProposal best_propose(const BestInputs& in)
{
    const double song_p99 = (std::max)(in.gpu_ms, 0.05) * (1.47 / 0.62);
    auto gpu_at = [&](int h) { return song_p99 * std::pow(h / (double)(std::max)(in.cur_h, 360), 1.2); };
    const int cpu_cap = in.cores < 6 ? 60 : in.cores < 8 ? 120 : 240;
    const int cap = (std::min)(cpu_cap, (int)std::floor(in.hz + 1.0));
    const int vram_h = in.vram_mb < 3000 ? 1080 : in.vram_mb < 6000 ? 1440 : 2160;
    BestProposal best{720, 30, gpu_at(720), 1000.0 / 30 * kBestMargin};
    bool found = false;
    for (int fi = 5; fi >= 0 && !found; fi--) {
        if (kBestFps[fi] > cap && fi > 0) continue;
        const double budget = 1000.0 / kBestFps[fi] * kBestMargin;
        int h = 0;
        for (int hi = 0; hi < 5; hi++) if (kBestH[hi] <= vram_h && gpu_at(kBestH[hi]) <= budget) h = kBestH[hi];
        if (h >= 1080 || (h > 0 && fi == 0)) { best = {h, kBestFps[fi], gpu_at(h), budget}; found = true; }
        else if (fi == 0) best = {720, 30, gpu_at(720), budget};
    }
    return best;
}

void best_table()
{
    const struct { const char* n; double gpu_ms, vram; int cores; double hz; } t[] = {
        {"RTX 3060 / Ryzen 5 5600G / 179 Hz (developer PC)", 0.62, 12288, 12, 179}, {"RTX 3060, same, 60 Hz monitor", 0.62, 12288, 12, 60},
        {"GTX 1050 2 GB / 4 cores / 60 Hz", 1.80, 2048, 4, 60}, {"Intel UHD (shared) / 8 threads / 60 Hz", 4.0, 1024, 8, 60},
        {"RTX 4090 / 16 cores / 240 Hz", 0.20, 24576, 32, 240}};
    for (auto& x : t) {
        const BestProposal p = best_propose({"", x.gpu_ms, x.vram, x.cores, x.hz, 720, 0, 0});
        fprintf(stderr, "[best] %-44s gpu %.2f ms, %5.0f MB, %2d threads, %3.0f Hz -> %dp at %d FPS (song GPU p99 %.2f of %.2f ms)\n",
                x.n, x.gpu_ms, x.vram, x.cores, x.hz, p.h, p.fps, p.gpu_p99, p.budget);
    }
}

int g_best_state;   // 0 idle, 1 measuring, 2 proposal shown
double g_best_t0, g_best_auto = -1;
double g_best_snap[6]; unsigned long long g_best_renders0;
int g_best_prof_prev;
std::vector<double> g_best_ft;
double g_best_last_present;
BestInputs g_best_in; BestProposal g_best_p;
std::string g_best_override;   // PORT_MENU_BEST_TEST="gpu_ms:vram_mb:cores:hz:height": inputs instead of measuring

std::string to_utf8(const wchar_t* w) { return narrow(w); }

void best_hw(BestInputs& in)
{
    in.gpu = "?"; in.vram_mb = 0;
    IDXGIFactory4* f = nullptr;
    if (s_device && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        IDXGIAdapter1* a = nullptr;
        if (SUCCEEDED(f->EnumAdapterByLuid(s_device->GetAdapterLuid(), IID_PPV_ARGS(&a)))) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(a->GetDesc1(&d))) { in.gpu = to_utf8(d.Description); in.vram_mb = d.DedicatedVideoMemory / 1048576.0; }
            a->Release();
        }
        f->Release();
    }
    in.cores = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    in.hz = 60;
    if (s_hwnd) {
        MONITORINFOEXW mi; mi.cbSize = sizeof mi;
        DEVMODEW dm; dm.dmSize = sizeof dm;
        if (GetMonitorInfoW(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST), &mi) &&
            EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) in.hz = dm.dmDisplayFrequency;
    }
}

void open_confirm_text(const std::string& rml, const char* yes, const char* no)
{
    g_confirm = C_BEST;
    g_mode = M_CONFIRM;
    g_conf_sel = 0;
    set_text("confirm_text", rml);
    set_text("confirm_note", "");
    set_text("cyes", esc(yes));
    set_text("cno", esc(no));
    set_class("confirm", "shown", true);
    set_class("cyes", "focus", true);
    set_class("cno", "focus", false);
}

void best_start()
{
    if (g_best_state) return;
    g_best_state = 1;
    g_best_t0 = now_s();
    g_best_ft.clear(); g_best_last_present = 0;
    g_port_gpu_prof = 1;   // the renderer's GPU timestamps; off again when the measure ends
    g_best_snap[0] = g_frame_prof.gpu; g_best_snap[1] = g_frame_prof.wait + g_frame_prof.cmd + g_frame_prof.exec + g_frame_prof.present + g_frame_prof.next;
    g_best_renders0 = g_frame_prof.renders;
    open_confirm_text(esc(tr("best.measuring")), "...", tr("confirm.cancel").c_str());
    fprintf(stderr, "[best] measuring 5 s\n");
}

void best_apply()
{
    struct { const char* id; std::string v; } t[] = {{"render_height", std::to_string(g_best_p.h)}, {"fps_target", std::to_string(g_best_p.fps)}};
    for (auto& x : t) {
        const int i = find_row(x.id);
        Row& r = g_rows[i];
        for (size_t k = 0; k < r.vals.size(); k++) if (r.vals[k] == x.v) r.cur = (int)k;
        if (r.ini) ini_write(r.ini, r.vals[r.cur]);
        apply_live(r);
        refresh_row(i);
    }
    refresh_desc();
    fprintf(stderr, "[best] applied %dp at %d FPS\n", g_best_p.h, g_best_p.fps);
}

void best_answer(bool yes)
{
    const bool was_ready = g_best_state == 2;
    if (g_best_state == 1) g_port_gpu_prof = 0;
    g_best_state = 0;
    g_mode = M_PANEL;
    if (yes && was_ready) best_apply();
    else fprintf(stderr, "[best] %s\n", was_ready ? "cancelled: nothing changed" : "measure cancelled");
}

// Every frame the menu draws.
void best_tick(double now)
{
    if (g_best_auto >= 0 && g_best_state == 0 && g_mode == M_PANEL && now - g_boot_s >= g_best_auto) { g_best_auto = -1; best_start(); }
    if (g_best_state != 1) return;
    if (g_best_last_present > 0) g_best_ft.push_back(now - g_best_last_present);
    g_best_last_present = now;
    if (now - g_best_t0 < 5.0) return;
    BestInputs in{};
    best_hw(in);
    in.cur_h = atoi(val("render_height").c_str());
    const unsigned long long n = (std::max<unsigned long long>)(g_frame_prof.renders - g_best_renders0, 1);
    in.gpu_ms = (g_frame_prof.gpu - g_best_snap[0]) / (double)n * 1e3;
    std::sort(g_best_ft.begin(), g_best_ft.end());
    if (!g_best_ft.empty()) {
        double sum = 0; for (double v : g_best_ft) sum += v;
        in.ft_mean = sum / g_best_ft.size() * 1e3;
        in.ft_p99 = g_best_ft[(std::min)(g_best_ft.size() - 1, (size_t)(g_best_ft.size() * 0.99))] * 1e3;
    }
    g_port_gpu_prof = 0;
    if (!g_best_override.empty()) {   // diagnostic: the author's PC without being on it
        double a, b, d; int c, e;
        if (sscanf(g_best_override.c_str(), "%lf:%lf:%d:%lf:%d", &a, &b, &c, &d, &e) == 5) { in.gpu_ms = a; in.vram_mb = b; in.cores = c; in.hz = d; in.cur_h = e; in.gpu = "(PORT_MENU_BEST_TEST)"; }
    }
    g_best_in = in;
    g_best_p = best_propose(in);
    g_best_state = 2;
    char b1[256], b2[256], b3[256];
    snprintf(b1, sizeof b1, "%s - %.0f GB VRAM - %d threads - %.0f Hz", in.gpu.c_str(), in.vram_mb / 1024.0, in.cores, in.hz);
    snprintf(b2, sizeof b2, tr("best.measured").c_str(), in.gpu_ms, in.ft_mean, in.ft_p99);
    snprintf(b3, sizeof b3, tr("best.proposal").c_str(), g_best_p.h, g_best_p.fps);
    open_confirm_text("<span class=\"small\">" + esc(b1) + "<br/>" + esc(b2) + "</span><br/><br/>" + esc(b3), tr("best.apply").c_str(), tr("confirm.cancel").c_str());
    fprintf(stderr, "[best] %s | %s -> %dp at %d FPS (song GPU p99 %.2f ms of %.2f ms; was %dp, menu frame time p99 %.1f ms)\n",
            b1, b2, g_best_p.h, g_best_p.fps, g_best_p.gpu_p99, g_best_p.budget, in.cur_h, in.ft_p99);
}

void refresh_binds()
{
    for (size_t i = 0; i < g_rows.size(); i++) if (g_rows[i].kind == BIND) refresh_row((int)i);
    refresh_desc();
}
void bind_note(const char* key, int other)
{
    g_bind_note = tr(key);
    const size_t p = g_bind_note.find("%s");
    if (p != std::string::npos) g_bind_note.replace(p, 2, tr(std::string("act.") + kActKeys[other]));
    g_bind_note_until = now_s() + 5.0;
}
void bind_start(int i)
{
    g_listen = i;
    g_listen_at = now_s();
    g_listen_left = (int)kListenTime;
    g_listen_armed = false;
    g_listen_prev = 0;
    InterlockedExchange(&s_listen_vk, 0);
    InterlockedExchange(&s_listen_esc, 0);
    InterlockedExchange(&s_listen_kb, g_rows[i].pad ? 2 : 1);
    refresh_row(i);
    fprintf(stderr, "[menu] binding %s: waiting\n", g_rows[i].id);
}
void bind_stop(const char* why)
{
    if (g_listen < 0) return;
    const int i = g_listen;
    g_listen = -1;
    InterlockedExchange(&s_listen_kb, 0);
    g_listen_mute = true;
    if (why) fprintf(stderr, "[menu] binding %s: %s\n", g_rows[i].id, why);
    refresh_binds();
}
void bind_key(int a, int slot, int vk)
{
    int other = -1;
    for (int b = 0; b < PORT_BIND_COUNT; b++)
        for (int t = 0; t < PORT_BIND_KEYS; t++)
            if ((b != a || t != slot) && g_kb[b][t] == vk) {
                g_kb[b][t] = 0;
                if (b != a) { other = b; ini_write(kKbIds[b], kb_value(b), "bindings"); }
            }
    g_kb[a][slot] = vk;
    ini_write(kKbIds[a], kb_value(a), "bindings");
    binds_apply();
    if (other >= 0) bind_note("note.bind_taken", other);
    fprintf(stderr, "[menu] binding %s: key %d = %s\n", kKbIds[a], slot + 1, key_name(vk).c_str());
}
void bind_pad(int a, int src)
{
    int other = -1;
    for (int b = 0; b < PORT_BIND_PAD_COUNT; b++) if (b != a && g_pb[b] == src) other = b;
    if (other >= 0) { g_pb[other] = g_pb[a]; ini_write(kPadIds[other], pad_value(other), "bindings"); }
    g_pb[a] = src;
    ini_write(kPadIds[a], pad_value(a), "bindings");
    binds_apply();
    if (other >= 0) bind_note("note.bind_swapped", other);
    fprintf(stderr, "[menu] binding %s = %s\n", kPadIds[a], kSrc[src][0]);
}
void bind_clear()
{
    if (g_sel < 0 || g_listen >= 0) return;
    const Row& r = g_rows[g_sel];
    if (r.kind != BIND) return;
    if (r.pad) { g_pb[r.act] = -1; ini_write(kPadIds[r.act], "none", "bindings"); }
    else { g_kb[r.act][g_slot] = 0; ini_write(kKbIds[r.act], kb_value(r.act), "bindings"); }
    binds_apply();
    refresh_binds();
}
// Every frame while the panel is open and a row waits.
void bind_tick(double now, uint32_t held)
{
    const Row& r = g_rows[g_listen];
    if (now - g_listen_at >= kListenTime) { bind_stop("no press, cancelled"); return; }
    const int left = (int)ceil(kListenTime - (now - g_listen_at));
    if (left != g_listen_left) { g_listen_left = left; refresh_row(g_listen); }
    const unsigned raw = pad_raw_sources() & ~kSrcReserved;
    // The key or button that chose the row has to come up first.
    if (!g_listen_armed) {
        if (raw == 0 && (r.pad || held == 0)) g_listen_armed = true;
        if (!r.pad) InterlockedExchange(&s_listen_vk, 0);
        g_listen_prev = raw;
        return;
    }
    const unsigned fresh = g_listen_armed ? raw & ~g_listen_prev : 0;
    g_listen_prev = raw;
    if (r.pad) {
        if (InterlockedExchange(&s_listen_esc, 0)) { bind_stop("Esc, cancelled"); return; }
        if (!fresh) return;
        int src = 0;
        while (!(fresh >> src & 1)) src++;
        bind_pad(r.act, src);
        bind_stop(nullptr);
    } else {
        if (fresh) { bind_stop("controller button, cancelled"); return; }
        const int vk = (int)InterlockedExchange(&s_listen_vk, 0);
        if (!vk) return;
        if (!key_bindable(vk)) return;   // F1 never gets here; the Windows keys are left alone
        bind_key(r.act, g_slot, vk);
        bind_stop(nullptr);
    }
}

void choose_f2nd_folder();
void activate()
{
    if (g_listen >= 0) { bind_stop("click, cancelled"); return; }
    if (g_sel < 0) return;
    Row& r = g_rows[g_sel];
    if (r.soon) return;
    if (r.kind == CHOICE || r.kind == TOGGLE) { change(+1); return; }
    if (r.kind == BIND) { bind_start(g_sel); return; }
    if (r.kind != BUTTON) return;
    if (!strcmp(r.id, "kb_reset") || !strcmp(r.id, "pad_reset")) {
        const bool pad = r.id[0] == 'p';
        if (pad) memcpy(g_pb, kPadDefault, sizeof g_pb); else memcpy(g_kb, kKbDefault, sizeof g_kb);
        for (int a = 0; a < (pad ? (int)PORT_BIND_PAD_COUNT : (int)PORT_BIND_COUNT); a++)
            ini_write(pad ? kPadIds[a] : kKbIds[a], pad ? pad_value(a) : kb_value(a), "bindings");
        binds_apply();
        refresh_binds();
        fprintf(stderr, "[menu] %s: defaults back\n", r.id);
        return;
    }
    if (!strcmp(r.id, "open_folder")) {
        CreateDirectoryW(g_data_dir.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", g_data_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (!strcmp(r.id, "choose_f2nd")) {
        choose_f2nd_folder();
    } else if (!strcmp(r.id, "open_mods")) {
        const std::string d = mods_dir(g_dir);
        CreateDirectoryA(d.c_str(), nullptr);
        ShellExecuteA(nullptr, "open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (!strcmp(r.id, "stick_reset")) {
        for (const char* id : {"stick_deadzone", "stick_hysteresis", "stick_rearm"}) {
            const int i = find_row(id);
            Row& x = g_rows[i];
            x.cur = x.orig;
            ini_write(x.ini, x.vals[x.cur]);
            refresh_row(i);
        }
        apply_stick();
        refresh_desc();
    } else if (!strcmp(r.id, "best")) {
        best_start();
    } else if (!strcmp(r.id, "restore")) {
        for (const char* id : {"display_mode", "vsync", "renderer", "render_height", "fps_target", "menu_fps"}) {
            const int i = find_row(id);
            Row& x = g_rows[i];
            if (x.cur == x.orig) continue;
            x.cur = x.orig;
            ini_write(x.ini, x.vals[x.cur]);
            if (!x.restart) apply_live(x);
            refresh_row(i);
        }
        refresh_desc();
    }
}

int step_row(int dir)
{
    int i = g_sel;
    for (;;) {
        i += dir;
        if (i < 0 || i >= (int)g_rows.size() || g_rows[i].tab != g_tab) return g_sel;
        return i;
    }
}

void bar_step(int dir)
{
    int b = g_bar_sel;
    do { b = (b + dir + B_COUNT) % B_COUNT; } while (kBarSoon[b]);
    bar_focus(b);
}

void bar_activate()
{
    if (g_bar_sel == B_SETTINGS) open_panel();
    else if (g_bar_sel == B_QUIT) open_confirm(C_QUIT);
}

// A folder from the Windows picker, kept in refine.ini under `key`. False when cancelled.
bool pick_folder(const wchar_t* title, const char* key)
{
    BROWSEINFOW bi = {};
    bi.hwndOwner = s_hwnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bool ok = false;
    if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) ok = ini_write(key, narrow(path));
        CoTaskMemFree(pidl);
    }
    return ok;
}

// "Choose game folder": shown only when the disc's PARAM.SFO could not be read. It keeps the choice in
// refine.ini (game_folder). ASSUMPTION: main.cpp does not read game_folder yet (part 4); today the game
// folder comes from the command line.
void launcher_choose_folder()
{
    pick_folder(L"Choose the folder of your Project DIVA F disc (the one with PS3_GAME)", "game_folder");
}

// Plan 6h S1: the F 2nd folder (F1 > Interface > Games). The start screen's game choice shows from the next start.
void choose_f2nd_folder()
{
    if (!pick_folder(L"Choose the folder of your Project DIVA F 2nd disc (the one with PS3_GAME)", "f2nd_folder")) return;
    f2nd_probe();
    f2nd_rows_fill();
    refresh_row(find_row("f2nd_folder"));
    refresh_desc();
}

// Plan 6h S1: whose menu PLAY opens, on the start screen when both games are set up. 0 F, 1 F 2nd.
int g_lgame;
void launcher_games_fill()
{
    set_class("l_game", "gone", !g_f2nd.disc);
    if (!g_f2nd.disc) return;
    const std::string dot = " \xC2\xB7 ";
    std::string h = "<div class=\"gopt" + std::string(g_lgame == 0 ? " sel" : "") + "\" id=\"g0\">Project DIVA F<br/><span class=\"gsub\">" +
                    esc(cellGame_title_id_str()) + dot + esc(tr("launcher.ready")) + "</span></div>";
    h += "<div class=\"gopt" + std::string(g_lgame == 1 ? " sel" : "") + (g_f2nd.runnable ? "" : " off") + "\" id=\"g1\">Project DIVA F 2nd<br/>" +
         "<span class=\"gsub\">" + esc(g_f2nd.title_id) + dot + esc(tr(g_f2nd.runnable ? "launcher.ready" : "launcher.f2nd_pending")) + "</span></div>";
    set_text("l_game", h);
}
void launcher_toast(const std::string& rml)
{
    set_text("l_toast", rml);
    set_class("l_toast", "shown", true);
    g_toast_until = now_s() + 3.0;
}
void launcher_game(int k)
{
    if (g_launch_t >= 0 || !g_f2nd.disc || k == g_lgame) return;
    if (k == 1 && !g_f2nd.runnable) {
        launcher_toast(esc(tr("launcher.f2nd_toast")));
        fprintf(stderr, "[launcher] F 2nd chosen, but %s is not there: the menu stays F's\n", f2nd_exe().c_str());
        return;
    }
    g_lgame = k;
    ini_write("menu_game", k ? "f2nd" : "f");
    launcher_games_fill();
}

void launcher_focus(int i)
{
    if (g_launch_t >= 0) return;   // on its way out
    g_lsel = (i + 3) % 3;
    for (int k = 0; k < 3; k++) { char id[8]; snprintf(id, sizeof id, "m%d", k); set_class(id, "focus", k == g_lsel); }
}

void launcher_activate()
{
    if (g_launch_t >= 0) return;
    if (g_lsel == 0) { g_launch_t = now_s(); set_class("launcher", "launching", true); return; }
    if (g_lsel == 1) { g_bar_sel = B_SETTINGS; open_panel(); g_mode = M_PANEL; return; }
    ExitProcess(0);   // QUIT: the game has not started, nothing to save
}

// One logical press, from the pad (edges and auto-repeat), the keyboard (through cellPad) or the mouse.
void press(uint32_t b)
{
    switch (g_mode) {
    case M_LAUNCHER:
        if (b & PAD_UP) launcher_focus(g_lsel - 1);
        if (b & PAD_DOWN) launcher_focus(g_lsel + 1);
        if (b & PAD_LEFT) launcher_game(0);
        if (b & PAD_RIGHT) launcher_game(1);
        if (b & PAD_CROSS) launcher_activate();
        else if (b & PAD_START) { g_lsel = 0; launcher_activate(); }
        break;
    case M_BAR:
        if (b & (PAD_LEFT | PAD_L1)) bar_step(-1);
        if (b & (PAD_RIGHT | PAD_R1)) bar_step(+1);
        if (b & PAD_CROSS) bar_activate();
        else if (b & (PAD_CIRCLE | PAD_START)) close_all();
        break;
    case M_PANEL:
        if (b & PAD_UP) focus_row(step_row(-1), true);
        if (b & PAD_DOWN) focus_row(step_row(+1), true);
        if (b & PAD_LEFT) change(-1);
        if (b & PAD_RIGHT) change(+1);
        if (b & (PAD_L1 | PAD_R1)) select_tab((g_tab + ((b & PAD_R1) ? 1 : T_COUNT - 1)) % T_COUNT, true);
        if (b & PAD_SQUARE) bind_clear();
        if (b & PAD_CROSS) activate();
        else if (b & (PAD_CIRCLE | PAD_START)) panel_back();
        break;
    case M_CONFIRM:
        if (g_quitting) break;
        if (b & (PAD_LEFT | PAD_RIGHT)) {
            g_conf_sel = 1 - g_conf_sel;
            set_class("cyes", "focus", g_conf_sel == 0);
            set_class("cno", "focus", g_conf_sel == 1);
        }
        if (b & PAD_CROSS) confirm_answer(g_conf_sel == 0);
        else if (b & (PAD_CIRCLE | PAD_START)) confirm_answer(false);
        break;
    default: break;
    }
}

// The mouse: hovering gives the focus, a click selects, a click on a tab opens it.
int id_num(Rml::Element* e, char c)
{
    for (; e; e = e->GetParentNode()) {
        const Rml::String& id = e->GetId();
        if (id.size() > 1 && id[0] == c && isdigit((unsigned char)id[1])) return atoi(id.c_str() + 1);
    }
    return -1;
}
bool has_id(Rml::Element* e, const char* want)
{
    for (; e; e = e->GetParentNode()) if (e->GetId() == want) return true;
    return false;
}
class Mouse final : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& ev) override
    {
        Rml::Element* t = ev.GetTargetElement();
        const bool click = ev.GetId() == Rml::EventId::Click;
        if (g_mode == M_CONFIRM) {
            if (g_quitting) return;
            const int yes = has_id(t, "cyes") ? 1 : has_id(t, "cno") ? 0 : -1;
            if (yes < 0) return;
            g_conf_sel = yes ? 0 : 1;
            set_class("cyes", "focus", yes == 1);
            set_class("cno", "focus", yes == 0);
            if (click) confirm_answer(yes == 1);
            return;
        }
        if (g_mode == M_LAUNCHER) {
            if (click && has_id(t, "gfolder")) { launcher_choose_folder(); return; }
            if (const int g = id_num(t, 'g'); click && g >= 0 && g < 2) { launcher_game(g); return; }
            if (const int m = id_num(t, 'm'); m >= 0 && m < 3) { launcher_focus(m); if (click) launcher_activate(); }
            return;
        }
        if (const int b = id_num(t, 'b'); b >= 0 && !kBarSoon[b] && g_mode == M_BAR) {
            bar_focus(b);
            if (click) bar_activate();
            return;
        }
        if (g_mode != M_PANEL) return;
        if (click && has_id(t, "xclose")) { panel_back(); return; }
        if (const int k = id_num(t, 't'); k >= 0 && k < T_COUNT) { if (click && k != g_tab) select_tab(k, true); return; }
        if (const int r = id_num(t, 'r'); r >= 0 && r < (int)g_rows.size()) {
            if (r != g_sel) focus_row(r, false);
            if (click) activate();
        }
    }
};
Mouse g_mouse;

// The small things over the game: the last note's timing and the short messages.
void hud_tick(double now)
{
    static long seen;
    if (s_tm_seq != seen) {
        seen = s_tm_seq;
        char t[64];
        const double ms = g_tm_ms;
        snprintf(t, sizeof t, "%s %.0f ms", tr(ms < -0.5 ? "timing.early" : ms > 0.5 ? "timing.late" : "timing.exact").c_str(), fabs(ms));
        set_text("tm_now", t);
        snprintf(t, sizeof t, "%s %+.0f ms", tr("timing.mean").c_str(), g_tm_avg);
        set_text("tm_avg", t);
        set_class("tm_now", "late", ms > 0.5);
        set_class("tm_now", "early", ms < -0.5);
    }
    set_class("timing", "shown", now < g_tm_until && g_mode == M_CLOSED);
    if (const long n = InterlockedExchange(&s_note, 0)) {
        set_text("note", esc(tr(n == 1 ? "note.shot" : "note.shot_failed")));
        g_note_until = now + 2.0;
    }
    set_class("note", "shown", now < g_note_until);
}

void update_fps_and_clock(double now)
{
    g_present_times.push_back(now);
    size_t drop = 0;
    while (drop < g_present_times.size() && now - g_present_times[drop] > 1.0) drop++;
    g_present_times.erase(g_present_times.begin(), g_present_times.begin() + drop);
    static double s_last_text;
    if (now - s_last_text < 0.25) return;
    s_last_text = now;
    const size_t n = g_present_times.size();
    const double fps = n > 1 ? (n - 1) / (g_present_times.back() - g_present_times.front()) : 0;
    char t[48];
    snprintf(t, sizeof t, "%.0f FPS", fps);
    if (g_mode != M_CLOSED) set_text("bar_fps", t);
    if (g_show_fps) set_text("fps", t);
    SYSTEMTIME st; GetLocalTime(&st);
    if (st.wMinute != g_clock_min) {
        g_clock_min = st.wMinute;
        snprintf(t, sizeof t, "%02d:%02d", st.wHour, st.wMinute);
        set_text("bar_clock", t);
    }
    const int c = find_row("controller");
    std::string pad = g_port_pad_name[0] ? std::string(g_port_pad_name) : tr("val.no_pad");
    if (g_rows[c].info != pad) { g_rows[c].info = pad; refresh_row(c); }
}

// The four inks, from the layout RmlUi made last frame.
void update_inks()
{
    static Ink tab_ink, bar_ink, l_ink, pill[T_COUNT];
    char id[16];
    if (g_mode == M_PANEL || g_mode == M_CONFIRM) {
        snprintf(id, sizeof id, "t%d", g_tab);
        place_ink("tab_ink", el(id), true, tab_ink);   // the track index is a column (SETLIST, 2026-11-12)
        if (g_sel >= 0 && g_rows[g_sel].tab == g_tab) {
            char pid[16]; snprintf(pid, sizeof pid, "pill%d", g_tab);
            snprintf(id, sizeof id, "r%d", g_sel);
            place_ink(pid, el(id), true, pill[g_tab]);
        }
    }
    if (g_mode == M_BAR || g_mode == M_PANEL || g_mode == M_CONFIRM) {
        snprintf(id, sizeof id, "b%d", g_bar_sel);
        place_ink("bar_ink", el(id), false, bar_ink);
    }
    if (g_launcher_on) {
        snprintf(id, sizeof id, "m%d", g_lsel);
        place_ink("l_ink", el(id), true, l_ink);
    }
}

void draw(const port_overlay_frame* f)
{
    if (!g_gpu_ok) {
        if (g_gpu_failed || s_ready != 1) { InterlockedExchange(&g_port_overlay_active, 0); return; }
        const double t0 = now_s();
        g_gpu_ok = s_gpu_ready || g_vk || g_render.Init((ID3D12Device*)f->device, f->rtv_format, f->frame_count);
        g_doc->AddEventListener(Rml::EventId::Mouseover, &g_mouse);
        g_doc->AddEventListener(Rml::EventId::Click, &g_mouse);
        fprintf(stderr, "[menu] first draw: GPU %s (%s) in %.1f ms\n", g_gpu_ok ? "ready" : "FAILED",
                s_gpu_ready ? "built at boot" : "built now", (now_s() - t0) * 1e3);
        if (!g_gpu_ok) { g_gpu_failed = true; InterlockedExchange(&g_port_overlay_active, 0); return; }
    }
    const double now = now_s();
    if (InterlockedExchange(&s_toggle, 0) && !g_quitting && !g_launcher_on) {
        if (g_mode == M_CLOSED) open_bar(); else close_all();
        if (g_mode == M_BAR && g_auto_panel >= 0) { g_tab = g_auto_panel; g_bar_sel = B_SETTINGS; open_panel(); }
    }
    if (g_port_exclusive_state == -1) {   // 0167: the swap chain refused exclusive: the row goes back to borderless
        g_port_exclusive_state = 0;
        const int i = find_row("display_mode");
        Row& r = g_rows[i];
        r.cur = 1;
        ini_write(r.ini, r.vals[r.cur]);
        if (s_hwnd) PostMessageW(s_hwnd, WM_PORT_DISPLAY, 1, 0);
        g_excl_err_until = now + 10.0;
        fprintf(stderr, "[menu] exclusive fullscreen refused: the row goes back to borderless\n");
        if (g_mode == M_PANEL) { refresh_row(i); refresh_desc(); }
    }
    { unsigned cur = 0;   // 0162: the renderer could not make a height and went back: the row goes back too
      if (const unsigned bad = rsx_d3d12_take_render_height_failure(&cur)) {
          const int i = find_row("render_height");
          if (i >= 0) {
              Row& r = g_rows[i];
              for (size_t k = 0; k < r.vals.size(); k++) if ((unsigned)atoi(r.vals[k].c_str()) == cur) r.cur = (int)k;
              g_rh_err_until = now + 10.0;
              if (r.ini) ini_write(r.ini, r.vals[r.cur]);
              fprintf(stderr, "[menu] render_height %u failed in the renderer; the row goes back to %u\n", bad, cur);
              if (g_mode == M_PANEL) { refresh_row(i); refresh_desc(); }
          }
      } }
    // Diagnostic: PORT_MENU_RESTART_TEST=<s> answers "Restart now?" with yes at that second since boot. The
    // child gets PORT_MENU_RESTARTED=1 so that it does not do the same.
    if (g_restart_test >= 0 && now - g_boot_s >= g_restart_test && !g_quitting) {
        g_restart_test = -1;
        _putenv_s("PORT_MENU_RESTARTED", "1");
        g_confirm = C_RESTART;
        fprintf(stderr, "[menu] PORT_MENU_RESTART_TEST: restart now\n");
        confirm_answer(true);
    }
    if (g_auto_close >= 0 && g_mode != M_CLOSED && now - g_boot_s >= g_auto_close) { g_auto_close = -1; close_all(); }
    if (!g_auto_tabs.empty() && g_mode == M_PANEL && now - g_boot_s >= g_auto_tabs[0].first) {
        select_tab(g_auto_tabs[0].second, true);
        g_auto_tabs.erase(g_auto_tabs.begin());
    }
    if (g_pause_check_at > 0 && now >= g_pause_check_at) {
        g_pause_check_at = 0;
        const float sp = guest_f32(kGameSpeed);
        fprintf(stderr, "[menu] game pause %s (speed %.3f)\n", sp == 0.0f ? "confirmed" : "NOT confirmed", sp);
    }
    best_tick(now);
    const bool fading = hides_tick(now);
    hud_tick(now);
    const bool hud = g_show_fps || now < g_tm_until || now < g_note_until;
    if (g_mode == M_CLOSED && !hud && !fading) {
        if (g_doc->IsVisible()) g_doc->Hide();
        InterlockedExchange(&g_port_overlay_active, 0); g_present_times.clear(); return;
    }
    if (g_mode == M_CLOSED && !g_doc->IsVisible()) g_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    set_class("fps", "shown", g_show_fps && g_mode == M_CLOSED);

    if (g_mode != M_CLOSED) {
        uint32_t e = (uint32_t)InterlockedExchange(&s_edges, 0);
        // Holding a direction repeats it: first after 0.4 s, then every 75 ms.
        // A port still at 0xFFFF has not been read since the gate went on (F reads port 0 only): left out.
        uint32_t held = 0;
        for (int p = 0; p < 4; p++) if (s_last[p] != 0xFFFF) held |= (uint32_t)s_last[p];
        const uint32_t dirs[4] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT};
        for (int d = 0; d < 4; d++) {
            if (e & dirs[d]) g_rep_next[d] = now + kRepeatDelay;
            else if ((held & dirs[d]) && g_rep_next[d] > 0 && now >= g_rep_next[d]) {
                e |= dirs[d]; g_rep_next[d] += kRepeatStep;
                if (g_rep_next[d] < now) g_rep_next[d] = now + kRepeatStep;
            } else if (!(held & dirs[d])) g_rep_next[d] = 0;
        }
        if (g_listen >= 0) {
            if (g_mode == M_PANEL) bind_tick(now, held); else bind_stop("menu left");
            e = 0;
        } else if (g_listen_mute) {
            if (!held && !(pad_raw_sources() & ~kSrcReserved)) g_listen_mute = false;
            e = 0;
        }
        if (InterlockedExchange(&s_clear_req, 0) && g_mode == M_PANEL) bind_clear();
        if (e) press(e);
        const long mx = s_mouse_x, my = s_mouse_y;
        if (InterlockedExchange(&s_mouse_moved, 0) && mx >= 0) g_ctx->ProcessMouseMove(mx, my, 0);
        for (long c = InterlockedExchange(&s_clicks, 0); c > 0; c--) {
            g_ctx->ProcessMouseButtonDown(0, 0);
            g_ctx->ProcessMouseButtonUp(0, 0);
        }
        if (const long w = InterlockedExchange(&s_wheel, 0)) g_ctx->ProcessMouseWheel(Rml::Vector2f(0.0f, -(float)w / 120.0f), 0);
    }
    if (g_mode == M_CLOSED && !hud && !fading) { InterlockedExchange(&g_port_overlay_active, 0); return; }

    update_fps_and_clock(now);
    update_inks();
    const Rml::Vector2i dim((int)f->width, (int)f->height);
    if (g_ctx->GetDimensions() != dim) g_ctx->SetDimensions(dim);
    g_ctx->SetDensityIndependentPixelRatio((float)f->height / 1080.0f * g_scale);
    if (g_mode == M_PANEL) update_sview();
    g_ctx->Update();
    if (g_trace && now - g_opened_at < 0.3) {
        if (Rml::Element* p = el(g_mode == M_PANEL ? "panel" : "bar"))
            fprintf(stderr, "[menu] +%.3f s opacity %.3f\n", now - g_opened_at, p->GetProperty<float>("opacity"));
    }
    if (g_vk) g_render_vk.BeginFrame(f->width, f->height);
    else g_render.BeginFrame((ID3D12GraphicsCommandList*)f->cmd_list, f->rtv, f->width, f->height, f->frame_index);
    g_ctx->Render();
    if (g_vk) g_render_vk.EndFrame(); else g_render.EndFrame();
}

// The Vulkan executor's overlay hook (render thread, inside its pass on the presented image).
void draw_vk(uint32_t w, uint32_t h)
{
    port_overlay_frame f = {};
    f.width = w; f.height = h;
    g_vk = true;
    draw(&f);
}

void request_toggle(const char* why)
{
    if (s_ready != 1) { fprintf(stderr, "[menu] %s: menu %s\n", why, s_ready ? "failed to load" : "still loading"); return; }
    InterlockedExchange(&s_toggle, 1);
    InterlockedExchange(&g_port_overlay_active, 1);
}

void shot_done(const std::wstring& path)
{
    fprintf(stderr, "[menu] screenshot: %s\n", path.empty() ? "failed" : narrow(path).c_str());
    InterlockedExchange(&s_note, path.empty() ? 2 : 1);
    InterlockedExchange(&g_port_overlay_active, 1);
}

int wndproc(void* hwnd, unsigned msg, uint64_t wp, int64_t lp)
{
    if (!s_hwnd) {
        s_hwnd = (HWND)hwnd;
        if (!g_rows.empty()) {
            if (row("display_mode").cur == 0) PostMessageW(s_hwnd, WM_PORT_DISPLAY + 4, 0, 0);   // after CreateWindow has placed it
            else PostMessageW(s_hwnd, WM_PORT_DISPLAY, row("display_mode").cur, 0);
        }
        if (s_big_w) PostMessageW(s_hwnd, WM_PORT_DISPLAY + 1, 0, 0);
    }
    if (msg == WM_PORT_DISPLAY) { set_display_mode((HWND)hwnd, (int)wp); return 1; }
    if (msg == WM_PORT_DISPLAY + 3) { save_window((HWND)hwnd); return 1; }
    if (msg == WM_PORT_DISPLAY + 4) { restore_window((HWND)hwnd); return 1; }
    if (msg == WM_EXITSIZEMOVE) save_window((HWND)hwnd);              // dragged or resized: the new place
    if (msg == WM_ACTIVATEAPP && wp == 0) InterlockedExchange(&s_focus_lost, 1);   // watch() decides
    if (msg == WM_ACTIVATEAPP) { InterlockedExchange(&s_active, wp != 0); if (g_mute_bg) volume_apply(); }
    if (msg == WM_KEYDOWN && wp == VK_F12 && g_shot_key && !s_listen_kb) {
        if (!(lp & (1ll << 30))) extras_screenshot((HWND)hwnd, shot_done);
        return 1;
    }
    // Diagnostic only: PORT_MENU_BIGWINDOW=<w>x<h> sets the client area to that size once, also past the
    // monitor (captures at 1440p on a 1080p screen). The backend then handles the message as always.
    if (msg == WM_PORT_DISPLAY + 1) {
        RECT r = {0, 0, s_big_w, s_big_h};
        AdjustWindowRect(&r, (DWORD)GetWindowLongPtrW((HWND)hwnd, GWL_STYLE), FALSE);
        SetWindowPos((HWND)hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 1;
    }
    if (msg == WM_GETMINMAXINFO && s_big_w) {
        ((MINMAXINFO*)lp)->ptMaxTrackSize.x = 8192; ((MINMAXINFO*)lp)->ptMaxTrackSize.y = 8192;
        return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wp == VK_F1) {
        if (msg == WM_KEYDOWN && !(lp & (1ll << 30))) request_toggle("F1");   // not an auto-repeat
        return 1;
    }
    // 6f p1: a row waiting for a key gets it here, with left and right told apart, and nothing else sees it.
    if (s_listen_kb && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        if (lp & (1ll << 30)) return 1;   // auto-repeat
        int vk = (int)wp;
        const bool ext = (lp >> 24) & 1;
        if (vk == VK_SHIFT) vk = (int)MapVirtualKeyW((UINT)((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
        else if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;
        if (s_listen_kb == 1) InterlockedExchange(&s_listen_vk, vk);
        else if (vk == VK_ESCAPE) InterlockedExchange(&s_listen_esc, 1);
        return 1;
    }
    if (s_listen_kb && (msg == WM_KEYUP || msg == WM_SYSKEYUP)) return 1;
    if (msg == WM_KEYDOWN && wp == VK_DELETE && s_shown && !(lp & (1ll << 30))) InterlockedExchange(&s_clear_req, 1);
    // The cursor hides after kCursorIdle without moving (watch() posts the message), in and out of the menu.
    if (msg == WM_PORT_DISPLAY + 2) {
        POINT p; RECT rc;
        if (GetCursorPos(&p) && ScreenToClient((HWND)hwnd, &p) && GetClientRect((HWND)hwnd, &rc) && PtInRect(&rc, p)) SetCursor(nullptr);
        return 1;
    }
    if (msg == WM_SETCURSOR && s_cursor_hidden && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return 1; }
    if (msg == WM_MOUSEMOVE) {
        static int64_t last = -1;
        if (lp != last) {   // Windows sends a move without one after SetCursor and focus changes
            last = lp;
            s_mouse_seen_at = now_s();
            if (InterlockedExchange(&s_cursor_hidden, 0)) SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
    }
    if (!s_shown) {
        // The game's own menus. In a song (its clock moved in the last 0.3 s) only with "Mouse in songs".
        const bool song = port_in_song() && now_s() - s_song_seen_at < 0.3;
        // Scripted runs (speed test, PAD_KEYS, PAD_SCRIPT): the mouse presses nothing. A wheel turn or a click over
        // the window while a test ran moved its cursor and the run took another path (2026-11-13).
        static int scripted = -1;
        if (scripted < 0) scripted = getenv("REFINE_ROUTE") || getenv("PAD_KEYS") || getenv("PAD_SCRIPT") || getenv("PAD_BOT");
        if (scripted) return 0;
        if (!g_mouse_menus || (song && !g_mouse_songs)) return 0;
        static int wheel = 0;
        switch (msg) {
        case WM_MOUSEWHEEL:
            for (wheel += (short)((wp >> 16) & 0xFFFF); wheel >= WHEEL_DELTA; wheel -= WHEEL_DELTA) InterlockedIncrement(&s_mouse_pend[0]);
            for (; wheel <= -WHEEL_DELTA; wheel += WHEEL_DELTA) InterlockedIncrement(&s_mouse_pend[1]);
            return 1;
        case WM_LBUTTONDOWN: InterlockedIncrement(&s_mouse_pend[2]); return 1;
        case WM_RBUTTONDOWN: InterlockedIncrement(&s_mouse_pend[3]); return 1;
        case WM_MBUTTONDOWN: InterlockedIncrement(&s_mouse_pend[4]); return 1;
        default: return 0;
        }
    }
    switch (msg) {
    case WM_MOUSEMOVE:
        InterlockedExchange(&s_mouse_x, (short)(lp & 0xFFFF));
        InterlockedExchange(&s_mouse_y, (short)((lp >> 16) & 0xFFFF));
        InterlockedExchange(&s_mouse_moved, 1);
        return 1;
    case WM_LBUTTONDOWN: InterlockedIncrement(&s_clicks); return 1;
    case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: return 1;
    case WM_MOUSEWHEEL: InterlockedExchangeAdd(&s_wheel, (short)((wp >> 16) & 0xFFFF)); return 1;
    default: return 0;
    }
}

// 6f p2, diagnostic only: REFINE_JUDGE_LOG=1 prints each change of a live note in the game's judgment list
// (the list PAD_BOT reads, src/main.cpp: 0x12CE588 +0x14 head, a note +4 next, +0xC type, +0x10 seconds
// left, +0x118 result) and of the result counters at 0x12CCA00. Read-only, from the guest thread.
int g_judge_log = -1;
bool judge_node_ok(uint32_t p);
void judge_log(double now)
{
    static struct { uint32_t node, type, res; } seen[16];
    static uint32_t counts[12];
    uint32_t p = guest_u32(0x012CE588u + 0x14);
    bool live[16] = {};
    for (int hops = 0; judge_node_ok(p) && hops < 16; hops++, p = guest_u32(p + 4)) {
        const uint32_t type = guest_u32(p + 0xC), res = guest_u32(p + 0x118);
        int k = -1;
        for (int i = 0; i < 16; i++) if (seen[i].node == p) k = i;
        if (k < 0) {
            for (int i = 0; i < 16 && k < 0; i++) if (!seen[i].node) k = i;
            if (k < 0) continue;
            seen[k] = {p, type, res};
            fprintf(stderr, "[judge] t=%.3f new %08X type=%u res=%u left=%.3f\n", now - g_boot_s, p, type, res, guest_f32(p + 0x10));
        } else if (seen[k].type != type || seen[k].res != res) {
            fprintf(stderr, "[judge] t=%.3f node %08X type=%u res=%u (was %u/%u) left=%.3f\n", now - g_boot_s, p, type, res,
                    seen[k].type, seen[k].res, guest_f32(p + 0x10));
            seen[k].type = type; seen[k].res = res;
        }
        live[k] = true;
    }
    for (int i = 0; i < 16; i++) if (!live[i] && seen[i].node) {
        fprintf(stderr, "[judge] t=%.3f gone %08X type=%u res=%u\n", now - g_boot_s, seen[i].node, seen[i].type, seen[i].res);
        seen[i].node = 0;
    }
    for (int i = 0; i < 12; i++) {
        const uint32_t c = guest_u32(0x012CCA00u + i * 4);
        if (c != counts[i]) { counts[i] = c; fprintf(stderr, "[judge] t=%.3f count +0x%02X=%u\n", now - g_boot_s, i * 4, c); }
    }
}

// 6f p2 (R12): Star vibration. The game takes a judged note out of its live list (0x12CE588 +0x14) in the
// same frame it adds to the result counters (0x12CCA00: +0x0C COOL, +0x10 FINE, +0x14 SAFE, +0x18 SAD,
// +0x1C WORST), so a star note (type 12 and up, as PAD_BOT plays them) leaving the list while COOL or FINE
// goes up is a star hit (run judge1, docs/STATUS.md). A slot whose seconds-left jumps up is a new note in
// the place of the old one. Read-only, from the guest thread, the same reads as REFINE_JUDGE_LOG.
// Type 16 is the star that ends Chance Time (run vib1: the only one, the last of the notes counted at +0x28
// from the start of Chance Time); hit, it gets the long pulse. ASSUMPTION: hitting it is the success; no
// counter told success from failure, and a Chance Time with the gauge short of full was not tried.
constexpr uint32_t kJudge = 0x012CE588u, kCounts = 0x012CCA00u;
bool judge_node_ok(uint32_t p) { return p >= 0x01200000u && p < 0x01400000u; }   // the game's data, never past it
//
// Timing display (6g): the same note, when the game judged it. A note's +0x10 is the song seconds left to its moment, so
// at the judgement it is what was left the last time it was seen less what the song has gone on since: the press was
// that much early (+) or late (-), to within a frame of the game.
void timing_judged(float left)
{
    static double hist[20]; static int n;
    const double ms = -(left + port_judge_offset()) * 1000.0;   // as it was judged
    if (ms < -250.0 || ms > 250.0) return;       // not a press: the song moved on without it
    hist[n++ % 20] = ms;
    double sum = 0; const int m = n < 20 ? n : 20;
    for (int i = 0; i < m; i++) sum += hist[i];
    g_tm_ms = ms; g_tm_avg = sum / m; g_tm_until = now_s() + 0.9;
    InterlockedIncrement(&s_tm_seq);
    InterlockedExchange(&g_port_overlay_active, 1);
    if (g_trace || g_judge_log) fprintf(stderr, "[menu] timing %+.1f ms (mean of %d: %+.1f)\n", ms, m, sum / m);
}
void star_watch()
{
    static uint32_t node[16], type[16]; static float left[16]; static int n; static uint32_t good_last;
    static uint32_t hits_last; static float song_last;
    uint32_t cn[16], ct[16]; float cl[16]; int m = 0;
    for (uint32_t p = guest_u32(kJudge + 0x14); judge_node_ok(p) && m < 16; p = guest_u32(p + 4)) {
        cn[m] = p; ct[m] = guest_u32(p + 0xC); cl[m] = guest_f32(p + 0x10); m++;
    }
    bool star_gone = false, chance_gone = false;
    for (int i = 0; i < n; i++) {
        if (type[i] < 12) continue;
        bool still = false;
        for (int k = 0; k < m && !still; k++) still = cn[k] == node[i] && ct[k] == type[i] && cl[k] <= left[i] + 0.1f;
        if (!still) { star_gone = true; if (type[i] == 16) chance_gone = true; }
    }
    if (g_timing) {
        const float song = guest_f32(kGameSongClock);
        uint32_t hits = 0;                       // COOL, FINE, SAFE and SAD: presses; WORST is a note let go by
        for (uint32_t a = 0x0C; a <= 0x18; a += 4) hits += guest_u32(kCounts + a);
        if (hits == hits_last + 1 || hits == hits_last + 2)
            for (int i = 0; i < n; i++) {
                bool still = false;
                for (int k = 0; k < m && !still; k++) still = cn[k] == node[i] && ct[k] == type[i] && cl[k] <= left[i] + 0.1f;
                if (!still) { timing_judged(left[i] - (song - song_last)); break; }
            }
        hits_last = hits; song_last = song;
    }
    const uint32_t good = guest_u32(kCounts + 0x0C) + guest_u32(kCounts + 0x10);
    if (star_gone && good > good_last && g_star_vib > 0) {
        // Star: short, mostly the small motor. Chance Time success: long, both motors.
        static const unsigned short lo[4] = {0, 0, 0x1800, 0x4000}, hi[4] = {0, 0x3000, 0x6000, 0xA000};
        static const unsigned short clo[4] = {0, 0x3000, 0x6000, 0xB000}, chi[4] = {0, 0x3000, 0x5000, 0x9000};
        static const int ms[4] = {0, 60, 80, 100}, cms[4] = {0, 250, 350, 450};
        if (chance_gone) pad_pulse(clo[g_star_vib], chi[g_star_vib], cms[g_star_vib]);
        else pad_pulse(lo[g_star_vib], hi[g_star_vib], ms[g_star_vib]);
        if (g_trace || g_judge_log) fprintf(stderr, "[menu] %s hit: pulse %d\n", chance_gone ? "chance star" : "star", g_star_vib);
    }
    good_last = good;
    memcpy(node, cn, sizeof cn); memcpy(type, ct, sizeof ct); memcpy(left, cl, sizeof cl); n = m;
}

// Discord (discord.h): in a song while its clock moves, and while the game is stopped in one (pause menu);
// back to the menus 3 s after the clock stops with the game running. Only changes reach discord.cpp.
// The title of the song being played, as the game itself holds it (run title1, MEM_DUMP in a song: the block
// beside the result counters). 0x12CCDB0: the PV number, then at 0x12CCDBC a string: 16 bytes of characters
// (or, past 15 characters, a pointer to them in the first 4), its length at +0x10 and its capacity at +0x14.
// Only what the game already decoded is read; nothing of the disc is parsed. "" when it does not look right.
// ASSUMPTION: seen with Cat Food (PV 600) only, in the run of this step; checked with a second song below.
constexpr uint32_t kSongPv = 0x012CCDB0u, kSongName = 0x012CCDBCu;
std::string song_title()
{
    const uint32_t pv = guest_u32(kSongPv), len = guest_u32(kSongName + 0x10), cap = guest_u32(kSongName + 0x14);
    if (pv == 0 || pv > 999 || len == 0 || len > 200 || cap < len || cap > 4096) return "";
    uint32_t at = kSongName;
    if (cap > 15) { at = guest_u32(kSongName); if (at < 0x10000u || at >= 0x40000000u) return ""; }
    std::string t((const char*)vm_base + at, len);
    for (unsigned char c : t) if (c < 0x20) return "";
    return t;
}
void presence_watch(double now)
{
    static bool in_song;
    static double since_s;
    const bool moving = port_in_song() && now - s_song_seen_at < 3.0;
    const bool paused = guest_f32(kGameSpeed) == 0.0f;
    const bool song = moving || (in_song && paused);
    if (song == in_song) return;
    in_song = song;
    if (song) {
        since_s = (double)time(nullptr) - guest_f32(kGameSongClock);
        const std::string t = song_title();
        fprintf(stderr, "[menu] song: '%s' (PV %u)\n", t.c_str(), guest_u32(kSongPv));
        discord_set_song(t.empty() ? "Playing a song" : t, (int64_t)since_s);
    } else discord_set_song("", 0);
}

// Guest thread, every cellPadGetData: the long SELECT, the touchpad flag, and whether a song is running.
// "Skip Intro Cinematic" (6g): START, as a player presses it, while the logos (11TaskAdvLogo) or the opening movie
// (9TaskMovie) are up, until the title (12TaskAdvTitle) comes the first time. Vtables from the EBOOT's RTTI.
void skip_intro(double now)
{
    static bool done; static double next;
    if (done) return;
    const uint32_t work = guest_u32(guest_u32(0x00BF2A4Cu)), head = work >= 0x10000u && work < 0x40000000u ? guest_u32(work + 4) : 0;
    bool intro = false;
    if (head >= 0x10000u && head < 0x40000000u) {
        uint32_t node = guest_u32(head);
        for (int i = 0; i < 300 && node != head && node >= 0x10000u && node < 0x40000000u; i++, node = guest_u32(node)) {
            const uint32_t task = guest_u32(node + 8), vt = task >= 0x10000u && task < 0x40000000u ? guest_u32(task) : 0;
            if (vt == 0x00BA8220u) done = true;
            else if (vt == 0x00BA8270u || vt == 0x00BB0DE0u) intro = true;
        }
    }
    if (done) {
        if (s_inject_start) { g_inject_until = 0; InterlockedExchange(&s_inject_start, 0); }
        fprintf(stderr, "[menu] skip intro: title at %.2f s\n", now - g_boot_s);
    } else if (intro && now >= next && g_mode == M_CLOSED) {
        next = now + 0.5;
        g_inject_until = now + 0.1;
        InterlockedExchange(&s_inject_start, PAD_START);
        InterlockedExchange(&g_port_overlay_inject, s_inject_start | s_inject_mouse);
    }
}

void watch(unsigned port, uint16_t mask)
{
    const double now = now_s();
    if (g_judge_log < 0) g_judge_log = getenv("REFINE_JUDGE_LOG") != nullptr;
    if (port == 0 && g_judge_log) judge_log(now);
    if (port == 0 && g_skip_intro) skip_intro(now);
    if (port == 0 && g_shot_test >= 0 && now - g_boot_s >= g_shot_test) { g_shot_test = -1; extras_screenshot(s_hwnd, shot_done); }
    if (port == 0 && (g_star_vib > 0 || g_judge_log || g_timing) && port_in_song() && now - s_song_seen_at < 0.3) star_watch();
    if (port == 0) presence_watch(now);
    if (port == 0) {
        const float song = guest_f32(kGameSongClock);
        if (song != s_song_last) { s_song_last = song; s_song_seen_at = now; }
        // ASSUMPTION: F reads SELECT somewhere (not ruled out), so only a hold of 0.5 s opens the bar; a short
        // press still reaches the game as before.
        if (mask & PAD_SELECT) {
            if (s_select_since == 0) s_select_since = now;
            else if (!s_select_fired && now - s_select_since >= kSelectHold) { s_select_fired = true; request_toggle("long SELECT"); }
        } else { s_select_since = 0; s_select_fired = false; }
    }
    if (g_port_overlay_touchpad) { InterlockedExchange(&g_port_overlay_touchpad, 0); request_toggle("touchpad"); }
    // Pause on focus lost: the game's own pause (its START, as the bar does), only inside a song that is
    // running and with the menu closed. PORT_MENU_FOCUS_TEST=<s> raises the same flag at that second.
    if (port == 0 && g_pause_check_at > 0 && g_mode == M_CLOSED && now >= g_pause_check_at) {   // draw() is not called with the menu closed
        g_pause_check_at = 0;
        const float sp = guest_f32(kGameSpeed);
        fprintf(stderr, "[menu] game pause %s (speed %.3f)\n", sp == 0.0f ? "confirmed" : "NOT confirmed", sp);
    }
    if (port == 0 && g_focus_test >= 0 && now - g_boot_s >= g_focus_test) { g_focus_test = -1; InterlockedExchange(&s_focus_lost, 1); }
    if (port == 0 && InterlockedExchange(&s_focus_lost, 0) && g_pause_focus && g_mode == M_CLOSED && !g_launcher_on) {
        const bool in_song = port_in_song() && now - s_song_seen_at < 0.3 && guest_f32(kGameSpeed) != 0.0f;
        fprintf(stderr, "[menu] focus lost: %s\n", in_song ? "in a song, START for the game's pause" : "not in a song, nothing");
        if (in_song) {
            g_inject_until = now + 0.12;
            InterlockedExchange(&s_inject_start, PAD_START);
            InterlockedExchange(&g_port_overlay_inject, s_inject_start | s_inject_mouse);
            g_pause_check_at = now + 0.6;
        }
    }
    // The START for the game's pause lets go here, on every read, not in draw: draw stops being called once
    // the menu is closed, and a bar closed within 0.12 s left START held for the rest of the session.
    if (s_inject_start && now >= g_inject_until) { g_inject_until = 0; InterlockedExchange(&s_inject_start, 0); }
    // The mouse in the game's menus: one queued press at a time, a release between two.
    if (port == 0) {
        if (s_inject_mouse && now >= s_mouse_until) { InterlockedExchange(&s_inject_mouse, 0); s_mouse_gap_until = now + kMouseGap; }
        if (!s_inject_mouse && now >= s_mouse_gap_until)
            for (int b = 0; b < 5; b++)
                if (s_mouse_pend[b] > 0) {
                    InterlockedDecrement(&s_mouse_pend[b]);
                    InterlockedExchange(&s_inject_mouse, (long)kMouseBtn[b]);
                    s_mouse_until = now + kMousePress;
                    if (g_trace) fprintf(stderr, "[menu] mouse: game button 0x%04X\n", (unsigned)kMouseBtn[b]);
                    break;
                }
        InterlockedExchange(&g_port_overlay_inject, s_inject_start | s_inject_mouse);
        if (!s_cursor_hidden && s_hwnd && s_mouse_seen_at > 0 && now - s_mouse_seen_at > kCursorIdle) {
            InterlockedExchange(&s_cursor_hidden, 1);
            PostMessageW(s_hwnd, WM_PORT_DISPLAY + 2, 0, 0);
        }
    }
    // The menu never leaves the game without input: closed for more than kGateMax and still holding
    // the pad (a button that never reads as up), it lets go.
    if (g_port_overlay_gate && !s_shown && now - s_closed_at > kGateMax) {
        char held[64] = "";
        for (int p = 0; p < 4; p++) snprintf(held + strlen(held), sizeof held - strlen(held), " %04lX", (unsigned long)s_last[p]);
        InterlockedExchange(&s_closing, 0);
        InterlockedExchange(&g_port_overlay_gate, 0);
        fprintf(stderr, "[menu] input hold still on %.1f s after closing: released (ports%s)\n", now - s_closed_at, held);
    }
}

int pad(unsigned port, uint16_t mask, uint8_t, uint8_t)
{
    if (port >= 4) return 1;
    const long prev = InterlockedExchange(&s_last[port], (long)mask);
    const long pressed = (long)mask & ~prev;
    if (pressed) InterlockedOr(&s_edges, pressed);
    if (s_closing) {
        bool up = true;
        for (int p = 0; p < 4; p++) up = up && (s_last[p] == 0 || s_last[p] == 0xFFFF);   // 0xFFFF: not read
        if (up) {
            InterlockedExchange(&s_closing, 0); InterlockedExchange(&g_port_overlay_gate, 0);
            fprintf(stderr, "[menu] input back to the game %.0f ms after closing\n", (now_s() - s_closed_at) * 1e3);
            return 0;
        }
    }
    return 1;
}

DWORD WINAPI auto_open(LPVOID)
{
    const double wait = g_auto_open - (now_s() - g_boot_s);
    if (wait > 0) Sleep((DWORD)(wait * 1000));
    WaitForSingleObject(s_ready_event, INFINITE);
    if (s_ready == 1) request_toggle("PORT_MENU_AT");
    return 0;
}

// Diagnostic only (0161): PORT_LIVE_TEST=<s>:<row>=<value>,... sets a row at that second since boot, as if the
// user had confirmed it in the menu (refine.ini is not written). Example: 60:fps_target=120,90:render_height=1440.
struct LiveStep { double at; std::string id, val; };
std::vector<LiveStep> g_live_test;

DWORD WINAPI live_test(LPVOID)
{
    WaitForSingleObject(s_ready_event, INFINITE);
    for (const LiveStep& s : g_live_test) {
        const double wait = s.at - (now_s() - g_boot_s);
        if (wait > 0) Sleep((DWORD)(wait * 1000));
        if (g_trace) fprintf(stderr, "[menu] live test: step %s = %s (waited %.1f s)\n", s.id.c_str(), s.val.c_str(), wait);
        const int i = find_row(s.id.c_str());
        if (i < 0) continue;
        Row& r = g_rows[i];
        for (size_t k = 0; k < r.vals.size(); k++)
            if (r.vals[k] == s.val) { r.cur = (int)k; fprintf(stderr, "[menu] live test: %s = %s\n", s.id.c_str(), s.val.c_str()); apply_live(r); }
    }
    return 0;
}


// Pad and disc facts the start screen needs: the runtime's own functions (cellPad in libs/input,
// the PARAM.SFO values in libs/system/cellGame.c, 0164).
extern "C" int cellPadInit(unsigned max_connect);
extern "C" int cellPadEnd(void);
extern "C" int cellPadGetData(unsigned port, unsigned guest_addr);
extern "C" unsigned int ppu_scratch_alloc(unsigned int size, unsigned int align);   // runtime allocator (0164)
extern "C" const char* ppu_vfs_root;

// The start screen (0164). Guest thread, inside cellGcmInit, before the game has drawn anything:
// presents its own frames (rsx_d3d12_launcher_frame) until PLAY. cellPad is opened for it and closed
// again: the game calls cellPadInit itself later. The pad report goes to a block from the runtime's own scratch allocator (ppu_scratch_alloc).
void launcher_hook()
{
    const char* e = getenv("REFINE_LAUNCHER");
    if (!(e ? atoi(e) != 0 : (!g_rows.empty() && val("show_launcher") == "on"))) return;
    if (WaitForSingleObject(s_ready_event, 15000) != WAIT_OBJECT_0 || s_ready != 1) {
        fprintf(stderr, "[launcher] the menu is not ready (%s): the game starts without the start screen\n", s_ready ? "failed" : "timeout");
        return;
    }
    const char* au = getenv("REFINE_LAUNCHER_AUTO");   // diagnostic: leave by itself after N seconds
    const double auto_s = au ? atof(au) : -1.0;
    // The game may have opened cellPad already (it does, before cellGcmInit): then Init says "already opened"
    // and the pad is the game's, so it is not ours to End (that closed it for the whole session).
    const bool own_pad = cellPadInit(7) == 0;
    const unsigned pad_buf = ppu_scratch_alloc(0x200, 16);   // CellPadData is 0x10C bytes
    const bool disc_ok = strcmp(cellGame_title_id_str(), "BLES00000") != 0;
    std::string disc = disc_ok
        ? "<span class=\"ok\">\xE2\x9C\x93" " " + esc(tr("launcher.disc_ok")) + "</span><br/>" + esc(std::string(cellGame_title_id_str()) + " \xC2\xB7" " v" + cellGame_app_ver_str())
        : "<span class=\"bad\">" + esc(tr("launcher.disc_missing")) + "</span><br/><span class=\"btn\" id=\"gfolder\">" + esc(tr("launcher.choose")) + "</span>";
    set_text("l_disc", disc);
    set_text("l_rver", std::string("Re:Fine ") + REFINE_VERSION);
    for (int k = 0; k < 3; k++) { char id[8]; snprintf(id, sizeof id, "m%d", k); set_text(id, "<div class=\"mtxt\"><span class=\"mno\">0" + std::to_string(k + 1) + "</span>" + esc(tr(k == 0 ? "launcher.play" : k == 1 ? "launcher.settings" : "launcher.quit")) + "</div>"); }
    // The shade from the left is a gradient in theme.rcss (#l_shade) since the renderer has them (2026-11-11).
    // A picture missing on the disc folder (or REFINE_LAUNCHER_NOPIC): the element goes away, the theme's
    // background stays. The renderer would draw a white square for it.
    for (const char* f : {"PIC1.PNG", "ICON0.PNG"}) {
        bool have = false;
        for (const char* fmt : {"%s/PS3_GAME/%s", "%s/../%s", "%s/%s"}) {
            char path[1200];
            snprintf(path, sizeof path, fmt, ppu_vfs_root, f);
            if (FILE* fp = fopen(path, "rb")) { fclose(fp); have = true; break; }
        }
        if (!strcmp(f, "PIC1.PNG") && getenv("REFINE_LAUNCHER_NOPIC")) have = false;
        set_class(!strcmp(f, "PIC1.PNG") ? "l_pic" : "l_icon", "gone", !have);
    }
    g_lgame = g_f2nd.runnable && val("menu_game") == "f2nd" ? 1 : 0;
    if (g_f2nd.disc && !g_lgame && val("menu_game") == "f2nd")
        fprintf(stderr, "[launcher] menu_game = f2nd, but %s is not there: F's menu\n", f2nd_exe().c_str());
    launcher_games_fill();
    gate_on();
    g_launcher_on = true; g_launcher_done = false; g_launch_t = -1;
    set_class("launcher", "launching", false);
    g_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    set_class("launcher", "shown", true);
    g_mode = M_LAUNCHER;
    launcher_focus(0);
    if (g_auto_panel >= 0) { g_tab = g_auto_panel; g_lsel = 1; launcher_activate(); }   // diagnostic
    InterlockedExchange(&g_port_overlay_active, 1);
    const double t0 = now_s();
    fprintf(stderr, "[launcher] start screen up at %.3f s, disc %s\n", t0 - g_boot_s, disc_ok ? "ready" : "NOT found");
    std::string seen_pad;
    for (;;) {
        cellPadGetData(0, pad_buf);
        const double now = now_s();
        if (g_port_pad_name[0] && seen_pad != g_port_pad_name) {
            seen_pad = g_port_pad_name;
            launcher_toast("<b>" + esc(tr("launcher.pad")) + "</b><br/>" + esc(seen_pad));
        } else if (!g_port_pad_name[0]) seen_pad.clear();
        if (g_toast_until > 0 && now > g_toast_until) { g_toast_until = 0; set_class("l_toast", "shown", false); }
        rsx_d3d12_launcher_frame();
        if (g_launch_t >= 0 && now_s() - g_launch_t >= kLaunchSecs) break;
        if (auto_s >= 0 && g_launch_t < 0 && now_s() - t0 > auto_s) {
            fprintf(stderr, "[launcher] REFINE_LAUNCHER_AUTO: PLAY\n");
            g_mode = M_LAUNCHER; g_lsel = 0; launcher_activate();
        }
    }
    if (own_pad) cellPadEnd();
    if (g_lgame == 1) {
        // F 2nd's menu: its own exe with its disc folder, and this one leaves (plan_6h.md §1). The game has not
        // started, nothing to save. ASSUMPTION: ReFine-F2nd.exe takes its disc folder as ReFine.exe does (P3).
        const std::string exe = f2nd_exe(), cmd = "\"" + exe + "\" \"" + g_f2nd.folder + "\"";
        g_relaunch.assign(cmd.begin(), cmd.end());   // ASCII paths (the same ASSUMPTION as g_dir)
        fprintf(stderr, "[launcher] PLAY: F 2nd's menu, starting %s\n", f2nd_exe().c_str());
        relaunch_at_exit();
        ExitProcess(0);
    }
    for (const char* id : {"dim", "panel", "confirm"}) set_class(id, "shown", false);   // PLAY with the panel open
    set_class("launcher", "shown", false);
    g_launcher_on = false;
    g_mode = M_CLOSED;
    InterlockedExchange(&s_shown, 0);
    InterlockedExchange(&s_closing, 0);
    InterlockedExchange(&g_port_overlay_gate, 0);
    if (!g_show_fps) { g_doc->Hide(); InterlockedExchange(&g_port_overlay_active, 0); }
    fprintf(stderr, "[launcher] PLAY after %.1f s (way out %.2f s)\n", now_s() - t0, g_launch_t >= 0 ? now_s() - g_launch_t : 0.0);
    g_launch_t = -1;
    fprintf(stderr, "[launcher] input -> game (pad %s, gate %ld, shown %ld)\n", own_pad ? "closed by us" : "left to the game", (long)g_port_overlay_gate, (long)s_shown);
}

} // namespace

// The speed test's route (src/evt.cpp): what F1 does.
extern "C" void port_menu_toggle(void) { request_toggle("route"); }

extern "C" void port_menu_start(int render_height, int fps_target)
{
    g_boot_s = now_s();
    wchar_t w[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, w, MAX_PATH);
    std::wstring ws(w, n);
    ws = ws.substr(0, ws.find_last_of(L'\\') + 1);
    // ASSUMPTION: the exe's folder is plain ASCII (RmlUi opens files by narrow path). True for
    // build\ and the installer's folder here; a non-ASCII user folder is untested.
    g_dir.assign(ws.begin(), ws.end());
    if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA")) g_data_dir = std::wstring(la) + L"\\ReFine";
    ini_load();
    load_lang(g_dir + "ui\\lang\\en.lang");
    build_rows(render_height, fps_target);
    mods_boot();   // before the game opens a file: port_menu_start runs before ppu_run (main.cpp)
    binds_load();
    binds_apply();
    // The Application ID of the author's Discord app (Discord Developer Portal); refine.ini discord_app_id wins.
    discord_start(g_ini.count("discord_app_id") ? g_ini["discord_app_id"] : kDiscordAppId, REFINE_VERSION);
    for (const Row& r : g_rows) if (r.ini && !r.restart) apply_live(r);
    if (const char* a = getenv("PORT_MENU_AT")) {
        if (sscanf(a, "%lf-%lf", &g_auto_open, &g_auto_close) < 1) g_auto_open = -1;
    }
    if (const char* p = getenv("PORT_MENU_PANEL")) g_auto_panel = atoi(p);
    for (const char* p = getenv("PORT_MENU_TABS"); p && *p;) {
        int t; double at; int used = 0;
        if (sscanf(p, "%d@%lf%n", &t, &at, &used) != 2) break;
        g_auto_tabs.push_back({at, t});
        p += used; if (*p == ',') p++;
    }
    g_trace = getenv("PORT_MENU_TRACE") != nullptr;
    if (const char* ft = getenv("PORT_MENU_FOCUS_TEST")) g_focus_test = atof(ft);
    if (const char* ss = getenv("PORT_MENU_SHOT")) g_shot_test = atof(ss);
    if (val("backups") == "on")
        if (const wchar_t* hd = _wgetenv(L"PS3_HDD0_ROOT")) extras_backup_saves(hd, 10);
    if (const char* bt = getenv("PORT_MENU_BEST")) g_best_auto = atof(bt);
    if (const char* bo = getenv("PORT_MENU_BEST_TEST")) g_best_override = bo;
    if (getenv("PORT_MENU_BEST_TABLE")) best_table();
    if (const char* rt = getenv("PORT_MENU_RESTART_TEST")) if (!getenv("PORT_MENU_RESTARTED")) g_restart_test = atof(rt);
    if (const char* lt = getenv("PORT_LIVE_TEST")) {
        std::string s = lt;
        for (size_t a = 0; a < s.size();) {
            size_t e = s.find_first_of(",;", a); if (e == std::string::npos) e = s.size();
            const std::string it = s.substr(a, e - a); a = e + 1;
            const size_t c = it.find(':'), q = it.find('=');
            if (c != std::string::npos && q != std::string::npos && c < q)
                g_live_test.push_back({atof(it.substr(0, c).c_str()), it.substr(c + 1, q - c - 1), it.substr(q + 1)});
        }
        if (!g_live_test.empty()) CreateThread(nullptr, 0, live_test, nullptr, 0, nullptr);
    }
    if (const char* b = getenv("PORT_MENU_BIGWINDOW")) sscanf(b, "%dx%d", &s_big_w, &s_big_h);
    g_port_launcher = launcher_hook;
    g_port_overlay_draw = draw;
    g_rsx_vk_ui = draw_vk;
    g_port_overlay_wndproc = wndproc;
    g_port_overlay_pad = pad;
    g_port_overlay_watch = watch;
    g_port_overlay_device_ready = device_ready;
    s_ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_device_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 1u << 20, boot_thread, nullptr, 0, nullptr);
    if (g_auto_open >= 0) CreateThread(nullptr, 64u << 10, auto_open, nullptr, 0, nullptr);
}
