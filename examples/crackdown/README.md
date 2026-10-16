# Example: Crackdown

A complete, working configuration for **Crackdown** (title `4D5307DC`, `default.xex` MD5
`a4c8a553ce76ddd792bc1141fd0b16fc`), the game the kit's pipeline was last proven on.
Recompiling with these files reproduces, file for file, the C++ the playable build was made from.

| file | what |
|---|---|
| `config.toml` | the recompiler config: the eight save/restore helpers, the C runtime's `setjmp`/`longjmp`, every function boundary the loop found, and the 6 GPU + 36 XMA MMIO instructions |
| `switch.toml` | the 795 jump tables, after the scan/audit fixes |
| `zz_lock.cpp` | a Crackdown-only instrument (and opt-in fix) for one D3D lock function: an example of a per-game runtime file |

## Using it

You need your own Crackdown disc, unpacked with xdvdfs.

```powershell
.\scripts\new-game.ps1 -Name crackdown -Xex "D:\games\Crackdown\default.xex"
Copy-Item examples\crackdown\config.toml, examples\crackdown\switch.toml games\crackdown\ -Force
```

Add the frame meter's present-interval offset to `games\crackdown\game.json`:

```json
"defines": [ "ZZ_FPS_DEV_INTERVAL=13220u" ]
```

Then:

```powershell
.\scripts\recompile.ps1 -Game crackdown      # 2 passes, 0 switch errors, 0 traps
.\scripts\build.ps1     -Game crackdown      # ~50 MB DLL
.\scripts\install.ps1   -Game crackdown -Console "D:\XeO3 Console"
```

Switches it runs with ([docs/RUNTIME.md](../../docs/RUNTIME.md)):

```powershell
"xma","zz_tile","zz_mrtfix","zz_vgpu","zz_vgpu_conv","zz_vgpu_memo2","zz_vgpu_skip",
"zz_edram_init","zz_cast_fix","zz_map_clamp","zz_sampler32" |
  ForEach-Object { New-Item -ItemType File -Force "D:\XeO3 Console\crackdown_$_.on" | Out-Null }
```

and boot it:

```powershell
.\scripts\run.ps1 -Game crackdown -Console "D:\XeO3 Console" -Disc "D:\games\Crackdown"
```

## State

Campaign playable, sound and saves work, ~21 fps. **Open:** the lower part of the 3D view
stays black. The frame is rendered in two tile bins and the second bin's resolve never
reaches the texture the game samples (bin 1, rows 480-719). `<game>_zz_tile_probe.on` turns on
the instruments that were narrowing it down.

## zz_lock.cpp

Not needed to play. To use it, copy it into `games\crackdown\ppc\` after `recompile.ps1`
(it is compiled with the rest), and re-add the two `zz_lock_note_*` calls that
`runtime\zz_save.cpp`'s `zz_obs_log` describes. It logs every D3D resource lock to
`crackdown_mem.log`. With `crackdown_lock_alias.on` it returns an aliased view of the page
when the one D3D handed out isn't committed. It registers its lock ring with the crash
reporter through `zz_game_dump`.
