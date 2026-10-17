# Turn a console folder into a dashboard console: the recompiled NXE dashboard boots when
# Emu.exe starts, and games launch from it (docs\DASHBOARD.md).
#
#   scripts\install-dashboard.ps1 -Console "D:\XeO3 Dash" -Flash "D:\packages\<title>\Flash"
#
# -Console  a console folder (docs\CONSOLE_FOLDER.md). Use a separate COPY of your console
#           folder for the dashboard -- a real copy, not hard links: this replaces the title
#           it boots. -Force is needed when its Game\default.xex is some other title.
# -Flash    the Flash folder of the back-compat package your dash.xex came from: dash.xex is
#           taken from there again, plus the boot files that only that (2014) flash has.
#
# Steps: the dashboard DLL (scripts\install.ps1 -Game dash), dash.xex as the boot title
# (Game\default.xex, unmodified -- its header is integrity-checked in the guest), the 2014
# flash files the console package lacks (added, never overwritten), LaunchArguments.txt
# rewritten to the dashboard's title (the original kept as LaunchArguments.txt.before_dash;
# add-game-to-my-games.ps1 takes the games' media ID from it), zz_switch.exe (the title
# switcher's relay) built and copied in, and the markers that switch the runtime's fixes on.
# Nothing is launched.
param(
    [Parameter(Mandatory = $true)][string]$Console,
    [Parameter(Mandatory = $true)][string]$Flash,
    [switch]$Force
)
. "$PSScriptRoot\common.ps1"
$info = Get-GameInfo "dash"
if ($info.kind -ne "dashboard") { Fail "games\dash is not the dashboard -- run scripts\recompile-dashboard.ps1" }
$known = Get-Content (Join-Path $KitRoot "dashboard\dash.json") -Raw | ConvertFrom-Json
$py = Get-Python

if (-not (Test-Path -LiteralPath (Join-Path $Console "Emu.exe"))) { Fail "$Console has no Emu.exe -- that is not a console folder (docs\CONSOLE_FOLDER.md)." }
$Console = (Resolve-Path -LiteralPath $Console).Path
$running = @(Get-Process -Name Emu -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -like "$Console\*" })
if ($running.Count) { Fail "Emu.exe is running from $Console (pid $($running[0].Id)). Close it first." }
$srcDash = Join-Path $Flash "dash.xex"
if (-not (Test-Path -LiteralPath $srcDash)) { Fail "$Flash has no dash.xex" }
if ((Get-Md5 $srcDash) -ne $known.dashXexMd5) { Fail "$srcDash is not the dash.xex build games\dash was made from ($($known.version))." }

# --- the boot title -----------------------------------------------------------------------
$boot = Join-Path $Console "Game\default.xex"
if ((Test-Path $boot) -and ((Get-Md5 $boot) -ne $known.dashXexMd5)) {
    if (-not $Force) {
        Fail ("$Console boots another title (Game\default.xex). Use a separate copy of your console folder" +
              " for the dashboard, or pass -Force to replace it (the old one is kept as default.xex.before_dash).")
    }
    if (-not (Test-Path "$boot.before_dash")) { Move-Item $boot "$boot.before_dash" } else { Remove-Item $boot -Force }
}

Say "== the dashboard DLL"
& "$PSScriptRoot\install.ps1" -Game dash -Console $Console
if ($LASTEXITCODE -ne 0) { Fail "install.ps1 failed" }

Say "== boot title and flash files"
New-Item -ItemType Directory -Force -Path (Join-Path $Console "Game") | Out-Null
Copy-Item -LiteralPath $srcDash $boot -Force
Say "  Game\default.xex = dash.xex $($known.version)"
$added = @(); $kept = @(); $absent = @()
foreach ($f in @("dash.xex") + @($known.flashSupport)) {
    $s = Join-Path $Flash $f; $d = Join-Path $Console "Flash\$f"
    if (-not (Test-Path -LiteralPath $s)) { $absent += $f; continue }
    if (Test-Path -LiteralPath $d) { $kept += $f; continue }
    Copy-Item -LiteralPath $s $d
    $added += $f
}
Say ("  flash files added: " + $(if ($added) { $added -join ", " } else { "(none)" }))
if ($kept) { Say ("  already present, left alone: " + ($kept -join ", ")) }
if ($absent) { Write-Host ("  not in $Flash (skipped): " + ($absent -join ", ")) -ForegroundColor Yellow }

