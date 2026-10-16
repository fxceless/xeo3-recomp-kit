// Crackdown s3 A3 (2026-10-14): the D3D lock / allocation instrument.
//
// WHY. Run 4's Campaign crash is the renderer's CPU auto-exposure (sub_825AA008) reading a
// read-only-locked 320x180x32 surface at 0xDFCC5000. D3D's generic resource lock
// sub_826AC8F0 turns the texture header's GPU address into `0xC0000000 | physical` (+0x1000
// for an address that is itself in the 0xE window; 826ACB3C-826ACB50), so the CPU pointer
// is in the 16M-page window whatever window the memory was allocated through -- and D3D's
// pool allocates 4 KB pages (MmAllocatePhysicalMemoryEx protect 0x404 / 0x4 = 0xE window).
// Two things decide the fix (S3_PLAN §4) and both are measured here:
//   (a) is the 0xC alias of a page committed through 0xE visible (one shared section) or not;
//   (b) was physical 0x1FCC5000 ever allocated by the game at all.
//
// HOW. A strong PPC_FUNC(sub_826AC8F0) over the recompiler's weak wrapper calls the real
// body and returns EXACTLY what it returned (no behaviour change in build 14). Around it:
// r3 = resource, r8 = GPU address, r10 = lock flags (0x10 = READONLY) in; r3 = CPU address
// out. VirtualQuery(base + cpu) names the page state. The allocation and
// MmGetPhysicalAddress answers zz_save.cpp's observe path sees (crackdown_observe.on) are
// kept in two rings here, so a LOCK line can name the allocation that owns its physical page
// and whether THAT alias is committed. One line per lock for the first 64 locks, and for
// every lock whose page is not MEM_COMMIT (first 2000, then every 100th).
//
//   crackdown_mem.log (console folder)
//     ALLOC va=.. size=.. prot=.. -> phys=..          (from the observe path)
//     GPA   va=.. -> phys=..                          (from the observe path)
//     LOCK  res=.. gpu=.. flags=.. -> cpu=.. page=COMMIT|RESERVE|FREE  owner=<va..va+size> alias=<va> alias_page=..
//           | gpa=<va> gpa_page=.. | owner=none
//
// Guest-fiber context: no host lock, no allocation (zz_wlog.h; the rings are static).
#include <windows.h>
#include <stdint.h>
#include "ppc_recomp_shared.h"
#include "zz_wlog.h"

PPC_FUNC_IMPL(__imp__sub_826AC8F0);

static ZzWLog g_ml = { "crackdown_mem.log" };
// Copy this file into games\crackdown\ppc\ after scripts\recompile.ps1. It also needs the two
// zz_lock_note_* calls in zz_save.cpp's zz_obs_log (see the comment there) to fill its rings.

#define ZZ_RING 4096
struct ZzAlloc { volatile uint32_t va, size; };
struct ZzGpa   { volatile uint32_t va, phys; };
static ZzAlloc g_alloc[ZZ_RING]; static volatile LONG g_nalloc;
static ZzGpa   g_gpa[ZZ_RING];   static volatile LONG g_ngpa;
static volatile LONG g_nlock, g_nbad;

// The console's three physical windows: 0xA0000000 (64K pages) and 0xC0000000 (16M pages)
// are physical + window; 0xE0000000 (4K pages) is physical + window - 0x1000 (the D3D
// helper adds the 0x1000 back for an E-window GPU address). Below 0xA0000000: not physical.
static uint32_t zz_phys_of(uint32_t va)
{
    if (va >= 0xE0000000u) return (va & 0x1FFFFFFFu) + 0x1000u;
    if (va >= 0xA0000000u) return va & 0x1FFFFFFFu;
    return 0xFFFFFFFFu;
}

static const char* zz_page(uintptr_t base, uint32_t va)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((const void*)(base + va), &mbi, sizeof(mbi))) return "QUERYFAIL";
    if (mbi.State == MEM_COMMIT)  return "COMMIT";
    if (mbi.State == MEM_RESERVE) return "RESERVE";
    if (mbi.State == MEM_FREE)    return "FREE";
    return "?";
}

static uintptr_t g_base;            // guest memory base, captured at the first lock

