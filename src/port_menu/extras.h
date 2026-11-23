// Small things around the game, asked from the port's menu (6g): copies of the save data and screenshots.
// Windows only for now (hito 9: src/platform).
#pragma once
#include <string>
#include <windows.h>

// Copies <hdd0>\home\00000001\savedata to <hdd0>\..\backups\<date>_<time> when it differs
// from the newest copy there, and keeps the newest <keep>. Call it before the game opens a save.
void extras_backup_saves(const std::wstring& hdd0, int keep);

// The client area of the window as it is on the screen, to a PNG in Pictures\ReFine, on a thread of its own.
// done(path) runs on that thread: the file, or "" when it could not be made.
void extras_screenshot(HWND h, void (*done)(const std::wstring& path));
