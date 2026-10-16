# Compile games\<Game>\ppc\ into the game's DLL with the custom clang, in parallel.
#
#   scripts\build.ps1 -Game crackdown [-Jobs 12]
#
# Output: games\<Game>\dll\<xeo3 name>.dll and the identical <xeo3 name>_no.dll (the
# emulator loads both names), plus ppc\game.map for tools\resolve_fault.py.
#
# It REFUSES to build when the result could not be trusted -- each gate below exists
# because skipping it once cost a whole test run:
#   * another build of this game is still running (two compilers racing in one folder
#     both produce "fresh" objects; the link takes whichever won)
#   * the recompiler binary is older than its sources (a stale tool's output compiles fine)
#   * ppc\ is not exactly one recompiler run (a stale file links twice, or runs old code)
#   * zz_save_idx.h predates this run's import table (its indices would be wrong)
#   * any compile error, or any object older than this build
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 2)
)
. "$PSScriptRoot\common.ps1"
$info = Get-GameInfo $Game
$dir = $info.Dir
$ppc = Join-Path $dir "ppc"
$out = Join-Path $dir "dll"
$started = Get-Date
$clang = Get-ClangCl
$vcvars = Get-VcVars
New-Item -ItemType Directory -Force -Path $out | Out-Null

# --- gate: one build per game at a time ---------------------------------------------
$lock = Join-Path $ppc ".build.lock"
if (Test-Path $lock) {
    $old = Get-Content $lock -ErrorAction SilentlyContinue
    if ($old -and (Get-Process -Id ([int]$old) -ErrorAction SilentlyContinue)) {
        Fail "another build of $Game is running (pid $old). If it is an orphan from a cancelled build, end that process (and its clang-cl children) first."
    }
}
Set-Content -Path $lock -Value $PID
try {
    # --- gates: tool fresh, one clean run, save indices current ---------------------
    Assert-ToolsFresh
    $py = Get-Python
    & $py -I (Join-Path $ToolsDir "check_ppc_set.py") $ppc
    if ($LASTEXITCODE -ne 0) { Fail "ppc\ is not one clean recompiler run -- re-run scripts\recompile.ps1" }
    $idx = Get-Item (Join-Path $ppc "zz_save_idx.h") -ErrorAction SilentlyContinue
    $imp = Get-Item (Join-Path $ppc "ppc_import_mapping.cpp")
    if (-not $idx -or $idx.LastWriteTime -lt $imp.LastWriteTime) { Fail "zz_save_idx.h is older than this run's import table -- re-run scripts\recompile.ps1" }
    foreach ($h in "zz_title.h", "zz_switch_hook.h", "zz_import_hook.h", "zz_mmio_hook.h", "ppc_precomp_init.cpp") {
        if (-not (Test-Path (Join-Path $ppc $h))) { Fail "ppc\$h is missing -- re-run scripts\recompile.ps1" }
    }

    Push-Location $ppc
    try {
        Remove-Item *.obj, chunk*.rsp, chunk*.log, chunk*.log.err, game_recomp.dll -ErrorAction SilentlyContinue
        $files = @(Get-ChildItem *.cpp | ForEach-Object { $_.Name })
        $defs = ""
        foreach ($d in @($info.defines)) { if ($d) { $defs += " /D$d" } }
        Say "compiling $($files.Count) files on $Jobs jobs$(if ($defs) { " with$defs" })"

        $chunks = @{}
        for ($i = 0; $i -lt $files.Count; $i++) { $chunks[$i % $Jobs] += @($files[$i]) }
        $procs = @()
        foreach ($k in $chunks.Keys) {
            # The force-included headers opt every source into the runtime: the game's name,
            # the jump-table miss reporter, the import thunks (saves, sign-in, fps meter) and
            # the MMIO hooks. ppc_context.h defaults each of them to the plain behaviour.
            $rsp = "/c /arch:AVX /clang:-Wno-c++17-extensions /Oy /O2$defs /FIzz_title.h /FIzz_switch_hook.h /FIzz_import_hook.h /FIzz_mmio_hook.h " +
                   (($chunks[$k] | ForEach-Object { "`"$_`"" }) -join " ")
            Set-Content -Path "chunk$k.rsp" -Value $rsp -Encoding Ascii
            $procs += Start-Process -FilePath "cmd.exe" -NoNewWindow -PassThru -WorkingDirectory $ppc `
                -ArgumentList "/c", "`"`"$vcvars`" >nul 2>&1 && `"$clang`" @chunk$k.rsp`"" `
                -RedirectStandardOutput "chunk$k.log" -RedirectStandardError "chunk$k.log.err"
        }
        $procs | Wait-Process

        $failed = $false
        foreach ($k in $chunks.Keys) {
            $err = @(Get-Content "chunk$k.log", "chunk$k.log.err" -ErrorAction SilentlyContinue | Where-Object { $_ -match "error:" })
            if ($err.Count) { $failed = $true; Write-Host "== chunk $k errors:" -ForegroundColor Red; $err | Select-Object -First 10 | Out-Host }
        }
        $stale = @()
        foreach ($f in $files) {
            $obj = [System.IO.Path]::ChangeExtension($f, ".obj")
            if (-not (Test-Path $obj)) { $stale += "$f (no object)" }
            elseif ((Get-Item $obj).LastWriteTime -lt $started) { $stale += "$f (object predates this build)" }
        }
        if ($stale.Count) { $failed = $true; Write-Host "== $($stale.Count) missing/stale object(s):" -ForegroundColor Red; $stale | Select-Object -First 15 | Out-Host }
        if ($failed) { Fail "not linking: the compile did not produce a complete, fresh set of objects" }

        Say "linking"
        cmd /c "`"$vcvars`" >nul 2>&1 && `"$clang`" /LD /Fe:game_recomp.dll *.obj /link /MAP:game.map" | Out-Host
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path "game_recomp.dll")) { Fail "link failed" }
        if ((Get-Item "game_recomp.dll").LastWriteTime -lt $started) { Fail "the DLL predates this build" }

        # The artefact, not the log: the four exports the emulator looks up must be there.
        $exports = cmd /c "`"$vcvars`" >nul 2>&1 && dumpbin /nologo /exports game_recomp.dll" | Out-String
        foreach ($e in "InitPrecompiledDll", "CleanupPrecompiledDll", "PrecompiledPointers", "PrecompiledImportTable") {
            if ($exports -notmatch "\b$e\b") { Fail "the DLL does not export $e" }
        }
    } finally { Pop-Location }

    $name = $info.dllName
    Copy-Item (Join-Path $ppc "game_recomp.dll") (Join-Path $out "$name.dll") -Force
    Copy-Item (Join-Path $ppc "game_recomp.dll") (Join-Path $out "${name}_no.dll") -Force
    $md5 = Get-Md5 (Join-Path $out "$name.dll")
    Set-Content -Path (Join-Path $out "build.txt") -Value "$md5  $(Get-Date -Format s)"
    Say ("built  {0}.dll (+ _no.dll)  md5 {1}  {2:N0} bytes  in {3:N1} min" -f $name, $md5,
         (Get-Item (Join-Path $out "$name.dll")).Length, ((Get-Date) - $started).TotalMinutes)
    Say "Next: scripts\install.ps1 -Game $Game -Console <your console folder>"
} finally {
    Remove-Item $lock -ErrorAction SilentlyContinue
}
