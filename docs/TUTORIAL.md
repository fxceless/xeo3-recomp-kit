# Tutorial: a game, start to finish

This walks through one game from disc to first boot, with **Crackdown's real numbers** at
each step so you can tell whether yours look sane. Commands are run from the kit's folder
in PowerShell. If PowerShell refuses to run the scripts, allow local scripts once with
`Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` (or prefix every command with
`powershell -ExecutionPolicy Bypass -File`).

Before you start you need [SETUP.md](SETUP.md) done and a working console folder
([CONSOLE_FOLDER.md](CONSOLE_FOLDER.md)).

---

## 1. Unpack your disc

```powershell
xdvdfs unpack "D:\isos\Crackdown.iso" "D:\games\Crackdown"
```

The folder's root now holds `default.xex` (sometimes `Default.xex`) and the game's data.
This folder is what the emulator boots with `-dvd`. There's no repackaging, no GoD
conversion and no dashboard involved. A `$SystemUpdate` folder on the disc can be ignored,
because the console folder brings its own system.

> Crackdown: 197 files, 6.56 GB.

Look at the executable's header:

```powershell
python -I tools\xex_hdr.py "D:\games\Crackdown\default.xex"
```

Things to note:
- **titleId**: the game's ID (Crackdown `4D5307DC`).
- **imports**: the libraries the game calls. `xam.xex` + `xboxkrnl.exe` only is the easy
  case. Any other import library has to exist as a system module in your console folder.
- **image size / load address**: for scale (Crackdown: 23.5 MB at `0x82000000`).

## 2. Create the game

```powershell
.\scripts\new-game.ps1 -Name crackdown -Xex "D:\games\Crackdown\default.xex"
```

This makes `games\crackdown\` with:

| file | what |
|---|---|
| `xex\default.xex` | a copy of the executable |
| `switch.toml` | every jump table XenonAnalyse found (Crackdown: **795**) |
| `image.bin` | the decrypted, decompressed image, for the analysis tools |
| `config.toml` | the recompiler's config, with the 8 register save/restore helper addresses filled in |
| `game.json` | name, title ID, the DLL name, extra compiler defines |
| `analyse.log` | XenonAnalyse's output |

The **DLL name** is derived from a hash in the XEX header
(`xeo3_14cefedb_c6da7da6_65145c7f_9e12e13b_80eef7c7` for Crackdown). It's the name the
emulator probes for, so the same disc always gives the same name. See
[HOW_IT_WORKS.md](HOW_IT_WORKS.md#dll-names).

> If XenonAnalyse can't read the XEX at all (some executables use a compression form the
> analyser can't open), make a decoded copy **for the recompiler only**:
> `recompiler\build\XexRepack\XexRepack.exe default.xex decoded.xex`, and point
> `new-game.ps1 -Xex` at it. The header is left byte-identical, so the DLL name doesn't change.
> Never give the emulator a repacked XEX: it rejects it.

## 3. Check the save/restore addresses

Open `config.toml`. The eight `*_address` lines are the compiler's register spill helpers;
if any is `0x0`, the recompile fails. Sanity check: in every title so far the distances
were identical, which is a good sign the pattern matches are real:

```
restgprlr_14 - savegprlr_14 = 0x50      restfpr_14 - savefpr_14 = 0x4C
restvmx_14   - savevmx_14   = 0x298     savevmx_64 = savevmx_14 + 0x94, restvmx_64 = restvmx_14 + 0x94
```

> Crackdown: `savegprlr_14 0x82ABDEE0`, `restgprlr_14 0x82ABDF30` (+0x50), and so on: all
> four deltas match Fable II and Need for Speed.

## 4. Recompile

```powershell
.\scripts\recompile.ps1 -Game crackdown
```

It runs the loop described at the top of `scripts\recompile.ps1`:

1. **The boundary loop.** XenonAnalyse sometimes ends a function too early, and the
   recompiler then reports `Switch case at X is trying to jump outside function`. The script
   reads the enclosing function straight out of the generated C++, widens it in
   `config.toml`'s `functions` list, and recompiles, until there are no such errors.
   > Fable II went 2027 -> 0 errors in 15 passes. Crackdown needed few, because the jump-table
   > analysis has had register validation since.
2. **MMIO.** Finds every instruction that reads or writes the GPU (`0x7FC8xxxx`) and XMA
   audio (`0x7FEAxxxx`) hardware registers and lists them in `config.toml`, then recompiles
   once more. Missing one means a register read silently returns RAM.
   > Crackdown: 6 GPU sites (the same six register offsets as Fable II) and 36 XMA sites.
3. **Runtime.** Copies `runtime\` into `ppc\` and generates the import index header.
4. **Report.** Read it:

```
ppc set: 62204 functions wanted, 62204 defined, 0 duplicated, 0 not in the final run, 0 missing
  functions          62440
  __builtin_trap     0
  PPC_SETJMP         12
  PPC_LONGJMP        30
  zz_mftb sites      576
