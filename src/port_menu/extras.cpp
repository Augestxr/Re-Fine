#include "extras.h"

#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace fs = std::filesystem;

namespace {

std::wstring stamp()
{
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t b[32];
    swprintf(b, 32, L"%04d-%02d-%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

bool same_bytes(const fs::path& a, const fs::path& b)
{
    FILE* fa = _wfopen(a.c_str(), L"rb");
    FILE* fb = _wfopen(b.c_str(), L"rb");
    bool same = fa && fb;
    std::vector<char> x(1 << 16), y(1 << 16);
    while (same) {
        const size_t na = fread(x.data(), 1, x.size(), fa), nb = fread(y.data(), 1, y.size(), fb);
        same = na == nb && !memcmp(x.data(), y.data(), na);
        if (na < x.size()) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

// The files under both folders are the same, name by name and byte by byte.
bool same_tree(const fs::path& a, const fs::path& b)
{
    auto list = [](const fs::path& r) {
        std::vector<std::wstring> v;
        for (const auto& e : fs::recursive_directory_iterator(r)) if (e.is_regular_file()) v.push_back(fs::relative(e.path(), r).wstring());
        std::sort(v.begin(), v.end());
        return v;
    };
    const auto la = list(a), lb = list(b);
    if (la != lb) return false;
    for (const auto& f : la) if (!same_bytes(a / f, b / f)) return false;
    return true;
}

void backup(fs::path hdd0, int keep)
{
    std::error_code ec;
    const fs::path src = hdd0 / L"home" / L"00000001" / L"savedata";
    const fs::path root = fs::absolute(hdd0, ec).parent_path() / L"backups";
    try {
        if (!fs::exists(src) || fs::is_empty(src)) { fprintf(stderr, "[backup] no save data yet\n"); return; }
        fs::create_directories(root);
        std::vector<fs::path> have;
        for (const auto& e : fs::directory_iterator(root)) {
            if (!e.is_directory()) continue;
            if (e.path().extension() == L".part") fs::remove_all(e.path());   // a copy that did not finish
            else if (e.path().filename().wstring().size() == 17) have.push_back(e.path());
        }
        std::sort(have.begin(), have.end());
        if (!have.empty() && same_tree(src, have.back())) { fprintf(stderr, "[backup] the save data has not changed since %ls\n", have.back().filename().c_str()); return; }
        const fs::path dst = root / stamp(), part = dst.wstring() + L".part";
        fs::copy(src, part, fs::copy_options::recursive);
        fs::rename(part, dst);                     // a half-made copy never looks like a good one
        have.push_back(dst);
        fprintf(stderr, "[backup] save data copied to backups\\%ls\n", dst.filename().c_str());
        // Only folders this function names (YYYY-MM-DD_HHMMSS) are ever removed.
        while ((int)have.size() > keep) { fs::remove_all(have.front()); have.erase(have.begin()); }
    } catch (const std::exception& e) {
        fprintf(stderr, "[backup] failed: %s\n", e.what());
    }
}

bool write_png(const std::wstring& path, const std::vector<BYTE>& px, UINT w, UINT h)
{
    IWICImagingFactory* f = nullptr; IWICStream* s = nullptr; IWICBitmapEncoder* e = nullptr;
    IWICBitmapFrameEncode* fr = nullptr; IWICBitmap* bmp = nullptr; IWICFormatConverter* cv = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR, w * 4, (UINT)px.size(), (BYTE*)px.data(), &bmp);
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&cv);
    if (SUCCEEDED(hr)) hr = cv->Initialize(bmp, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = f->CreateStream(&s);
    if (SUCCEEDED(hr)) hr = s->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e);
    if (SUCCEEDED(hr)) hr = e->Initialize(s, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = e->CreateNewFrame(&fr, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = fr->SetSize(w, h);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&pf);
    if (SUCCEEDED(hr)) hr = fr->WriteSource(cv, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = e->Commit();
    for (IUnknown* u : {(IUnknown*)fr, (IUnknown*)e, (IUnknown*)s, (IUnknown*)cv, (IUnknown*)bmp, (IUnknown*)f}) if (u) u->Release();
    return SUCCEEDED(hr);
}

std::wstring shot(HWND h)
{
    RECT rc; POINT o = {0, 0};
    if (!h || !GetClientRect(h, &rc) || rc.right <= 0 || rc.bottom <= 0 || !ClientToScreen(h, &o)) return L"";
    const int w = rc.right, t = rc.bottom;
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof bi.bmiHeader, w, -t, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ was = SelectObject(mem, bm);
    const BOOL ok = bm && BitBlt(mem, 0, 0, w, t, scr, o.x, o.y, SRCCOPY);
    GdiFlush();
    std::vector<BYTE> px;
    if (ok) px.assign((BYTE*)bits, (BYTE*)bits + (size_t)w * t * 4);
    SelectObject(mem, was);
    if (bm) DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    if (!ok) return L"";
    // REFINE_SHOT_DIR: where test runs put them.
    std::wstring dir;
    if (const wchar_t* e = _wgetenv(L"REFINE_SHOT_DIR")) dir = e;
    else {
        PWSTR p = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &p))) return L"";
        dir = std::wstring(p) + L"\\ReFine";
        CoTaskMemFree(p);
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::wstring path = dir + L"\\" + stamp() + L".png";
    for (int i = 2; fs::exists(path, ec) && i < 100; i++) path = dir + L"\\" + stamp() + L"_" + std::to_wstring(i) + L".png";
    return write_png(path, px, (UINT)w, (UINT)t) ? path : L"";
}

} // namespace

void extras_backup_saves(const std::wstring& hdd0, int keep)
{
    backup(hdd0, keep);
}

void extras_screenshot(HWND h, void (*done)(const std::wstring& path))
{
    std::thread([h, done] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        done(shot(h));
        CoUninitialize();
    }).detach();
}
