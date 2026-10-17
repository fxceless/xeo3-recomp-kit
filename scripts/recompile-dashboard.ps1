# Recompile YOUR copy of the Xbox 360 NXE dashboard (dash.xex) into games\dash\ppc\, ready
# for scripts\build.ps1 -Game dash.
#
#   scripts\recompile-dashboard.ps1 -DashXex "D:\packages\<title>\Flash\dash.xex"
#
# -DashXex  the dashboard's executable, from the Flash folder of a back-compat package you
#           own (docs\DASHBOARD.md says which). Only the build dashboard\dash.json names is
#           accepted: the config's addresses fit that build and no other.
#
# What it does, in order:
#   1. checks the file is that build (size + MD5), copies it to games\dash\xex\
#   2. XexRepack -> dash_dec.xex (decrypted, basic compression, header byte-identical, so
#      the DLL name does not change), checks its MD5 too
#   3. XenonRecomp with dashboard\config.toml + switch.toml, then checks the output against
#      the known-good recompile: file count, function count, no switch errors
#   4. copies the dashboard runtime (dashboard\runtime\) and the kit runtime pieces it uses
#      into ppc\, writes ppc_precomp_init.cpp (the recompiler's, plus the dashboard's
#      start/stop calls), zz_title.h and zz_dash_idx.h (import indices by name)
#   5. writes games\dash\game.json so build.ps1 / install.ps1 treat it like a game
# Nothing is built or installed. Next: scripts\build.ps1 -Game dash.
param(
    [Parameter(Mandatory = $true)][string]$DashXex
)
. "$PSScriptRoot\common.ps1"
$Name = "dash"
$known = Get-Content (Join-Path $KitRoot "dashboard\dash.json") -Raw | ConvertFrom-Json
$py = Get-Python

if (-not (Test-Path -LiteralPath $DashXex)) { Fail "no such file: $DashXex" }
$DashXex = (Resolve-Path -LiteralPath $DashXex).Path
$fs = [System.IO.File]::OpenRead($DashXex); $head = New-Object byte[] 4; [void]$fs.Read($head, 0, 4); $fs.Close()
if ([System.Text.Encoding]::ASCII.GetString($head) -ne "XEX2") { Fail "$DashXex is not an Xbox 360 executable (no XEX2 header)." }
$md5 = Get-Md5 $DashXex
if ($md5 -ne $known.dashXexMd5) {
    $nameLine = & $py -I (Join-Path $ToolsDir "xeo3_name.py") $DashXex | Out-String
    Fail ("$DashXex is not the dashboard build this kit was made for.`n" +
          "  expected: dash.xex $($known.version), $($known.dashXexSize) bytes, md5 $($known.dashXexMd5)`n" +
          "            ($($known.dllName).dll)`n" +
          "  got:      $((Get-Item -LiteralPath $DashXex).Length) bytes, md5 $md5`n" +
          "            $($nameLine.Trim())`n" +
          "  A different dashboard build needs its own config (addresses, jump tables, setjmp/longjmp)" +
          " -- docs\DASHBOARD.md, 'Another dashboard build'.")
}
Assert-ToolsFresh

$dir = Join-Path $GamesRoot $Name
$gj = Join-Path $dir "game.json"
if ((Test-Path $gj) -and ((Get-Content $gj -Raw | ConvertFrom-Json).kind -ne "dashboard")) {
    Fail "games\$Name exists and is not the dashboard. Move it aside first."
}
$ppc = Join-Path $dir "ppc"
foreach ($d in "", "xex", "ppc", "dll") { New-Item -ItemType Directory -Force -Path (Join-Path $dir $d) | Out-Null }

Say "== 1. dash.xex $($known.version) (md5 $md5)"
Copy-Item -LiteralPath $DashXex (Join-Path $dir "xex\dash.xex") -Force

Say "== 2. XexRepack"
$repack = Join-Path $Recompiler "build\XexRepack\XexRepack.exe"
if (-not (Test-Path $repack)) { Fail "XexRepack.exe is not built yet. Run scripts\setup.ps1." }
$dec = Join-Path $dir "xex\dash_dec.xex"
$out = & $repack (Join-Path $dir "xex\dash.xex") $dec 2>&1 | Out-String
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dec)) { Write-Host $out; Fail "XexRepack failed" }
$decMd5 = Get-Md5 $dec
if ($decMd5 -ne $known.dashDecMd5) { Fail "dash_dec.xex md5 $decMd5, expected $($known.dashDecMd5) -- XexRepack does not match the kit's" }
Say "  dash_dec.xex md5 $decMd5 (as expected)"

