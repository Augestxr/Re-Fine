// Re:Fine mods (hito 6e, docs/plan_6h.md P1): file replacement in the style of Unleashed.
//
// <exe folder>\mods\<folder>\mod.ini   [mod] title, author, version, description, game, priority
// <exe folder>\mods\<folder>\rom\...   files laid out like the disc's PS3_GAME\USRDIR
//
// A guest open of /dev_bdvd/PS3_GAME/USRDIR/<path> is served from the first ON mod (highest priority, then
// folder name) that has <path>. The index is built once at boot, before the game runs, and is read-only after:
// switching a mod on or off applies after a restart. REFINE_MODS_DIR overrides the folder; REFINE_MODS_LOG=1
// prints each redirected path once.
#pragma once
#include <string>
#include <vector>

struct ModInfo {
    std::string folder;     // the folder's name under mods\ (its id in refine.ini: mod.<folder>)
    std::string dir;        // full path, with a trailing backslash
    std::string title, author, version, desc, game;
    int priority = 0;
    size_t files = 0;       // files under its subfolders
    bool for_this_game = true;
};

// The mods folder of this install (REFINE_MODS_DIR, else <exe_dir>mods).
std::string mods_dir(const std::string& exe_dir);
// Every subfolder of `dir` with a mod.ini, sorted by priority (high first), then folder name.
std::vector<ModInfo> mods_scan(const std::string& dir);
// Builds the index from the mods that are ON (in the order given: the first that has a file wins) and installs
// the runtime's redirect hook (0177). With none, the hook stays off. Returns the number of files indexed.
size_t mods_activate(const std::vector<const ModInfo*>& on);
