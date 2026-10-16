# Setup

Everything here is free. Do it once, then `scripts\setup.ps1` checks it and builds the
recompiler.

## 1. Visual Studio

Install **Visual Studio 2022 or later** (Community is fine, or the smaller *Build Tools for
Visual Studio*) with:

- the **Desktop development with C++** workload
- the individual component **C++ CMake tools for Windows** (this brings `cmake` and `ninja`)

The scripts find it with `vswhere`. If you have several versions installed and want a
specific one, set `XEO3_VCVARS` to its `vcvars64.bat`, e.g.
`C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`.

## 2. Python and Git

- [Python 3.10+](https://www.python.org/downloads/): tick *Add python.exe to PATH*
- [Git for Windows](https://git-scm.com/download/win)

## 3. WitherOrNot's clang (required)

The emulator calls recompiled code with its own register convention (see
[HOW_IT_WORKS.md](HOW_IT_WORKS.md)). Stock clang can't produce it; WitherOrNot's LLVM build
replaces `preserve_none` with exactly that convention. **A DLL built with any other compiler
loads and then crashes.**

1. Download the Windows build from
   [WitherOrNot/llvm-project releases](https://github.com/WitherOrNot/llvm-project/releases).
2. Extract it so that this file exists:
   `xeo3-recomp-kit\toolchain\clang-xeo3\bin\clang-cl.exe`
   (or extract it anywhere and set `XEO3_CLANG_CL` to the full path of `clang-cl.exe`).
3. Check it:
   ```powershell
   toolchain\clang-xeo3\bin\clang-cl.exe --version
   ```
   The first line must name `https://github.com/WitherOrNot/llvm-project.git`. The kit was
   tested with `clang version 24.0.0git (... 806935948ba3532ba559d614f97cc7688d9cb2ee)`.

`toolchain\` is in `.gitignore`, so the compiler never ends up in your commits.

## 4. xdvdfs

[xdvdfs](https://github.com/antangelo/xdvdfs/releases) unpacks a 360 disc image into a
plain folder, which is what the emulator boots from:

```powershell
xdvdfs unpack "D:\isos\Crackdown.iso" "D:\games\Crackdown"
```

## 5. Optional

- **Debugging Tools for Windows** (part of the
  [Windows SDK](https://developer.microsoft.com/windows/downloads/windows-sdk/) installer):
  `cdb.exe`, used by `tools\read_crash_dump.ps1`.
- **FFmpeg 7.x shared DLLs** (`avcodec-61.dll`, `avutil-59.dll`) for game audio. See
  [RUNTIME.md](RUNTIME.md#audio-xma).

## 6. Run setup

Clone with submodules (the recompiler's third-party libraries are git submodules; a ZIP
download won't have them):

```powershell
git clone --recursive <this repository> xeo3-recomp-kit
cd xeo3-recomp-kit
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1
```

It prints each prerequisite it found, fetches any missing submodules, and builds
`recompiler\build\XenonRecomp\XenonRecomp.exe`, `XenonAnalyse.exe` and `XexRepack.exe`.

Re-run it whenever `recompiler\` changes. The other scripts refuse to run if
`XenonRecomp.exe` is older than its own sources. A stale recompiler once silently emitted
the old form of every `setjmp`/`longjmp` in a game, and nothing else caught it.

Next: [CONSOLE_FOLDER.md](CONSOLE_FOLDER.md).
