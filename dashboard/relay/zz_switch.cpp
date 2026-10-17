// xeo3-recomp-kit dashboard -- zz_switch.exe, the title-switch relay.
// Built and copied into the console folder by scripts\install-dashboard.ps1.
//
//   zz_switch.exe <parentPid> [-noreturn] <command line to run>
//
// The dashboard's runtime (dashboard\runtime\zz_dash_launch.cpp) starts this when the dash
// launches a title, then ends its own Emu. The relay waits until that Emu has fully exited
// -- so its virtual hard drive files are closed; a child started directly raced it and died
// with "Failed to create backing file ... (gle=32)" -- then starts the given command line
// in the console folder, handing over stdin/stdout/stderr so a redirected log keeps flowing.
//
// BACK TO THE DASHBOARD. It then stays resident until the game's Emu exits -- the game's
// own "quit to dashboard", the Guide's return to the dashboard, or the window being closed
// all end that process -- and starts the same Emu with NO arguments, which boots the
// console folder's own title = the dashboard, on the same identity and profile. A title
// switch is a round trip, like a console. <console folder>\dash_no_return.on disables it.
//
// -noreturn: the dash relaunching ITSELF (some dash scenes reboot into the current title).
// The new process IS the dashboard; staying resident would start a SECOND console the next
// time it launches a game. Start the child and let go.
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: zz_switch.exe <parentPid> [-noreturn] <command line>\n");
        return 2;
    }
    DWORD pid = (DWORD)strtoul(argv[1], nullptr, 10);
    const bool noReturn = (_stricmp(argv[2], "-noreturn") == 0);
    if (noReturn && argc < 4)
    {
        fprintf(stderr, "usage: zz_switch.exe <parentPid> [-noreturn] <command line>\n");
        return 2;
    }
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h)
    {
        WaitForSingleObject(h, 30000);
        CloseHandle(h);
    }
    // a little slack for the OS to release the file handles
    Sleep(300);

    // rebuild the command line from the raw one: everything after the pid token (and after
    // the optional -noreturn token, which is ours and must not reach Emu)
    char* cl = GetCommandLineA();
    char* p = cl;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; } else { while (*p && *p != ' ') p++; }
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (noReturn) { while (*p && *p != ' ') p++; while (*p == ' ') p++; }
    char cmd[4096];
    strncpy_s(cmd, sizeof(cmd), p, _TRUNCATE);

    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
    {
        fprintf(stderr, "zz_switch: CreateProcess failed gle=%lu for: %s\n", GetLastError(), cmd);
        return 1;
    }
    CloseHandle(pi.hThread);
    if (noReturn)
    {
        CloseHandle(pi.hProcess);
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    if (GetFileAttributesA("dash_no_return.on") != INVALID_FILE_ATTRIBUTES)
        return 0;
    Sleep(500);
    // the exe = first (quoted) token of the command line we just ran
    char exe[MAX_PATH];
    {
        const char* s = cmd; char* d = exe;
        if (*s == '"') { s++; while (*s && *s != '"' && d < exe + MAX_PATH - 1) *d++ = *s++; }
        else { while (*s && *s != ' ' && d < exe + MAX_PATH - 1) *d++ = *s++; }
        *d = 0;
    }
    fprintf(stderr, "zz_switch: title exited -- returning to the dashboard: %s\n", exe);
    char dashcmd[MAX_PATH + 4];
    _snprintf_s(dashcmd, sizeof(dashcmd), _TRUNCATE, "\"%s\"", exe);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(nullptr, dashcmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
    {
        fprintf(stderr, "zz_switch: dashboard CreateProcess failed gle=%lu\n", GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
