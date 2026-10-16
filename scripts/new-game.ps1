# Start a new game: makes games\<Name>\, copies its default.xex in, runs XenonAnalyse
# (jump tables + the eight register save/restore helpers + a flat image dump), and writes
# config.toml and game.json.
#
#   scripts\new-game.ps1 -Name crackdown -Xex "D:\extracted\Crackdown\default.xex"
#
# -Name   short name, a-z 0-9 _ (prefixes the game's log and marker files)
# -Xex    the game's executable from YOUR disc (extract the disc with xdvdfs; see docs)
# -Force  overwrite an existing games\<Name>\ (your config.toml edits are lost)
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Xex,
    [switch]$Force
)
. "$PSScriptRoot\common.ps1"
Test-GameName $Name
if (-not (Test-Path -LiteralPath $Xex)) { Fail "no such file: $Xex" }
$fs = [System.IO.File]::OpenRead((Resolve-Path -LiteralPath $Xex).Path); $head = New-Object byte[] 4; [void]$fs.Read($head, 0, 4); $fs.Close()
if ([System.Text.Encoding]::ASCII.GetString($head) -ne "XEX2") {
    Fail "$Xex is not an Xbox 360 executable (no XEX2 header). Original Xbox .xbe files are not recompiled -- see docs\ORIGINAL_XBOX.md."
}
Assert-ToolsFresh

$dir = Join-Path $GamesRoot $Name
if ((Test-Path $dir) -and -not $Force) { Fail "games\$Name already exists (use -Force to start over)." }
foreach ($d in "", "xex", "ppc", "dll") { New-Item -ItemType Directory -Force -Path (Join-Path $dir $d) | Out-Null }
Copy-Item -LiteralPath $Xex (Join-Path $dir "xex\default.xex") -Force
$xexIn = Join-Path $dir "xex\default.xex"

Say "== reading the XEX header"
$py = Get-Python
$hdr = & $py -I (Join-Path $ToolsDir "xex_hdr.py") $xexIn | Out-String
$titleId = if ($hdr -match "titleId=0x([0-9a-fA-F]{8})") { $Matches[1].ToUpper() } else { Fail "could not read the title ID from the XEX header" }
$nameLine = & $py -I (Join-Path $ToolsDir "xeo3_name.py") $xexIn | Out-String
$dllName = if ($nameLine -match "(xeo3_[0-9a-f_]+)\.dll") { $Matches[1] } else { Fail "could not compute the DLL name" }
Say "  title ID  $titleId"
Say "  DLL name  $dllName.dll (+ ${dllName}_no.dll)"

Say "== XenonAnalyse (jump tables, save/restore helpers, image dump)"
Push-Location $dir
try {
    $out = & $XenonAnalyseExe "xex\default.xex" "switch.toml" "image.bin" 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { Write-Host $out; Fail "XenonAnalyse failed" }
} finally { Pop-Location }
Set-Content -Path (Join-Path $dir "analyse.log") -Value $out
$addr = @{}
foreach ($k in "restgprlr_14", "savegprlr_14", "restfpr_14", "savefpr_14", "restvmx_14", "savevmx_14", "restvmx_64", "savevmx_64") {
    if ($out -match "$k`_address = (0x[0-9A-Fa-f]+)") { $addr[$k] = $Matches[1] } else { $addr[$k] = "0x0" }
}
$missing = @($addr.Keys | Where-Object { $addr[$_] -eq "0x0" })
$tables = ([regex]::Matches((Get-Content (Join-Path $dir "switch.toml") -Raw), "\[\[switch\]\]")).Count
Say "  $tables jump tables -> switch.toml"
if ($missing.Count) {
    Write-Host "  WARNING: not found: $($missing -join ', '). Find them by hand (docs\TUTORIAL.md, step 3)." -ForegroundColor Yellow
}

$cfg = @"
# $Name (title $titleId) -- XenonRecomp config, made by scripts\new-game.ps1.
# DLL name: $dllName.dll (+ _no.dll)

[main]
file_path = "./xex/default.xex"
out_directory_path = "./ppc"
switch_table_file_path = "./switch.toml"

# Given by XenonAnalyse (analyse.log). Sanity check: in every title so far the
# save->rest distances were gpr +0x50, fpr +0x4C, vmx +0x298, and vmx_64 = vmx_14 + 0x94.
restgprlr_14_address = $($addr.restgprlr_14)
savegprlr_14_address = $($addr.savegprlr_14)
restfpr_14_address   = $($addr.restfpr_14)
savefpr_14_address   = $($addr.savefpr_14)
restvmx_14_address   = $($addr.restvmx_14)
savevmx_14_address   = $($addr.savevmx_14)
restvmx_64_address   = $($addr.restvmx_64)
savevmx_64_address   = $($addr.savevmx_64)

# The C runtime's setjmp/longjmp, if the game uses them (docs\TUTORIAL.md, step 5).
# Naming them makes the recompiler emit PPC_SETJMP/PPC_LONGJMP; left unnamed, a guest
# longjmp just returns to its caller and the game misbehaves wherever it relies on one.
# setjmp_address  = 0x00000000
# longjmp_address = 0x00000000

# Function boundaries. Filled by scripts\recompile.ps1 (tools\fix_boundaries.py +
# tools\apply_boundaries.py) until the recompiler reports no switch errors.
functions = [
]

# First 4 bytes and size of any data wrongly interpreted as instructions
invalid_instructions = [
]

# Instructions that read or write hardware registers (GPU block 0x7FC8xxxx, XMA audio
# block 0x7FEAxxxx). scripts\recompile.ps1 fills the AUTO section with tools\mmio_scan.py;
# add any extra ones ABOVE the markers.
mmio_instructions = [
# >>> AUTO mmio (scripts\recompile.ps1) -- do not edit between the markers
# <<< AUTO mmio
]
"@
[System.IO.File]::WriteAllText((Join-Path $dir "config.toml"), $cfg)   # UTF-8, no BOM

$info = [ordered]@{
    name      = $Name
    titleId   = $titleId
    dllName   = $dllName
    xexMd5    = Get-Md5 $xexIn
    # Extra /D defines for build.ps1, e.g. "ZZ_FPS_DEV_INTERVAL=13220" (docs\RUNTIME.md)
    defines   = @()
}
$info | ConvertTo-Json | Set-Content -Path (Join-Path $dir "game.json") -Encoding utf8
Say ""
Say "Created games\$Name. Next: scripts\recompile.ps1 -Game $Name"