Say "== LaunchArguments.txt"
# The emulator takes the title identity from here; when it disagreed with the boot XEX's own
# title id the guest read the header and quit without one error line. So it is rewritten
# from the dashboard's id, keeping every other setting the package had.
$la = Join-Path $Console "LaunchArguments.txt"
$old = if (Test-Path $la) { [System.IO.File]::ReadAllText($la).Trim() } else { "" }
if ($old -and -not (Test-Path "$la.before_dash") -and $old -notmatch "titleId=$($known.titleId)") { Copy-Item $la "$la.before_dash" }
$q = $old.TrimStart('?')
$parts = @($q -split '&' | Where-Object { $_ -and $_ -notmatch '^(mediaId|titleId)=' })
$new = "?" + ((@("mediaId=$($known.titleId)-0000-0000-0000-000000000000", "titleId=$($known.titleId)") + $parts) -join "&")
[System.IO.File]::WriteAllText($la, $new)      # exact bytes: no BOM, no newline
Say "  $new"
if (Test-Path "$la.before_dash") { Say "  (the package's own arguments are in LaunchArguments.txt.before_dash)" }

Say "== zz_switch.exe (the title switcher's relay)"
$relayDir = Join-Path $info.Dir "relay"
New-Item -ItemType Directory -Force -Path $relayDir | Out-Null
$clang = Get-ClangCl
$src = Join-Path $KitRoot "dashboard\relay\zz_switch.cpp"
$rc = Invoke-InVcEnv "`"$clang`" /nologo /O2 /Fe:zz_switch.exe `"$src`" /link kernel32.lib > build.log 2>&1" $relayDir
if ($rc -ne 0 -or -not (Test-Path (Join-Path $relayDir "zz_switch.exe"))) { Get-Content (Join-Path $relayDir "build.log") | Out-Host; Fail "zz_switch.exe did not build" }
Copy-Item (Join-Path $relayDir "zz_switch.exe") (Join-Path $Console "zz_switch.exe") -Force
Say "  installed zz_switch.exe"

Say "== markers"
foreach ($m in "dash_swapfix.on", "dash_self_relaunch.on") {
    $p = Join-Path $Console $m
    if (-not (Test-Path $p)) { New-Item -ItemType File -Path $p | Out-Null }
    Say "  $m"
}
# The Guide button calls XamShowGuideUI by address in ONE xam build; with another, leave it off.
$xam = Join-Path $Console "Flash\xam.xex"
$xamName = if (Test-Path $xam) { (& $py -I (Join-Path $ToolsDir "xeo3_name.py") $xam | Out-String) } else { "" }
$guide = Join-Path $Console "dash_guide.on"
if ($xamName -match [regex]::Escape($known.xam.dllName)) {
    if (-not (Test-Path $guide)) { New-Item -ItemType File -Path $guide | Out-Null }
    Say "  dash_guide.on (xam is the build the Guide button was made for)"
} else {
    Remove-Item $guide -ErrorAction SilentlyContinue
    Write-Host "  dash_guide.on NOT created: this console's xam is not the build the Guide button was made for (everything else works)" -ForegroundColor Yellow
}

Say ""
Write-Host ("STILL NEEDED: the console's 360 kernel refuses to load dash.xex from the game folder (it is a" +
            " flash-only system title). This kit does not change that yet -- docs\DASHBOARD.md, 'The kernel loader'.") -ForegroundColor Yellow
Say "Then: run Emu.exe in $Console (no arguments). Add games with scripts\add-game-to-my-games.ps1."
