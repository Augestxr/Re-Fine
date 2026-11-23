// Re:Fine mods: see mods.h. Own code (no asset is read here: files are only listed and their paths handed to
// the game's own loaders through the runtime's path translation, 0177).
#include "mods.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>

// The game this exe runs, for mod.ini's "game" (P2 makes it per target). Empty in mod.ini = any game.
static const char* const kGameKeys[] = {"F", "BLUS31319"};

extern "C" int (*ps3_vfs_redirect_hook)(const char* guest, char* host, size_t cap);   // sys_fs.c, 0177

namespace {

std::unordered_map<std::string, std::string> g_index;   // lower-case USRDIR-relative path ("rom/x/y") -> host path
bool g_log;
std::mutex g_log_mx;
std::set<std::string> g_logged;

std::string lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n' || s[a] == '"')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n' || s[b - 1] == '"')) b--;
    return s.substr(a, b - a);
}

// Every file under `dir` + `rel` (recursive), into `out` as rel-path -> host path. Wide API: a mod's file
// names may not be ASCII; the key and the host path go out as UTF-8 (ASSUMPTION: the runtime opens UTF-8 paths
// correctly; only ASCII names were tested).
void walk(const std::wstring& dir, const std::string& rel, std::vector<std::pair<std::string, std::string>>& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char name[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, nullptr, nullptr);
        const std::string r = rel.empty() ? std::string(name) : rel + "/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk(dir + fd.cFileName + L"\\", r, out);
        else if (!rel.empty()) {   // files at the mod's root (mod.ini, a readme, a picture) are not game files
            char host[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, (dir + fd.cFileName).c_str(), -1, host, sizeof host, nullptr, nullptr);
            out.push_back({lower(r), host});
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
std::wstring widen(const std::string& s)
{
    std::wstring w(s.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], (int)w.size());
    w.resize(n > 0 ? (size_t)n - 1 : 0);
    return w;
}

int redirect(const char* guest, char* host, size_t cap)
{
    static const char kPrefix[] = "/dev_bdvd/PS3_GAME/USRDIR/";
    if (_strnicmp(guest, kPrefix, sizeof kPrefix - 1) != 0) return 0;
    std::string key;
    for (const char* p = guest + sizeof kPrefix - 1; *p; p++) {
        char c = *p == '\\' ? '/' : *p;
        if (c == '/' && (key.empty() || key.back() == '/')) continue;   // "//" from a joined path
        key += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    auto it = g_index.find(key);
    if (it == g_index.end() || it->second.size() + 1 > cap) return 0;
    memcpy(host, it->second.c_str(), it->second.size() + 1);
    if (g_log) {
        std::lock_guard<std::mutex> lk(g_log_mx);
        if (g_logged.insert(key).second) fprintf(stderr, "[mods] %s -> %s\n", guest, host);
    }
    return 1;
}

} // namespace

std::string mods_dir(const std::string& exe_dir)
{
    const char* e = getenv("REFINE_MODS_DIR");
    std::string d = e && *e ? std::string(e) : exe_dir + "mods";
    if (!d.empty() && d.back() != '\\' && d.back() != '/') d += '\\';
    return d;
}

std::vector<ModInfo> mods_scan(const std::string& dir)
{
    std::vector<ModInfo> mods;
    const std::wstring wdir = widen(dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((wdir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return mods;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        char name[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, nullptr, nullptr);
        ModInfo m;
        m.folder = name;
        m.dir = dir + name + "\\";
        FILE* f = _wfopen((wdir + fd.cFileName + L"\\mod.ini").c_str(), L"rb");
        if (!f) { fprintf(stderr, "[mods] %s: no mod.ini, skipped\n", name); continue; }
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            const std::string l = line;
            const size_t eq = l.find('=');
            if (l.empty() || l[0] == ';' || l[0] == '#' || l[0] == '[' || eq == std::string::npos) continue;
            const std::string k = lower(trim(l.substr(0, eq))), v = trim(l.substr(eq + 1));
            if (k == "title") m.title = v;
            else if (k == "author") m.author = v;
            else if (k == "version") m.version = v;
            else if (k == "description") m.desc = v;
            else if (k == "game") m.game = v;
            else if (k == "priority") m.priority = atoi(v.c_str());
        }
        fclose(f);
        if (m.title.empty()) m.title = m.folder;
        if (!m.game.empty()) {
            m.for_this_game = false;
            for (const char* g : kGameKeys) if (!_stricmp(m.game.c_str(), g)) m.for_this_game = true;
        }
        std::vector<std::pair<std::string, std::string>> files;
        walk(wdir + fd.cFileName + L"\\", "", files);
        m.files = files.size();
        mods.push_back(std::move(m));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(mods.begin(), mods.end(), [](const ModInfo& a, const ModInfo& b) {
        return a.priority != b.priority ? a.priority > b.priority : _stricmp(a.folder.c_str(), b.folder.c_str()) < 0;
    });
    return mods;
}

size_t mods_activate(const std::vector<const ModInfo*>& on)
{
    g_index.clear();
    g_log = getenv("REFINE_MODS_LOG") && atoi(getenv("REFINE_MODS_LOG")) != 0;
    for (const ModInfo* m : on) {
        std::vector<std::pair<std::string, std::string>> files;
        walk(widen(m->dir), "", files);
        size_t won = 0;
        for (auto& kv : files) if (g_index.emplace(kv.first, kv.second).second) won++;
        fprintf(stderr, "[mods] on: %s (%s), %zu files, %zu served by it\n", m->folder.c_str(), m->title.c_str(), files.size(), won);
    }
    // Set before the game runs and never changed after (port_menu_start, before ppu_run): no lock on the
    // translators' path.
    ps3_vfs_redirect_hook = g_index.empty() ? nullptr : redirect;
    fprintf(stderr, "[mods] %zu mods on, %zu files replaced or added%s\n", on.size(), g_index.size(), g_index.empty() ? " (hook off)" : "");
    return g_index.size();
}
