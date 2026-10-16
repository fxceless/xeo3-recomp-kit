# Original Xbox games

The same emulator runs original Xbox games too. On the Xbox 360, original-Xbox back-compat
was itself a program, `xefu.xex`, the 360's own original-Xbox emulator. XEO3 runs it the way
it runs everything else: as a 360 module whose code comes from a precompiled DLL. Microsoft's
back-compat packages for original Xbox games ship all of it, plus two DLLs named `xefu_...`.

Nothing in this route is recompiled by the kit.

## What has been seen working

BLiNX: The Time Sweeper's back-compat package boots and runs on PC. Its boot log shows:

- the **360 kernel DLL** and **xam** load, exactly as for a 360 game, then
- the kernel loads the original-Xbox emulator as the title, selected by the **`fusion`** flag
  in `LaunchArguments.txt`:
  `Loaded EXE xefu.xex (\Device\Harddisk0\SystemPartition\Compatibility\xefu.xex)`, its code
  coming from `xeo3_6a3039f4_50252f55_3eff95ac_d4d154cc_05a5ef9b.dll` (the name
  `tools\xeo3_name.py` gives for that `xefu.xex`; the same file in every console folder seen).

That package's launch arguments, one per line:

```
aaBoostOn
aaBoostTargetMsaa=1
fusion
mediaId=4D530013-0000-0000-0000-000000000000
scalingResolutions=640x480 1280x960
titleId=4D530013
vertexBuffersWriteProtected
configurationId=23e30100-9bf7-41ff-afeb-f3c73967b50c
xoallowtitletoskipresolves=true
```

The two `xefu_...` DLLs. Neither appears in the part of the boot log that was captured, so
what they contain is read from their names and sizes only:

| file | facts | likely role (conjecture) |
|---|---|---|
| `xefu_69c41281_00027bcf.dll` | two-part name, the form XEO3 uses for a PE image (as for the 360 kernel); identical in every package seen | the original Xbox kernel, translated |
| `xefu_<five hashes>.dll` | five-part name; **different per package**: BLiNX's 24 MB, Fuzion Frenzy's 37 MB | that package's game code (x86), translated |
## Not tested yet

Running an original Xbox game that Microsoft didn't ship this way. The expected route is
the one above: a console folder with the `xefu` DLLs, the `fusion` flag, and the game's own
title ID and files. Questions to answer first, in order:

1. Where the emulator expects the game's files for a `fusion` boot (`Game\` in the package,
   or `-dvd <folder>`).
2. What the per-package `xefu_<five hashes>.dll` really holds, and what it is named after. If it is
   the game's own code translated (the conjecture above), a game Microsoft never shipped needs
   one made for it: an x86-to-`xefu` translator, the original-Xbox counterpart of this kit. That
   doesn't exist yet.
3. What it logs (`run.ps1 -Log`) at the first thing it can't do.

If you try it, open an issue with your `emu.log`: this is the least-explored part of the
project. As with everything here, use only games and packages you own.
