// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Crackdown s1 (2026-10-14): LOCAL SAVES -- a local player and save data in a folder on
// the PC, no profile sign-in. Ported from NFS's work\ppc\zz_profile.cpp (s12 pt3 fake
// user, s12 pt4 / s25 / s25b save layer), the part of it that is identity + storage;
// none of its tracing, XMA, PM4 or fps machinery.
//
// WHY. Booted with -dvd (no dashboard) nobody signs a profile in, and a sign-in made
// while this title runs fails inside xam (measured: XamUserLogonEx -> extended error
// 0x80070002 -- xam wants this title's 4D5307DC.gpd in the profile, which does not
// exist; the dashboard escapes it because title FFFE07D1 is special-cased). The user's
// call: a separate recomp needs no profile; save to the PC like NFS did; real profiles
// come back through the dashboard later.
//
// WHAT IS ANSWERED HERE (everything else still goes to real xam / the real kernel):
//   identity  XamUserGetSigninState(0)=1 (signed in locally), GetXUID, GetName,
//             CheckPrivilege=FALSE, XamShowSigninUI=success-without-UI
//   storage   XamShowDeviceSelectorUI -> our device 1 (IO_PENDING, as NFS measured the
//             caller requires), GetDeviceState healthy, CreateEnumerator/XamEnumerate =
//             a scan of the save folder, ContentCreateEx = "mount" (remember the root),
//             ContentClose, ContentDelete
//   profile   (s2) XamUserReadProfileSettings = the settings' default values, completed at
//             once; XamUserWriteProfileSettings = logged, completed. Forwarding the read to a
//             real xam with no user was the "signing into gaming profile" hang: sub_82A8ED90
//             polls the read's XOVERLAPPED every tick and xam never completed it.
//   files     Nt* calls on "<root>:\..." go to <save dir>\<package>\... through host
//             stdio, because Emu's VirtualDrive has no write path (NFS s12 pt4: guest
//             writes through the real kernel return ACCESS_DENIED). Our own handles
//             (0x5A5Bxxxx files, 0x5A5Cxxxx dirs, 0x5A5A00xx enumerators) never reach
//             the real kernel.
// Save folder: "<-dvd folder>\SAVES" (NFS s36 rule: it
// follows the disc, not the cwd). One sub-folder per package + a <package>.xcd sidecar
// holding the game's own XCONTENT_DATA, so the enumerator reports it back next boot.
//
// Kill switch: <console folder>\<game>_localsaves.off (then every call goes to real xam).
// Log: <console folder>\<game>_saves.log -- one line per answered call.
//
// The local player's XUID and gamertag come from <console folder>\xeo3_profile.txt:
//     xuid=E000000000000001
//     gamertag=Player1
// (both optional; those are the defaults). Use the XUID of a profile that exists in your
// console folder if you want the dashboard and the game to agree on who is playing.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <io.h>
#include <share.h>
#include <errno.h>
#include "ppc_context.h"
#include "zz_save_idx.h"

extern "C" unsigned char zz_save_claim[ZZ_SAVE_N_IMPORTS];
unsigned char zz_save_claim[ZZ_SAVE_N_IMPORTS];
// OBSERVE-ONLY imports (run 2 of local saves, "stuck on signing into gaming profile"):
// the real import still runs; the thunk logs its arguments and real xam's answer.
extern "C" unsigned char zz_obs_claim[ZZ_SAVE_N_IMPORTS];
unsigned char zz_obs_claim[ZZ_SAVE_N_IMPORTS];
static volatile LONG g_obs_n[ZZ_SAVE_N_IMPORTS];
// s2: observe is an instrument for a wall, not a default. It takes the wrap path through
// real xam (the one delicate mechanism in the thunk), so it arms only on
// <console folder>\<game>_observe.on and never rides along on a launch that tests a fix.
static bool g_obs_armed;

// The local player (see the header). Loaded once by zz_profile_load(), shared with the
// auto-logon in zz_import.cpp through zz_profile_xuid().
static unsigned long long g_xuid = 0xE000000000000001ull;
static char g_gamertag[16] = "Player1";
#define ZZ_XUID      g_xuid
#define ZZ_GAMERTAG  g_gamertag
#define ZZ_DEV_ID    1
#define ZZ_DEV_TOTAL (2048ull * 1024 * 1024)
#define ZZ_DEV_FREE  (1024ull * 1024 * 1024)
#define ZZ_XCD_SIZE  0x134u
#define ZZ_XCD_EXT   ".xcd"
#define ZZ_ENUM_HANDLE 0x5A5A0001u
#define ZZ_ENUM_N    4
#define ZZ_ENUM_MAX  16
#define ZZ_FH_BASE   0x5A5B0000u
#define ZZ_FH_N      8
#define ZZ_DH_BASE   0x5A5C0000u
#define ZZ_DH_N      4
#define ZZ_NO_MORE_FILES 18u
#define ZZ_OVL_LOW   0x00
#define ZZ_OVL_HIGH  0x04
#define ZZ_OVL_ERR   0x18

static CRITICAL_SECTION g_lock;
static FILE* g_fp;
static char  g_dir[MAX_PATH] = "Game\\SAVES";
static char  g_root[32];        // the root ContentCreate was asked to mount
static char  g_key[64];         // the package most recently mounted
static ULONGLONG g_t0;

struct ZzFile { unsigned h; FILE* f; };
struct ZzDir  { unsigned h; char host[MAX_PATH]; unsigned cur; };
struct ZzEnum { unsigned h, n, cur; unsigned char item[ZZ_ENUM_MAX][ZZ_XCD_SIZE]; };
static ZzFile g_files[ZZ_FH_N];
static ZzDir  g_dirs[ZZ_DH_N];
static ZzEnum g_enums[ZZ_ENUM_N];

static void zz_log(const char* fmt, ...)
{
    if (!g_fp) return;
    va_list ap; va_start(ap, fmt);
    EnterCriticalSection(&g_lock);
    fprintf(g_fp, "%7llu ", (unsigned long long)(GetTickCount64() - g_t0));
    vfprintf(g_fp, fmt, ap);
    fputc('\n', g_fp);
    fflush(g_fp);
    LeaveCriticalSection(&g_lock);
    va_end(ap);
}

static const char* g_obs_want[48]; static int g_obs_nwant;
static void zz_obs_want(const char* n) { if (g_obs_nwant < 48) g_obs_want[g_obs_nwant++] = n; }

