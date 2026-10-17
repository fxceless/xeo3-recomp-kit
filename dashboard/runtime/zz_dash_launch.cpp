// xeo3-recomp-kit dashboard runtime -- TITLE LAUNCH from the dashboard: the switcher.
//
// WHAT HAPPENS WITHOUT THIS. "Play Game" makes the dash call
//     XamLoaderLaunchTitleEx(NULL, 0, 0, 2)          (NULL path = the disc)
// and xam/kernel perform a real 360 soft reboot IN-PROCESS: the profile is flushed, every
// handle closed, the dash image unmapped (Emu FreeLibrary's this DLL, hence
// CleanupPrecompiledDll), and the next title is mapped. Emu gets that far -- and then its
// GPU layer dies (c0000005 in VGPUDX12, reading a command-buffer pointer that no longer
// maps) because the GPU side is never reset for a second guest lifetime in one process.
// Back-compat games never soft-reboot, so that path was never exercised by Microsoft.
//
// WHAT WE DO INSTEAD. Emu takes the boot title on its command line:
//     Emu.exe -dvd "<folder>" -titleId <hex> -mediaId <guid>
// so the launch is answered by the host. The request is REMEMBERED when the dash issues it
// (the real import still runs, so xam's orderly teardown happens and the profile is
// flushed), and at CleanupPrecompiledDll -- after the teardown, before the doomed
// in-process reload -- a fresh Emu is started on the requested title from the SAME
// console folder and this process ends. Every title's DLL sits beside the others, one
// identity, one profile: a title switch is a window closing and reopening, which is about
// what a real console does.
//
// The new Emu must not open the virtual hard drive before this process has let go of it
// (a direct child raced us: "Failed to create backing file ... gle=32"), so the start goes
// through <console folder>\zz_switch.exe (dashboard\relay), which waits for our pid to exit
// first -- and stays resident to reopen the dashboard when the game exits.
//
// WHICH TITLE.
//   * "Play Disc" (no path): <console folder>\zz_disc.txt says what is "in the drive":
//         dvd=D:\games\MyGame
//         titleId=4D5307E6
//         mediaId=<guid>
//         args=<anything else, appended verbatim>       (optional)
//   * My Games (installed content): the path carries the title id as a folder,
//     ...\Content\0000000000000000\<titleId>\00007000\..., looked up in
//     <console folder>\zz_titles.txt, one line per game:
//         <titleId>=<disc folder>|<mediaId>|<extra args>
// scripts\add-game-to-my-games.ps1 writes both. A launch neither file answers is left to
// Emu (which then dies in its GPU layer, as above) -- and says so in dash_runtime.log.
//
// THE DASH'S OWN RELAUNCH (marker <console folder>\dash_self_relaunch.on).
// Some dash actions (the avatar scenes) are not title launches:
//     XamLoaderSetLaunchData(buf, 0x3FC)          <- the scene to open, 1020 bytes
//     XamLoaderLaunchTitleEx(NULL, 0, 0, 0)       <- flags 0, NOT the 2 of "Play Game"
// = "reboot into the CURRENT title (the dash) and pick the scene up from the launch data".
// Without the marker any NULL path is "the disc" and such an action boots the game in the
// drive. With it, flags 0 + no path starts this folder's Emu with no arguments (exactly how
// the dash boots), and the 1020-byte block -- which dies with xam's copy in the old
// process -- is carried across in <console folder>\zz_launchdata.bin and replayed into the
// new process's XamLoaderGetLaunchData (zz_dash_imports.cpp). The file is written only on
// that path, read once, deleted, and ignored when older than 15 minutes.
#include "zz_dash_common.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cctype>

namespace
{
    volatile long g_launchRequested;   // set by the import hook, consumed at cleanup
    char g_launchPath[260];            // the guest's path argument, if any
    unsigned int g_launchFlags = 2;    // XamLoaderLaunchTitleEx r6; 2 = "Play Game"

    // the last XamLoaderSetLaunchData block, held in the HOST until the switcher knows
    // whether it is worth writing out. Never written for a game launch.
    unsigned char g_launchData[1024];
    unsigned int  g_launchDataLen;

    // the replay, on the receiving side: read once from the file, then served for the
    // rest of this process (real xam keeps the block for the whole title lifetime, and the
    // dash asks for it more than once per boot).
    unsigned char g_replay[1024];
    unsigned int  g_replayLen;
    int           g_replayState;       // 0 = untried, 1 = have it, -1 = nothing

    bool SelfRelaunchEnabled()
    {
        static int s_on = -1;
        if (s_on < 0)
            s_on = zz_dash_marker("dash_self_relaunch.on");
        return s_on == 1;
    }

