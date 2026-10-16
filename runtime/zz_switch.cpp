// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Fable II: the jump-table default-arm reporter.
//
// WHY THIS EXISTS. s3 died at sub_822D8180 with an unguarded
// `movsxd rax,[rcx+rax*4]` -- clang had deleted the bounds check because the
// recompiler emitted `default: __builtin_unreachable()`, and switch.toml
// recorded four of the guest's five labels. The failure surfaced as an
// anonymous c0000005 at a host RVA and cost a launch, a relink with /MAP and a
// host disassembly just to learn WHICH guest instruction it was.
//
// The default arm is now live and falls back to PPC_CALL_INDIRECT_FUNC(ctr).
// This hook turns that fallback from silent into named: it records the guest
// dispatch address, the index that missed, and the target ctr held.
//
// WHAT A LINE IN THIS FILE MEANS. The guest guards a table with cmplwi/bgt
// BEFORE the bctr, so an in-range index is the only thing that reaches the
// dispatch. A correct translation therefore never takes the default arm. Every
// line here is a table whose recorded case list is still short -- i.e. exactly
// the s3 defect, but survivable now, and named. An EMPTY (or absent) file on a
// healthy run is the expected result.
//
// Modelled on zz_setjmp.cpp in this folder, deliberately: same open-once
// pattern, same hard cap, same "the log is the evidence" reasoning.
//
// Log: <game>_switchmiss.log in the working directory (= the the console folder console
// folder, which is what run_fable2_v5.cmd sets).
#include <windows.h>
#include <stdio.h>

extern "C" void zz_switch_miss(unsigned int addr, unsigned int index,
                               unsigned int target);

static CRITICAL_SECTION s_cs;
static volatile LONG    s_init;
static FILE*            s_fp;

static void zz_switch_open()
{
    if (InterlockedCompareExchange(&s_init, 1, 0) != 0)
    {
        while (s_init < 2) Sleep(0);
        return;
    }
    InitializeCriticalSection(&s_cs);
    fopen_s(&s_fp, ZZ_TITLE "_switchmiss.log", "w");
    InterlockedExchange(&s_init, 2);
}

extern "C" void zz_switch_miss(unsigned int addr, unsigned int index,
                               unsigned int target)
{
    static volatile LONG s_count = 0;

    // A miss inside a hot loop could otherwise fill the disk. 200 is plenty to
    // identify every distinct site; the count keeps rising so the last line
    // still reports the true total.
    const LONG n = InterlockedIncrement(&s_count);
    if (n > 200)
        return;

    zz_switch_open();
    if (!s_fp)
        return;

    EnterCriticalSection(&s_cs);
    fprintf(s_fp, "ZZSWITCHMISS #%ld bctr=%08X index=%u ctr=%08X\n",
            n, addr, index, target);
    if (n == 200)
        fprintf(s_fp, "ZZSWITCHMISS capped at 200; further misses not logged\n");
    fflush(s_fp);
    LeaveCriticalSection(&s_cs);
}
