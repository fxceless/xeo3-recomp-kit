// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Crackdown s3: a lock-free line logger for code that runs in GUEST-FIBER context
// ([[reference_xeo3_no_host_lock_in_guest_context]]: s65's freeze was a CRITICAL_SECTION
// orphaned by a guest fiber). No CRT stream, no lock: each line is formatted on the stack
// and written with ONE WriteFile on a FILE_APPEND_DATA handle, which the kernel appends
// atomically. The file is opened lazily (first line), shared read/write/delete, so it can be
// read -- once the game is closed ([[reference_xeo3_never_tail_the_zz_logs]]) -- and moved
// aside by install_crackdown.ps1.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

struct ZzWLog { const char* path; volatile HANDLE h; volatile LONG state; ULONGLONG t0; };

static inline HANDLE zz_wlog_handle(ZzWLog* L)
{
    if (L->state == 2) return L->h;
    if (InterlockedCompareExchange(&L->state, 1, 0) == 0)
    {
        HANDLE h = CreateFileA(L->path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        L->h = h;
        L->t0 = GetTickCount64();
        InterlockedExchange(&L->state, 2);
        if (h != INVALID_HANDLE_VALUE)
        {
            SYSTEMTIME st; GetLocalTime(&st);
            char b[160];
            int n = _snprintf_s(b, sizeof(b), _TRUNCATE, "# run %04u-%02u-%02u %02u:%02u:%02u pid %lu\r\n",
                                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
            DWORD w; if (n > 0) WriteFile(h, b, (DWORD)n, &w, nullptr);
        }
        return h;
    }
    return INVALID_HANDLE_VALUE;                 // another thread is opening it right now: drop
}

static inline void zz_wlog(ZzWLog* L, const char* fmt, ...)
{
    HANDLE h = zz_wlog_handle(L);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    char b[512];
    int n = _snprintf_s(b, sizeof(b), _TRUNCATE, "%8llu %5lu ",
                        (unsigned long long)(GetTickCount64() - L->t0), GetCurrentThreadId());
    if (n < 0) return;
    va_list ap; va_start(ap, fmt);
    int m = _vsnprintf_s(b + n, sizeof(b) - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)strlen(b + n);
    n += m;
    b[n++] = '\r'; b[n++] = '\n';
    DWORD w; WriteFile(h, b, (DWORD)n, &w, nullptr);
}