    // The carry file: 16-byte header { 'ZZLD', len, flags, tick } then `len` raw bytes.
    struct LaunchDataHeader { unsigned int magic, len, flags, tick; };
    constexpr unsigned int kLaunchDataMagic = 0x5A5A4C44u;      // 'ZZLD'
    constexpr unsigned int kLaunchDataMaxAgeMs = 15u * 60u * 1000u;

    void Trim(char* line)
    {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r' || line[L - 1] == ' ')) line[--L] = 0;
    }

    bool ReadDisc(char* dvd, size_t ndvd, char* title, size_t ntitle, char* media, size_t nmedia, char* extra, size_t nextra)
    {
        char path[MAX_PATH];
        zz_dash_path(path, sizeof(path), "zz_disc.txt");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") != 0 || !f)
            return false;
        dvd[0] = title[0] = media[0] = extra[0] = 0;
        char line[600];
        while (fgets(line, sizeof(line), f))
        {
            Trim(line);
            if (!strncmp(line, "dvd=", 4)) strcpy_s(dvd, ndvd, line + 4);
            else if (!strncmp(line, "titleId=", 8)) strcpy_s(title, ntitle, line + 8);
            else if (!strncmp(line, "mediaId=", 8)) strcpy_s(media, nmedia, line + 8);
            else if (!strncmp(line, "args=", 5)) strcpy_s(extra, nextra, line + 5);
        }
        fclose(f);
        return dvd[0] != 0;
    }

    // `noReturn`: zz_switch normally stays resident and reopens the DASH when the title it
    // started exits -- a console's round trip. A self relaunch must NOT do that: the relay
    // would still be waiting when the new dash later launches a game, and would then start
    // a SECOND console beside it. The token is consumed by the relay, not passed to Emu.
    void StartRelayAndDie(const char* childCmdLine, bool noReturn)
    {
        char relay[MAX_PATH], cwd[MAX_PATH];
        zz_dash_path(relay, sizeof(relay), "zz_switch.exe");
        zz_dash_path(cwd, sizeof(cwd), "");
        if (GetFileAttributesA(relay) == INVALID_FILE_ATTRIBUTES)
        {
            zz_dash_logf("ZZLAUNCH %s is missing (scripts\\install-dashboard.ps1 puts it there) -- leaving the reboot to Emu\n", relay);
            return;
        }

        char cmd[1500];
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "\"%s\" %lu %s%s",
            relay, GetCurrentProcessId(), noReturn ? "-noreturn " : "", childCmdLine);
        zz_dash_logf("ZZLAUNCH switching: %s\n", cmd);

        STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
        // Hand the child our stdout/stderr, so a redirected emulator log keeps flowing.
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
        if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, 0, nullptr, cwd, &si, &pi))
        {
            zz_dash_logf("ZZLAUNCH CreateProcess FAILED gle=%lu\n", GetLastError());
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        zz_dash_log("ZZLAUNCH relay started; ending this process\n");
        // Blunt on purpose: we are inside Emu's unload of this very DLL, and the in-process
        // reload that follows would crash anyway.
        TerminateProcess(GetCurrentProcess(), 0);
    }

    // Reboot into THIS folder's dash: the same exe with no arguments, which is how the
    // dashboard boots (LaunchArguments.txt names it).
    void SelfSwitch()
    {
        char exe[MAX_PATH], child[MAX_PATH + 4];
        GetModuleFileNameA(nullptr, exe, sizeof(exe));
        _snprintf_s(child, sizeof(child), _TRUNCATE, "\"%s\"", exe);
        StartRelayAndDie(child, true);
    }

    // <titleId>=<dvd>|<mediaId>|<extra>  ->  the four fields. Returns false if absent.
    bool LookupTitle(const char* tid, char* dvd, size_t ndvd, char* media, size_t nmedia, char* extra, size_t nextra)
    {
        char path[MAX_PATH];
        zz_dash_path(path, sizeof(path), "zz_titles.txt");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") != 0 || !f)
            return false;
        bool found = false;
        char line[800];
        while (!found && fgets(line, sizeof(line), f))
        {
            Trim(line);
            if (_strnicmp(line, tid, 8) != 0 || line[8] != '=') continue;
            char* a = line + 9;
            char* b = strchr(a, '|'); if (b) *b++ = 0;
            char* c = b ? strchr(b, '|') : nullptr; if (c) *c++ = 0;
            strcpy_s(dvd, ndvd, a);
            strcpy_s(media, nmedia, b ? b : "");
            strcpy_s(extra, nextra, c ? c : "");
            found = true;
        }
        fclose(f);
        return found;
    }
}

