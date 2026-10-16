# Boot a recompiled game straight from its extracted disc folder (no dashboard).
#
#   scripts\run.ps1 -Game crackdown -Console "D:\XeO3 Console" -Disc "D:\games\Crackdown"
#
# -Disc     the folder you extracted the disc into (it holds default.xex)
# -MediaId  defaults to the mediaId= in the console folder's LaunchArguments.txt
# -Log      capture the emulator's own output to games\<Game>\emu.log. Emu prints a LOT
#           (it slows the game down); use it for boot problems, not for playing.
#
# The emulator exits silently and at once on a bad argument, so this checks the process
# is still alive a few seconds later and says so.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$Console,
    [Parameter(Mandatory = $true)][string]$Disc,
    [string]$MediaId = "",
    [switch]$Log
)
. "$PSScriptRoot\common.ps1"
$info = Get-GameInfo $Game
$emu = Join-Path $Console "Emu.exe"
if (-not (Test-Path -LiteralPath $emu)) { Fail "$Console has no Emu.exe (docs\CONSOLE_FOLDER.md)." }
if (-not (Test-Path -LiteralPath (Join-Path $Console "$($info.dllName).dll"))) { Fail "the game DLL is not installed in $Console -- run scripts\install.ps1" }
$Console = (Resolve-Path -LiteralPath $Console).Path
$Disc = (Resolve-Path -LiteralPath $Disc).Path.TrimEnd('\')
if (-not (Get-ChildItem -LiteralPath $Disc -Filter "default.xex" -ErrorAction SilentlyContinue)) { Fail "$Disc has no default.xex -- point -Disc at the extracted disc folder." }

if (-not $MediaId) {
    $la = Join-Path $Console "LaunchArguments.txt"
    if ((Test-Path $la) -and ((Get-Content $la -Raw) -match "mediaId=([0-9A-Fa-f-]{36})")) { $MediaId = $Matches[1] }
    else { Fail "no -MediaId given and none found in $la (docs\CONSOLE_FOLDER.md, 'mediaId')." }
}

$argList = "-dvd `"$Disc`" -titleId $($info.titleId) -mediaId $MediaId"
Say "Emu.exe $argList"
if ($Log) {
    $logFile = Join-Path $info.Dir "emu.log"
    if (Test-Path $logFile) { Move-Item $logFile "$logFile.prev" -Force }
    $p = Start-Process -FilePath $emu -ArgumentList $argList -WorkingDirectory $Console -PassThru -RedirectStandardOutput $logFile
} else {
    $p = Start-Process -FilePath $emu -ArgumentList $argList -WorkingDirectory $Console -PassThru
}
Start-Sleep -Seconds 8
if ($p.HasExited) { Fail "Emu.exe exited within 8 s (code $($p.ExitCode)). Re-run with -Log and read games\$Game\emu.log." }
Say "running (pid $($p.Id)). Logs: $Console\${Game}_*.log"
