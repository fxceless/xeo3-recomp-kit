// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// The FPS METER: zz_swaps is bumped once per VdSwap by the import thunk (zz_import_hook.h),
// and a thread turns it into one line per second in <console folder>\<game>_fps.log:
//     t_ms  frames  fps  total  interval
//
// interval = D3DRS_PRESENTINTERVAL as the game's D3D device holds it right now (0/1 = a
// 60 fps cap, 2 = 30, FFFFFFFF = unknown). The field's offset inside the device depends on
// the XDK the game was built with, so it is only read when the game's build defines
// ZZ_FPS_DEV_INTERVAL (Crackdown, XDK 2.0.4802: 13220 -- found by reading
// SynchronizeToPresentationInterval in the recompiled D3D). The device pointer itself comes
// from VdSetGraphicsInterruptCallback's thunk (D3D passes it as the callback context).
//
// Always on. The rate comes from a measured GetTickCount64 delta, not the nominal
// 1000 ms sleep -- a guest running below real time is exactly when the sleep overshoots.
// Both twins (xeo3_<hash>.dll and _no.dll) run this static init; the file is opened
// only once frames are actually presented, so the idle twin never touches it.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <share.h>

extern "C" volatile long zz_swaps;
volatile long zz_swaps = 0;
extern "C" uintptr_t zz_fps_base;
uintptr_t zz_fps_base = 0;                           // set by VdSwap's thunk
extern "C" volatile unsigned zz_fps_dev;
volatile unsigned zz_fps_dev = 0;                    // set by VdSetGraphicsInterruptCallback's thunk

static ULONGLONG g_fps_t0;

static DWORD WINAPI zz_fps_reporter(LPVOID)
{
    FILE* f = nullptr;
    long last = 0, total = 0;
    ULONGLONG lastT = GetTickCount64();
    for (;;)
    {
        Sleep(1000);
        const long now = zz_swaps;
        const ULONGLONG nowT = GetTickCount64();
        const long d = now - last;
        const ULONGLONG dt = nowT - lastT;
        last = now; lastT = nowT;
        total += d;
        if (total == 0) continue;                    // nothing presented yet: stay quiet
        if (!f)
        {
            f = _fsopen(ZZ_TITLE "_fps.log",
                        "a", _SH_DENYNO);
            if (!f) return 0;
            SYSTEMTIME st; GetLocalTime(&st);
            fprintf(f, "# run %04u-%02u-%02u %02u:%02u:%02u   t_ms  frames  fps  total  interval   (VdSwap counted in the thunk)\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        }
        const uintptr_t gb = zz_fps_base;
        const unsigned dev = zz_fps_dev;
#ifdef ZZ_FPS_DEV_INTERVAL
        const unsigned iv = (gb && dev) ? _byteswap_ulong(*(volatile unsigned*)(gb + dev + ZZ_FPS_DEV_INTERVAL)) : 0xFFFFFFFFu;
#else
        (void)gb; (void)dev;
        const unsigned iv = 0xFFFFFFFFu;
#endif
        fprintf(f, "%8llu %6ld %8.2f %8ld  %08X\n", (unsigned long long)(nowT - g_fps_t0), d,
                dt ? (double)d * 1000.0 / (double)dt : 0.0, total, iv);
        fflush(f);                                   // Emu exits without CRT teardown
    }
}

static int zz_fps_init()
{
    g_fps_t0 = GetTickCount64();
    if (HANDLE h = CreateThread(nullptr, 0, zz_fps_reporter, nullptr, 0, nullptr)) CloseHandle(h);
    return 1;
}
static int g_fps_init = zz_fps_init();
