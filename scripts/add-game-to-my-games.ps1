# Put one of your recompiled games in the dashboard's My Games (and optionally in the
# disc drive, for "Play Disc").
#
#   scripts\add-game-to-my-games.ps1 -Game crackdown -Console "D:\XeO3 Dash" `
#       -God "D:\god\Crackdown" -Disc "D:\games\Crackdown" [-InDrive]
#
# -Game     a game made with new-game.ps1 and already installed in THIS console folder
#           (scripts\install.ps1 -Game <name> -Console <this folder>): the dashboard starts
#           the game from the same folder, so its DLL has to be there.
# -Console  the dashboard's console folder (scripts\install-dashboard.ps1)
# -God      the Games-on-Demand package of YOUR disc, made with iso2god (docs\DASHBOARD.md):
#           the output folder, or the package file itself (<titleId>\00007000\<name>)
# -Disc     the extracted disc folder the game boots from (holds default.xex)
# -MediaId  the media ID passed to the emulator for this game. Default: the one the console
#           package's own LaunchArguments.txt had before install-dashboard.ps1 rewrote it
#           (LaunchArguments.txt.before_dash), which is what scripts\run.ps1 uses too.
# -InDrive  also make it the game "in the drive" (zz_disc.txt), for Play Disc on the home tab
# -Copy     copy the package's data files instead of hard-linking them (default: hard links
#           when the package is on the same drive as the virtual hard drive -- several GB saved)
#
# What it does: copies the package into the console's virtual hard drive, CON-signs its
# header with the console key in YOUR Emu.exe (tools\consign.py; without it xam rejects the
# package and My Games says "no games found"), writes the game's line in zz_titles.txt (what
# the dashboard's launcher reads to know which folder and title to boot), and moves the
# dashboard's content cache aside so the new package is picked up. Nothing is rebuilt.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$Console,
    [Parameter(Mandatory = $true)][string]$God,
    [Parameter(Mandatory = $true)][string]$Disc,
    [string]$MediaId = "",
    [switch]$InDrive,
    [switch]$Copy
)
. "$PSScriptRoot\common.ps1"
$info = Get-GameInfo $Game
if ($info.kind -eq "dashboard") { Fail "-Game is the game to add, not the dashboard." }
$py = Get-Python

