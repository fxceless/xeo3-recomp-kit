# Troubleshooting

Roughly in the order you'll meet them. Every entry here happened on a real game.

**Before anything else:** close the game before reading its logs, and check the newest log
is from *this* run (`install.ps1` moves old ones aside as `.prev_<time>`). Reading a two-day-old
log as if it were the new run is an easy mistake to make.

---

## The emulator closes immediately

- **Run with `-Log`** (`scripts\run.ps1 ... -Log`) and read `games\<game>\emu.log`. The
  emulator writes nothing without it.
- `Failed to load precompiled dll xeo3_...` / `Precompiled binary ... not found`: the DLL
  name is wrong or one of the two copies is missing. `install.ps1` copies both. Compare the
  name with `python -I tools\xeo3_name.py <disc>\default.xex`.
- No error at all: a bad command-line argument. The emulator exits silently on any it
  doesn't understand. Remove extras from `LaunchArguments.txt` one at a time, and never test
  two argument changes in one launch.
- Check the console folder itself still boots its own title ([CONSOLE_FOLDER.md](CONSOLE_FOLDER.md)).

## `FATAL ERROR - Unable to execute precompiled code for IAR 0x8XXXXXXX`

The guest jumped to an address the DLL has no entry for. XEO3 can't interpret code, so an
address that wasn't recompiled as a function start is fatal.