```

- **`ppc set`** must show 0 / 0 / 0. Anything else means stale files, so re-run.
- **`__builtin_trap`** should be **0**. Each trap is a PowerPC instruction the recompiler
  can't translate yet, and the generated code prints which one. Implement it in
  `recompiler\XenonRecomp\recompiler.cpp` (the fork's history has plenty of examples: `vpkd3d128`,
  `vupkd3d128`, `lhaux`, `vsubuwm` ...), re-run `scripts\setup.ps1`, then recompile again.
- **`PPC_SETJMP` / `PPC_LONGJMP`** are 0 until you do step 5c.

## 5. The manual checks

The script gets you a clean recompile. These three checks are what separate "it compiles"
from "it runs", and each one was learned from a crash.

### 5a. Jump tables the analyser missed

A `bctr` through a table that has no bounds check next to it is invisible to XenonAnalyse.
The recompiler then emits an indirect call to whatever the table holds, the emulator finds
no function at that mid-function address, and the game dies with
`FATAL ERROR - Unable to execute precompiled code for IAR <address>`.

```powershell
python -I tools\scan_jumptables.py games\crackdown --toml
```

It rebuilds each missed table from the generated source and prints ready-to-paste
`[[switch]]` blocks. Paste **only the `[[switch]]` blocks** into `switch.toml` (the boundary
loop handles function sizes) and recompile. The goal is no unrecognised `bctr` sites. A
`bctr` that is really a tail call through a data table is the one allowed leftover: read the
site to prove it.

### 5b. Audit the tables that were found

```powershell
python -I tools\audit_switch_tables.py games\crackdown
```

This re-derives every table in `switch.toml` from the image and flags any whose index
register, default arm or label count disagrees with the code. One missing label once let
clang delete a bounds check and crash a game at a random-looking address. The goal is
**0 unexplained findings**. `--toml` prints corrected blocks. The generated code also has a
safety net now: a switch that misses its table calls `zz_switch_miss` and lands in
`<game>_switchmiss.log` instead of crashing (an empty file is the healthy result).

### 5c. setjmp / longjmp

If the game uses the C runtime's `setjmp`/`longjmp` (Lua-scripted games always do, and C++
games often do), the recompiler must be told where they are. Otherwise a guest `longjmp`
just returns to its caller and the game goes wrong far from the cause.

Find them in the generated source by **behaviour**, not by another game's bytes (the CRT
differs between SDK versions):

- **setjmp**: saves `f14..f31`, `r13..r31` and vector registers into one buffer, stores LR
  and CR into it, stores `r1`, then `li r3,0` / `blr`.
- **longjmp**: restores all of those from the buffer, `mtlr`, `ld r1,<offset>(...)`,
  `mtcr`, returns its second argument (0 becomes 1).

`tools\fn_body.py` dumps a function with its original assembly in the comments, and
`tools\import_callers.py` finds who calls what. Then in `config.toml`:

```toml
setjmp_address  = 0x82AC2330
longjmp_address = 0x82AC1CA0
```

Recompile. `PPC_SETJMP`/`PPC_LONGJMP` must both be above 0. At runtime `<game>_setjmp.log`
shows `armed` lines, and `LANDED` lines prove an unwind worked.

> Crackdown (an old SDK, 2.0.4802, early 2007): 11 setjmp call sites, 33 longjmp sites.

## 6. Build

```powershell
.\scripts\build.ps1 -Game crackdown
```

Compiles every file in parallel with the custom clang and links the DLL.

> Crackdown: 258 source files, about 15-20 minutes on 14 jobs, a ~50 MB DLL.

The script refuses to build when the result couldn't be trusted (another build running, a
stale recompiler, stale or missing objects), and checks the DLL exports the four functions
the emulator needs. Output:
`games\crackdown\dll\xeo3_14cefedb_..._80eef7c7.dll` and the identical `..._no.dll`.

**Per-game compiler defines** go in `game.json`'s `defines` list, e.g. Crackdown's frame
meter reads the present interval from the D3D device:

```json
"defines": [ "ZZ_FPS_DEV_INTERVAL=13220u" ]
```

## 7. Install and boot

```powershell
.\scripts\install.ps1 -Game crackdown -Console "D:\XeO3 Console"
.\scripts\run.ps1     -Game crackdown -Console "D:\XeO3 Console" -Disc "D:\games\Crackdown"
```

`install.ps1` copies both DLL names, re-hashes them, backs up the previous install, moves
old logs aside and makes sure `zz_mftb.on` exists. `run.ps1` boots the game from the disc
folder, with no dashboard.

### What to read after the first boot

Close the game first. The runtime keeps some logs open while it runs, and reading
them mid-run can block the game or show you half a line. All logs are in the console folder:

| log | what it tells you |
|---|---|
| `<game>_crash.log` | every fault, with the guest function named (`sub_XXXXXXXX+0xNN`) |
| `<game>_saves.log` | every sign-in, save-device and file call the local-save layer answered |
| `<game>_fps.log` | frames per second, once a second |
| `<game>_switchmiss.log` | jump tables that were incomplete (should be empty) |
| `<game>_setjmp.log` | setjmp/longjmp activity |

If the emulator died with `FATAL ERROR` and the crash log has nothing, that's a fast-fail;
read the minidump with `tools\read_crash_dump.ps1 -Game crackdown`
([TROUBLESHOOTING.md](TROUBLESHOOTING.md)).

## 8. Turn on the fixes

Everything optional in the runtime is off until you create an empty **marker file** in the
console folder named `<game>_<switch>.on`. Crackdown runs with:

```powershell
$c = "D:\XeO3 Console"
"xma","zz_tile","zz_mrtfix","zz_vgpu","zz_vgpu_conv","zz_vgpu_memo2","zz_vgpu_skip",
"zz_edram_init","zz_cast_fix","zz_map_clamp","zz_sampler32" |
  ForEach-Object { New-Item -ItemType File -Force "$c\crackdown_$_.on" | Out-Null }
```

What each one does, and the audio DLLs `xma` needs, is in [RUNTIME.md](RUNTIME.md). Turn
them on **one at a time** on a new game: each was written for a specific bug, and an A/B is
just deleting a file.

## 9. When it doesn't work

It won't, the first time. [TROUBLESHOOTING.md](TROUBLESHOOTING.md) lists every failure
these games have shown, in the order you're likely to meet them, with how each was found.
Two habits matter more than any single fix:

- **Write down what you expect a run to show before you run it**, then compare. A run with
  no prediction teaches you very little.
- **A probe that fails silently is worthless.** Every instrument in the runtime says
  when it is off and why; keep it that way when you add your own.
