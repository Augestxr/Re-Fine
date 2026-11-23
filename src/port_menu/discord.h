// Re:Fine: Discord Rich Presence through Discord's local IPC pipe (\\.\pipe\discord-ipc-N), our own code,
// no Discord SDK. Nothing leaves the PC except through the Discord client the user runs; without it, or
// without an Application ID, this does nothing.
#pragma once
#include <cstdint>
#include <string>

// Starts the background thread (once). app_id empty: never connects.
void discord_start(const std::string& app_id, const std::string& version);
// The menu's switch, live: off clears the presence and closes the pipe.
void discord_enable(bool on);
// What to show. song empty: in the menus. since: Unix seconds the song started (0: none).
void discord_set_song(const std::string& song, int64_t since);