// Called from every thunk's first execution path that is not claimed: the thunk passes
// its own name, so the observe table needs no index file. Returns 1 to observe.
extern "C" int zz_obs_match(unsigned idx, const char* name)
{
    if (idx >= ZZ_SAVE_N_IMPORTS) return 0;
    if (!g_obs_armed) { zz_obs_claim[idx] = 2; return 0; }
    for (int k = 0; k < g_obs_nwant; k++) if (!strcmp(g_obs_want[k], name)) { zz_obs_claim[idx] = 1; return 1; }
    zz_obs_claim[idx] = 2;                                   // seen, not wanted
    return 0;
}
extern "C" void zz_xma_note_create(void);                                         // zz_xmamm.cpp (s4 A3)
static volatile LONG g_obs_mm;
// in[] = r3..r8 and LR captured BEFORE the real call (they are volatile across it).
extern "C" void zz_obs_log(uintptr_t ctx, unsigned idx, const char* name, const unsigned* in)
{
    PPCContext* c = (PPCContext*)(ctx - 0x80);
    LONG n = InterlockedIncrement(&g_obs_n[idx]);
    // s3 A3: the allocator / physical-address answers feed zz_lock.cpp's rings (every call,
    // whatever this log keeps) and are logged EVERY call up to a shared 4000-line ceiling --
    // the default first-40-then-every-500th would drop the one allocation the crash needs.
    // (Crackdown also fed MmAllocatePhysicalMemoryEx / MmGetPhysicalAddress into its own D3D
    // lock instrument here -- examples\crackdown\zz_lock.cpp.)
    if (!strcmp(name, "XMACreateContext")) zz_xma_note_create();
    if (!strcmp(name, "XNotifyGetNext")) { if (c->r3.u32 == 0) return; }   // only DELIVERED notifications
    else if (name[0] == 'M' && name[1] == 'm') { if (InterlockedIncrement(&g_obs_mm) > 4000) return; }
    else if (n > 40 && (n % 500) != 0) return;
    zz_log("OBS %-34s #%ld (%08X %08X %08X %08X %08X %08X) -> %08X  lr=%08X",
           name, (long)n, in[0], in[1], in[2], in[3], in[4], in[5], c->r3.u32, in[6]);
}

// xeo3_profile.txt in the console folder (cwd): "xuid=<16 hex digits>" and "gamertag=<name>"
// lines, either optional. Read once; both zz_save and the auto-logon use the result.
static volatile LONG g_profile_loaded;
static void zz_profile_load(void)
{
    if (InterlockedExchange(&g_profile_loaded, 1)) return;
    FILE* f = nullptr;
    if (fopen_s(&f, "xeo3_profile.txt", "r") || !f) return;
    char line[128];
    while (fgets(line, sizeof(line), f))
    {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (!_strnicmp(line, "xuid=", 5))
        {
            unsigned long long v = 0; int digits = 0;
            for (const char* p = line + 5; *p; ++p)
            {
                int h = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                      : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
                if (h < 0) break;
                v = (v << 4) | (unsigned)h; digits++;
            }
            if (digits == 16) g_xuid = v;
        }
        else if (!_strnicmp(line, "gamertag=", 9) && line[9])
            strncpy_s(g_gamertag, sizeof(g_gamertag), line + 9, _TRUNCATE);
    }
    fclose(f);
}

extern "C" unsigned long long zz_profile_xuid(void) { zz_profile_load(); return g_xuid; }

static int zz_save_init(void)
{
    g_t0 = GetTickCount64();
    zz_profile_load();
    InitializeCriticalSection(&g_lock);
    if (GetFileAttributesA(ZZ_TITLE "_localsaves.off") != INVALID_FILE_ATTRIBUTES) return 0;
    g_fp = _fsopen(ZZ_TITLE "_saves.log", "a", _SH_DENYNO);   // readable while the game runs
    const char* cl = GetCommandLineA();
    if (const char* d = strstr(cl, "-dvd "))
    {
        d += 5; while (*d == ' ') d++;
        char dvd[MAX_PATH]; size_t k = 0;
        if (*d == '"') { d++; while (*d && *d != '"' && k < MAX_PATH - 1) dvd[k++] = *d++; }
        else { while (*d && *d != ' ' && k < MAX_PATH - 1) dvd[k++] = *d++; }
        dvd[k] = 0;
        while (k && dvd[k - 1] == '\\') dvd[--k] = 0;
        if (k) _snprintf_s(g_dir, sizeof(g_dir), _TRUNCATE, "%s\\SAVES", dvd);
    }
    CreateDirectoryA(g_dir, nullptr);
    static const struct { int i; const char* n; } names[] = ZZ_SAVE_NAMES;
    for (const auto& e : names)
        if (e.i >= 0 && e.i < ZZ_SAVE_N_IMPORTS && e.i != ZZ_SI_VDSWAP && e.i != ZZ_SI_GFXCALLBACK)
            zz_save_claim[e.i] = 1;                    // VdSwap / the callback: the fps meter's, not ours
    // everything profile/achievement/message/notify-shaped that zz_save does NOT answer
    static const char* const kObs[] = {
        "XNotifyGetNext", "XamNotifyCreateListener", "XMsgStartIORequest", "XMsgStartIORequestEx",
        "XMsgInProcessCall", "XMsgCancelIORequest", "XamUserReadProfileSettings",
        "XamUserWriteProfileSettings", "XamUserCreateAchievementEnumerator",
        "XamUserCreateStatsEnumerator", "XamUserAreUsersFriends", "XamUserGetMembershipTierFromXUID",
        "XamUserGetOnlineCountryFromXUID", "XamTaskSchedule", "XamTaskShouldExit", "XamTaskCloseHandle",
        "XamShowMessageBoxUIEx", "XamCreateEnumeratorHandle", "XamGetPrivateEnumStructureFromHandle",
        "XamSessionCreateHandle", "XamSessionRefObjByHandle", "XamReadTileToTexture",
        "XamParseGamerTileKey", "XamGetExecutionId", "XamGetSystemVersion", "XamContentLaunchImage",
        "XamLoaderSetLaunchData", "XamLoaderLaunchTitle", "XamLoaderTerminateTitle",
        "XamShowGamerCardUIForXUID", "XamShowFriendsUI", "XamShowMarketplaceUI", "XamEnumerate",
        // s3 A3: the D3D/XPhysicalAlloc allocator and the XMA contexts (crash + audio evidence)
        "MmAllocatePhysicalMemoryEx", "MmFreePhysicalMemory", "MmGetPhysicalAddress",
        "XMACreateContext", "XMAReleaseContext" };
    g_obs_armed = GetFileAttributesA(ZZ_TITLE "_observe.on") != INVALID_FILE_ATTRIBUTES;
    if (g_obs_armed) for (const char* nm : kObs) zz_obs_want(nm);
    zz_log("ZZSAVE armed: local player %s (%016llX), saves in \"%s\"; observe %s", ZZ_GAMERTAG, ZZ_XUID, g_dir,
           g_obs_armed ? "ARMED (" ZZ_TITLE "_observe.on)" : "off");
    return 1;
}
static int g_init = zz_save_init();