extern "C" void zz_lock_note_alloc(uint32_t va, uint32_t size, uint32_t prot)
{
    if (!va) return;
    const LONG k = InterlockedIncrement(&g_nalloc) - 1;
    g_alloc[k % ZZ_RING].size = size; g_alloc[k % ZZ_RING].va = va;
    if (k < 4000)
        zz_wlog(&g_ml, "ALLOC va=%08X size=%08X prot=%08X -> phys=%08X  #%ld", va, size, prot, zz_phys_of(va), (long)k + 1);
}

extern "C" void zz_lock_note_gpa(uint32_t va, uint32_t phys)
{
    const LONG k = InterlockedIncrement(&g_ngpa) - 1;
    g_gpa[k % ZZ_RING].phys = phys; g_gpa[k % ZZ_RING].va = va;
    if (k < 4000) zz_wlog(&g_ml, "GPA   va=%08X -> phys=%08X  #%ld", va, phys, (long)k + 1);
}

// s4 A1: page state as hex AND words, for the LOCKC lines.
static void zz_pinfo(uintptr_t base, uint32_t va, char* out, size_t cap)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((const void*)(base + va), &mbi, sizeof(mbi))) { _snprintf_s(out, cap, _TRUNCATE, "QUERYFAIL"); return; }
    const char* st = mbi.State == MEM_COMMIT ? "COMMIT" : mbi.State == MEM_RESERVE ? "RESERVE" : mbi.State == MEM_FREE ? "FREE" : "?";
    const DWORD pr = mbi.Protect & 0xFF;
    const char* pw = pr == PAGE_NOACCESS ? "NOACCESS" : pr == PAGE_READONLY ? "RO" : pr == PAGE_READWRITE ? "RW"
                   : pr == PAGE_WRITECOPY ? "WCOPY" : pr == PAGE_EXECUTE_READWRITE ? "XRW" : pr == PAGE_EXECUTE_READ ? "XR"
                   : pr == 0 ? "-" : "other";
    const char* ty = mbi.Type == MEM_PRIVATE ? "PRIVATE" : mbi.Type == MEM_MAPPED ? "MAPPED" : mbi.Type == MEM_IMAGE ? "IMAGE" : "-";
    _snprintf_s(out, cap, _TRUNCATE, "%s/%s%s%s/%s (%lX %lX %lX) ab=%08llX",
                st, pw, (mbi.Protect & PAGE_GUARD) ? "+GUARD" : "", (mbi.Protect & PAGE_WRITECOMBINE) ? "+WC" : "", ty,
                (unsigned long)mbi.State, (unsigned long)mbi.Protect, (unsigned long)mbi.Type,
                (unsigned long long)((uintptr_t)mbi.AllocationBase - base));
}

static bool zz_readable(uintptr_t base, uint32_t va)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((const void*)(base + va), &mbi, sizeof(mbi))) return false;
    return mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

// The allocation that owns a physical page (allocation ring first, the GPA ring as a fallback).
static void zz_owner(uintptr_t base, uint32_t cpu, char* who, size_t cap)
{
    _snprintf_s(who, cap, _TRUNCATE, "owner=none");
    const uint32_t ph = zz_phys_of(cpu);
    if (ph == 0xFFFFFFFFu) return;
    const LONG na = g_nalloc < ZZ_RING ? g_nalloc : ZZ_RING;
    for (LONG i = 0; i < na; i++)
    {
        const uint32_t va = g_alloc[i].va, sz = g_alloc[i].size, p0 = zz_phys_of(va);
        if (p0 == 0xFFFFFFFFu || ph < p0 || ph - p0 >= sz) continue;
        const uint32_t alias = va + (ph - p0);
        _snprintf_s(who, cap, _TRUNCATE, "owner=%08X..%08X alias=%08X alias_page=%s", va, va + sz, alias, zz_page(base, alias));
        return;
    }
    const LONG ng = g_ngpa < ZZ_RING ? g_ngpa : ZZ_RING;
    for (LONG i = 0; i < ng; i++)
    {
        if ((g_gpa[i].phys & ~0xFFFu) != (ph & ~0xFFFu)) continue;
        const uint32_t alias = (g_gpa[i].va & ~0xFFFu) | (ph & 0xFFFu);
        _snprintf_s(who, cap, _TRUNCATE, "gpa=%08X gpa_page=%s", alias, zz_page(base, alias));
        return;
    }
}