// Called by the import layer when the dash asks xam to launch a title. `path` is the
// guest's path argument as ANSI (may be empty = the disc).
extern "C" void zz_dash_launch_request(const char* path, unsigned int flags)
{
    strncpy_s(g_launchPath, sizeof(g_launchPath), path ? path : "", _TRUNCATE);
    g_launchFlags = flags;
    InterlockedExchange(&g_launchRequested, 1);
    const bool self = SelfRelaunchEnabled() && flags != 2u && g_launchPath[0] == 0;
    zz_dash_logf("ZZLAUNCH request remembered: \"%s\" flags=%08X -> %s -- will switch at cleanup\n",
                 g_launchPath, flags, self ? "the dashboard itself" : "the disc/a title");
}

// XamLoaderSetLaunchData(buf, len), held until the switcher knows what kind of launch follows.
extern "C" void zz_dash_launch_save_data(const void* bytes, unsigned int len)
{
    if (len > sizeof(g_launchData))
        len = sizeof(g_launchData);
    memcpy(g_launchData, bytes, len);
    g_launchDataLen = len;
    zz_dash_logf("ZZLAUNCH SetLaunchData held %u B\n", len);
}

// The receiving side: reads <console folder>\zz_launchdata.bin once, deletes it, and then
// serves the same bytes for the rest of this process. Returns the length, or 0.
extern "C" unsigned int zz_dash_launch_replay_len()
{
    if (g_replayState != 0)
        return g_replayState > 0 ? g_replayLen : 0u;
    g_replayState = -1;
    if (!SelfRelaunchEnabled())
        return 0;

    char path[MAX_PATH];
    zz_dash_path(path, sizeof(path), "zz_launchdata.bin");
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    LaunchDataHeader hdr = {};
    DWORD got = 0;
    FILETIME ftw = {}, ftnow = {};
    bool ok = ReadFile(h, &hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr)
              && hdr.magic == kLaunchDataMagic && hdr.len > 0 && hdr.len <= sizeof(g_replay)
              && GetFileTime(h, nullptr, nullptr, &ftw);
    if (ok)
        ok = ReadFile(h, g_replay, hdr.len, &got, nullptr) && got == hdr.len;
    CloseHandle(h);
    DeleteFileA(path);              // one file, one boot, either way
    if (!ok)
    {
        zz_dash_log("ZZLAUNCH launch data present but unusable (bad header or short read) -- deleted\n");
        return 0;
    }

    // freshness: a file left over from an earlier session must not open a stale scene
    GetSystemTimeAsFileTime(&ftnow);
    ULARGE_INTEGER a, b;
    a.LowPart = ftw.dwLowDateTime;   a.HighPart = ftw.dwHighDateTime;
    b.LowPart = ftnow.dwLowDateTime; b.HighPart = ftnow.dwHighDateTime;
    unsigned long long ageMs = (b.QuadPart > a.QuadPart) ? (b.QuadPart - a.QuadPart) / 10000ull : 0ull;
    if (ageMs > kLaunchDataMaxAgeMs)
    {
        zz_dash_logf("ZZLAUNCH launch data is %llu ms old -- discarded\n", ageMs);
        return 0;
    }
    g_replayLen = hdr.len;
    g_replayState = 1;
    zz_dash_logf("ZZLAUNCH replaying launch data (%u B, %llu ms old)\n", hdr.len, ageMs);
    return g_replayLen;
}

// Copies min(cb, len) replayed bytes out. Call zz_dash_launch_replay_len() first.
extern "C" unsigned int zz_dash_launch_replay_copy(void* out, unsigned int cb)
{
    if (g_replayState != 1)
        return 0;
    unsigned int n = cb < g_replayLen ? cb : g_replayLen;
    memcpy(out, g_replay, n);
    return n;
}