// s2 run 1 (build 11): EVENT SIGNALLING. The save writer (sub_8254BD.., WriteFile at
// 8254BF30) makes an event with CreateEvent (sub_82743C10), puts it in the OVERLAPPED's
// hEvent, and XAPI's WriteFile passes it to NtWriteFile as r4. The real kernel signals that
// event when the I/O completes -- synchronous completion included. We completed the status
// block and never signalled it: the save wrote its 20-byte header, never closed the file, and
// the game said "cannot reach storage device". A handler that completes an I/O with an event
// leaves it here; the thunk (zz_import_hook.h) then calls the REAL NtSetEvent with it before
// returning to the guest. Per thread: the thunk runs on the thread that made the call.
// s2 build 13: the stall was never the event. The save op sub_82551778 (WriteFile at 8255186C)
// and the load op sub_82551218 (ReadFile at 825513D8) treat a TRUE return as an ERROR -- on the
// 360 an overlapped I/O always answers STATUS_PENDING. Overlapped callers now get 0x103.
static thread_local unsigned t_sig_event;
extern "C" unsigned zz_save_signal_take(void) { unsigned e = t_sig_event; t_sig_event = 0; return e; }
extern "C" void zz_save_signal_done(unsigned ev, unsigned status)
{
    static volatile LONG n; LONG k = InterlockedIncrement(&n);
    if (k <= 40 || status != 0) zz_log("  signalled event %08X via NtSetEvent -> %08X (#%ld)", ev, status, (long)k);
}

// --- helpers (all from zz_profile.cpp; guest memory is big-endian) -----------------
static unsigned zz_be32(const unsigned char* p)
{ return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }

