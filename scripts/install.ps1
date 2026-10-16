# Copy a built game DLL into your console folder (the folder Emu.exe runs from).
#
#   scripts\install.ps1 -Game crackdown -Console "D:\XeO3 Console"
#
# * BOTH names: the emulator loads <name>.dll and <name>_no.dll; installing one of them is a
#   half-install that does not fail, it just runs the old code half the time.
# * the artefact, not the log: both copies are re-hashed after the copy.
# * the previous install is backed up to games\<Game>\dll\backup_<md5>\ once.
# * every <game>_*.log the next run writes is moved aside (.prev_<time>), so an old log can
#   never be mistaken for the new run's.
# * refuses while a running Emu.exe has THIS game's module loaded.
# * creates zz_mftb.on in the console folder if it is missing (the 50 MHz guest timebase --
#   without it every timer in the game runs 70-100x too fast). -NoMftb skips that.
# Nothing is launched: run the game with scripts\run.ps1.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$Console,
    [switch]$Force,
    [switch]$NoMftb
)
. "$PSScriptRoot\common.ps1"
$info = Get-GameInfo $Game
$name = $info.dllName
$stage = Join-Path $info.Dir "dll"
if (-not (Test-Path -LiteralPath (Join-Path $Console "Emu.exe"))) { Fail "$Console has no Emu.exe -- that is not a console folder (docs\CONSOLE_FOLDER.md)." }
$Console = (Resolve-Path -LiteralPath $Console).Path

$running = @(Get-Process -Name Emu -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -like "$Console\*" })
foreach ($p in $running) {
    $has = $true
    try { $has = [bool]($p.Modules | Where-Object { $_.ModuleName -like "$name*" }) } catch { }
    if ($has) { Fail "Emu.exe (pid $($p.Id)) has this game loaded. Close the game first." }
}

foreach ($f in "$name.dll", "${name}_no.dll") {
    if (-not (Test-Path (Join-Path $stage $f))) { Fail "games\$Game\dll\$f is missing -- run scripts\build.ps1" }
}
$m1 = Get-Md5 (Join-Path $stage "$name.dll")
$m2 = Get-Md5 (Join-Path $stage "${name}_no.dll")
if ($m1 -ne $m2) { Fail "the two staged copies differ ($m1 / $m2) -- rebuild" }
$built = (Get-Item (Join-Path $stage "$name.dll")).LastWriteTime
$newer = @(Get-ChildItem (Join-Path $info.Dir "ppc") -Filter "zz_*" | Where-Object { $_.LastWriteTime -gt $built })
if ($newer.Count -and -not $Force) {
    $newer | ForEach-Object { Say "  $($_.Name) changed $($_.LastWriteTime)" }
    Fail "a runtime source is newer than the built DLL ($built). Rebuild, or pass -Force."
}

$dst = Join-Path $Console "$name.dll"
if (Test-Path $dst) {
    $old = Get-Md5 $dst
    $bak = Join-Path $stage "backup_$($old.Substring(0, 8))"
    if (-not (Test-Path $bak) -and $old -ne $m1) {
        New-Item -ItemType Directory -Path $bak | Out-Null
        Copy-Item $dst, (Join-Path $Console "${name}_no.dll") $bak -ErrorAction SilentlyContinue
        Say "backed up the previous install to games\$Game\dll\backup_$($old.Substring(0, 8))"
    }
}
Copy-Item (Join-Path $stage "$name.dll") $dst -Force
Copy-Item (Join-Path $stage "${name}_no.dll") (Join-Path $Console "${name}_no.dll") -Force
$i1 = Get-Md5 $dst; $i2 = Get-Md5 (Join-Path $Console "${name}_no.dll")
if ($i1 -ne $m1 -or $i2 -ne $m1) { Fail "post-copy hash mismatch ($i1 / $i2, expected $m1)" }
Say "installed $name.dll + _no.dll, md5 $m1 (re-hashed after the copy)"

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
Get-ChildItem $Console -Filter "${Game}_*" -File | Where-Object { $_.Extension -in ".log", ".raw" } | ForEach-Object {
    Move-Item $_.FullName "$($_.FullName).prev_$stamp" -Force
    Say "moved aside $($_.Name)"
}

$mftb = Join-Path $Console "zz_mftb.on"
if (-not (Test-Path $mftb)) {
    if ($NoMftb) { Write-Host "zz_mftb.on is ABSENT: the game's timers will run on the raw CPU counter (too fast)." -ForegroundColor Yellow }
    else { New-Item -ItemType File -Path $mftb | Out-Null; Say "created zz_mftb.on (50 MHz guest timebase)" }
}
$markers = @(Get-ChildItem $Console -Filter "${Game}_*.on" -File)
if ($markers.Count) { Say "markers armed for ${Game}: $(($markers | ForEach-Object Name) -join ', ')" }
else { Say "no ${Game}_*.on markers: every optional fix and instrument is off (docs\RUNTIME.md lists them)" }
Say "Next: scripts\run.ps1 -Game $Game -Console `"$Console`" -Disc <extracted disc folder>"
