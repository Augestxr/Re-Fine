# The installer

`tools\installer\build_setup.ps1 -Version <x.y.z>` produces `dist\ReFineSetup-<x.y.z>\` with `ReFineSetup.exe`,
its `ui\` and a `payload\`.

**What the public payload holds:** FFmpeg and SDL3 DLLs, `dxcompiler.dll` (Microsoft, signed), the MSVC runtime
DLLs, `licenses\`, `refine.cmd`, `refine.ini`, `build_game.ps1` and `src\` (this repository's sources, the patched
ps3recomp, third-party headers, lift/build scripts). It holds **no** `ReFine.exe`, no game data, no saves and no
keys; the build script refuses ELF/SCE/PSF headers, generated code and saves.

**What it does on the user's PC (`build_game.ps1`):**
1. Checks for LLVM, a real Python (3.9+) and the Visual Studio C++ Build Tools. Missing ones are downloaded
   from their official sources with pinned SHA-256 (LLVM, Python) or a checked Microsoft signature (Build
   Tools; one administrator prompt).
2. Decrypts nothing itself: it uses the user's RPCS3 to produce `EBOOT.elf` from their own `EBOOT.BIN`.
3. Fetches the Vulkan headers, then lifts the PPU and SPU code, compiles, and copies `ReFine.exe`, `ui\` and
   `dxcompiler.dll` to the install folder. Log: `data\logs\install_build.log`.

Uninstall asks whether to keep saves and settings (kept by default).