static void zz_lock_log(uintptr_t base, uint32_t res, uint32_t gpu, uint32_t flags, uint32_t cpu, uint32_t lr)
{
    const LONG n = InterlockedIncrement(&g_nlock);
    const char* pg = zz_page(base, cpu);
    const bool bad = pg[0] != 'C';
    LONG nb = 0;
    if (bad) nb = InterlockedIncrement(&g_nbad);
    if (!(n <= 64 || (bad && (nb <= 2000 || (nb % 100) == 0)))) return;

    char who[160];
    zz_owner(base, cpu, who, sizeof(who));
    zz_wlog(&g_ml, "LOCK  res=%08X gpu=%08X flags=%X -> cpu=%08X page=%s %s  lr=%08X  #%ld%s",
            res, gpu, flags, cpu, pg, who, lr, (long)n, bad ? "  <<< NOT COMMITTED" : "");
}

// ---------------------------------------------------------------------------------------------
// Crackdown s4 (2026-10-14, S4_PLAN §1 / A1 / A2): the read-only lock.
//
// sub_826AC8F0 hands back `0xC0000000 | phys` ONLY for a D3DLOCK_READONLY lock (826ACAB4
// flags & 0x10 -> 826ACB34 r6 != 0 -> 826ACB3C-826ACB50); every other lock returns the
// game's own E/A-window pointer. Run 5 crashed on the run's first read-only lock (the
// auto-exposure histogram, sub_825AA008, ~50 s): READ 0xDFCC5000, while the game allocated
// and committed the same physical page through the E window (FFCC3000.., data page FFCC4000).
// In Emu the three windows are views of one section and commit is per view (project notes,
// "HOW EMU'S GUEST MEMORY ACTUALLY WORKS"), so the conjecture is: the C view is not
// committed. Build 15:
//   * LOCKC: one line for EVERY lock that returns a 0xC-window pointer (first 2000), with the
//     state of the C page, its E alias and its A alias (state/protect/type, words + hex), and
//     the owning allocation.
//   * C-1: <console folder>\crackdown_lock_alias.on -> when the C page is not readable and the E (or
//     A) alias is, return the alias instead (the console's windows alias the same RAM; the
//     game owns the E view). crackdown_lock_alias_all.on -> translate every 0xC result to the
//     E alias unconditionally (the K-c fallback). LOCK FIX / LOCK KEEP lines are written
//     whatever the markers say: they are the measurement.
//   * a 64-entry ring of the last locks (all kinds) for zz_crash.cpp's fault record, and the
//     guest base for zz_xmamm.cpp's context dump.
// The unlock (sub_826ACB80) recomputes the C alias itself and only passes it to the dcbf
// loop sub_826C4C70 (no loads or stores of the range; dcbf compiles to nothing), so the
// translation is invisible to it.
static int g_alias_on, g_alias_all;
static int zz_lock_init()
{
    g_alias_on  = GetFileAttributesA("crackdown_lock_alias.on")     != INVALID_FILE_ATTRIBUTES;
    g_alias_all = GetFileAttributesA("crackdown_lock_alias_all.on") != INVALID_FILE_ATTRIBUTES;
    return 1;
}
static int g_lock_init = zz_lock_init();
static void zz_lock_dump_ring(void (*emit)(const char*));
typedef void (*zz_game_dump_fn)(void (*emit)(const char*));
extern "C" zz_game_dump_fn zz_game_dump;
static int g_dump_reg = (zz_game_dump = zz_lock_dump_ring, 1);

static volatile LONG g_nro, g_n270, g_nlockc, g_nfix;

struct ZzLockRec { volatile uint32_t res, gpu, flags, cpu, lr, out; volatile ULONGLONG t; volatile LONG seq; };
static ZzLockRec g_lring[64];
static volatile LONG g_lringn;

