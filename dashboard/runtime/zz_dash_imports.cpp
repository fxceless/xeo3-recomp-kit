// xeo3-recomp-kit dashboard runtime -- the import layer (see zz_dash_hook.h for the
// claim/wrap mechanism). Everything here answers or observes one xam/kernel call.
//
// THE CAMERA. With the dash's jump tables right it runs into its real startup -- storage
// enumeration, the content roots, the UI fonts -- and then calls XUsbcamCreate, the Xbox
// Live Vision camera. The emulator logs
//     NYI Kernel API ThunkXUsbcamCreate (546) was called!
// and the dash's main thread ends up in the guest kernel's KeBugCheckEx, whose tail call
// is meant never to return; here it returns into a zero word and the emulator reports
//     FATAL ERROR - Unable to execute precompiled code for IAR 0x8005f0b4
// A retail console with nothing plugged in fails XUsbcamCreate with
// ERROR_DEVICE_NOT_CONNECTED, and the dash copes with that because most consoles have no
// camera. So every camera entry point answers exactly that; Destroy succeeds.
//
// THE REST is the title launcher (zz_dash_launch.cpp) and the Guide button
// (zz_dash_input.cpp); this file only routes the calls to them.
#include "ppc_recomp_shared.h"
#include "zz_dash_common.h"
#include <windows.h>
#include <cstdio>

static_assert(ZZ_DI_XAMCONTENTLAUNCHIMAGEINTERNALEX >= 0 && ZZ_DI_XAMLOADERLAUNCHTITLEEX >= 0,
              "this dash does not import the My Games launch pair (gen_dash_idx.py should have refused)");

// zz_dash_launch.cpp
extern "C" void zz_dash_launch_request(const char* path, unsigned int flags);
extern "C" int zz_dash_launch_resolve_content(unsigned int contentType, const char* fileName);
extern "C" void zz_dash_launch_save_data(const void* bytes, unsigned int len);
extern "C" unsigned int zz_dash_launch_replay_len();
extern "C" unsigned int zz_dash_launch_replay_copy(void* out, unsigned int cb);
// zz_dash_input.cpp
extern "C" void zz_dash_guide_poll();

namespace
{
    constexpr unsigned int kErrorDeviceNotConnected = 0x48F;   // ERROR_DEVICE_NOT_CONNECTED
    constexpr unsigned int kStatusDllNotFound = 0xC0000135u;   // STATUS_DLL_NOT_FOUND

    // A guest address that is in range is NOT necessarily mapped -- a trace helper once
    // died on an argument that was a value, not a pointer. No SEH here (unwinding through
    // a preserve_none guest frame is fatal), so ask the OS instead.
    bool GuestReadable(uintptr_t base, unsigned int va, size_t n)
    {
        if (va < 0x10000u || va >= 0xC0000000u)
            return false;
        MEMORY_BASIC_INFORMATION mbi;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(base + va);
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            return false;
        return p + n <= static_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
    }

    // guest memory is BIG-ENDIAN
    unsigned int GuestU32(uintptr_t base, unsigned int va)
    {
        if (!GuestReadable(base, va, 4))
            return 0;
        const unsigned char* q = reinterpret_cast<const unsigned char*>(base + va);
        return (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3];
    }

    void GuestPutU32(uintptr_t base, unsigned int va, unsigned int v)
    {
        if (!GuestReadable(base, va, 4))
            return;
        unsigned char* q = reinterpret_cast<unsigned char*>(base + va);
        q[0] = (unsigned char)(v >> 24); q[1] = (unsigned char)(v >> 16);
        q[2] = (unsigned char)(v >> 8);  q[3] = (unsigned char)v;
    }

    void GuestStrA(uintptr_t base, unsigned int va, char* out, size_t n)
    {
        out[0] = 0;
        if (!GuestReadable(base, va, n)) return;
        const char* p = reinterpret_cast<const char*>(base + va);
        size_t i = 0;
        for (; i + 1 < n && p[i]; i++) out[i] = (p[i] >= 32 && p[i] < 127) ? p[i] : '.';
        out[i] = 0;
    }

