# One-time setup: checks every prerequisite and builds the recompiler (XenonRecomp +
# XenonAnalyse) from recompiler\ with CMake + Ninja inside the Visual Studio environment.
#
#   powershell -ExecutionPolicy Bypass -File scripts\setup.ps1
#
# Re-run it after pulling changes to recompiler\ -- the other scripts refuse to run with a
# recompiler binary older than its sources.
. "$PSScriptRoot\common.ps1"

Say "== prerequisites"
$py = Get-Python
Say ("  python        " + (& $py --version 2>&1))
$vc = Get-VcVars
Say "  vcvars64      $vc"

foreach ($tool in "cmake", "ninja") {
    $found = Invoke-InVcEnv "where $tool >nul 2>&1"
    if ($found -ne 0) {
        Fail "$tool was not found. In the Visual Studio Installer add 'C++ CMake tools for Windows' (it brings both)."
    }
}
Say "  cmake + ninja found (Visual Studio environment)"

$clang = Get-ClangCl
$ver = (& $clang --version 2>&1 | Out-String)
if ($ver -notmatch "WitherOrNot") {
    Write-Host "  WARNING: $clang does not identify as WitherOrNot's LLVM build." -ForegroundColor Yellow
    Write-Host "  A stock clang compiles the game but the DLL crashes: the recompiled code needs the" -ForegroundColor Yellow
    Write-Host "  emulator's calling convention, which only that build provides. See docs\SETUP.md." -ForegroundColor Yellow
} else {
    Say ("  clang-cl      " + (($ver -split "`n")[0]).Trim())
}

Say "== third-party sources (git submodules)"
$need = @("fmt", "libmspack", "tiny-AES-c", "tomlplusplus", "xxHash") |
        Where-Object { -not (Get-ChildItem (Join-Path $Recompiler "thirdparty\$_") -ErrorAction SilentlyContinue) }
if ($need) {
    Say "  fetching: $($need -join ', ')"
    git -C $KitRoot submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { Fail "git submodule update failed. Clone the kit with git (not a ZIP download) so the submodules can be fetched." }
} else { Say "  all present" }

Say "== building the recompiler (Release, a few minutes the first time)"
$build = Join-Path $Recompiler "build"
$rc = Invoke-InVcEnv "cmake -S `"$Recompiler`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl"
if ($rc -ne 0) { Fail "cmake configure failed" }
$rc = Invoke-InVcEnv "cmake --build `"$build`" --target XenonRecomp XenonAnalyse XexRepack"
if ($rc -ne 0) { Fail "recompiler build failed" }
Assert-ToolsFresh
Say "  $XenonRecompExe"
Say "  $XenonAnalyseExe"
Say "  $(Join-Path $Recompiler 'build\XexRepack\XexRepack.exe')"
Say ""
Say "Setup complete. Next: scripts\new-game.ps1 -Name <shortname> -Xex <path to the game's default.xex>"
