// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Fable II: the guest timebase (mftb). Ported from work\ppc\zz_time.cpp (NFS, s23);
// the NFS-only zz_time_status() reporter is NOT ported -- it reads hardcoded NFS
// guest addresses and is only ever called by zz_profile.cpp, which this build omits.
//
// WHY IT MATTERS HERE: XenonRecomp lowers `mftb` to __rdtsc(). The Xbox 360 timebase
// ticks at 50 MHz and MS's own recompiled kernel (xeo3_5d12c3b3_00175344.dll, +0x24a41)
// answers KeQueryPerformanceFrequency with 50,000,000, so a raw ~4 GHz TSC makes every
// mftb-derived delta ~70-100x too large. In NFS that turned into world time advancing
// 66.7 ms per rendered frame regardless of framerate.
//
// Fable II calls mftb from 654 sites in the recompiled image -- far more than NFS --
// so this is not an optional polish item for this title.
//
// Opt-in marker "zz_mftb.on" in the build folder (cwd). Without it the raw-TSC
// behaviour is kept exactly as before, so the A/B stays a file toggle.
#include <windows.h>
#include <intrin.h>
#include <stdint.h>

static LONGLONG           s_qpf;
static unsigned long long s_mult;   // (50e6 << 32) / qpf, fixed-point ticks per QPC count
static volatile LONG      s_init;   // 0 = not yet, 1 = raw TSC, 2 = 50 MHz timebase
static const unsigned long long ZZ_TB_HZ = 50000000ull;

static void zz_time_init()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_qpf  = f.QuadPart ? f.QuadPart : 1;
    s_mult = (ZZ_TB_HZ << 32) / (unsigned long long)s_qpf;   // 5e7<<32 < 2^64, no overflow
    LONG mode = (GetFileAttributesA("zz_mftb.on") != INVALID_FILE_ATTRIBUTES) ? 2 : 1;
    InterlockedExchange(&s_init, mode);
}

extern "C" unsigned long long zz_mftb(void)
{
    LONG mode = s_init;
    if (!mode) { zz_time_init(); mode = s_init; }
    if (mode == 1) return __rdtsc();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    // 50 MHz timebase: (t * mult) >> 32 with a 128-bit product (one MUL, no libcall --
    // a 128-bit divide would need compiler-rt's __udivti3).
    return (unsigned long long)(((unsigned __int128)(unsigned long long)t.QuadPart * s_mult) >> 32);
}

extern "C" unsigned long long zz_mftb_raw(void) { return __rdtsc(); }

extern "C" int zz_mftb_mode(void)
{
    LONG mode = s_init;
    if (!mode) { zz_time_init(); mode = s_init; }
    return (int)mode;
}