    // big-endian UTF-16 guest string -> ASCII (non-printables as '?')
    void GuestStrW(uintptr_t base, unsigned int va, char* out, size_t n)
    {
        out[0] = 0;
        if (!GuestReadable(base, va, 2 * n))
            return;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(base + va);
        size_t i = 0;
        for (; i + 1 < n; i++)
        {
            unsigned int ch = (p[2 * i] << 8) | p[2 * i + 1];
            if (!ch) break;
            out[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        }
        out[i] = 0;
    }

    // ---------------------------------------------------------------------------------
    // XexLoadImage: a module whose precompiled DLL is absent.
    // The emulator turns a module's header digest into a DLL name and, if that DLL is not
    // in the console folder, stops the whole process with a hard FATAL ("Failed to create
    // precompiled image path"). The dash loads some of its pages as separate modules
    // (Dash.MP.*.lex, mostly the online ones), and a console package does not ship a DLL
    // for every one. So: if the image about to be loaded can be identified AND neither
    // <name>.dll nor <name>_no.dll exists, answer STATUS_DLL_NOT_FOUND -- a normal,
    // survivable error for the dash. ANY uncertainty passes the call through untouched, so
    // a load that would have worked is never disturbed.
    bool XeoNameOf(const char* hostFile, char* nameOut, size_t n)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, hostFile, "rb") != 0 || !f) return false;
        unsigned char hdr[0x400];
        size_t got = fread(hdr, 1, sizeof(hdr), f);
        fclose(f);
        if (got < 0x18 || hdr[0] != 'X' || hdr[1] != 'E' || hdr[2] != 'X' || hdr[3] != '2') return false;
        unsigned int sec = (hdr[0x10] << 24) | (hdr[0x11] << 16) | (hdr[0x12] << 8) | hdr[0x13];
        if ((size_t)sec + 0x178 > got) return false;
        // tools\xeo3_name.py: the 20-byte header digest at security_info + 0x164, five
        // groups of four bytes, each read little-endian
        const unsigned char* dg = hdr + sec + 0x164;
        unsigned int w[5];
        for (int i = 0; i < 5; i++)
            w[i] = dg[i * 4] | (dg[i * 4 + 1] << 8) | (dg[i * 4 + 2] << 16) | ((unsigned)dg[i * 4 + 3] << 24);
        _snprintf_s(nameOut, n, _TRUNCATE, "xeo3_%08x_%08x_%08x_%08x_%08x", w[0], w[1], w[2], w[3], w[4]);
        return true;
    }

    int XexLoadImageGuard(uintptr_t base, PPCContext* c)
    {
        char guestName[260];
        GuestStrA(base, c->r3.u32, guestName, sizeof(guestName));   // XexLoadImage(PSZ name, ...)
        if (!guestName[0]) return 0;
        const char* bn = guestName;
        for (const char* p = guestName; *p; ++p) if (*p == '\\' || *p == '/') bn = p + 1;
        if (!bn[0]) return 0;

        // where the console package keeps the dash's online modules, else beside the exe
        char file[MAX_PATH], rel[MAX_PATH], xeo[80];
        _snprintf_s(rel, sizeof(rel), _TRUNCATE, "SystemAuxPartition\\online\\20426B00\\%s", bn);
        zz_dash_path(file, sizeof(file), rel);
        if (!XeoNameOf(file, xeo, sizeof(xeo)))
        {
            zz_dash_path(file, sizeof(file), bn);
            if (!XeoNameOf(file, xeo, sizeof(xeo))) return 0;     // cannot identify: pass through
        }
        char dll[MAX_PATH], dllno[MAX_PATH], nm[100];
        _snprintf_s(nm, sizeof(nm), _TRUNCATE, "%s.dll", xeo);
        zz_dash_path(dll, sizeof(dll), nm);
        _snprintf_s(nm, sizeof(nm), _TRUNCATE, "%s_no.dll", xeo);
        zz_dash_path(dllno, sizeof(dllno), nm);
        if (GetFileAttributesA(dll) != INVALID_FILE_ATTRIBUTES || GetFileAttributesA(dllno) != INVALID_FILE_ATTRIBUTES)
            return 0;
        zz_dash_logf("ZZIMP XexLoadImage \"%s\" -> %s: no DLL, declining 0xC0000135 (would have been a FATAL)\n", bn, xeo);
        c->r3.u64 = kStatusDllNotFound;
        return 1;
    }

    // XamLoaderLaunchTitle(LPCSTR path, DWORD flags) and friends: the launch may reboot
    // the guest and never come back, so the request is recorded on the way IN; the real
    // import still runs, so xam does its orderly teardown (profile flushed, handles
    // closed) and the switch happens at CleanupPrecompiledDll.
    //
    // Only XamLoaderLaunchTitleEx has been measured carrying flags in r6 (2 = "Play Game",
    // 0 = the dash rebooting into itself for an avatar scene); every other entry point is
    // reported as 2.
    void LaunchIn(uintptr_t base, unsigned int idx, const PPCContext* c)
    {
        char path[200];
        GuestStrA(base, c->r3.u32, path, sizeof(path));
        zz_dash_logf("ZZLAUNCH import %u IN r3=%08X \"%s\" r4=%08X r5=%08X r6=%08X lr=%08X\n",
                     idx, c->r3.u32, path, c->r4.u32, c->r5.u32, c->r6.u32, (unsigned int)c->lr);
        zz_dash_launch_request(path, (int)idx == ZZ_DI_XAMLOADERLAUNCHTITLEEX ? c->r6.u32 : 2u);
    }
}

