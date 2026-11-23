# Building Re:Fine

You need your own decrypted `EBOOT.elf` of *Hatsune Miku: Project DIVA F* (BLUS31319, v01.00). It is not
part of this repository and is never downloaded. Decrypt `EBOOT.BIN` with RPCS3
(*Utilities > Decrypt PS3 Binaries*) and keep the result outside git.

## Tools
Windows 10/11 x64, Git, PowerShell 5.1, Python 3.9+, LLVM (`clang-cl`), Visual Studio Build Tools with the
C++ workload (headers and `link.exe`), CMake.

## Steps (from the repository root)
```
git submodule update --init external/ps3recomp                 # ps3recomp, pinned commit
powershell -ExecutionPolicy Bypass -File tools\apply_patches.ps1 # pinned commit + patches\*.patch
powershell -ExecutionPolicy Bypass -File tools\fetch_ffmpeg.ps1  # each fetch script checks a pinned SHA-256
powershell -ExecutionPolicy Bypass -File tools\fetch_sdl3.ps1
powershell -ExecutionPolicy Bypass -File tools\fetch_rmlui.ps1
powershell -ExecutionPolicy Bypass -File tools\fetch_vulkan.ps1  # Vulkan headers + DXC
# put the decrypted executable at game\EBOOT.elf
powershell -ExecutionPolicy Bypass -File tools\lift.ps1          # PPU code -> work\recompiled (generated, not in git)
powershell -ExecutionPolicy Bypass -File tools\lift_spu.ps1      # SPU programs -> work\spu
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -InlineMem   # Release build -> build\ReFine.exe
```
`tools\build.ps1` finds clang-cl through `REFINE_LLVM_BIN` or `C:\Program Files\LLVM\bin`. Without
`-InlineMem` you get the faster-to-compile development build (slower at run time).

Without the Vulkan headers the CMake step stops (`REFINE_VULKAN=ON` by default); it does not silently build a
Direct3D 12-only exe. Vulkan needs `dxcompiler.dll` next to `ReFine.exe` (the build copies it from `work\dxc`).

## Smoke test
`tools\smoke.ps1` runs the title screen and a short stretch of a song and prints at most 10 lines: exit code,
unresolved imports, shader failures, frame pacing, and whether the song script was opened. Logs go to `work\run\`.

## Installer
`tools\installer\build_setup.ps1 -Version <x.y.z>` builds the public installer in `dist\`. It carries the
sources and tools, **not** `ReFine.exe`: the installer lifts and compiles on the user's PC. See
[INSTALLER.md](INSTALLER.md).

## Rules for contributions
- Generated code is never edited by hand: fixes go in `patches\` (numbered, applied on top of ps3recomp) or
  in `src\`, then regenerate.
- Nothing from the game goes in git: no executables, firmware, assets, logs, captures or saves.
- RPCS3 (GPL-2.0) is a behavioral reference only. Do not copy its code here.
