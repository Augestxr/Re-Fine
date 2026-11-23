// Re:Fine: Discord Rich Presence (discord.h). The IPC protocol: frames of a 32-bit opcode, a 32-bit length
// (both little-endian) and that many bytes of JSON. Opcode 0 is the handshake ({"v":1,"client_id":...}),
// 1 a command (SET_ACTIVITY), 2 close. Discord answers each one; the answers are read and dropped.
// SET_ACTIVITY is limited by Discord to 5 per 20 s: one update every kMinGap at most, always the latest.
#include "discord.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace {

constexpr DWORD kRetryMs = 15000;   // no Discord running: look again this often
constexpr DWORD kMinGap = 4000;

std::mutex g_m;
std::string g_app, g_version, g_song;
int64_t g_since;
bool g_on = true, g_dirty = true, g_started;
HANDLE g_wake;

std::string json_str(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;   // UTF-8 passes as it is
    }
    return o + "\"";
}

bool frame_write(HANDLE p, uint32_t op, const std::string& json)
{
    std::string buf(8 + json.size(), '\0');
    const uint32_t len = (uint32_t)json.size();
    memcpy(&buf[0], &op, 4);
    memcpy(&buf[4], &len, 4);
    memcpy(&buf[8], json.data(), json.size());
    DWORD wrote = 0;
    return WriteFile(p, buf.data(), (DWORD)buf.size(), &wrote, nullptr) && wrote == buf.size();
}

// Drops whatever Discord has sent; false when the pipe is gone. waitms: how long to wait for an answer.
bool drain(HANDLE p, DWORD waitms, std::string* first = nullptr)
{
    const DWORD until = GetTickCount() + waitms;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(p, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail) {
            std::string b(avail, '\0');
            DWORD got = 0;
            if (!ReadFile(p, &b[0], avail, &got, nullptr)) return false;
            if (first && first->empty() && got > 8) *first = b.substr(8, got - 8);
            continue;
        }
        if ((int)(GetTickCount() - until) >= 0) return true;
        Sleep(20);
    }
}

HANDLE connect_pipe()
{
    for (int i = 0; i < 10; i++) {
        wchar_t name[64];
        swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE p = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (p != INVALID_HANDLE_VALUE) return p;
    }
    return INVALID_HANDLE_VALUE;
}

std::string activity_json(const std::string& song, int64_t since, const std::string& version)
{
    std::string a = "{\"details\":" + json_str(song.empty() ? "In the menus" : song) +
                    ",\"state\":" + json_str("Re:Fine " + version) +
                    ",\"assets\":{\"large_image\":\"logo\",\"large_text\":" + json_str("Re:Fine " + version) + "}";
    if (!song.empty() && since > 0) a += ",\"timestamps\":{\"start\":" + std::to_string(since) + "}";
    return a + "}";
}

DWORD WINAPI discord_thread(LPVOID)
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD last_send = 0, next_try = 0;
    unsigned nonce = 0;
    bool said_none = false;
    for (;;) {
        WaitForSingleObject(g_wake, 1000);
        std::string app, version, song;
        int64_t since;
        bool on, dirty;
        { std::lock_guard<std::mutex> lk(g_m); app = g_app; version = g_version; song = g_song; since = g_since; on = g_on; dirty = g_dirty; }
        const DWORD now = GetTickCount();
        if (!on || app.empty()) {
            if (pipe != INVALID_HANDLE_VALUE) {   // off: clear what we showed, then let go
                frame_write(pipe, 1, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
                                     "},\"nonce\":\"" + std::to_string(++nonce) + "\"}");
                drain(pipe, 300);
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
                fprintf(stderr, "[discord] off\n");
            }
            continue;
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            if ((int)(now - next_try) < 0) continue;
            pipe = connect_pipe();
            if (pipe == INVALID_HANDLE_VALUE) {
                if (!said_none) { fprintf(stderr, "[discord] no Discord client running; looking again every %u s\n", (unsigned)(kRetryMs / 1000)); said_none = true; }
                next_try = now + kRetryMs;
                continue;
            }
            std::string ready;
            if (!frame_write(pipe, 0, "{\"v\":1,\"client_id\":" + json_str(app) + "}") || !drain(pipe, 1500, &ready) || ready.empty()) {
                fprintf(stderr, "[discord] handshake failed\n");
                CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; next_try = now + kRetryMs;
                continue;
            }
            fprintf(stderr, "[discord] connected (%s)\n", ready.find("\"READY\"") != std::string::npos ? "READY" : "no READY in the answer");
            said_none = false;
            dirty = true;
        }
        if (!dirty || (last_send && now - last_send < kMinGap)) { if (!drain(pipe, 0)) { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; } continue; }
        const std::string cmd = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
                                ",\"activity\":" + activity_json(song, since, version) + "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
        std::string answer;
        if (!frame_write(pipe, 1, cmd) || !drain(pipe, 500, &answer)) {
            fprintf(stderr, "[discord] pipe lost\n");
            CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; next_try = now + 2000;
            continue;
        }
        { std::lock_guard<std::mutex> lk(g_m); if (g_song == song && g_since == since) g_dirty = false; }
        last_send = now;
        fprintf(stderr, "[discord] activity: %s%s\n", song.empty() ? "menus" : song.c_str(),
                answer.find("\"ERROR\"") != std::string::npos ? " (Discord answered ERROR)" : "");
    }
}

}  // namespace

void discord_start(const std::string& app_id, const std::string& version)
{
    std::lock_guard<std::mutex> lk(g_m);
    if (g_started) return;
    g_started = true;
    g_app = app_id;
    g_version = version;
    if (app_id.empty()) { fprintf(stderr, "[discord] no Application ID: Rich Presence stays off\n"); return; }
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CreateThread(nullptr, 64u << 10, discord_thread, nullptr, 0, nullptr);
}

void discord_enable(bool on)
{
    std::lock_guard<std::mutex> lk(g_m);
    if (g_on == on) return;
    g_on = on;
    g_dirty = true;
    if (g_wake) SetEvent(g_wake);
}

void discord_set_song(const std::string& song, int64_t since)
{
    std::lock_guard<std::mutex> lk(g_m);
    if (g_song == song && g_since == since) return;
    g_song = song;
    g_since = since;
    g_dirty = true;
    if (g_wake) SetEvent(g_wake);
}
