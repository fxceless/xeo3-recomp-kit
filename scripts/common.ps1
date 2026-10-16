# Shared by every script in scripts\. Dot-source it:  . "$PSScriptRoot\common.ps1"
#
# Paths are found, never typed: the kit root is this file's parent's parent, the custom
# clang comes from $env:XEO3_CLANG_CL or toolchain\clang-xeo3\, and Visual Studio's
# vcvars64.bat is located with vswhere.

$ErrorActionPreference = "Stop"
$KitRoot   = Split-Path -Parent $PSScriptRoot
$GamesRoot = Join-Path $KitRoot "games"
$Recompiler = Join-Path $KitRoot "recompiler"
$XenonRecompExe  = Join-Path $Recompiler "build\XenonRecomp\XenonRecomp.exe"
$XenonAnalyseExe = Join-Path $Recompiler "build\XenonAnalyse\XenonAnalyse.exe"
$ToolsDir   = Join-Path $KitRoot "tools"
$RuntimeDir = Join-Path $KitRoot "runtime"

function Fail([string]$msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

function Say([string]$msg) { Write-Host $msg }

function Get-Python {
    foreach ($n in "python", "py") {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { return $c.Source }
    }
    Fail "Python 3 was not found on PATH. Install it from python.org (tick 'Add to PATH')."
}

# Run one of the kit's Python tools in isolated mode (-I): never picks up a stray module
# from the current directory.
function Invoke-Tool([string]$tool, [string[]]$toolArgs) {
    $py = Get-Python
    & $py -I (Join-Path $ToolsDir $tool) @toolArgs
    if ($LASTEXITCODE -ne 0) { Fail "$tool failed (exit $LASTEXITCODE)" }
}

function Get-VcVars {
    # XEO3_VCVARS picks a specific Visual Studio when several are installed.
    if ($env:XEO3_VCVARS) { if (Test-Path $env:XEO3_VCVARS) { return $env:XEO3_VCVARS }; Fail "XEO3_VCVARS points at a missing file: $env:XEO3_VCVARS" }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $inst = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($inst) {
            $bat = Join-Path $inst "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $bat) { return $bat }
        }
    }
    Fail "Visual Studio 2022 (or Build Tools) with the 'Desktop development with C++' workload was not found."
}

function Get-ClangCl {
    $c = $env:XEO3_CLANG_CL
    if (-not $c) { $c = Join-Path $KitRoot "toolchain\clang-xeo3\bin\clang-cl.exe" }
    if (-not (Test-Path $c)) {
        Fail ("The custom clang was not found at $c.`n" +
              "  Download WitherOrNot's LLVM build (see docs\SETUP.md) and extract it so that`n" +
              "  toolchain\clang-xeo3\bin\clang-cl.exe exists, or set XEO3_CLANG_CL to its path.")
    }
    return $c
}

# A command line run inside the Visual Studio x64 environment.
function Invoke-InVcEnv([string]$cmdline, [string]$workdir = $KitRoot) {
    $vc = Get-VcVars
    Push-Location $workdir
    try { cmd /c "`"$vc`" >nul 2>&1 && $cmdline" | Out-Host; return $LASTEXITCODE }
    finally { Pop-Location }
}

function Test-GameName([string]$name) {
    if ($name -notmatch '^[a-z0-9_]{1,16}$') {
        Fail "Game names are 1-16 characters of a-z, 0-9 and _ (got '$name'). They prefix log and marker files."
    }
}

function Get-GameDir([string]$name) {
    Test-GameName $name
    $d = Join-Path $GamesRoot $name
    if (-not (Test-Path (Join-Path $d "game.json"))) { Fail "No game called '$name' (run scripts\new-game.ps1 first)." }
    return $d
}

function Get-GameInfo([string]$name) {
    $d = Get-GameDir $name
    $info = Get-Content (Join-Path $d "game.json") -Raw | ConvertFrom-Json
    $info | Add-Member -NotePropertyName Dir -NotePropertyValue $d -Force
    return $info
}

# Refuse to use a recompiler binary older than its own sources: a stale XenonRecomp.exe
# once silently emitted the pre-fix form of every setjmp/longjmp in a game. A build is only
# as trustworthy as the tool that generated its input.
function Assert-ToolsFresh {
    # The sources compiled INTO each tool. (XenonUtils\ppc_context.h is not one: XenonRecomp
    # reads it at run time and copies it into ppc\, so recompile.ps1 picks up its changes.)
    $checks = @(
        @{ exe = $XenonRecompExe;  dirs = @("XenonRecomp", "XenonUtils") },
        @{ exe = $XenonAnalyseExe; dirs = @("XenonAnalyse", "XenonUtils") }
    )
    foreach ($t in $checks) {
        if (-not (Test-Path $t.exe)) { Fail "$(Split-Path $t.exe -Leaf) is not built yet. Run scripts\setup.ps1." }
        $built = (Get-Item $t.exe).LastWriteTime
        foreach ($d in $t.dirs) {
            $newer = Get-ChildItem (Join-Path $Recompiler $d) -Recurse -Include *.cpp, *.c |
                     Where-Object { $_.LastWriteTime -gt $built } | Select-Object -First 1
            if ($newer) { Fail "$(Split-Path $t.exe -Leaf) is older than $($newer.Name). Rebuild it: scripts\setup.ps1" }
        }
    }
}

function Get-Md5([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm MD5).Hash.ToLower() }