// MY GAMES: a launch of INSTALLED content, XamContentLaunchImageInternalEx(XCONTENT_DATA*,
// "default.xex", 0, -1) -- no path, just {DeviceID, ContentType, DisplayName, FileName}.
// Find the package under an identity's virtual hard drive, take the title id from its
// folder, and if zz_titles.txt knows that title, remember it as the content path
// zz_dash_launch_switch() understands. Returns 1 when the title is mapped.
extern "C" int zz_dash_launch_resolve_content(unsigned int contentType, const char* fileName)
{
    char titles[MAX_PATH];
    zz_dash_path(titles, sizeof(titles), "zz_titles.txt");
    FILE* f = nullptr;
    if (fopen_s(&f, titles, "r") != 0 || !f)
    {
        zz_dash_log("ZZLAUNCH no zz_titles.txt -- My Games launch left to xam\n");
        return 0;
    }
    char local[MAX_PATH] = {0};
    GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local));
    int hit = 0;
    char line[800];
    while (!hit && fgets(line, sizeof(line), f))
    {
        if (strlen(line) < 9 || line[8] != '=') continue;
        char tid[9]; memcpy(tid, line, 8); tid[8] = 0;
        // every back-compat identity: %LOCALAPPDATA%\Xbox360BackwardCompatibil.<identity>\VirtualDrive
        char pat[MAX_PATH];
        _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\Xbox360BackwardCompatibil.*", local);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            char pkg[MAX_PATH];
            _snprintf_s(pkg, sizeof(pkg), _TRUNCATE,
                "%s\\%s\\VirtualDrive\\Harddisk\\Partition1\\Content\\0000000000000000\\%s\\%08X\\%s",
                local, fd.cFileName, tid, contentType, fileName);
            if (GetFileAttributesA(pkg) != INVALID_FILE_ATTRIBUTES)
            {
                char pseudo[200];
                _snprintf_s(pseudo, sizeof(pseudo), _TRUNCATE,
                    "\\Content\\0000000000000000\\%s\\%08X\\%s", tid, contentType, fileName);
                zz_dash_logf("ZZLAUNCH My Games package %s (identity %s) -> title %s\n", fileName, fd.cFileName, tid);
                zz_dash_launch_request(pseudo, 2u);
                hit = 1;
                break;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    fclose(f);
    if (!hit)
        zz_dash_logf("ZZLAUNCH My Games package ctype %08X \"%s\" is not in zz_titles.txt -- left to xam\n",
                     contentType, fileName);
    return hit;
}

// Called from CleanupPrecompiledDll, i.e. after xam's teardown of the dashboard.
extern "C" void zz_dash_launch_switch()
{
    if (!g_launchRequested)
        return;

    char dataFile[MAX_PATH];
    zz_dash_path(dataFile, sizeof(dataFile), "zz_launchdata.bin");
    const bool self = SelfRelaunchEnabled() && g_launchFlags != 2u && g_launchPath[0] == 0;
    if (self)
    {
        // carry the SetLaunchData block across; if this launch set none, make sure an older
        // one cannot be picked up by the new process
        if (g_launchDataLen)
        {
            LaunchDataHeader hdr = { kLaunchDataMagic, g_launchDataLen, g_launchFlags, GetTickCount() };
            HANDLE h = CreateFileA(dataFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
                zz_dash_logf("ZZLAUNCH could not write %s gle=%lu -- relaunching without the scene\n", dataFile, GetLastError());
            else
            {
                DWORD wrote = 0;
                WriteFile(h, &hdr, sizeof(hdr), &wrote, nullptr);
                WriteFile(h, g_launchData, g_launchDataLen, &wrote, nullptr);
                CloseHandle(h);
                zz_dash_logf("ZZLAUNCH saved launch data: %u B\n", g_launchDataLen);
            }
        }
        else
            DeleteFileA(dataFile);
        SelfSwitch();
        return;                                    // SelfSwitch never returns on success
    }
    DeleteFileA(dataFile);   // a title launch must never leave a scene behind for the dash

    // A path naming installed content carries the title id as a folder
    // (...\Content\0000000000000000\<titleId>\00007000\...): look it up in zz_titles.txt.
    // Anything else (NULL, \Device\Cdrom0, game:, d:) is the disc, i.e. zz_disc.txt.
    char dvd[MAX_PATH] = {0}, title[64] = {0}, media[64] = {0}, extra[300] = {0};
    char tid[9] = {0};
    for (const char* q = g_launchPath; *q; q++)
    {
        if (q[0] == '\\' && strlen(q) > 9 && q[9] == '\\')
        {
            bool hex = true;
            for (int i = 1; i <= 8 && hex; i++) hex = isxdigit((unsigned char)q[i]) != 0;
            if (hex && _strnicmp(q + 1, "00000000", 8) != 0) { memcpy(tid, q + 1, 8); break; }
        }
    }
    if (tid[0])
    {
        bool found = LookupTitle(tid, dvd, sizeof(dvd), media, sizeof(media), extra, sizeof(extra));
        zz_dash_logf("ZZLAUNCH title %s from path \"%s\": %s\n", tid, g_launchPath,
                     found ? dvd : "NOT in zz_titles.txt -- leaving it to Emu");
        if (!found) return;
        strcpy_s(title, sizeof(title), tid);
    }
    else if (!ReadDisc(dvd, sizeof(dvd), title, sizeof(title), media, sizeof(media), extra, sizeof(extra)))
    {
        zz_dash_log("ZZLAUNCH Play Disc with no zz_disc.txt -- leaving the reboot to Emu\n");
        return;
    }

    char exe[MAX_PATH], child[1400];
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    _snprintf_s(child, sizeof(child), _TRUNCATE, "\"%s\" -dvd \"%s\"%s%s%s%s%s%s",
        exe, dvd,
        title[0] ? " -titleId " : "", title,
        media[0] ? " -mediaId " : "", media,
        extra[0] ? " " : "", extra);
    StartRelayAndDie(child, false);
}
