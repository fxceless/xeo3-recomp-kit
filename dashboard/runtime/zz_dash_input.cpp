// xeo3-recomp-kit dashboard runtime -- THE GUIDE BUTTON.
//
// Emu answers the controller APIs on the host, so the guest input driver -- where xam
// would notice the Xbox button and raise the Guide -- never sees it. hud.xex/huduiskin.xex
// ARE loaded next to every title, and xam exports XamShowGuideUI (it posts a message to
// the system-UI app for a user index). So: read the Guide bit from the host (XInput
// ordinal 100, "XInputGetStateEx"; Back+Start held together is the fallback for pads and
// drivers that hide it), edge-detect it here, and let the import layer call
// XamShowGuideUI(0) on the guest UI thread at its next keystroke poll (zz_dash_hook.h).
//
// Armed by <console folder>\dash_guide.on, which scripts\install-dashboard.ps1 creates only
// when the console folder's xam is the build whose XamShowGuideUI address the hook calls.
//
// On Windows, Steam (Guide button chord) and the Xbox Game Bar may grab the button first;
// turn those off for the controller if the Guide never opens.
//
// NEVER poll XInput from a guest thread: XInput 1.4 is COM underneath, and a port of this
// that polled from a game's render thread died inside combase.dll. ONE host thread of ours
// polls; the guest side only reads a flag.
#include "zz_dash_common.h"
#include <windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")

namespace
{
    // XINPUT_GAMEPAD / XINPUT_STATE, declared here so the file needs no SDK header.
    struct XiGamepad
    {
        WORD wButtons;
        BYTE bLeftTrigger;
        BYTE bRightTrigger;
        SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
    };
    struct XiState
    {
        DWORD dwPacketNumber;
        XiGamepad Gamepad;
    };
    typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XiState*);

    constexpr WORD XI_START = 0x0010, XI_BACK = 0x0020, XI_GUIDE = 0x0400;

    volatile long g_pending;
    volatile long g_stop;
    HANDLE g_thread;
    int g_enabled = -1;

    PFN_XInputGetState LoadXInput()
    {
        const char* dlls[] = { "XInput1_4.dll", "XInput1_3.dll", "XInput9_1_0.dll" };
        for (const char* name : dlls)
        {
            HMODULE h = LoadLibraryA(name);
            if (!h)
                continue;
            // undocumented but stable since XInput 1.3: ordinal 100 reports the Guide button
            auto ex = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(h, reinterpret_cast<LPCSTR>(100)));
            auto plain = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(h, "XInputGetState"));
            if (ex || plain)
                return ex ? ex : plain;
        }
        return nullptr;
    }

    DWORD WINAPI GuideThread(void*)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        PFN_XInputGetState get = LoadXInput();
        if (!get)
        {
            zz_dash_log("ZZGUIDE no XInput on this PC -- Guide button off\n");
            return 0;
        }
        bool held = false;
        while (!g_stop)
        {
            Sleep(16);
            bool down = false;
            for (DWORD i = 0; i < 4 && !down; i++)
            {
                XiState st{};
                if (get(i, &st) != ERROR_SUCCESS)
                    continue;
                WORD b = st.Gamepad.wButtons;
                down = (b & XI_GUIDE) != 0 || ((b & (XI_BACK | XI_START)) == (XI_BACK | XI_START));
            }
            if (down && !held)
                InterlockedExchange(&g_pending, 1);
            held = down;
        }
        return 0;
    }
}

// From the keystroke poll's wrap path; starts the host poll thread once.
extern "C" void zz_dash_guide_poll()
{
    static volatile long started;
    if (started || InterlockedCompareExchange(&started, 1, 0) != 0)
        return;
    if (g_enabled < 0)
        g_enabled = zz_dash_marker("dash_guide.on");
    if (!g_enabled)
    {
        zz_dash_log("ZZGUIDE dash_guide.on absent -- Guide button off\n");
        return;
    }
    g_thread = CreateThread(nullptr, 0, GuideThread, nullptr, 0, nullptr);
}

// CleanupPrecompiledDll: the thread must be gone before the DLL is unmapped.
extern "C" void zz_dash_guide_shutdown()
{
    InterlockedExchange(&g_stop, 1);
    if (g_thread)
    {
        WaitForSingleObject(g_thread, 500);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

// Import layer: returns 1 exactly once per press; the caller then raises the Guide.
extern "C" int zz_dash_guide_take()
{
    if (!g_pending || InterlockedExchange(&g_pending, 0) == 0)
        return 0;
    zz_dash_log("ZZGUIDE Xbox button -> XamShowGuideUI(0)\n");
    return 1;
}