// Non-zero means "r3 is set, do not call the real import" (2 = redirect, see the hook).
extern "C" int zz_dash_import(uintptr_t ctx, uintptr_t base, unsigned int idx)
{
    PPCContext* c = reinterpret_cast<PPCContext*>(ctx - 0x80);
    const int i = (int)idx;

    if (i == ZZ_DI_XEXLOADIMAGE)
        return XexLoadImageGuard(base, c);

    if (i == ZZ_DI_XAMCONTENTLAUNCHIMAGEINTERNALEX)
    {
        // MY GAMES "A" = XamContentLaunchImageInternalEx(XCONTENT_DATA*, "default.xex", 0, -1).
        // Real xam answers 0x80070005 here (it would have to mount the game's package
        // in-guest, which this emulator was never asked to do). So the package is resolved
        // to a title on the host (zz_titles.txt) and, if it is one of ours, the call becomes
        // XamLoaderLaunchTitleEx(NULL, 0, 0, 2) -- "Play Game", the soft reboot whose
        // teardown the switcher already rides -- with the target remembered for the
        // switcher. Anything unresolved goes to xam as before.
        // XCONTENT_DATA = { DWORD DeviceID; DWORD ContentType; WCHAR DisplayName[128];
        //                   CHAR FileName[42]; }
        unsigned int cd = c->r3.u32;
        char name[64], file[48];
        GuestStrW(base, cd + 8, name, sizeof(name));
        GuestStrA(base, cd + 0x108, file, 43);
        unsigned int dev = GuestU32(base, cd), ctype = GuestU32(base, cd + 4);
        zz_dash_logf("ZZLAUNCH My Games: content{dev=%08X ctype=%08X \"%s\" file=%s} image=%08X\n",
                     dev, ctype, name, file, c->r4.u32);
        if (file[0] && zz_dash_launch_resolve_content(ctype, file))
        {
            zz_dash_log("ZZLAUNCH redirecting to XamLoaderLaunchTitleEx(NULL,0,0,2)\n");
            c->r3.u64 = 0; c->r4.u64 = 0; c->r5.u64 = 0; c->r6.u64 = 2;
            return 2;
        }
        LaunchIn(base, idx, c);
        return 0;
    }

    if (i == ZZ_DI_XAMLOADERLAUNCHTITLE || i == ZZ_DI_XAMLOADERLAUNCHTITLEEX
        || i == ZZ_DI_XAMLOADERLAUNCHTITLEFORREASON)
    {
        LaunchIn(base, idx, c);
        return 0;
    }

    if (i == ZZ_DI_XAMLOADERSETLAUNCHDATA)
    {
        // XamLoaderSetLaunchData(pData, cbData): held on the host side; the switcher writes
        // it out only if the launch that follows turns out to be the dash relaunching itself.
        unsigned int len = c->r4.u32;
        if (len && len <= 0x1000u && GuestReadable(base, c->r3.u32, len))
            zz_dash_launch_save_data(reinterpret_cast<const void*>(base + c->r3.u32), len);
        return 0;
    }

    if (i == ZZ_DI_XAMLOADERGETLAUNCHDATA || i == ZZ_DI_XAMLOADERGETLAUNCHDATASIZE)
    {
        // THE LAUNCH-DATA REPLAY. xam's copy of the SetLaunchData block dies with the old
        // process, so after the dash's self relaunch XamLoaderGetLaunchData answers 0x490
        // (not found) and the dash opens its home instead of the scene it asked for. The
        // block was carried across in a file (zz_dash_launch.cpp); hand it back. With no
        // file (or dash_self_relaunch.on absent) real xam answers, exactly as before.
        unsigned int len = zz_dash_launch_replay_len();
        if (!len)
            return 0;
        if (i == ZZ_DI_XAMLOADERGETLAUNCHDATASIZE)
        {
            // XamLoaderGetLaunchDataSize(DWORD* pcbData)
            if (!GuestReadable(base, c->r3.u32, 4))
                return 0;
            GuestPutU32(base, c->r3.u32, len);
            c->r3.u64 = 0;
            return 1;
        }
        // XamLoaderGetLaunchData(void* pBuffer, DWORD cbBuffer)
        unsigned int cb = c->r4.u32;
        if (!cb || cb > 0x1000u || !GuestReadable(base, c->r3.u32, cb))
            return 0;
        unsigned int n = zz_dash_launch_replay_copy(reinterpret_cast<void*>(base + c->r3.u32), cb);
        zz_dash_logf("ZZLAUNCH GetLaunchData answered locally: %u of %u B -> 0\n", n, cb);
        c->r3.u64 = 0;
        return 1;
    }

    unsigned int ret;
    if (i == ZZ_DI_XUSBCAMDESTROY)
        ret = 0;   // destroying a handle we never created must not fail
    else if (i == ZZ_DI_XUSBCAMCREATE || i == ZZ_DI_XUSBCAMGETSTATE || i == ZZ_DI_XUSBCAMGETCONFIG
             || i == ZZ_DI_XUSBCAMSETCONFIG || i == ZZ_DI_XUSBCAMSETVIEW
             || i == ZZ_DI_XUSBCAMSETCAPTUREMODE || i == ZZ_DI_XUSBCAMREADFRAME)
        ret = kErrorDeviceNotConnected;
    else
        return 0;   // not ours after all: let it go to the real import

    static volatile long s_said;
    if (InterlockedIncrement(&s_said) <= 8)
        zz_dash_logf("ZZIMP camera import %u answered locally -> %08X (no camera)\n", idx, ret);
    c->r3.u64 = ret;
    return 1;
}

// The wrap path (zz_dash_hook.h): the real import has run, its result is in r3.
extern "C" void zz_dash_import_ret(uintptr_t, uintptr_t, unsigned int idx)
{
    // The keystroke poll runs once per frame on the UI thread from the moment the dash's
    // UI is up -- the right time to start the Guide-button poll thread (once).
    if ((int)idx == ZZ_DI_XAMINPUTGETKEYSTROKEEX)
        zz_dash_guide_poll();
}
