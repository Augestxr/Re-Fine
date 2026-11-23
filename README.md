<div align="center">

<!-- ART SLOT: top banner (about 1280x400). Own art only: no game assets, screenshots or official logos.
     Upload the image by dragging it into GitHub's web editor (it gives a user-attachments URL; the
     pre-commit hook rejects .png/.jpg in the repo) and uncomment:
<img src="BANNER_URL" alt="Re:Fine" width="100%">
-->

# Re:Fine

**A native PC port of *Hatsune Miku: Project DIVA F* (PS3), built by static recompilation.**
<br>The game's own code, translated to C/C++ and running natively on Windows, at any frame rate.

[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D6?logo=windows&logoColor=white)](#-requirements)
[![Renderer](https://img.shields.io/badge/renderer-Vulkan%20%7C%20D3D12-AC162C?logo=vulkan&logoColor=white)](#-features)
[![Frame rate](https://img.shields.io/badge/fps-30%20to%201000%20%2B%20unlimited-39C5BB)](#-features)
[![Built with](https://img.shields.io/badge/built%20with-ps3recomp-555555)](https://github.com/sp00nznet/ps3recomp)
[![Language](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](src/)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Status](https://img.shields.io/badge/status-work%20in%20progress-orange)](#-known-issues)

**English** · [Español](README.es.md)

</div>

> [!IMPORTANT]
> **Fan project. Not affiliated with, endorsed by or sponsored by SEGA or Crypton Future Media.**
> You need **your own, legally dumped copy** of the game. **No game files, firmware, keys or assets
> are included or distributed here**, and none are ever downloaded. All trademarks belong to their
> owners and are used only to name the game this port works with.

## 💡 What is this?

Re:Fine takes the PS3 executable of *Project DIVA F* (BLUS31319, USA, version 01.00) and translates it
ahead of time into C/C++ with [ps3recomp](https://github.com/sp00nznet/ps3recomp): both the PPU code and
the SPU programs. The result is linked with a runtime that stands in for the PS3 (graphics, audio,
input, files, threads) and compiled into a normal `ReFine.exe`.

It is **not an emulator** and **not a remake**: the game that runs is SEGA's original code, on your
PC, built from your own copy. This repository only holds the tools, patches and runtime that make
that possible.

<!-- ART SLOT: section divider or illustration (about 1280x200). Own art only. Uncomment when ready:
<p align="center"><img src="DIVIDER_URL" alt="" width="100%"></p>
-->

## ✨ Features

- 🎞️ **Any frame rate.** 60 fps by default; the menu offers 30 to 360 and Unlimited, and `refine.ini`
  takes anything from 30 to 1000, `unlimited`, or `display` (match the main monitor). The game keeps
  its normal speed, and note judgement and audio stay tied to the audio clock.
- 🎯 **30 fps is the original.** At 30 fps nothing is changed: it is the game as it shipped.
- 🖥️ **Vulkan renderer** with a Direct3D 12 fallback; render height up to 2160p, anisotropic
  filtering, sharpening and resampling options.
- ⚙️ **In-game menu on F1** for video, audio, controls and interface. Settings live in
  `%LOCALAPPDATA%\ReFine\refine.ini`, logs in `%LOCALAPPDATA%\ReFine\logs`.
- 🎮 Keyboard, mouse in the game's menus, and controllers through SDL3.
- 🧰 **Optional extras** (off by default): judgement offset, timing display, skip the intro movie,
  automatic save backups, F12 screenshots and mute in background. Discord Rich Presence is optional.
- 🔁 Each high frame rate fix has a `REFINE_*` switch to turn it off and get the original behaviour back.

## 📋 Requirements

- Windows 10 or 11, x64.
- A GPU with Vulkan (preferred) or Direct3D 12.
- **Your own copy** of the disc (BLUS31319, USA, v01.00, no update), extracted to a folder that
  contains `PS3_GAME`.
- [RPCS3](https://rpcs3.net/) with its firmware installed, used **once** to decrypt `EBOOT.BIN`
  (*Utilities → Decrypt PS3 Binaries*). Re:Fine contains no decryption code and no keys.
- About 10 GB of free disk space, and an administrator prompt once if the Visual Studio Build Tools
  (C++ workload) need to be installed. *These figures are estimates, not measured on a clean PC.*
- Time: the game is translated and compiled **on your PC**. On the development PC the compile step
  alone takes about 6 minutes with all threads; expect longer on smaller machines.

## 🚀 Installation

1. Get the installer, `ReFineSetup-<version>.exe`. No release has been published here yet, so for
   now build it yourself with `tools\installer\build_setup.ps1 -Version <x.y.z>`
   (see [docs/BUILD.md](docs/BUILD.md)).
2. Run it and pick your game folder. If LLVM, Python or the Build Tools are missing, the installer
   offers to download them from their official sources and checks their hashes and signatures.
3. Wait for the translation and compile steps. The result is `ReFine.exe` in the install folder.
4. Play. Press **F1** at any time for the port's menu.

### Building from source

```powershell
git clone --recurse-submodules https://github.com/Augestxr/Re-Fine.git
cd Re-Fine
```

Then follow [docs/BUILD.md](docs/BUILD.md): apply the patches to ps3recomp, fetch the pinned
libraries, put your decrypted executable at `game\EBOOT.elf`, and run the lift and build scripts.
The installer is described in [docs/INSTALLER.md](docs/INSTALLER.md).

## 🐞 Known issues

- **Hair and cloth physics.** Above 30 fps the small hair pieces (bangs) and chest physics still step
  30 times a second, blended in between. Cloth has not been checked in every song.
- **Frame pacing.** At 60 fps a few frames still arrive unevenly. With V-Sync, a rate that does not
  divide your monitor's (120 on a 180 Hz screen) can look uneven: use **Match display**.
- **Not every song has been checked** at high frame rates yet; a census of the rest is in progress.
- **`display` mode** reads the main monitor only; with the window on a monitor of a different refresh
  rate it may pick the wrong value.
- **F / F2nd selector.** The menu can already switch between F and F2nd, but F2nd is not ported yet
  and shows "port pending".
- **Controllers.** Vibration, remapping and double stars on a physical controller are not fully verified.
- **Tested on few PCs.** PCs with little RAM and install paths with non-ASCII characters are untested.
  Machines without a recent Vulkan driver fall back to D3D12.

<!-- ART SLOT: second divider (about 1280x200). Own art only. Uncomment when ready:
<p align="center"><img src="DIVIDER2_URL" alt="" width="100%"></p>
-->

## 🗂️ Repository layout

| Path | What it is |
|---|---|
| [`src/`](src/) | the port's own runtime: window, input, audio, renderer glue, timing, in-game menu |
| [`patches/`](patches/) | patches applied on top of ps3recomp (pinned in `external/`), numbered in order |
| [`config/`](config/) | lifter configuration: function hooks and instruction patches (by address and hash) |
| [`tools/`](tools/) | build, lift, test and measurement scripts, and the installer |
| [`third_party/`](third_party/) | bundled libraries with their licenses |
| [`docs/`](docs/) | how to build, the installer, and notes on video decoding |

## 🤖 about artificial intelligence 

So yeah, of course artificial intelligence helped build this port, and i dont really care in the slightest
what anyone thinks about that. Fr, the people who criticize it should maybe try making projects of
their own if theyre so smart, because they never contribute anything to the community rather than just criticising and doing song mods to mm+ but anyone could do that.
Anyone can complain and keep crying while i complete this project alr?. You are welcome to prove otherwise with code tho, but i know yall could never ❤ xx. 
And no. You are not cool, mysterious, nonchalant and dont even mention BETTER for refusing to play ports like this, you are just cringe and should retire.

## 🙏 Credits and license

Our code is under the [MIT License](LICENSE). Third-party code and its licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) (MIT): the static recompiler and runtime this
  port is built on.
- [ico-recomp](https://github.com/feanor-noldorin/ico-recomp) (MIT) and the
  [JonathanDC64 fork of ps3recomp](https://github.com/JonathanDC64/ps3recomp): fixes and ideas we adapted.
- [SDL3](https://github.com/libsdl-org/SDL) (zlib), [FFmpeg](https://ffmpeg.org/) (LGPL-3.0, separate DLLs),
  [RmlUi](https://github.com/mikke89/RmlUi) (MIT), FreeType (FTL), DirectX Shader Compiler,
  Vulkan-Headers and stb.
- Fonts: Inter, Barlow, Chakra Petch, Delagothic and IBM Plex Mono (SIL OFL 1.1).
- [RPCS3](https://rpcs3.net/) (GPL-2.0) is used only as an external reference for behaviour. It is
  neither bundled nor linked, and no RPCS3 code is copied here.

Thanks to the ps3recomp, ico-recomp and RPCS3 projects, and to everyone keeping this game alive. 💙

<!-- ART SLOT: footer banner (about 1280x240). Own art only. Uncomment when ready:
<p align="center"><img src="FOOTER_URL" alt="" width="100%"></p>
-->