- **Usually a missed jump table.** An entry of an unrecognised table points into the middle
  of a function. Run `tools\scan_jumptables.py` ([TUTORIAL.md](TUTORIAL.md#5a-jump-tables-the-analyser-missed)).
  The address in the message is the table entry that was taken.
- **Or a function the analyser never found** (only reached through a pointer). Add it to
  `config.toml`'s `functions` with its real size and recompile.
- A **random-looking address that changes every run** is not a missing function: something
  corrupted a return address or function pointer first. Treat it as a crash and find the
  first fault.

## It dies with no `<game>_crash.log` entry

The emulator ends itself with a fast-fail (`0xC0000409`) after printing a fatal error.
Fast-fails skip every exception handler, including the runtime's. Have Windows write a
minidump instead. Run this once, as administrator:

```powershell
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Emu.exe" /v DumpType /t REG_DWORD /d 1 /f
```

Each crash then lands in `%LOCALAPPDATA%\CrashDumps\`, and

```powershell
powershell -File tools\read_crash_dump.ps1 -Game <game>
```

prints the fatal message and the stack, with frames in the game DLL named as guest
functions. **Read the dump before forming a theory.**

## Access violation (`c0000005`)

`<game>_crash.log` names the guest function (`sub_XXXXXXXX+0xNN`), the registers and the
guest stack. Then:

- `python -I tools\fn_body.py games\<game>\ppc <outdir> sub_XXXXXXXX` writes the function
  with its original PowerPC in the comments.
- `python -I tools\resolve_fault.py games\<game> <offset>` turns a module offset the
  emulator printed into a guest function.
- **Check which module an address belongs to before reading anything into it.** The game
  DLL, `Emu.exe`, `VGPUDX12.dll` and the system DLLs all have their own offsets, and an offset
  with no module named is meaningless. Reading one module's offset as another's once cost
  three sessions.
- Crashes **inside `VGPUDX12.dll`** are usually the emulator's GPU layer, not the game. See
  Graphics below.
- The recompiler's `lis` comments in the generated code can show the wrong address.
  Compute addresses from the C++ value the code actually uses.

## Stuck at "signing in" / "signing into gaming profile"

With `-dvd` there is no dashboard to sign anyone in. The local-save layer answers sign-in,
profile reads and the save device itself ([RUNTIME.md](RUNTIME.md#saves-and-the-local-player)).
If a game still waits, `<game>_saves.log` shows the last call it answered. Create
`<game>_observe.on` to log the xam calls it does **not** answer, with xam's replies. Crackdown's
hang was a profile-settings read whose completion xam never signalled.

## Everything runs too fast, or the game's clock is wrong

`zz_mftb.on` is missing from the console folder. Need for Speed's world time advanced
66.7 ms per rendered frame without it.

## Slow motion

- `run.ps1 -Log`: the emulator's own logging slows everything down. Use it only for boot
  problems.
- An instrument switch is on (`*_probe`, `observe`, `zz_import_log`, `zz_pcsample`).
- Then it's real performance. See Speed below.

## The game freezes

A guest thread waiting inside a kernel call has **no guest frames on its host stack**: the
call unwinds through the emulator's bridge. A debugger stack won't show you who is waiting
or on what. The guest caller is still in the link register at the moment of the call, so:

- `<game>_zz_import_log.on` records kernel calls (waits, events, `VdSwap`) with their guest
  caller and thread, and photographs the last calls of every import when one stalls.
- `<game>_zz_pcsample.on` samples where each guest thread is.

Fable II's freeze looked like a graphics hang (presentation, streaming and audio all stopped
in the same tick). It was one thread stuck inside a mutex acquire it had made 64 million
times before. Find the first thing that stopped, not the loudest.

## Graphics

| what you see | cause | fix |
|---|---|---|
| scene drawn only in a band, black below; HUD fine | predicated tiling: every bin drawn with the first bin's viewport | `<game>_zz_tile.on` |
| scene doubled side by side | the same, for horizontally tiled frames | `<game>_zz_tile.on` |
| scene blue / colours wrong after full-screen effects, UI correct | the emulator's resolve write-back packs R/B swapped | `tools\patch_vgpu.py apply`, then clear the shader cache |
| long black triangles across geometry | the emulator's index fetch reads one element early on PC GPUs | `tools\patch_vgpu.py apply`, then clear the shader cache |
| reflections / mirror upside down | the emulator samples cube side faces flipped | `tools\patch_vgpu.py apply`, then clear the shader cache |
| crash in `VGPUDX12`, D3D12 "device removed" | EDRAM alias textures used before initialisation | `<game>_zz_edram_init.on` (+ `zz_cast_fix`, `zz_map_clamp`) |
| stack-cookie fast-fail on the render thread at the first cutscene | a shader uses more than 16 samplers | `<game>_zz_sampler32.on` |
| normals, lighting or reflections subtly wrong everywhere | was a recompiler bug: `vpkd3d128` SHORT packing zeroed negative values | fixed in this recompiler; recompile old builds |
| lower part of the 3D view black (Crackdown) | a resolve into a second tile bin never reaches the texture | **open** |

**Shader cache:** after any change to `VGPUDX12.dll`, move
`%LOCALAPPDATA%\Xbox360BackwardCompatibil.<identity>\ShaderCache` aside. Shaders already
compiled from the old templates are reused otherwise, and the fix looks like it did nothing.
The copy of `XeO3_ShaderCache` inside the console folder is **not** the live one.

## Speed

Measure first: `<game>_fps.log`. On Need for Speed most of the emulator's render thread went
to re-hashing and byte-swap-copying the same vertex and index buffers hundreds of times
per frame. The `zz_vgpu*` switches remove that ([RUNTIME.md](RUNTIME.md)): `zz_vgpu`,
`zz_vgpu_memo2`, `zz_vgpu_skip`, `zz_vgpu_conv`. Profile before writing another one.

## No sound

`<game>_xma.on` plus the FFmpeg DLLs ([RUNTIME.md](RUNTIME.md#audio-xma)).
`<game>_xma_dec.log` says whether the decoder loaded and which contexts it decoded. Games
whose audio goes through the kernel's XMA functions rather than the registers may instead
need the bare `xma` flag in `LaunchArguments.txt` (the emulator's own decoder).

## Saves don't appear next boot

`<game>_saves.log`, then `<disc folder>\SAVES\`. Each save package has a folder plus a
`<package>.xcd` sidecar holding the game's own content header. Without the sidecar the
enumerator can't report the save back. Need for Speed's three save-loading bugs were all in
this layer, never in the game.

## Build problems

| message | meaning |
|---|---|
| `REFUSING TO BUILD - ... not one clean recompiler run` / duplicate symbols at link | a stale `ppc_recomp.N.cpp` from an earlier pass; re-run `recompile.ps1` |
| `XenonRecomp.exe is older than ...` | `scripts\setup.ps1` |
| `__builtin_trap` count above 0 | an instruction the recompiler can't translate yet ([TUTORIAL.md](TUTORIAL.md#4-recompile)) |
| switch-error count stops falling | read `errors.txt`: usually a real table the analyser sized wrong. Fix the `[[switch]]` block rather than widening functions further |
| the DLL loads, then crashes in every function | built with a stock clang, not WitherOrNot's ([SETUP.md](SETUP.md)) |
| `another build of <game> is running` | an orphaned build from a cancelled run. End it by process ID, never by name |

## Habits that save the most time

- **Predict, then launch.** Write what the run should show before running it.
- **One change per launch.** Two changes in one run can't be told apart afterwards.
- **A null result needs a positive control.** Before concluding "it never happens", make
  sure the check would have caught it if it did.
- **Your own log line is not proof it works.** Only the game's behaviour is: a hit, a
  frame, a sound, a save that comes back.