Say "== 3. XenonRecomp"
Copy-Item (Join-Path $KitRoot "dashboard\config.toml") (Join-Path $dir "config.toml") -Force
Copy-Item (Join-Path $KitRoot "dashboard\switch.toml") (Join-Path $dir "switch.toml") -Force
# Start from an empty ppc\: XenonRecomp never deletes surplus files, and the runtime copied
# in by an earlier run must not be counted as recompiler output below.
Get-ChildItem $ppc -File -ErrorAction SilentlyContinue | Remove-Item -Force
$ctxHeader = Join-Path $Recompiler "XenonUtils\ppc_context.h"
$started = Get-Date
Push-Location $dir
try { cmd /c "`"$XenonRecompExe`" config.toml `"$ctxHeader`" > recomp.log 2>&1" } finally { Pop-Location }
$rc = $LASTEXITCODE
$log = Get-Content (Join-Path $dir "recomp.log")
$sw = @($log | Where-Object { $_ -like "ERROR: Switch case*" })
$other = @($log | Where-Object { $_ -like "ERROR*" -and $_ -notlike "ERROR: Switch case*" })
$src = @(Get-ChildItem $ppc -Filter "ppc_recomp.*.cpp")
$generated = @(Get-ChildItem $ppc -File).Count
$funcs = ($src | Select-String -Pattern "PPC_FUNC_IMPL(__imp__" -SimpleMatch | Measure-Object).Count
Say ("  exit {0} in {1:N0} s" -f $rc, ((Get-Date) - $started).TotalSeconds)
$checks = @(
    @("source files (ppc_recomp.*.cpp)", $src.Count, $known.expect.recompFiles),
    @("generated files in total", $generated, $known.expect.generatedFiles),
    @("functions", $funcs, $known.expect.functions),
    @("switch errors", $sw.Count, 0),
    @("other recompiler errors", $other.Count, 0)
)
$bad = 0
foreach ($c in $checks) {
    $ok = ($c[1] -eq $c[2])
    if (-not $ok) { $bad++ }
    Say ("  {0,-34} {1,6}   expected {2,6}   {3}" -f $c[0], $c[1], $c[2], $(if ($ok) { "ok" } else { "MISMATCH" }))
}
if ($bad) {
    $other | Select-Object -First 10 | ForEach-Object { Write-Host "    $_" }
    Fail "the recompile does not match the known-good one (games\$Name\recomp.log)"
}

Say "== 4. runtime"
Copy-Item (Join-Path $KitRoot "dashboard\runtime\*") $ppc -Force
# from the kit's game runtime: the 50 MHz timebase (zz_mftb), the setjmp/longjmp reporter
# and the jump-table miss reporter. Nothing else of it: the dash runs on real xam.
foreach ($f in "zz_time.cpp", "zz_setjmp.cpp", "zz_switch.cpp", "zz_switch_hook.h") {
    Copy-Item (Join-Path $RuntimeDir $f) $ppc -Force
}
$title = "// GENERATED by scripts\recompile-dashboard.ps1 -- log/marker prefix (see runtime\zz_title.h).`n#pragma once`n#define ZZ_TITLE `"$Name`"`n"
[System.IO.File]::WriteAllText((Join-Path $ppc "zz_title.h"), $title)

# ppc_precomp_init.cpp = the recompiler's own, plus the dashboard's start and stop. Each
# anchor must be found exactly once, or the file is not the one this was written against.
$init = [System.IO.File]::ReadAllText((Join-Path $Recompiler "XenonUtils\ppc_precomp_init.cpp"))
$nl = if ($init.Contains("`r`n")) { "`r`n" } else { "`n" }
$a1 = '__declspec(dllexport) extern "C" int InitPrecompiledDll('
$a2 = '    // Version negotiation as in the official DLL'
$a3 = '__declspec(dllexport) extern "C" void CleanupPrecompiledDll() {'
$edits = @(
    , @($a1, ("// GENERATED by scripts\recompile-dashboard.ps1: the dashboard runtime's start/stop (dashboard\runtime\zz_dash_init.cpp)" +
              $nl + 'extern "C" void zz_dash_init();' + $nl + 'extern "C" void zz_dash_cleanup();' + $nl + $nl + $a1))
    , @($a2, ("    zz_dash_init();" + $nl + $nl + $a2))
    , @($a3, ($a3 + $nl + "    zz_dash_cleanup();"))
)
foreach ($e in $edits) {
    if ($e.Count -ne 2) { Fail "internal: edit list malformed" }
    $n = ([regex]::Matches($init, [regex]::Escape($e[0]))).Count
    if ($n -ne 1) { Fail "recompiler\XenonUtils\ppc_precomp_init.cpp: '$($e[0])' found $n times (expected 1)" }
    $init = $init.Replace($e[0], $e[1])
}
[System.IO.File]::WriteAllText((Join-Path $ppc "ppc_precomp_init.cpp"), $init)
$gen = @(& $py -I (Join-Path $ToolsDir "gen_dash_idx.py") $dir)
if ($LASTEXITCODE -ne 0) { $gen | Out-Host; Fail "gen_dash_idx.py failed" }
Say "  $($gen[0])"
Invoke-Tool "check_ppc_set.py" @($ppc)

Say "== 5. game.json"
$info = [ordered]@{
    name          = $Name
    kind          = "dashboard"
    titleId       = $known.titleId
    dllName       = $known.dllName
    xexMd5        = $md5
    defines       = @()
    # scripts\build.ps1: the dashboard opts into its own import hook instead of the game
    # runtime's (saves, sign-in, audio): the dash runs on the console's real xam.
    forceIncludes = @("zz_title.h", "zz_switch_hook.h", "zz_dash_hook.h")
}
$info | ConvertTo-Json | Set-Content -Path $gj -Encoding utf8
Say ""
Say "games\$Name is ready. Next: scripts\build.ps1 -Game $Name, then scripts\install-dashboard.ps1 (docs\DASHBOARD.md)."