// The kit's runtime owns zz_guest_base(); this file adds its lock ring to fault records
// through zz_game_dump (zz_import.cpp).
static void zz_lock_dump_ring(void (*emit)(const char*))
{
    char b[256];
    const LONG n = g_lringn;
    _snprintf_s(b, sizeof(b), _TRUNCATE, "  lock ring: %ld locks, %ld read-only, %ld from lr=826AE270, %ld LOCKC, %ld translated; "
                "markers alias=%d alias_all=%d; guest base %p; the last %ld:",
                (long)g_nlock, (long)g_nro, (long)g_n270, (long)g_nlockc, (long)g_nfix, g_alias_on, g_alias_all,
                (void*)g_base, (long)(n < 64 ? n : 64));
    emit(b);
    const ULONGLONG now = GetTickCount64();
    for (LONG k = (n > 64 ? n - 64 : 0); k < n; k++)
    {
        const ZzLockRec& r = g_lring[k % 64];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "    #%ld res=%08X gpu=%08X flags=%X -> cpu=%08X ret=%08X lr=%08X  %llu ms ago",
                    (long)r.seq, r.res, r.gpu, r.flags, r.cpu, r.out, r.lr, (unsigned long long)(now - r.t));
        emit(b);
    }
}

// A lock whose result is in the 0xC window: log it, apply C-1, return what the guest gets.
static uint32_t zz_lock_c(uintptr_t base, uint32_t res, uint32_t gpu, uint32_t flags, uint32_t cpu, uint32_t lr)
{
    const LONG k = InterlockedIncrement(&g_nlockc);
    const uint32_t phys = cpu & 0x1FFFFFFFu;
    const uint32_t e = phys >= 0x1000u ? 0xE0000000u + (phys - 0x1000u) : 0;   // inverse of zz_phys_of
    const uint32_t a = 0xA0000000u + phys;
    const bool rc = zz_readable(base, cpu), re = e && zz_readable(base, e), ra = zz_readable(base, a);
    if (k <= 2000)
    {
        char pc_[96], pe[96] = "n/a", pa[96], who[160];
        zz_pinfo(base, cpu, pc_, sizeof(pc_));
        if (e) zz_pinfo(base, e, pe, sizeof(pe));
        zz_pinfo(base, a, pa, sizeof(pa));
        zz_owner(base, cpu, who, sizeof(who));
        zz_wlog(&g_ml, "LOCKC res=%08X gpu=%08X flags=%X -> cpu=%08X lr=%08X | C: %s | E %08X: %s | A %08X: %s | %s  #%ld",
                res, gpu, flags, cpu, lr, pc_, e, pe, a, pa, who, (long)k);
    }
    uint32_t out = cpu;
    const char* why;
    if (g_alias_all && e)  { out = e; why = "FIX(all) -> e"; }
    else if (!g_alias_on)  why = rc ? "KEEP (marker off; C readable)" : re ? "KEEP (marker off; would -> e)"
                               : ra ? "KEEP (marker off; would -> a)" : "KEEP (marker off; no alias readable)";
    else if (!rc && re)    { out = e; why = "FIX -> e"; }
    else if (!rc && ra)    { out = a; why = "FIX -> a"; }
    else                   why = rc ? "KEEP (C readable)" : "KEEP (no alias readable)";
    if (out != cpu) InterlockedIncrement(&g_nfix);
    if (k <= 2000)
        zz_wlog(&g_ml, "LOCK %s  gpu=%08X cpu=%08X -> %08X  readable C=%d E=%d A=%d  #%ld", why, gpu, cpu, out, rc, re, ra, (long)k);
    return out;
}

PPC_FUNC(sub_826AC8F0)
{
    const uint32_t res = ADJ(ctx)->r3.u32, gpu = ADJ(ctx)->r8.u32, flags = ADJ(ctx)->r10.u32;
    const uint32_t lr = (uint32_t)ADJ(ctx)->lr;
    if (!g_base) g_base = (uintptr_t)base;
    __imp__sub_826AC8F0(ctx, base, frame);
    const uint32_t cpu = ADJ(ctx)->r3.u32;
    if (flags & 0x10) InterlockedIncrement(&g_nro);
    if (lr == 0x826AE270u) InterlockedIncrement(&g_n270);
    zz_lock_log((uintptr_t)base, res, gpu, flags, cpu, lr);
    uint32_t out = cpu;
    if (cpu - 0xC0000000u < 0x20000000u) out = zz_lock_c((uintptr_t)base, res, gpu, flags, cpu, lr);
    const LONG s = InterlockedIncrement(&g_lringn);
    ZzLockRec& r = g_lring[(s - 1) % 64];
    r.res = res; r.gpu = gpu; r.flags = flags; r.cpu = cpu; r.lr = lr; r.out = out; r.t = GetTickCount64(); r.seq = s;
    ADJ(ctx)->r3.u32 = out;
}
