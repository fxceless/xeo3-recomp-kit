# Read the newest emulator crash dump and name the guest function that crashed.
#
#   powershell -File tools\read_crash_dump.ps1 -Game crackdown [-Dump <path>] [-Frames 40]
#
# WHY. When the emulator cannot continue it prints "FATAL ERROR - ..." and ends itself with a
# fast-fail (0xC0000409). A fast-fail never reaches an exception handler, so the runtime's
# fault namer (zz_crash.cpp, <game>_crash.log) records NOTHING. Windows can still write a
# minidump for it -- enable that once (docs\TROUBLESHOOTING.md, "crash dumps") and every crash
# lands in %LOCALAPPDATA%\CrashDumps. This prints the fatal message, then the stack with every
# frame inside the game module symbolised as sub_XXXXXXXX+0xNNN through ppc\game.map.
#
# Needs cdb.exe (Windows SDK -> "Debugging Tools for Windows"). Reads only.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Dump = "",
    [int]$Frames = 40
)
. (Join-Path (Split-Path -Parent $PSScriptRoot) "scripts\common.ps1")
$info = Get-GameInfo $Game
$map = Join-Path $info.Dir "ppc\game.map"
$name = $info.dllName
if (-not (Test-Path $map)) { Fail "no ppc\game.map -- build the game with scripts\build.ps1 first" }
$cdb = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Debuggers\x64\cdb.exe"
if (-not (Test-Path $cdb)) { Fail "cdb.exe not found ($cdb). Install 'Debugging Tools for Windows' from the Windows SDK installer." }

if (-not $Dump) {
    $d = Get-ChildItem "$env:LOCALAPPDATA\CrashDumps\Emu*.dmp" -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $d) { Fail "no Emu*.dmp in $env:LOCALAPPDATA\CrashDumps (crash dumps enabled? docs\TROUBLESHOOTING.md)" }
    $Dump = $d.FullName
}
$di = Get-Item $Dump
Say ("dump: {0}  ({1:N0} bytes, written {2})" -f $di.FullName, $di.Length, $di.LastWriteTime)

$out = & $cdb -z $Dump -c ".ecxr; da @rsp+50 L400; kv $Frames; lm m $name*; q" 2>&1 | Out-String
$tmp = [System.IO.Path]::GetTempFileName()
Set-Content -Path $tmp -Value $out -Encoding utf8
$py = @'
import bisect, re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
name = sys.argv[3]
m = re.search(r"([0-9a-f`]{16,17})\s+[0-9a-f`]{16,17}\s+" + re.escape(name) + r"\s", text, re.I)   # not the idle _no twin
base = int(m.group(1).replace("`", ""), 16) if m else None
syms = []
for ln in open(sys.argv[2], errors="replace"):
    p = ln.split()
    if len(p) >= 3 and p[0].startswith("0001:") and re.fullmatch(r"[0-9a-fA-F]{16}", p[2]):
        syms.append((int(p[2], 16) - 0x180000000, p[1]))
syms.sort()
keys = [s[0] for s in syms]
print("exception / fatal message:")
for ln in text.splitlines():
    if re.search(r"c0000409|FAST_FAIL|Subcode|FATAL|IAR|Unable", ln, re.I):
        print("  " + ln.strip()[:200])
print("stack (frames inside the game module symbolised; module base %s):" % (hex(base) if base else "not loaded?"))
for ln in text.splitlines():
    f = re.match(r"^([0-9a-f]{8}`[0-9a-f]{8}) ([0-9a-f]{8}`[0-9a-f]{8})\s.*?:\s(\S+)\s*$", ln)
    if not f:
        continue
    ret = int(f.group(2).replace("`", ""), 16)
    tag = ""
    if base and base <= ret < base + 0x10000000:
        rva = ret - base
        i = bisect.bisect_right(keys, rva) - 1
        if i >= 0:
            tag = "%s+0x%X" % (syms[i][1], rva - syms[i][0])
    print("  ret %016X  %-44s (frame: %s)" % (ret, tag, f.group(3)[:60]))
'@
$pyf = [System.IO.Path]::GetTempFileName() + ".py"
Set-Content -Path $pyf -Value $py -Encoding utf8
& (Get-Python) -I $pyf $tmp $map $name
Remove-Item $tmp, $pyf -ErrorAction SilentlyContinue

Say ""
Say "Application log (Emu.exe, newest 5):"
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'Application Error' } -MaxEvents 200 -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match 'Emu' } | Select-Object -First 5 |
    ForEach-Object { "  {0}  {1}" -f $_.TimeCreated, (($_.Message -split "`n" | Select-Object -First 6) -join ' | ') }