if (-not (Test-Path -LiteralPath (Join-Path $Console "Emu.exe"))) { Fail "$Console has no Emu.exe -- that is not a console folder." }
$Console = (Resolve-Path -LiteralPath $Console).Path
$emu = Join-Path $Console "Emu.exe"
$running = @(Get-Process -Name Emu -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -like "$Console\*" })
if ($running.Count) { Fail "Emu.exe is running from $Console (pid $($running[0].Id)). Close the console first: this writes to its virtual hard drive." }
foreach ($f in "$($info.dllName).dll", "$($info.dllName)_no.dll") {
    if (-not (Test-Path (Join-Path $Console $f))) { Fail "$f is not in $Console -- run scripts\install.ps1 -Game $Game -Console `"$Console`" first." }
}
if (-not (Test-Path (Join-Path $Console "zz_switch.exe"))) { Fail "$Console has no zz_switch.exe -- run scripts\install-dashboard.ps1 first." }
if (-not (Test-Path -LiteralPath (Join-Path $Disc "default.xex"))) { Fail "$Disc has no default.xex -- point -Disc at the extracted disc folder." }
$Disc = (Resolve-Path -LiteralPath $Disc).Path.TrimEnd('\')
$xex = Join-Path $info.Dir "xex\default.xex"

# --- the package ---------------------------------------------------------------------
function Read-Header([string]$path) {
    $fs = [System.IO.File]::OpenRead($path)
    try { $b = New-Object byte[] 0x400; [void]$fs.Read($b, 0, $b.Length) } finally { $fs.Close() }
    $magic = [System.Text.Encoding]::ASCII.GetString($b, 0, 4)
    $be = { param($o) ([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o + 1] -shl 16) -bor ([uint32]$b[$o + 2] -shl 8) -bor [uint32]$b[$o + 3] }
    return [pscustomobject]@{ Magic = $magic; ContentType = (& $be 0x344); TitleId = ('{0:X8}' -f (& $be 0x360)) }
}
$pkg = $null
if (Test-Path -LiteralPath $God -PathType Leaf) { $pkg = (Resolve-Path -LiteralPath $God).Path }
elseif (Test-Path -LiteralPath $God -PathType Container) {
    $cands = @(Get-ChildItem -LiteralPath $God -Recurse -File | Where-Object { $_.Directory.Name -eq "00007000" -and $_.Extension -eq "" })
    $cands = @($cands | Where-Object { (Read-Header $_.FullName).TitleId -eq $info.titleId })
    if ($cands.Count -ne 1) { Fail "found $($cands.Count) Games-on-Demand packages for title $($info.titleId) under $God (expected 1). Pass the package file itself." }
    $pkg = $cands[0].FullName
} else { Fail "no such file or folder: $God" }
$hdr = Read-Header $pkg
if ($hdr.Magic -notin "LIVE", "PIRS", "CON ") { Fail "$pkg is not a content package (magic '$($hdr.Magic)')." }
if ($hdr.TitleId -ne $info.titleId) { Fail "$pkg is title $($hdr.TitleId), but $Game is $($info.titleId)." }
if ($hdr.ContentType -ne 0x7000) { Write-Host ("  note: content type {0:X8}, not 00007000 (Games on Demand)" -f $hdr.ContentType) -ForegroundColor Yellow }
$data = "$pkg.data"
Say "package  $pkg ($($hdr.Magic.Trim()), title $($hdr.TitleId))"

# --- where it goes: the console identity's virtual hard drive ---------------------------
$cfg = Get-Content (Join-Path $Console "MicrosoftGame.config") -Raw
if ($cfg -notmatch '<Identity[^>]*\sName="([^"]+)"') { Fail "no Identity Name in $Console\MicrosoftGame.config" }
$identity = $Matches[1]
$vd = Join-Path $env:LOCALAPPDATA "$identity\VirtualDrive\Harddisk\Partition1"
if (-not (Test-Path $vd)) { Fail "$vd does not exist yet. Boot the dashboard once (it creates its virtual hard drive), then run this again." }
$dstDir = Join-Path $vd ("Content\0000000000000000\{0}\{1:X8}" -f $info.titleId, $hdr.ContentType)
$dst = Join-Path $dstDir (Split-Path $pkg -Leaf)
New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
Copy-Item -LiteralPath $pkg $dst -Force
Remove-Item "$dst.unsigned" -ErrorAction SilentlyContinue      # consign.py keeps the new original
Say "copied   -> $dst"
if (Test-Path -LiteralPath $data) {
    $dstData = "$dst.data"
    New-Item -ItemType Directory -Force -Path $dstData | Out-Null
    $sameDrive = ([System.IO.Path]::GetPathRoot($data) -eq [System.IO.Path]::GetPathRoot($dstData))
    $n = 0
    foreach ($f in Get-ChildItem -LiteralPath $data -File) {
        $t = Join-Path $dstData $f.Name
        if (Test-Path $t) { Remove-Item $t -Force }
        if ($sameDrive -and -not $Copy) { New-Item -ItemType HardLink -Path $t -Target $f.FullName | Out-Null }
        else { Copy-Item -LiteralPath $f.FullName $t }
        $n++
    }
    Say ("data     {0} files {1}" -f $n, $(if ($sameDrive -and -not $Copy) { "hard-linked" } else { "copied" }))
} else {
    Write-Host "  no $data folder next to the package -- copying the header alone (the tested setup had the data folder)" -ForegroundColor Yellow
}

# --- sign ----------------------------------------------------------------------------------
& $py -I (Join-Path $ToolsDir "consign.py") sign $dst --emu $emu --xex $xex
if ($LASTEXITCODE -ne 0) { Fail "signing failed" }
Remove-Item "$dst.unsigned" -ErrorAction SilentlyContinue      # the source package is the original

# --- zz_titles.txt / zz_disc.txt -----------------------------------------------------------
if (-not $MediaId) {
    foreach ($la in "LaunchArguments.txt.before_dash", "LaunchArguments.txt") {
        $p = Join-Path $Console $la
        if ((Test-Path $p) -and ((Get-Content $p -Raw) -match "mediaId=([0-9A-Fa-f-]{36})") -and ($Matches[1] -notlike "FFFE07D1-*")) { $MediaId = $Matches[1]; break }
    }
    if (-not $MediaId) { Fail "no -MediaId given and none found in $Console\LaunchArguments.txt.before_dash" }
}
$line = "$($info.titleId)=$Disc|$MediaId|"
$titles = Join-Path $Console "zz_titles.txt"
$keep = @()
if (Test-Path $titles) { $keep = @(Get-Content $titles | Where-Object { $_ -and ($_ -notlike "$($info.titleId)=*") }) }
[System.IO.File]::WriteAllText($titles, ((@($keep) + $line) -join "`r`n") + "`r`n", [System.Text.Encoding]::ASCII)
Say "zz_titles.txt: $line"
if ($InDrive) {
    $disc = "dvd=$Disc`r`ntitleId=$($info.titleId)`r`nmediaId=$MediaId`r`n"
    [System.IO.File]::WriteAllText((Join-Path $Console "zz_disc.txt"), $disc, [System.Text.Encoding]::ASCII)
    Say "zz_disc.txt: $Game is in the drive (Play Disc)"
}

# --- the dashboard's content cache ----------------------------------------------------------
# The dash keeps its own cache of what each package is (Content\0000000000000000\FFFE07DF\
# 00040000\ContentCache.pkg). A package it once judged invalid stays invalid in there, so the
# cache is moved aside and the dash rebuilds it at the next boot.
$cache = Join-Path $vd "Content\0000000000000000\FFFE07DF\00040000\ContentCache.pkg"
if (Test-Path $cache) {
    $aside = "$cache.prev_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
    Move-Item $cache $aside
    Say "content cache moved aside -> $(Split-Path $aside -Leaf) (rebuilt at the next boot)"
}
Say ""
Say "Done. Boot the dashboard; $Game is under My Games (and Play Disc if -InDrive)."
