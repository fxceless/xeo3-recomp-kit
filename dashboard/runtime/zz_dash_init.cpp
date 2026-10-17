// xeo3-recomp-kit dashboard runtime -- start and stop, and the shared helpers
// (zz_dash_common.h).
//
// scripts\recompile-dashboard.ps1 writes ppc\ppc_precomp_init.cpp from the recompiler's
// one (recompiler\XenonUtils\ppc_precomp_init.cpp) plus two calls: zz_dash_init() at the
// end of InitPrecompiledDll and zz_dash_cleanup() in CleanupPrecompiledDll.
//
// CleanupPrecompiledDll matters here in a way it never does for a game: Emu calls it before
// FreeLibrary when the dashboard launches a title (the 360 soft reboot unmaps the dash).
// Every host-side thing this DLL started must be undone first -- a thread or hook left
// behind runs in unmapped code and takes the process down -- and THEN the switcher starts
// the requested title in a new Emu (zz_dash_launch.cpp), which does not return.
#include "zz_dash_common.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>

extern "C" void zz_dash_vgpu_init();
extern "C" void zz_dash_guide_shutdown();
extern "C" void zz_dash_launch_switch();

extern "C" void zz_dash_path(char* out, size_t n, const char* name)
{
    out[0] = 0;
    DWORD len = GetModuleFileNameA(nullptr, out, (DWORD)n);
    if (!len || len >= n) { out[0] = 0; return; }
    char* p = strrchr(out, '\\');
    if (p) p[1] = 0; else out[0] = 0;
    strcat_s(out, n, name);
}

extern "C" int zz_dash_marker(const char* name)
{
    char p[MAX_PATH];
    zz_dash_path(p, sizeof(p), name);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
}

extern "C" void zz_dash_log(const char* line)
{
    OutputDebugStringA(line);
    static char s_path[MAX_PATH];
    if (!s_path[0])
        zz_dash_path(s_path, sizeof(s_path), "dash_runtime.log");
    // Opened per line and shared, so the log can be read while the dashboard runs (an
    // exclusive open from a viewer made earlier versions go silent).
    FILE* f = _fsopen(s_path, "a", _SH_DENYNO);
    if (!f)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02u:%02u:%02u.%03u %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
    fclose(f);
}

extern "C" void zz_dash_logf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    zz_dash_log(buf);
}

extern "C" void zz_dash_init()
{
    zz_dash_logf("---- dashboard runtime up (pid %lu) ----\n", GetCurrentProcessId());
    zz_dash_vgpu_init();
}

extern "C" void zz_dash_cleanup()
{
    zz_dash_guide_shutdown();   // the pad-poll thread must be gone before the unmap
    zz_dash_launch_switch();    // starts the requested title and ends this process
}
