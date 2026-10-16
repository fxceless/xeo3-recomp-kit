# The console folder

The **console folder** is the folder `Emu.exe` runs from. It is a whole Xbox 360 minus the
game: Microsoft's emulator, the PC-side graphics stack, the 360 kernel and system modules
(already translated to DLLs by Microsoft), the flash filesystem and the system partitions.
Your recompiled game adds two files to it.

None of this can be shipped with the kit. It's Microsoft's software, and you assemble it
from back-compat packages you own. One console folder serves every game you recompile.

## What goes in it

It is made from two kinds of XEO3 back-compat package, plus one open-source DLL:

| piece | where it comes from | what it supplies |
|---|---|---|
| **the host** | an unmodified XEO3 back-compat game package **for PC** | `Emu.exe` (the emulator, built for Windows), `MicrosoftGame.config` (the identity), the PC-side runtime files |
| **the 360 system stack** | an **Xbox One** XEO3 back-compat package of a **native Xbox 360** title (the modern generation, see below) | `Flash\` (the kernel image, xam, ...), `SystemAuxPartition\`, `SystemExtPartition\`, `Strings\`, the precompiled `xeo3_*.dll` set for every system module, the graphics stack (`VGPUDX12.dll`, ...), `LaunchArguments.txt` |
| `kernelx.dll` | build it from [XWine1/SlimEra](https://github.com/XWine1/SlimEra) (`make.cmd`) | a PC implementation of the Xbox One kernel library the package's DLLs import |

Why two packages: the Xbox One package has the complete, matching, translated 360 system
stack, but its own `Emu.exe` is built for the Xbox One OS. The PC host supplies the
Windows `Emu.exe`, and SlimEra's `kernelx.dll` lets the Xbox One-built DLLs load on Windows.

### Getting the right 360 package generation

XEO3 packages exist in two generations, and only the modern one works with this kit. Check the
exports of any `xeo3_*.dll` in the package (`dumpbin /exports`, from a VS developer prompt):

- **modern**: `InitPrecompiledDll`, `CleanupPrecompiledDll`, `PrecompiledPointers`,
  `PrecompiledImportTable`. This is the one you want. The folder has the shape
  `Game\ Flash\ Strings\ SystemAuxPartition\`.
- **legacy**: `GetPrecompiledHashData`, `PrecompiledFunctionPointers`. The current
  `Emu.exe` refuses these, and no amount of fixing gets around that.

## Assembling it

1. Copy the **host** package to a new folder, e.g. `D:\XeO3 Console`. Start from an
   untouched copy and keep a backup: the next step overwrites files.
2. Overlay **every** file from the **360 package** on top of it, **except `Emu.exe`**:
   ```powershell
   robocopy "D:\packages\<360 title>" "D:\XeO3 Console" /E /XF Emu.exe
   ```
   `Emu.exe` must stay the host's. Check its hash is unchanged afterwards
   (`Get-FileHash "D:\XeO3 Console\Emu.exe"`).
3. Copy `kernelx.dll` (built from SlimEra) into the folder.
4. **Patch the emulator's GPU layer** (recommended). Three graphics bugs live in shader
   code inside `VGPUDX12.dll`, where the runtime can't reach them: scenes turning blue after
   a resolve, long black triangle shards, and upside-down environment reflections.
   ```powershell
   python -I tools\patch_vgpu.py apply "D:\XeO3 Console"
   ```
   It only patches the exact original it was made from (MD5 `410a4dea62c9f073e081cbfb1b645386`),
   checks the result against the build that was tested in game, keeps the original as
   `VGPUDX12.dll.orig`, and `restore` puts it back. Clear the shader cache afterwards (it
   prints where). On another `VGPUDX12.dll` build it refuses and changes nothing.
5. **Test it before adding anything.** Run `Emu.exe` from the folder. It boots the 360
   package's own game, because `LaunchArguments.txt` names it. If that game runs, the
   console folder is good. If it doesn't, nothing you recompile will run either.

That's the whole recipe. It was found the hard way: weeks were spent recompiling the 360
kernel by hand before it turned out a modern 360 package already ships the complete,
matching, translated system stack.

## Things worth knowing

- **`LaunchArguments.txt`** carries the identity of the title the folder boots by default
  (`mediaId=...&titleId=...`). `scripts\run.ps1` boots *your* game instead, by passing
  `-dvd <disc folder> -titleId <your game> -mediaId <guid>` on the command line, and takes
  the `mediaId` from this file. The media ID is the package's content identity, not something
  in the game's XEX. Reusing the folder's own one is what has worked for every game so far.
- **The emulator exits silently and at once on a bad argument.** `run.ps1` checks the
  process is still alive 8 seconds later.
- **Emu writes its log to standard output.** No redirect means no log; `run.ps1 -Log`
  captures it. The log is verbose enough to slow the game down, so use it for boot problems
  only.
- **Saves and profiles** (the virtual hard drive) live outside the folder, in
  `%LOCALAPPDATA%\Xbox360BackwardCompatibil.<identity>\VirtualDrive\`, where `<identity>`
  comes from the host's `MicrosoftGame.config`. Don't rename that identity: it also names the
  shader cache.
- The runtime's local-save layer writes your recompiled game's saves to
  `<disc folder>\SAVES\` instead. See [RUNTIME.md](RUNTIME.md#saves-and-the-local-player).
- **`zz_mftb.on`**: `scripts\install.ps1` creates this empty file in the console folder. It
  switches the recompiled games' timebase to the 360's real 50 MHz; without it every timer
  in the game runs 70-100x fast.
- **Other emulator options**, for reference (`Emu.exe -<name> <value>`): `dvd`,
  `systemfiles`, `storage`, `mu0`, `mu1`, `log`, `precompiled`, `shadercache`, `root`,
  `titleId`, `mediaId`, `configurationId`, `nobootanim`, `noshadercache`. Bare words in
  `LaunchArguments.txt` are flags too (`xma` turns on the emulator's own XMA audio worker,
  `fusion` the original-Xbox emulator).
- **Never hard-link files between two console folders.** A package the guest decides is
  corrupt gets truncated and deleted *through* the link, in every folder at once.

## Which emulator build the fixes were written for

The graphics and audio fixes in the runtime patch the emulator's own code in memory. Each
one first checks the bytes it is about to patch and **refuses to act** on any other build,
so a different build is safe, just unfixed. They were written against:

| file | MD5 |
|---|---|
| `Emu.exe` | `e6058f1140b7e02e653afb6bf1bc05cf` |
| `VGPUDX12.dll` | `410a4dea62c9f073e081cbfb1b645386` (Microsoft's), or `3f0025705ff84d3678c28abbace5a637` after `tools\patch_vgpu.py`. The patch touches only headers, shader text, one data pointer and an added section, never `.text`, so the code bytes the runtime checks are identical in both. The games so far were played on the patched one. |

On another build, `<game>_vgpu.log` / `<game>_tile.log` say `signature mismatch ... not
patched`, and the fix needs porting to the new offsets ([RUNTIME.md](RUNTIME.md)).

Next: [TUTORIAL.md](TUTORIAL.md).
