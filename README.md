# xeo3-recomp-kit

Run Xbox 360 games on PC through Microsoft's own back-compat emulator (XEO3), including
games Microsoft never released for back-compat.

> **What you need, exactly.** None of it is in this repository; it all has to be yours.
>
> | to run | you need | what it gives the kit |
> |---|---|---|
> | your game | the **game disc**, dumped to an ISO yourself | its `default.xex` (recompiled) and its data (used as is) |
> | any game | **Fuzion Frenzy**, the **PC** back-compat package (Microsoft Store) | the emulator: `Emu.exe` and the PC-side files |
> | any game | **Super Meat Boy**, the **Xbox One** back-compat package | the Xbox 360 system: kernel, xam, system DLLs, graphics layer |
> | any game | `kernelx.dll`, built from [XWine1/SlimEra](https://github.com/XWine1/SlimEra) | lets the Xbox One DLLs load on Windows |
> | the dashboard (optional) | **Castle Crashers**, the **Xbox One** back-compat package (the older 2014 kind) | its `Flash` folder: the dashboard, `dash.xex` 2.0.17003.0, and its boot files |
> | the dashboard (optional) | an **offline** Xbox 360 profile, and a Games-on-Demand package of each disc ([iso2god-rs](https://github.com/iliazeus/iso2god-rs)) | My Games |
>
> Those are the exact packages everything here was built and tested with. Others of the same
> kind may work but haven't been tried. Plus free tools: Visual Studio 2022, Python 3, Git,
> WitherOrNot's clang build and xdvdfs ([docs/SETUP.md](docs/SETUP.md)). How the packages
> combine: [docs/CONSOLE_FOLDER.md](docs/CONSOLE_FOLDER.md); the dashboard:
> [docs/DASHBOARD.md](docs/DASHBOARD.md).

**What gets recompiled.** A 360 game on disc is one program file, `default.xex` (the game's
code, Crackdown's is 14 MB), plus gigabytes of data: levels, textures, sound, video. Only
the program is recompiled. Its PowerPC code is translated to x86-64 and compiled into **one
DLL**. The data is never touched: the emulator reads it straight from your unpacked disc
folder, just as the 360 read it off the disc.

**Why a DLL.** XEO3 doesn't interpret PowerPC code. For every program it runs (the 360's
kernel and system software, and the game) it loads a DLL holding that program's code already
translated to PC code. Microsoft made those DLLs only for the games on its back-compat
list. This kit makes the game's one yourself: it recompiles the `default.xex` from a disc you
own into C++, compiles that into a DLL with the name the emulator looks for, and adds a small
hand-written runtime that fills the gaps (saves without a dashboard, sign-in, audio decoding,
graphics fixes, crash diagnostics). The 360's kernel and system software come, already
translated, from Microsoft's own back-compat files.

It is built on [WitherOrNot's XenonRecomp-XEO3](https://github.com/WitherOrNot/XenonRecomp-XEO3),
the proof of concept that showed XEO3 can load a recompiled game, which itself is a fork of
[hedge-dev's XenonRecomp](https://github.com/hedge-dev/XenonRecomp). This kit carries that
fork much further: a recompiler that handles full retail games, a runtime proven on three of
them, and scripts that turn the whole process into six commands.

## What has run with it

| Game | State |
|---|---|
| **Crackdown** | Boots from the disc folder, campaign playable, sound (XMA) and saves work, ~21 fps. Open: the lower part of the 3D view is black (a tiling/resolve bug in the emulator's GPU path). Its full config ships in [`examples/crackdown`](examples/crackdown). |
| **Need for Speed: Most Wanted** | Menus, garage and races. The graphics fixes in this runtime (predicated tiling, colour order, sky/bloom) were first made and confirmed here. |
| **Fable II** | Boots and renders, then freezes about 30 seconds in (a mutex the game never gets back). Open. |

Every game is different: expect each new title to need some of the debugging described in
[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md). The kit gives you the pipeline, the
fixes found so far and the tools to find the next ones. It doesn't promise a playable result.

## What you need

**Bring your own (none of it is in this repository):**
- the game: an Xbox 360 disc **you own**, as an ISO you dumped yourself
- a **XEO3 console folder**: Microsoft's emulator and the 360 system files it runs
  (kernel, xam, ...), assembled from back-compat packages you own. The kit was made with
  **Fuzion Frenzy** (PC package: the emulator) and **Super Meat Boy** (Xbox One package:
  the 360 system stack). See [docs/CONSOLE_FOLDER.md](docs/CONSOLE_FOLDER.md).

**Free tools (setup.ps1 checks for all of them):**
- Windows 10/11 x64 with a CPU that has AVX
- Visual Studio 2022 or later (Community or Build Tools) with *Desktop development with C++*
  and *C++ CMake tools for Windows*
- [Python 3](https://www.python.org/) and [Git](https://git-scm.com/)
- WitherOrNot's custom LLVM/clang build (the generated code needs the emulator's calling
  convention, which only that build has). See [docs/SETUP.md](docs/SETUP.md).
- [xdvdfs](https://github.com/antangelo/xdvdfs) to unpack the disc image

## Quick start

```powershell
git clone --recursive <this repository> xeo3-recomp-kit
cd xeo3-recomp-kit
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1                          # once
xdvdfs unpack "D:\isos\MyGame.iso" "D:\games\MyGame"                                # your disc
powershell -ExecutionPolicy Bypass -File scripts\new-game.ps1  -Name mygame -Xex "D:\games\MyGame\default.xex"
powershell -ExecutionPolicy Bypass -File scripts\recompile.ps1 -Game mygame
powershell -ExecutionPolicy Bypass -File scripts\build.ps1     -Game mygame
powershell -ExecutionPolicy Bypass -File scripts\install.ps1   -Game mygame -Console "D:\XeO3 Console"
powershell -ExecutionPolicy Bypass -File scripts\run.ps1       -Game mygame -Console "D:\XeO3 Console" -Disc "D:\games\MyGame"
```

The full walkthrough, including the manual checks between those steps, is
**[docs/TUTORIAL.md](docs/TUTORIAL.md)**. Read it before your first game.

## Documentation

| | |
|---|---|
| [SETUP.md](docs/SETUP.md) | installing the toolchain |
| [CONSOLE_FOLDER.md](docs/CONSOLE_FOLDER.md) | building the folder the emulator runs from, and patching its GPU layer |
| [TUTORIAL.md](docs/TUTORIAL.md) | a game, start to finish, with Crackdown's real numbers |
| [RUNTIME.md](docs/RUNTIME.md) | the hand-written runtime: saves, sign-in, audio, graphics fixes, the switch files |
| [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | crashes, freezes, black screens, and how they were tracked down |
| [HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) | XEO3 internals: DLL exports, naming, calling convention |
| [ORIGINAL_XBOX.md](docs/ORIGINAL_XBOX.md) | original Xbox games on the same emulator |
| [DASHBOARD.md](docs/DASHBOARD.md) | the 360 dashboard: My Games, Play Disc and the Guide for your recompiled games |
| [recompiler/README.md](recompiler/README.md) | the recompiler and what this fork adds |

## Layout

```
recompiler/   XenonRecomp + XenonAnalyse (the fork), third-party libraries as submodules
runtime/      hand-written C++ compiled into every game DLL (see docs/RUNTIME.md)
scripts/      setup, new-game, recompile, build, install, run; recompile-dashboard,
              install-dashboard, add-game-to-my-games
tools/        Python helpers the scripts use, and debugging tools
examples/     crackdown/: a complete, working config
dashboard/    the 360 dashboard's config, its runtime and the title-switch relay
games/        your games (created by new-game.ps1; ignored by git)
docs/
```

## Original Xbox games

The same emulator also runs original Xbox games, through the 360's own original-Xbox emulator
(`xefu.xex`) and Microsoft's `xefu` DLLs, all from back-compat packages. That works for
Microsoft's own packages. Running an original Xbox game they never shipped is untested.
[docs/ORIGINAL_XBOX.md](docs/ORIGINAL_XBOX.md) has what is known.

## The dashboard

The kit also recompiles the Xbox 360 **NXE dashboard** from your own copy of `dash.xex`.
In a console folder it boots like a 360: your profile signs in, your recompiled games are
listed under **My Games** and launch from there or from **Play Disc**, the **Guide button**
works, and quitting a game brings the dashboard back. It runs offline (no Xbox Live).
[docs/DASHBOARD.md](docs/DASHBOARD.md) has what you need to supply, the steps, and what has
and hasn't been tested yet.

## Legal

This repository contains only original code and documentation, plus open-source
dependencies under their own licenses. It contains **no** Microsoft software, no game code
and no game data, and it never will: pull requests adding any of those will be closed. You
need to own the games and the back-compat packages you use with it. Xbox, Xbox 360 and
related names are trademarks of Microsoft; this project is not affiliated with or endorsed
by Microsoft or by any game publisher.

## Credits and license

- [hedge-dev/XenonRecomp](https://github.com/hedge-dev/XenonRecomp), the recompiler this all
  stands on (MIT)
- [WitherOrNot](https://github.com/WitherOrNot): the XEO3 research, the XEO3 fork of
  XenonRecomp and the custom clang (MIT)
- [XWine1/SlimEra](https://github.com/XWine1/SlimEra): `kernelx.dll`, used by the console
  folder
- [antangelo/xdvdfs](https://github.com/antangelo/xdvdfs): disc unpacking

Code in this repository is released under the MIT license ([LICENSE](LICENSE)).
