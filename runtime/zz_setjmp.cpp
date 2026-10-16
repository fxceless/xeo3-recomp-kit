// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Fable II: the reporting hook that PPC_SETJMP / PPC_LONGJMP require.
//
// ppc_context.h: "Every build that uses PPC_SETJMP must define it (an empty body is
// fine)." An empty body would link, but this build has a specific question to answer,
// so it counts instead -- see below.
//
// Ported in shape (not verbatim) from work_sysmods\dash\ppc\zz_dash_probe.cpp (s35).
// The dash's version writes through its own Emit(); this module has no logging layer,
// so it writes its own small file.
//
// WHAT THE VALUES MEAN
//     0  a setjmp armed
//    -1  a longjmp is being thrown
//    >0  that longjmp LANDED back at its setjmp, delivering this value
//
// WHY IT EARNS ITS PLACE HERE. The s1 build shipped with a STALE XenonRecomp binary
// (Aug 17) that predated the s35 setjmp/longjmp commit (Aug 18), so all 21 of this
// game's longjmp call sites emitted the upstream form and no unwind could ever happen
// -- the guest longjmp just returned to its caller. A "LANDED" line in the log is the
// direct proof that the mechanism now works; its ABSENCE while THROWN lines appear
// would mean the fix is still not doing its job. Without this hook we would be
// inferring that from a crash address again.
//
// Log: <game>_setjmp.log in the working directory (= the the console folder console folder).
// Arming happens on every pcall, so arms are capped hard and throws/lands kept.
#include <windows.h>
#include <stdio.h>
#include "ppc_context.h"

static CRITICAL_SECTION s_cs;
static volatile LONG    s_init;
static FILE*            s_fp;

static void zz_setjmp_open()
{
    if (InterlockedCompareExchange(&s_init, 1, 0) != 0)
    {
        while (s_init < 2) Sleep(0);
        return;
    }
    InitializeCriticalSection(&s_cs);
    fopen_s(&s_fp, ZZ_TITLE "_setjmp.log", "w");
    InterlockedExchange(&s_init, 2);
}

extern "C" void zz_setjmp_note(uintptr_t ctx, uintptr_t guest_jmpbuf, int value)
{
    static volatile LONG s_armed = 0, s_thrown = 0, s_landed = 0;
    LONG n;
    const char* what;
    if (value == 0)     { n = InterlockedIncrement(&s_armed);  what = "armed";  }
    else if (value < 0) { n = InterlockedIncrement(&s_thrown); what = "THROWN"; }
    else                { n = InterlockedIncrement(&s_landed); what = "LANDED"; }

    if ((value == 0 && n > 8) || n > 200)
        return;

    zz_setjmp_open();
    if (!s_fp)
        return;

    PPCContext* c = reinterpret_cast<PPCContext*>(ctx - 0x80);
    EnterCriticalSection(&s_cs);
    fprintf(s_fp, "ZZSETJMP %s #%ld buf=%08X value=%d r1=%08X lr=%08X\n",
            what, (long)n, (unsigned)guest_jmpbuf, value,
            c->r1.u32, (unsigned)c->lr);
    fflush(s_fp);
    LeaveCriticalSection(&s_cs);
}
