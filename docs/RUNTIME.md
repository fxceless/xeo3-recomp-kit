# The runtime

`runtime\` is hand-written C++ that `scripts\recompile.ps1` copies into every game's `ppc\`
folder and `scripts\build.ps1` compiles into the game DLL. The recompiled code calls into
it through a few hooks: every call the game makes into the 360 kernel or xam goes through an
*import thunk*, every hardware register access through an MMIO hook, every jump-table miss
through a reporter. The runtime decides what to answer, fix or log there.

It grew out of the Need for Speed, Fable II and Crackdown recompiles. The comments in each
file keep the history of why each piece exists, with the crash or measurement that called
for it. Read them before changing anything.

## Switches (marker files)

Everything optional is **off** unless an empty file named `<game>_<switch>.on` exists in the
console folder (`<game>` = the name you gave `new-game.ps1`; the prefix keeps several games
in one console folder from reading each other's switches). An A/B test is deleting a file.
Each file announces in its log whether its switch was found, so "off" is never confused with
"not built in".

| switch | file | what it does |
|---|---|---|
| *(always on)* | `zz_save.cpp` | local player + local saves (below). Turn **off** with `<game>_localsaves.off` |
| `autologon` | `zz_import.cpp` | before the game's first sign-in check, signs the profile in through real xam, like the dashboard does |
| `xma` | `zz_xma.cpp` | **audio**: decodes the game's XMA sound with FFmpeg (below) |
| `zz_tile` | `zz_tile.cpp` | **graphics**: predicated tiling. Frames too big for the 360's EDRAM are drawn in bins; without this every bin draws with the first bin's viewport (the scene appears in a band, black below) |
| `zz_mrtfix` | `zz_tile.cpp` | graphics: makes the emulator honour per-slot render targets (multiple render targets) |
| `zz_edram_init` | `zz_crash.cpp` | graphics: initialises the emulator's placed EDRAM-alias textures before first use. Without it D3D12 removes the device and the game crashes in `VGPUDX12` |
| `zz_cast_fix` | `zz_crash.cpp` | graphics: adds the castable formats those resources need (rides on `zz_edram_init`) |
| `zz_map_clamp` | `zz_crash.cpp` | graphics: clamps over-declared read ranges when mapping placed buffers (rides on `zz_edram_init`) |
| `zz_sampler32` | `zz_crash.cpp` | graphics: the 360 has 32 samplers per draw, the emulator's table holds 16; a shader using more overflowed the stack (Crackdown's first cutscene) |
| `zz_vgpu` | `zz_vgpu.cpp` | **speed**: hook for the emulator's per-draw buffer hashing (the switches below need it) |
| `zz_vgpu_memo2` | `zz_vgpu.cpp` | speed: per-frame cache for that hash, calling the original on a miss, so results are bit-identical |
| `zz_vgpu_skip` | `zz_vgpu.cpp` | speed: skips the per-draw byte-swap copy of vertex/index buffers when the guest bytes haven't changed (it was the whole render thread) |
| `zz_vgpu_conv` | `zz_vgpu.cpp` | speed: replaces an identity texture-format conversion with a row copy |
| `observe` | `zz_save.cpp` | instrument: logs arguments and xam's answers for profile, message and notification calls |
| `zz_import_log` | `zz_crash.cpp` + `zz_import.cpp` | instrument: watches selected kernel calls with the guest caller (for freezes) |
| `zz_pcsample` | `zz_crash.cpp` | instrument: samples where guest threads are (for freezes) |
| `zz_dred` | `zz_crash.cpp` | instrument: D3D12 device-removed diagnostics |
| `zz_tile_observe`, `zz_tile_probe`, `zz_vgpu_copy`, `xma_dump` | | instruments for the tiling, copy and audio work |

`zz_mftb.on` (no prefix, shared by all games) switches the 360 timebase to the real 50 MHz.
`scripts\install.ps1` creates it. Without it, the recompiler's default reads the PC's raw
CPU counter and every timer in the game runs 70-100x fast (Need for Speed's world clock ran
66.7 ms per frame regardless of framerate).

**Crackdown's set:** `xma zz_tile zz_mrtfix zz_vgpu zz_vgpu_conv zz_vgpu_memo2 zz_vgpu_skip
zz_edram_init zz_cast_fix zz_map_clamp zz_sampler32`.

Three more graphics bugs live in shader code inside `VGPUDX12.dll` and are fixed on disk by
`tools\patch_vgpu.py` instead ([CONSOLE_FOLDER.md](CONSOLE_FOLDER.md), step 4).

The graphics and speed switches patch the emulator in memory and check the exact bytes first.
On a different `Emu.exe`/`VGPUDX12.dll` build they log a signature mismatch and do nothing
([CONSOLE_FOLDER.md](CONSOLE_FOLDER.md#which-emulator-build-the-fixes-were-written-for)).

## Saves and the local player

Booted with `-dvd` there is no dashboard, so nobody signs a profile in, and the game stops
at "signing in". `zz_save.cpp` answers the identity and storage calls itself:

- **identity**: signed in locally as one player. Set who in `xeo3_profile.txt` in the
  console folder:
  ```
  xuid=E000000000000001
  gamertag=Player1
  ```
  (those are the defaults). Use the XUID of a profile that exists in your console folder if
  you want the game and the dashboard to agree.
- **storage**: one save device, enumerating and creating save content, and the game's
  file calls on it go to `<disc folder>\SAVES\<package>\` on the PC. The emulator's virtual
  drive has no write path for this.
- **profile settings** reads return the settings' defaults at once (forwarding them to an
  xam with no user was the "signing into gaming profile" hang).

Every answered call is a line in `<game>_saves.log`.

The answers are found by import **name**. `tools\gen_save_idx.py` turns the names into this
build's thunk indices (`zz_save_idx.h`) after every recompile, so nothing is typed by hand
and an import a game doesn't have is simply never claimed.

## Audio (XMA)

Games that drive the XMA hardware through its registers (Crackdown's XAudio does; the
`0x7FEA` MMIO sites in `config.toml`) get **no sound** from the emulator's own decoder in
this setup. `zz_xma.cpp` decodes the game's XMA packets itself with FFmpeg's `xma2` decoder,
behind the register model in `zz_xmamm.cpp`.

1. Get an FFmpeg **7.x shared** build for Windows x64 (e.g. the "shared" builds from
   [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) or
   [BtbN](https://github.com/BtbN/FFmpeg-Builds/releases)).
2. Copy `avcodec-61.dll`, `avutil-59.dll` and the DLLs they depend on
   (`swresample-5.dll`) into the console folder.
3. Create `<game>_xma.on`.

`<game>_xma_dec.log` says which avcodec it loaded, or why it turned itself off (missing DLL,
wrong major version, no xma2 decoder). An OBS Studio install also works: the decoder looks in
OBS's default folder if the console folder has no avcodec.

## Diagnostics that are always on

- **`zz_crash.cpp`, the fault namer.** A process-wide exception handler: every access
  violation in the game is written to `<game>_crash.log` with the guest function it was in
  (`sub_XXXXXXXX+0xNN`), the registers and the guest stack, instead of an anonymous host
  address. A second emulator process writes `<game>_crash_<pid>.log`.
- **`zz_switch.cpp`**: jump-table misses → `<game>_switchmiss.log` (empty = healthy).
- **`zz_setjmp.cpp`**: setjmp/longjmp → `<game>_setjmp.log`.
- **`zz_fps.cpp`**: frames per second → `<game>_fps.log`, once a second. With
  `ZZ_FPS_DEV_INTERVAL` defined (see `game.json`) it also prints the present interval
  (0/1 = 60 fps cap, 2 = 30).
- **`zz_time.cpp`**: the `mftb` timebase (see `zz_mftb.on` above).

Close the game before reading logs. Some files are held open while it runs.

## Writing your own

Rules every file here follows, each learned from a hang or a crash:

- **Never take a host lock (CRITICAL_SECTION, mutex) from code the guest runs.** Guest
  threads are fibers: one can take your lock and be switched out mid-section, and every
  other thread deadlocks behind it. Log from guest context with the lock-free `zz_wlog.h`
  (one `WriteFile` per line) or hand records to a host thread through a lock-free ring, as
  `zz_import.cpp` does.
- **An instrument's cost is part of the experiment.** An import logger that synced to disk
  under a spin lock made one boot 30x slower and was then mistaken for the bug.
- **Both DLL names load.** The emulator loads `<name>.dll` and `<name>_no.dll` into the same
  process, so every static initialiser runs twice. The files that patch the emulator use a
  per-process named mutex so only one copy acts.
- **Risky reads go behind a `__declspec(noinline)` helper with its own `__try`.** clang-cl's
  SEH misses a fault in an inlined load.
- **Say why you are off.** "marker absent", "signature mismatch at +0x...", "avcodec not
  found": one line, once.
- **Per-game addresses belong in the game, not the runtime.** `examples\crackdown\zz_lock.cpp`
  overrides one Crackdown function and is the pattern: copy such a file into
  `games\<name>\ppc\` after `recompile.ps1`, and it is compiled with the rest.