static bool zz_exists(const char* p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static bool zz_isdir(const char* p)  { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

static void zz_complete(uintptr_t base, unsigned ovl)
{
    if (!ovl) return;
    PPC_STORE_U32(ovl + ZZ_OVL_LOW, 0);
    PPC_STORE_U32(ovl + ZZ_OVL_HIGH, 0);
    PPC_STORE_U32(ovl + ZZ_OVL_ERR, 0);
    unsigned ev = PPC_LOAD_U32(ovl + 0x0C), rt = PPC_LOAD_U32(ovl + 0x10);
    if (ev || rt) zz_log("!! overlapped @%08X has hEvent=%08X routine=%08X (not signalled)", ovl, ev, rt);
}

static void zz_iosb(uintptr_t base, unsigned iosb, unsigned status, unsigned info)
{ if (iosb) { PPC_STORE_U32(iosb, status); PPC_STORE_U32(iosb + 4, info); } }

static void zz_xcd_key(const unsigned char* xcd, char* key, size_t n)
{
    unsigned k = 0;
    for (unsigned i = 0; i < 42 && k + 1 < n; i++)
    {
        char ch = (char)xcd[0x108 + i];
        if (!ch) break;
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '_' || ch == '-' || ch == '.' || ch == ' ';
        key[k++] = ok ? ch : '_';
    }
    while (k && key[k - 1] == ' ') k--;
    key[k] = 0;
    if (!k) strncpy_s(key, n, "unnamed", _TRUNCATE);
}
static void zz_xcd_path(const char* key, char* out, size_t n)
{ _snprintf_s(out, n, _TRUNCATE, "%s\\%s%s", g_dir, key, ZZ_XCD_EXT); }

static bool zz_pkg_exists(const char* key)
{
    char p[MAX_PATH]; zz_xcd_path(key, p, sizeof(p));
    if (zz_exists(p)) return true;
    _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", g_dir, key);
    return zz_isdir(p);
}
static void zz_pkg_delete(const char* key)
{
    char dir[MAX_PATH], pat[MAX_PATH], p[MAX_PATH];
    zz_xcd_path(key, p, sizeof(p)); DeleteFileA(p);
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\%s", g_dir, key);
    WIN32_FIND_DATAA fd; _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
             { _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", dir, fd.cFileName); DeleteFileA(p); } }
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

static unsigned zz_scan(ZzEnum* e, unsigned dev, unsigned type)
{
    e->n = 0; e->cur = 0;
    WIN32_FIND_DATAA fd; char pat[MAX_PATH];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\*%s", g_dir, ZZ_XCD_EXT);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || e->n >= ZZ_ENUM_MAX) continue;
        char p[MAX_PATH]; _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", g_dir, fd.cFileName);
        FILE* f = nullptr; fopen_s(&f, p, "rb");
        if (!f) continue;
        unsigned char* it = e->item[e->n];
        size_t r = fread(it, 1, ZZ_XCD_SIZE, f); fclose(f);
        if (r != ZZ_XCD_SIZE) continue;
        if (zz_be32(it) == 0) { it[0] = it[1] = it[2] = 0; it[3] = ZZ_DEV_ID; }
        if (dev && zz_be32(it) != dev) continue;
        if (type && zz_be32(it + 4) != type) continue;
        e->n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return e->n;
}
static ZzEnum* zz_enum_for(unsigned h)
{ for (auto& e : g_enums) if (e.h && e.h == h) return &e; return nullptr; }
static FILE* zz_file_for(unsigned h)
{ if ((h & 0xFFFF0000u) != ZZ_FH_BASE) return nullptr; for (auto& f : g_files) if (f.h == h) return f.f; return nullptr; }
static ZzDir* zz_dir_for(unsigned h)
{ if ((h & 0xFFFF0000u) != ZZ_DH_BASE) return nullptr; for (auto& d : g_dirs) if (d.h == h) return &d; return nullptr; }

// "<root>:\A\B" -> "<dir>\<key>\A\B"; "<root>:\" -> "<dir>\<key>"
static bool zz_hostpath(const char* guest, char* out, size_t n, bool* is_root)
{
    if (!g_root[0]) return false;
    const size_t rl = strlen(g_root);
    if (_strnicmp(guest, g_root, rl) != 0 || guest[rl] != ':') return false;
    const char* rest = guest + rl + 1;
    while (*rest == '\\' || *rest == '/') rest++;
    if (is_root) *is_root = (*rest == 0);
    if (*rest) _snprintf_s(out, n, _TRUNCATE, "%s\\%s\\%s", g_dir, g_key, rest);
    else       _snprintf_s(out, n, _TRUNCATE, "%s\\%s", g_dir, g_key);
    for (char* p = out; *p; p++) if (*p == '/') *p = '\\';
    size_t L = strlen(out);
    while (L > 1 && out[L - 1] == '\\') out[--L] = 0;
    return true;
}
static bool zz_objattr_path(uintptr_t base, unsigned oa, char* out, size_t n)
{
    if (!oa) return false;
    unsigned ps = PPC_LOAD_U32(oa + 4); if (!ps) return false;
    unsigned len = (PPC_LOAD_U32(ps) >> 16) & 0xFFFF, pb = PPC_LOAD_U32(ps + 4);
    if (!pb || !len || len >= n) return false;
    for (unsigned i = 0; i < len; i++) out[i] = (char)PPC_LOAD_U8(pb + i);
    out[len] = 0;
    // a name relative to one of OUR directory handles (RootDirectory = +0)
    if (zz_dir_for(PPC_LOAD_U32(oa)))
    {
        char rel[256]; strncpy_s(rel, out, _TRUNCATE);
        _snprintf_s(out, n, _TRUNCATE, "%s:\\%s", g_root, rel);
    }
    return true;
}
static void zz_put64(uintptr_t base, unsigned ea, unsigned long long v)
{ PPC_STORE_U32(ea, (unsigned)(v >> 32)); PPC_STORE_U32(ea + 4, (unsigned)v); }
static unsigned long long zz_ft(const FILETIME& t)
{ return ((unsigned long long)t.dwHighDateTime << 32) | t.dwLowDateTime; }

// ------------------------------------------------------------------------------------
extern "C" int zz_save_handle(uintptr_t ctx, uintptr_t base, unsigned int idx)
{
    PPCContext* c = (PPCContext*)(ctx - 0x80);
    const unsigned user = c->r3.u32;
    const int i = (int)idx;

    // ---------------- identity ----------------
    if (i == ZZ_SI_GETSIGNINSTATE)
    {
        if (user != 0) return 0;
        c->r3.u64 = 1;                                     // eXUserSigninState_SignedInLocally
        return 1;
    }
    if (i == ZZ_SI_GETXUID)
    {   // Crackdown's form: XamUserGetXUID(user, type, PXUID) -- pointer in r5
        if (user != 0) return 0;
        if (unsigned ea = c->r5.u32) PPC_STORE_U64(ea, ZZ_XUID);
        c->r3.u64 = 0;
        return 1;
    }
    if (i == ZZ_SI_GETNAME)
    {   // XamUserGetName(user, LPSTR, cch)
        if (user != 0) return 0;
        unsigned ea = c->r4.u32, cch = c->r5.u32;
        if (ea && cch)
        {
            unsigned n = (unsigned)strlen(ZZ_GAMERTAG) + 1; if (n > cch) n = cch;
            memcpy((void*)(base + ea), ZZ_GAMERTAG, n);
            *(char*)(base + ea + n - 1) = 0;
        }
        c->r3.u64 = 0;
        return 1;
    }
    if (i == ZZ_SI_CHECKPRIV)
    {   // XamUserCheckPrivilege(user, priv, PBOOL): a local player holds no Live privileges
        if (user != 0) return 0;
        if (unsigned ea = c->r5.u32) PPC_STORE_U32(ea, 0);
        c->r3.u64 = 0;
        zz_log("CheckPrivilege(%u) -> FALSE", c->r4.u32);
        return 1;
    }
    if (i == ZZ_SI_SHOWSIGNIN)
    {   // should never come, since GetSigninState(0) already says "signed in"; if it does,
        // succeed without opening the blade (whose sign-in cannot complete here)
        zz_log("XamShowSigninUI(%u, %08X) -> success, no UI", c->r3.u32, c->r4.u32);
        c->r3.u64 = 0;
        return 1;
    }

    // ---------------- profile settings (s2) ----------------
    if (i == ZZ_SI_READPROFILE)
    {   // XamUserReadProfileSettings(titleId, user, nXuids, pXuids, nIds, pIds, PDWORD pcb,
        //                            pResults, PXOVERLAPPED /*9th: stack r1+0x54*/)
        // Read from the caller (sub_82A8ED90 @82A8EFBC-EFEC): r3 0xFFFE07D1, r4 [obj+0], r5 r6 0,
        // r7 3, r8 0x82BA33C0 (ids 1004000C/D/E), r9 obj+180, r10 [obj+176], stw ovl,84(r1).
        // Result = XUSER_READ_PROFILE_SETTING_RESULT { count; pSettings } then 40-byte
        // XUSER_PROFILE_SETTING { source +0, xuid +8, settingId +16, XUSER_DATA +24 (type byte,
        // union at +32) } -- the game's own parse loop (loc_82A8EE50) uses exactly these offsets.
        const unsigned title = c->r3.u32, usr = c->r4.u32, nX = c->r5.u32, pX = c->r6.u32;
        const unsigned nIds = c->r7.u32, pIds = c->r8.u32, pcb = c->r9.u32, pRes = c->r10.u32;
        const unsigned ovl = PPC_LOAD_U32(c->r1.u32 + 0x54);
        if ((usr != 0 && usr != 0xFF) || nX != 0)
            zz_log("ReadProfileSettings unusual form: user=%u nXuids=%u -- answered anyway (forwarding = the hang)", usr, nX);
        const unsigned rows = (nX ? nX : 1u) * nIds;
        const unsigned needed = 8u + rows * 40u;
        const unsigned cbIn = pcb ? PPC_LOAD_U32(pcb) : 0u;
        char ids[96] = ""; size_t L = 0;
        for (unsigned k = 0; k < nIds && k < 8 && L + 10 < sizeof(ids); k++)
            L += _snprintf_s(ids + L, sizeof(ids) - L, _TRUNCATE, "%s%08X", k ? " " : "", PPC_LOAD_U32(pIds + 4 * k));
        if (!pRes || cbIn < needed)
        {   // the synchronous size query: report the size, touch nothing else
            if (pcb) PPC_STORE_U32(pcb, needed);
            c->r3.u64 = 122;                               // ERROR_INSUFFICIENT_BUFFER
            zz_log("ReadProfileSettings title=%08X user=%u ids=[%s] cb %u < %u -> 122 (size query)", title, usr, ids, cbIn, needed);
            return 1;
        }
        PPC_STORE_U32(pRes + 0, rows);
        PPC_STORE_U32(pRes + 4, pRes + 8);
        for (unsigned r = 0; r < rows; r++)
        {
            const unsigned id = PPC_LOAD_U32(pIds + 4 * (r % nIds));
            const unsigned e = pRes + 8 + 40 * r;
            for (unsigned k = 0; k < 40; k += 4) PPC_STORE_U32(e + k, 0);
            const unsigned long long xuid = nX ? PPC_LOAD_U64(pX + 8 * (r / nIds)) : ZZ_XUID;
            unsigned type = id >> 28;
            if (type == 4 || type == 6) { zz_log("ReadProfileSettings id %08X is %s -- returned as NULL", id, type == 4 ? "UNICODE" : "BINARY"); type = 0xFF; }
            PPC_STORE_U32(e + 0, 1);                       // XSOURCE_DEFAULT
            PPC_STORE_U64(e + 8, xuid);
            PPC_STORE_U32(e + 16, id);
            PPC_STORE_U8(e + 24, (unsigned char)type);
            if (id == 0x1004000Eu) PPC_STORE_U32(e + 32, 100);   // VOICE_VOLUME default; every other value 0
        }
        PPC_STORE_U32(pcb, needed);
        unsigned ret = 0;
        if (ovl)
        {
            zz_complete(base, ovl);
            PPC_STORE_U32(ovl + ZZ_OVL_HIGH, needed);
            ret = 997;                                     // ERROR_IO_PENDING; [ovl+0] already reads 0 = done
        }
        c->r3.u64 = ret;
        zz_log("ReadProfileSettings title=%08X user=%u ids=[%s] cb %u -> %u, ovl %08X -> %u  lr=%08X",
               title, usr, ids, cbIn, needed, ovl, ret, (unsigned)c->lr);
        return 1;
    }
    if (i == ZZ_SI_WRITEPROFILE)
    {   // XamUserWriteProfileSettings(titleId, user, nSettings, pSettings, PXOVERLAPPED) --
        // the import's own order, read from its only wrapper sub_8292A970 (li r3,0 = titleId,
        // then the wrapper's user / n / pSettings / ovl in r4-r7). Logged, not yet persisted.
        const unsigned usr = c->r4.u32, n = c->r5.u32, pS = c->r6.u32, ovl = c->r7.u32;
        for (unsigned k = 0; k < n && k < 16 && pS; k++)
        {
            const unsigned e = pS + 40 * k;
            zz_log("WriteProfileSettings user=%u  %08X = %08X (type %u)", usr, PPC_LOAD_U32(e + 16),
                   PPC_LOAD_U32(e + 32), (unsigned)PPC_LOAD_U8(e + 24));
        }
        unsigned ret = 0;
        if (ovl) { zz_complete(base, ovl); ret = 997; }
        c->r3.u64 = ret;
        zz_log("WriteProfileSettings user=%u n=%u ovl %08X -> %u", usr, n, ovl, ret);
        return 1;
    }

    // ---------------- storage device ----------------
    if (i == ZZ_SI_DEVICESELECTOR)
    {   // XamShowDeviceSelectorUI(user, ctype, cflags, u64 bytes, PDWORD pDeviceId, PXOVERLAPPED)
        const unsigned pDev = c->r7.u32, ovl = c->r8.u32;
        if (pDev) PPC_STORE_U32(pDev, ZZ_DEV_ID);
        zz_complete(base, ovl);
        c->r3.u64 = 997;                                   // ERROR_IO_PENDING (NFS: caller checks for it)
        zz_log("DeviceSelector -> device %u (ovl %08X)", ZZ_DEV_ID, ovl);
        return 1;
    }
    if (i == ZZ_SI_DEVICESTATE)
    {   // XamContentGetDeviceState(DeviceID, PXOVERLAPPED)
        if (c->r3.u32 != ZZ_DEV_ID) { zz_log("DeviceState(%u) foreign -> real xam", c->r3.u32); return 0; }
        zz_complete(base, c->r4.u32);
        c->r3.u64 = 0;
        zz_log("DeviceState(%u) -> healthy", ZZ_DEV_ID);
        return 1;
    }

    // ---------------- content ----------------
    if (i == ZZ_SI_CREATEENUM)
    {   // XamContentCreateEnumerator(user, dev, type, flags, cItem, PDWORD pcb, PHANDLE ph)
        const unsigned dev = c->r4.u32, type = c->r5.u32, cItem = c->r7.u32, pcb = c->r8.u32, ph = c->r9.u32;
        if (user != 0 || (dev != 0 && dev != ZZ_DEV_ID)) return 0;
        int s = 0;
        for (int k = 0; k < ZZ_ENUM_N; k++) if (!g_enums[k].h) { s = k; break; }
        ZzEnum* e = &g_enums[s];
        e->h = ZZ_ENUM_HANDLE + (unsigned)s;
        zz_scan(e, dev, type);
        if (pcb) PPC_STORE_U32(pcb, cItem * ZZ_XCD_SIZE);
        if (ph)  PPC_STORE_U32(ph, e->h);
        c->r3.u64 = 0;
        zz_log("CreateEnumerator dev=%u type=%u -> %u save(s), handle %08X", dev, type, e->n, e->h);
        return 1;
    }
    if (i == ZZ_SI_ENUMERATE)
    {   // XamEnumerate(h, flags, buf, cb, PDWORD pcItems, PXOVERLAPPED)
        ZzEnum* e = zz_enum_for(c->r3.u32);
        if (!e) return 0;
        const unsigned buf = c->r5.u32, cb = c->r6.u32, pc = c->r7.u32, ovl = c->r8.u32;
        unsigned room = buf ? cb / ZZ_XCD_SIZE : 0, given = 0;
        while (given < room && e->cur < e->n)
        {
            memcpy((void*)(base + buf + given * ZZ_XCD_SIZE), e->item[e->cur], ZZ_XCD_SIZE);
            e->cur++; given++;
        }
        if (pc) PPC_STORE_U32(pc, given);
        if (ovl)
        {
            zz_complete(base, ovl);
            PPC_STORE_U32(ovl + ZZ_OVL_HIGH, given);
            if (!given) PPC_STORE_U32(ovl + ZZ_OVL_LOW, ZZ_NO_MORE_FILES);
        }
        c->r3.u64 = given ? 0u : ZZ_NO_MORE_FILES;
        zz_log("Enumerate %08X -> %u item(s)", e->h, given);
        return 1;
    }
    if (i == ZZ_SI_CONTENTCREATE || i == ZZ_SI_CONTENTCREATEEX)
    {   // XamContentCreateEx(user, root, XCONTENT_DATA*, flags, PDWORD disp, PDWORD licmask,
        //                    cacheSize, u64 contentSize, PXOVERLAPPED /*9th: stack r1+0x54*/)
        // XamContentCreate has the overlapped as its 7th argument, r9.
        const unsigned pRoot = c->r4.u32, pData = c->r5.u32, flags = c->r6.u32;
        const unsigned pDisp = c->r7.u32, pLic = c->r8.u32;
        const unsigned ovl = (i == ZZ_SI_CONTENTCREATEEX) ? PPC_LOAD_U32(c->r1.u32 + 0x54) : c->r9.u32;
        char root[32]; unsigned n = 0;
        if (pRoot) for (; n < sizeof(root) - 1; n++) { char ch = (char)PPC_LOAD_U8(pRoot + n); if (!ch) break; root[n] = ch; }
        root[n] = 0;
        unsigned char xcd[ZZ_XCD_SIZE]; char key[64] = "unnamed"; bool exists = false;
        const unsigned mode = flags & 0xF;            // CreateFile-style: 1 NEW 2 ALWAYS 3 OPEN 4 OPEN_ALWAYS 5 TRUNC
        if (pData)
        {
            memcpy(xcd, (const void*)(base + pData), ZZ_XCD_SIZE);
            zz_xcd_key(xcd, key, sizeof(key));
            exists = zz_pkg_exists(key);
        }
        unsigned err = 0;
        if (mode == 1 && exists) err = 183;                     // ERROR_ALREADY_EXISTS
        if ((mode == 3 || mode == 5) && !exists) err = 2;       // ERROR_FILE_NOT_FOUND
        if (!err)
        {
            if (pData)
            {
                if (zz_be32(xcd) == 0) { xcd[0] = xcd[1] = xcd[2] = 0; xcd[3] = ZZ_DEV_ID; }
                char sc[MAX_PATH]; zz_xcd_path(key, sc, sizeof(sc));
                if (mode != 3 || !zz_exists(sc))
                {
                    FILE* f = nullptr; fopen_s(&f, sc, "wb");
                    if (f) { fwrite(xcd, 1, ZZ_XCD_SIZE, f); fclose(f); }
                }
            }
            if (pDisp) PPC_STORE_U32(pDisp, exists ? 2u : 1u);   // EXISTING / NEW
            if (pLic)  PPC_STORE_U32(pLic, 0);
            zz_complete(base, ovl);
            strncpy_s(g_root, root, _TRUNCATE);
            strncpy_s(g_key, key, _TRUNCATE);
            char dir[MAX_PATH]; _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\%s", g_dir, key);
            CreateDirectoryA(dir, nullptr);
        }
        else if (ovl) { PPC_STORE_U32(ovl + ZZ_OVL_LOW, err); PPC_STORE_U32(ovl + ZZ_OVL_HIGH, 0); }
        c->r3.u64 = err;
        zz_log("ContentCreate root=\"%s\" package=\"%s\" flags=%08X exists=%d -> %u (%s)",
               root, key, flags, exists ? 1 : 0, err, err ? "refused" : (exists ? "EXISTING" : "NEW"));
        return 1;
    }
    if (i == ZZ_SI_CONTENTCLOSE || i == ZZ_SI_CONTENTFLUSH)
    {   // XamContentClose(root, PXOVERLAPPED)
        for (auto& f : g_files) if (f.h && f.f) fflush(f.f);
        zz_complete(base, c->r4.u32);
        c->r3.u64 = 0;
        zz_log("ContentClose/Flush -> ok");
        return 1;
    }
    if (i == ZZ_SI_CONTENTDELETE)
    {   // XamContentDelete(user, XCONTENT_DATA*, PXOVERLAPPED)
        if (unsigned pData = c->r4.u32)
        {
            unsigned char xcd[ZZ_XCD_SIZE]; memcpy(xcd, (const void*)(base + pData), ZZ_XCD_SIZE);
            char key[64]; zz_xcd_key(xcd, key, sizeof(key));
            zz_pkg_delete(key);
            zz_log("ContentDelete \"%s\"", key);
        }
        zz_complete(base, c->r5.u32);
        c->r3.u64 = 0;
        return 1;
    }

    // ---------------- the kernel file calls, only for OUR handles / save paths ----------------
    if (i == ZZ_SI_NTCLOSE)
    {
        const unsigned h = c->r3.u32;
        if (ZzEnum* e = zz_enum_for(h)) { e->h = 0; c->r3.u64 = 0; zz_log("CLOSE enum %08X", h); return 1; }
        for (auto& f : g_files) if (f.h == h) { fclose(f.f); f.h = 0; f.f = nullptr; c->r3.u64 = 0; zz_log("CLOSE file %08X", h); return 1; }
        if (ZzDir* d = zz_dir_for(h)) { d->h = 0; c->r3.u64 = 0; zz_log("CLOSE dir %08X", h); return 1; }
        return 0;
    }
    if (i == ZZ_SI_NTCREATEFILE || i == ZZ_SI_NTOPENFILE)
    {   // NtCreateFile(PHANDLE, access, OBJECT_ATTRIBUTES*, IOSB*, alloc, attrs, share, disp /*r10*/, options /*stack*/)
        // NtOpenFile(PHANDLE, access, OBJECT_ATTRIBUTES*, IOSB*, share, options /*r8*/)
        const bool create = (i == ZZ_SI_NTCREATEFILE);
        const unsigned pH = c->r3.u32, oa = c->r5.u32, iosb = c->r6.u32;
        char guest[256], host[300]; bool is_root = false;
        if (!zz_objattr_path(base, oa, guest, sizeof(guest))) return 0;
        if (!zz_hostpath(guest, host, sizeof(host), &is_root)) return 0;
        const unsigned options = create ? PPC_LOAD_U32(c->r1.u32 + 0x54) : c->r8.u32;
        const unsigned disp = create ? c->r10.u32 : 1u;
        if (is_root || (options & 1u) || zz_isdir(host))
        {   // a directory (the package root is listed to validate a save -- NFS s25b)
            bool had = zz_isdir(host); unsigned fail = 0, info = 1;
            if (!had)
            {
                if (disp == 0 || disp == 2 || disp == 3 || disp == 5) { CreateDirectoryA(host, nullptr); had = zz_isdir(host); info = 2; }
                if (!had) fail = 0xC0000034u;
            }
            else if (disp == 2) fail = 0xC0000035u;
            int s = -1;
            if (!fail) { for (int k = 0; k < ZZ_DH_N; k++) if (!g_dirs[k].h) { s = k; break; } if (s < 0) fail = 0xC000009Au; }
            if (fail) { zz_iosb(base, iosb, fail, 0); c->r3.u64 = fail; }
            else
            {
                g_dirs[s].h = ZZ_DH_BASE | (unsigned)(s + 1);
                strncpy_s(g_dirs[s].host, host, _TRUNCATE);
                g_dirs[s].cur = 0;
                if (pH) PPC_STORE_U32(pH, g_dirs[s].h);
                zz_iosb(base, iosb, 0, info); c->r3.u64 = 0;
            }
            zz_log("DIR %s \"%s\" -> \"%s\" status %08X", create ? "create" : "open", guest, host, fail);
            return 1;
        }
        int s = -1;
        for (int k = 0; k < ZZ_FH_N; k++) if (!g_files[k].h) { s = k; break; }
        if (s < 0) { zz_log("!! no free save handle for \"%s\"", guest); return 0; }
        //   0 SUPERSEDE 1 OPEN 2 CREATE 3 OPEN_IF 4 OVERWRITE 5 OVERWRITE_IF
        const bool had = zz_exists(host);
        bool trunc = false; unsigned info = 1, fail = 0;
        switch (disp)
        {
        case 0: trunc = true; info = 0; break;
        case 1: if (!had) fail = 0xC0000034u; break;
        case 2: if (had) fail = 0xC0000035u; trunc = true; info = 2; break;
        case 3: trunc = !had; info = had ? 1 : 2; break;
        case 4: if (!had) fail = 0xC0000034u; trunc = true; info = 3; break;
        default: trunc = true; info = had ? 3 : 2; break;
        }
        FILE* f = nullptr;
        if (!fail)
        {
            // s2 run 2 (build 12): SHARED opens. fopen_s takes the host file exclusively, so a
            // second guest handle on the same file failed against OUR first one. Measured: the
            // game opened Crackdown.sav to load it (5A5B0001), found it damaged, kept that handle,
            // then opened it again to overwrite -> C0000034 -> "Unable to Access Storage Device".
            // The real kernel allows the second open when the share flags do; _SH_DENYNO is that.
            f = _fsopen(host, trunc ? "wb+" : "rb+", _SH_DENYNO);
            if (!f && !trunc) f = _fsopen(host, "rb", _SH_DENYNO);
            if (!f) { zz_log("!! host open failed \"%s\" errno %d", host, errno); fail = 0xC0000034u; }
        }
        if (fail) { zz_iosb(base, iosb, fail, 0); c->r3.u64 = fail; zz_log("FILE open \"%s\" disp=%u -> %08X", guest, disp, fail); return 1; }
        g_files[s].h = ZZ_FH_BASE | (unsigned)(s + 1);
        g_files[s].f = f;
        if (pH) PPC_STORE_U32(pH, g_files[s].h);
        zz_iosb(base, iosb, 0, info);
        c->r3.u64 = 0;
        zz_log("FILE %s \"%s\" -> \"%s\" disp=%u %s handle %08X", create ? "create" : "open", guest, host,
               disp, trunc ? "truncate" : "keep", g_files[s].h);
        return 1;
    }
    if (i == ZZ_SI_NTREADFILE || i == ZZ_SI_NTWRITEFILE)
    {   // Nt{Read,Write}File(h, event, apc, apcctx, IOSB*, buf, len, LARGE_INTEGER* off)
        FILE* f = zz_file_for(c->r3.u32);
        if (!f) return 0;
        const unsigned iosb = c->r7.u32, buf = c->r8.u32, len = c->r9.u32, pOff = c->r10.u32;
        if (pOff && PPC_LOAD_U32(pOff) != 0xFFFFFFFFu) fseek(f, (long)PPC_LOAD_U32(pOff + 4), SEEK_SET);
        size_t done;
        if (i == ZZ_SI_NTWRITEFILE) { done = fwrite((const void*)(base + buf), 1, len, f); fflush(f); }
        else done = fread((void*)(base + buf), 1, len, f);
        const unsigned st = done == len ? 0u : (i == ZZ_SI_NTREADFILE ? 0xC0000011u : 0xC000007Fu);
        zz_iosb(base, iosb, st, (unsigned)done);
        // build 13: an OVERLAPPED caller (XAPI passes hEvent as r4, the OVERLAPPED as r6) gets
        // STATUS_PENDING like the real kernel; the status block above is already filled, so
        // GetOverlappedResult completes at once. A synchronous caller (r4 = r6 = 0) is unchanged.
        const unsigned ev = c->r4.u32, apc = c->r5.u32, apcctx = c->r6.u32;
        const bool overlapped = (ev != 0) || (apcctx != 0);
        c->r3.u64 = overlapped ? 0x103u /* STATUS_PENDING */ : st;
        if (ev) t_sig_event = ev;
        zz_log("FILE %s %u/%u bytes -> %08X  event=%08X apc=%08X ctx=%08X %s", i == ZZ_SI_NTWRITEFILE ? "write" : "read",
               (unsigned)done, len, st, ev, apc, apcctx, overlapped ? "PENDING" : "sync");
        if (apc) zz_log("!! APC routine %08X NOT delivered", apc);
        return 1;
    }
    if (i == ZZ_SI_NTFLUSHFILE)
    {
        FILE* f = zz_file_for(c->r3.u32);
        if (!f && !zz_dir_for(c->r3.u32)) return 0;
        if (f) fflush(f);
        zz_log("FLUSH %08X", c->r3.u32);
        zz_iosb(base, c->r4.u32, 0, 0); c->r3.u64 = 0;
        return 1;
    }
    if (i == ZZ_SI_NTQUERYINFOFILE)
    {   // NtQueryInformationFile(h, IOSB*, info, len, class)
        const unsigned h = c->r3.u32, iosb = c->r4.u32, info = c->r5.u32, len = c->r6.u32, cls = c->r7.u32;
        FILE* f = zz_file_for(h);
        ZzDir* d = f ? nullptr : zz_dir_for(h);
        if (!f && !d) return 0;
        if (info) for (unsigned k = 0; k + 4 <= len && k < 64; k += 4) PPC_STORE_U32(info + k, 0);
        if (d)
        {
            if (info && cls == 4  && len >= 36) PPC_STORE_U32(info + 32, 0x10);
            if (info && cls == 5  && len >= 24) { PPC_STORE_U32(info + 16, 1); PPC_STORE_U8(info + 21, 1); }
            if (info && cls == 34 && len >= 52) PPC_STORE_U32(info + 48, 0x10);
        }
        else
        {
            long cur = ftell(f); fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, cur, SEEK_SET);
            if (info) switch (cls)
            {
            case 4:  if (len >= 36) PPC_STORE_U32(info + 32, 0x80); break;
            case 5:  if (len >= 24) { PPC_STORE_U32(info + 4, (unsigned)size); PPC_STORE_U32(info + 12, (unsigned)size); PPC_STORE_U32(info + 16, 1); } break;
            case 14: if (len >= 8)  PPC_STORE_U32(info + 4, (unsigned)cur); break;
            case 34: if (len >= 52) { PPC_STORE_U32(info + 36, (unsigned)size); PPC_STORE_U32(info + 44, (unsigned)size); PPC_STORE_U32(info + 48, 0x80); } break;
            default: if (len >= 8)  PPC_STORE_U32(info + 4, (unsigned)size); break;
            }
        }
        zz_iosb(base, iosb, 0, len); c->r3.u64 = 0;
        zz_log("QUERYINFO %08X class %u len %u -> 0", h, cls, len);
        return 1;
    }
    if (i == ZZ_SI_NTSETINFOFILE)
    {   // NtSetInformationFile(h, IOSB*, info, len, class)
        FILE* f = zz_file_for(c->r3.u32);
        if (!f) { if (!zz_dir_for(c->r3.u32)) return 0; zz_log("SETINFO dir %08X class %u -> 0", c->r3.u32, c->r7.u32);
                  zz_iosb(base, c->r4.u32, 0, 0); c->r3.u64 = 0; return 1; }
        const unsigned info = c->r5.u32, cls = c->r7.u32;
        zz_log("SETINFO %08X class %u value %08X:%08X -> 0", c->r3.u32, cls,
               info ? PPC_LOAD_U32(info) : 0u, info ? PPC_LOAD_U32(info + 4) : 0u);
        if (info)
        {
            const long v = (long)PPC_LOAD_U32(info + 4);
            if (cls == 14) fseek(f, v, SEEK_SET);
            else if (cls == 19 || cls == 20) { fflush(f); _chsize(_fileno(f), v); }
        }
        zz_iosb(base, c->r4.u32, 0, 0); c->r3.u64 = 0;
        return 1;
    }
    if (i == ZZ_SI_NTQUERYDIR)
    {   // NtQueryDirectoryFile(h, ev, apc, apcctx, IOSB*, info, len, STRING* name /*r10*/, restart /*stack*/)
        ZzDir* d = zz_dir_for(c->r3.u32);
        if (!d) return 0;
        const unsigned iosb = c->r7.u32, info = c->r8.u32, len = c->r9.u32, pName = c->r10.u32;
        if (PPC_LOAD_U32(c->r1.u32 + 0x54) & 0xFF) d->cur = 0;
        char filt[128] = ""; bool any = true;
        if (pName)
        {
            unsigned fl = (PPC_LOAD_U32(pName) >> 16) & 0xFFFF, fb = PPC_LOAD_U32(pName + 4);
            if (fb && fl && fl < sizeof(filt))
            {
                for (unsigned k = 0; k < fl; k++) filt[k] = (char)PPC_LOAD_U8(fb + k);
                filt[fl] = 0;
                any = strchr(filt, '*') || strchr(filt, '?');
            }
        }
        WIN32_FIND_DATAA fd; char pat[MAX_PATH];
        _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\*", d->host);
        HANDLE fh = FindFirstFileA(pat, &fd);
        bool found = false; unsigned seen = 0;
        if (fh != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                if (!any && _stricmp(fd.cFileName, filt) != 0) continue;
                if (seen++ == d->cur) { found = true; break; }
            } while (FindNextFileA(fh, &fd));
            FindClose(fh);
        }
        unsigned st, written = 0;
        if (!found) st = d->cur == 0 ? 0xC000000Fu : 0x80000006u;
        else
        {
            const unsigned nl = (unsigned)strlen(fd.cFileName), need = 0x40 + nl;
            if (!info || len < need) st = 0xC0000023u;
            else
            {
                for (unsigned k = 0; k < 0x40; k += 4) PPC_STORE_U32(info + k, 0);
                const unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                zz_put64(base, info + 0x08, zz_ft(fd.ftCreationTime));
                zz_put64(base, info + 0x10, zz_ft(fd.ftLastAccessTime));
                zz_put64(base, info + 0x18, zz_ft(fd.ftLastWriteTime));
                zz_put64(base, info + 0x20, zz_ft(fd.ftLastWriteTime));
                zz_put64(base, info + 0x28, sz);
                zz_put64(base, info + 0x30, (sz + 4095) & ~4095ull);
                PPC_STORE_U32(info + 0x38, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10u : 0x80u);
                PPC_STORE_U32(info + 0x3C, nl);
                memcpy((void*)(base + info + 0x40), fd.cFileName, nl);
                written = need; st = 0; d->cur++;
            }
        }
        zz_iosb(base, iosb, st, written); c->r3.u64 = st;
        if (unsigned ev = c->r4.u32) t_sig_event = ev;
        zz_log("DIR list \"%s\" -> %s (%08X) event=%08X", d->host, found ? fd.cFileName : "(end)", st, c->r4.u32);
        return 1;
    }
    if (i == ZZ_SI_NTQUERYVOLINFO)
    {   // NtQueryVolumeInformationFile(h, IOSB*, info, len, class)
        const unsigned h = c->r3.u32;
        if (!zz_file_for(h) && !zz_dir_for(h)) return 0;
        const unsigned iosb = c->r4.u32, info = c->r5.u32, len = c->r6.u32, cls = c->r7.u32;
        if (info) for (unsigned k = 0; k + 4 <= len && k < 64; k += 4) PPC_STORE_U32(info + k, 0);
        if (info && cls == 3 && len >= 24)
        {
            zz_put64(base, info, ZZ_DEV_TOTAL / 4096);
            zz_put64(base, info + 8, ZZ_DEV_FREE / 4096);
            PPC_STORE_U32(info + 16, 8); PPC_STORE_U32(info + 20, 512);
        }
        zz_iosb(base, iosb, 0, len); c->r3.u64 = 0;
        zz_log("QUERYVOL %08X class %u len %u -> 0", h, cls, len);
        return 1;
    }
    if (i == ZZ_SI_NTQUERYFULLATTR)
    {   // NtQueryFullAttributesFile(OBJECT_ATTRIBUTES*, FILE_NETWORK_OPEN_INFORMATION*)
        const unsigned oa = c->r3.u32, info = c->r4.u32;
        char guest[256], host[300];
        if (!zz_objattr_path(base, oa, guest, sizeof(guest))) return 0;
        if (!zz_hostpath(guest, host, sizeof(host), nullptr)) return 0;
        WIN32_FILE_ATTRIBUTE_DATA ad; unsigned st = 0;
        if (!GetFileAttributesExA(host, GetFileExInfoStandard, &ad)) st = 0xC0000034u;
        else if (info)
        {
            const unsigned long long sz = ((unsigned long long)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
            zz_put64(base, info,      zz_ft(ad.ftCreationTime));
            zz_put64(base, info + 8,  zz_ft(ad.ftLastAccessTime));
            zz_put64(base, info + 16, zz_ft(ad.ftLastWriteTime));
            zz_put64(base, info + 24, zz_ft(ad.ftLastWriteTime));
            zz_put64(base, info + 32, (sz + 4095) & ~4095ull);
            zz_put64(base, info + 40, sz);
            PPC_STORE_U32(info + 48, (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10u : 0x80u);
            PPC_STORE_U32(info + 52, 0);
        }
        c->r3.u64 = st;
        zz_log("QueryFullAttributes \"%s\" -> %08X", guest, st);
        return 1;
    }
    return 0;
}
