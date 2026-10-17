# The dashboard

The kit can also recompile the Xbox 360 **NXE dashboard** (`dash.xex`, the 360's home screen).
Put in a console folder, it boots like a 360 does: the dashboard comes up, signs in your
profile, lists your recompiled games under **My Games** and starts the one you pick. Quitting
the game brings the dashboard back. The **Play Disc** tile and the **Xbox (Guide) button**
work too.

It runs **offline**: there is no Xbox Live here, and nothing in this section talks to a
network.

As with games, the kit holds only the recipe: the config, our own runtime and the scripts.
You supply Microsoft's files and your own games.

## What has run

On the project's own console folder (2026-10-16): the dashboard boots and navigates at
60 fps. Need for Speed: Most Wanted shows under My Games and launches with A, signed in to
the offline profile, with saves written to that profile by the real xam. Quitting returns to
the dashboard. The Guide button opens the Guide. Play Disc boots the game "in the drive".

That was the project's private build of the same recompile. The kit's version is the same
recompile: `recompile-dashboard.ps1` reproduces it to the file (230 sources, 58,669
functions), with only the kit recompiler's later fixes differing. It has the same hooks, minus
the private Xbox Live experiments and the debugging probes. **The kit's build has not been
booted yet.** The [kernel loader](#the-kernel-loader) step isn't in the kit yet either. Expect
to be the one who tries it first.

## What you need

**Bring your own** (none of it is in this repository):

| | |
|---|---|
| a **console folder** | [CONSOLE_FOLDER.md](CONSOLE_FOLDER.md). Make a separate **copy** of it for the dashboard (a real copy, never hard links): the dashboard replaces the title the folder boots. |
| **`dash.xex` 2.0.17003.0** | from the `Flash` folder of an older (2014, legacy-generation) XEO3 back-compat package. Ours came from the Xbox One back-compat package of *Castle Crashers*. It must be exactly this build: 5,861,376 bytes, MD5 `be915f59fc0469c16b3cf26a19d08f31`, emulator DLL name `xeo3_c78b4e60_cf714a41_cb4682a0_d5cedc3e_04f30dfe`. The dashboard in a regular 360 system update is a different build (see [another build](#another-dashboard-build)). |
| the rest of that **`Flash` folder** | ten boot files the modern console package lacks (`dashboard.xbx`, `xlaunch.fdf`, `xlaunch.strings`, ...). `install-dashboard.ps1` copies them from there. |
| an **offline profile** | see [Profiles](#profiles). |
| your **games** | recompiled with the kit and installed in the dashboard's console folder, plus a Games-on-Demand package of each disc, made with [iso2god-rs](https://github.com/iliazeus/iso2god-rs) from your own ISO. |

Same free tools as for a game ([SETUP.md](SETUP.md)).

## Steps

```powershell
# 1. recompile your dash.xex (a minute; checks the build and the result)
powershell -ExecutionPolicy Bypass -File scripts\recompile-dashboard.ps1 -DashXex "D:\packages\<title>\Flash\dash.xex"

# 2. compile it (like a game; ~240 files)
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Game dash

# 3. make the console folder boot it
powershell -ExecutionPolicy Bypass -File scripts\install-dashboard.ps1 -Console "D:\XeO3 Dash" -Flash "D:\packages\<title>\Flash"

# 4. the kernel loader -- see below; not automated yet

# 5. boot it: run Emu.exe in the folder, no arguments. Once, so it creates its virtual hard drive.

# 6. each game: install its DLL here, then put it in My Games
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Game crackdown -Console "D:\XeO3 Dash"
powershell -ExecutionPolicy Bypass -File scripts\add-game-to-my-games.ps1 -Game crackdown -Console "D:\XeO3 Dash" -God "D:\god\Crackdown" -Disc "D:\games\Crackdown" -InDrive
```

`recompile-dashboard.ps1` refuses any `dash.xex` but the one build, and checks the recompile
against the known-good one: 230 source files, 235 generated files in total, 58,669
functions, no errors. Every address in `dashboard\config.toml` belongs to that build.

To see the emulator's own log, start it from a prompt with its output redirected:
`cmd /c "cd /d D:\XeO3 Dash && Emu.exe > dash_emu.log"`. The dashboard runtime writes its own
`dash_runtime.log` in the console folder.

## How it boots

The dashboard is title `FFFE07D1`. `install-dashboard.ps1` puts `dash.xex` in the folder's
`Game\default.xex` and rewrites `LaunchArguments.txt` to `mediaId=FFFE07D1-...&titleId=FFFE07D1`.
The rest of the line is kept, and the package's original is kept as
`LaunchArguments.txt.before_dash`. The two must agree: when the arguments named another title
than the boot XEX, the guest read the header and quit without a single error line.

`dash.xex` itself stays unmodified. Its header is integrity-checked inside the guest: changing
one benign byte in a header that boots made it stop booting.

The recompile needed three things a game didn't:

- **setjmp/longjmp** (`setjmp_address`/`longjmp_address` in the config). The dashboard's UI is
  Lua, and Lua errors are caught through `longjmp`. Without them, the first one (an empty UI
  string, which is blank by design) killed the dashboard.
- **the GPU registers** it writes from the CPU, listed as `mmio_instructions`.
- **jump tables** that XenonAnalyse used to miss (since fixed in the recompiler; `switch.toml`
  is the corrected one).

The runtime (`dashboard\runtime\`) adds:

| what | why |
|---|---|
| **present fix** (`zz_dash_vgpu.cpp`, `dash_swapfix.on`) | The dashboard flips the screen with a CPU register write. The emulator's GPU layer only presents flips that arrive on its own thread, so the dashboard drew its first screen and the window never changed again. The fix makes that thread present the CPU flips too. It checks the exact bytes it patches in `VGPUDX12.dll` (MD5 `410a4dea...` or `3f002570...` after `tools\patch_vgpu.py`) and does nothing on another build. |
| **no camera** (`zz_dash_imports.cpp`) | The dashboard probes the Xbox Live Vision camera. The emulator doesn't implement it, and the guest then crashed. Every camera call now answers "not connected", like a console with nothing plugged in. |
| **missing modules** (`zz_dash_imports.cpp`) | Some dashboard pages are separate modules (`Dash.MP.*.lex`). For one with no DLL in the console folder, the emulator stops the whole process. The runtime declines that load instead, so the dashboard gets an ordinary error. |
| **title launch** (`zz_dash_launch.cpp`, `zz_switch.exe`) | See below. |
| **Guide button** (`zz_dash_input.cpp`, `dash_guide.on`) | See below. |

## Launching a game

On a 360, starting a game is a soft reboot: xam tears the dashboard down and maps the game in
the same machine. The emulator gets through that, then crashes in its GPU layer, which is
never reset for a second title in one process (back-compat games never do this).

So the host does the switch. When the dashboard asks xam to launch something, the runtime
remembers the request and lets xam do its orderly teardown, which flushes the profile. When
the emulator then unloads the dashboard's DLL, the runtime starts `zz_switch.exe` and ends
that emulator. The relay waits until the old process has fully exited; starting the next one
any earlier failed with "Failed to create backing file". It then starts the same `Emu.exe`
in the same folder with `-dvd <disc folder> -titleId <id> -mediaId <guid>`.

So every game runs from the dashboard's console folder, on the same identity and profile.
That's why each game's DLL has to be installed there. When the game exits, the relay starts
`Emu.exe` with no arguments, which is the dashboard again.

Which game it starts:

- **My Games**: the package the dashboard launched carries the title ID, and
  `zz_titles.txt` gives its folder and media ID, one line per game:
  `<titleId>=<disc folder>|<mediaId>|<extra emulator arguments>`.
- **Play Disc**: `zz_disc.txt`, the game "in the drive":
  `dvd=<disc folder>`, `titleId=<id>`, `mediaId=<guid>`, optional `args=...`.

`add-game-to-my-games.ps1` writes both (`zz_disc.txt` only with `-InDrive`).

## My Games

My Games is the dashboard's game library: Lua over a native enumerator that shows whatever
xam lists under the title's content type `00007000` (Games on Demand). The pieces:

1. **A Games-on-Demand package** of the disc, in the console's virtual hard drive:
   `%LOCALAPPDATA%\Xbox360BackwardCompatibil.<identity>\VirtualDrive\Harddisk\Partition1\Content\0000000000000000\<titleId>\00007000\`.
   The `<identity>` is the one in the console folder's `MicrosoftGame.config`.
   `add-game-to-my-games.ps1` copies it there. When the `.data` folder is on the same drive,
   its files are hard links (`-Copy` copies instead). The game itself boots from your disc
   folder, not from the package, but the tested setup had the data folder in place.
2. **A CON signature.** iso2god writes a LIVE header with a blank signature. The retail xam
   checks LIVE headers against Microsoft's keys, rejects it, and the dashboard says "no games
   found". The emulator carries a console key and certificate, and signs every package it
   creates (profiles, saves) as CON with them. `tools\consign.py` signs yours the same way,
   using the key it finds in **your** `Emu.exe` at run time; nothing of the key is in the kit.
   It also sets the header's platform, console ID and version fields the way that console
   would have written them.
3. **A fresh content cache.** The dashboard keeps its own verdict on every package in
   `...\Content\0000000000000000\FFFE07DF\00040000\ContentCache.pkg`, and a package it once
   judged invalid stays that way. The script moves the cache aside and the dashboard rebuilds it.
4. **The launch.** Pressing A makes the dashboard ask xam to launch the package's
   `default.xex`. Real xam can't (it would have to mount the package in the guest), so the
   runtime finds the package on the host, looks its title up in `zz_titles.txt`, and turns
   the call into an ordinary "Play Game" launch that the switcher handles.

## The Guide button

The emulator reads the controller on the host side, so the guest never sees the Xbox
button. The runtime reads it from the host (XInput; Back+Start held together also works) and
calls xam's `XamShowGuideUI` from the dashboard's UI thread. That function isn't imported by
the dashboard, so it is called by its address in one xam build: the one in the console
package the kit was made with (xam DLL `xeo3_83fb8ce4_...`). `install-dashboard.ps1` arms it
(`dash_guide.on`) only when your console folder has that xam. If the Guide never opens,
Steam's Guide-button chord or the Xbox Game Bar may be taking the button first.

## Profiles

You need a profile in the console's virtual hard drive. When exactly one profile is there,
the dashboard signs it in at boot by itself, and a game launched from the dashboard is signed
in to it too. It lives at
`...\Partition1\Content\<XUID>\FFFE07D1\00010000\<XUID>`. Each game's record in it is a
`<titleId>.gpd` file inside that package.

**It must be an offline (local) profile.** A Live-enabled profile signs in on the dashboard,
but no game can sign it in. In Need for Speed every in-game sign-in failed; Crackdown's logon
answered "file not found". With the offline version of the same profile, Need for Speed signs
in. This console has no Xbox Live, so there is nothing for a Live profile to sign in to.

The kit has no profile tool, and no way to create a profile from inside this console has been
found yet. The one used on the project's console was put into the virtual drive by hand.
Getting a local profile into your console is up to you.

## Offline, and keeping it that way

Nothing in the kit's dashboard reports a network connection, so the dashboard takes its
offline path: My Games and Play Disc launch straight away. That matters. In the project's
private build, the Xbox Live experiments made the dashboard believe it was online. Then
My Games checked a title-update server before every launch, couldn't reach it, and aborted
the launch with a brief error. If you ever give the console a network, expect exactly that.

## Files in the console folder

| file | what |
|---|---|
| `zz_titles.txt` | My Games: title ID -> disc folder, media ID, extra arguments |
| `zz_disc.txt` | the game in the drive (Play Disc) |
| `zz_switch.exe` | the title switch relay (built by `install-dashboard.ps1`) |
| `zz_launchdata.bin` | short-lived: carries the dashboard's launch data across its own relaunch |
| `dash_runtime.log` | what the dashboard runtime did: launches, the present fix, the Guide button |
| `dash_swapfix.on` | the present fix (without it the picture freezes) |
| `dash_self_relaunch.on` | some dashboard scenes (the avatar ones) reboot into the dashboard itself; with this, that's what happens instead of starting the game in the drive |
| `dash_guide.on` | the Guide button |
| `dash_no_return.on` | optional: don't reopen the dashboard when a game exits |
| `zz_mftb.on` | the 360's 50 MHz timebase, as for every game ([CONSOLE_FOLDER.md](CONSOLE_FOLDER.md)) |

## When something goes wrong

- **My Games says "no games found"**: the package isn't CON-signed
  (`python -I tools\consign.py verify <package> --emu <console>\Emu.exe`), or the content
  cache still holds the old verdict (move `ContentCache.pkg` aside).
- **A brief error flashes and nothing launches**: the dashboard thinks it is online (see
  above), or the title isn't in `zz_titles.txt` (`dash_runtime.log` says which).
- **The game asks to sign in and it fails**: the profile is Live-enabled, or it lost the
  game's `<titleId>.gpd`.
- **Black screen, or a crash half a second into the game**: a corrupt shader cache for that
  title. A switch that ended a process mid-write left one once. Move
  `%LOCALAPPDATA%\Xbox360BackwardCompatibil.<identity>\ShaderCache\<titleId>` aside.
- **The picture freezes on the first screen**: `dash_swapfix.on` is missing, or
  `dash_runtime.log` says the present fix did not match your `VGPUDX12.dll`.
- **A game's own logs**: `<game>_*.log` in the console folder, as always.
- Don't open the dashboard's logs in a program that locks them while the dashboard runs.
- Never hard-link files between two console folders ([CONSOLE_FOLDER.md](CONSOLE_FOLDER.md)).

## The kernel loader

The 360 kernel the console package ships won't load `dash.xex` as a title from the game
folder. Three of its checks are there because on a real console the dashboard lives in flash:

- the XEX's allowed media is "system flash" only, and the game folder appears as a DVD
- the XEX names the one path it may be loaded from (`\Device\Flash\dash.xex`)
- a title that carries a device ID is expected to sit on a hard drive or memory unit

None of them can be satisfied by editing the XEX: its header is integrity-checked. The
project's console gets past them with a byte patch to the kernel's precompiled DLL in its
console folder. That patch isn't part of this kit yet. Until it is, the dashboard will stop
right after the kernel reads its header (the emulator's log shows the header reads and then
the kernel exiting), and this section is where the step will be documented.

## Another dashboard build

A different `dash.xex` (a later system update, say) needs its own config, made the way a
game's is ([TUTORIAL.md](TUTORIAL.md)):

- the jump tables (XenonAnalyse) and the boundary loop (`scripts\recompile.ps1` does both
  for a game)
- the MMIO list (`tools\mmio_scan.py`)
- the eight save/restore helpers
- the C runtime's `setjmp`/`longjmp`, found by reading the code

The runtime itself finds its imports by name (`tools\gen_dash_idx.py`), so it carries over.
The Guide button's xam address belongs to the console package, not the dashboard.
