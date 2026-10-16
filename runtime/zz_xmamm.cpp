// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Crackdown s3 A2 (2026-10-14): the MMIO wrapper + the XMA register log.
//
// WHY. This XDK-4802 XAudio drives the XMA hardware itself through the register block at
// 0x7FEA0000 (S3_PLAN §2.1): context-array base read at +0x1800, reset at +0x1804, the
// current index at +0x1818, and the CLEAR (+0x1A80) / LOCK (+0x1A40) / KICK (+0x1940) bit
// arrays. Build 14 lists those 36 instructions in config.toml's mmio_instructions, so they
// now reach Emu's MMIO callbacks (PrecompiledPointers[8] read / [9] write) instead of plain
// memory. What is NOT known is whether Emu's MMIO dispatcher routes 0x7FEA into its XMA
// model at all (NFS never exercised it: its XAudio used the kernel XMA exports), and whether
// Emu's XMA worker runs (gate byte Emu+0x12C115, written only by the bare `xma` launch flag;
// NFS s23). This file answers both in one launch, without changing any access:
//
//   <console folder>\<game>_xma.log
//     t_ms tid R|W addr hw=<value Emu returned / was given> be=<the same word as a guest
//     lwz/stw would see it>   -- first 400 XMA accesses. For lwbrx/stwbrx (34 of the 36
//     sites) the guest REGISTER equals hw; for the two stwx reset writes it equals be.
//     PP8/PP9 = the module-relative targets of Emu's MMIO callbacks (disassemble with
//     tools\re\emu_fn.py on the console folder\Emu.exe); "xma worker gate = N" read at the first XMA
//     access and again at t = 5 s and 30 s from a host thread (the flag may be parsed late).
//
// The wrapper forwards the six GPU (0x7FC8) sites untouched -- byte for byte the
// ppc_context.h definitions. Guest-fiber context: lock-free logger (zz_wlog.h).
#include <windows.h>
#include <stdint.h>
#include "ppc_context.h"
#include "zz_wlog.h"

#define ZZ_XMA_LO   0x7FEA0000u
#define ZZ_XMA_HI   0x7FEB0000u
#define ZZ_XMA_MAX  400
#define ZZ_GATE_RVA 0x12C115u        // measured on Emu.exe md5 e6058f11 (docs\CONSOLE_FOLDER.md)

static ZzWLog g_xl = { ZZ_TITLE "_xma.log" };
static volatile LONG g_xn;          // XMA accesses seen (all, not just logged)
static volatile LONG g_first;

static unsigned zz_gate_byte(void)
{
    const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);   // PEB read, no lock
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(exe + ((const IMAGE_DOS_HEADER*)exe)->e_lfanew);
    if (ZZ_GATE_RVA >= nt->OptionalHeader.SizeOfImage) return 0x100;   // not this Emu
    return *(volatile uint8_t*)(exe + ZZ_GATE_RVA);
}

static void zz_xma_first(void)
{
    const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(exe + ((const IMAGE_DOS_HEADER*)exe)->e_lfanew);
    zz_wlog(&g_xl, "XMA first access: exe base=%p timestamp=%08X size=%08X  PP8=%p (exe+%llX)  PP9=%p (exe+%llX)  "
            "xma worker gate = %u   [hw = Emu's word; be = the guest lwz/stw view]",
            (void*)exe, (unsigned)nt->FileHeader.TimeDateStamp, (unsigned)nt->OptionalHeader.SizeOfImage,
            (void*)PrecompiledPointers[8], (unsigned long long)(PrecompiledPointers[8] - exe),
            (void*)PrecompiledPointers[9], (unsigned long long)(PrecompiledPointers[9] - exe),
            zz_gate_byte());
}

// ---------------------------------------------------------------------------------------------
// Crackdown s4 A3 (2026-10-14, S4_PLAN §2 / §3): what Emu's XMA model does with the contexts.
//
// Run 5: Emu's register model takes every kick (0x1940 = KICK, 0x1A40 = LOCK, 0x1A80 = CLEAR,
// the LE word is the context bit mask), 0x1818 reads 0 all run, and the game tears its four
// voices down and recreates them every ~250 ms -- the title plays 250 ms slivers of noise.
// Unknown: does Emu's worker write PCM into the guest output rings at all, in which byte
// order, and does it ever mirror progress (out_w, valid bits) back into the 64-byte context
// the game's poll (sub_82A52710) reads. Build 15 dumps both, changing nothing:
//   CTX idx @KICK|LOCK|POLL  decoded context (S4_PLAN §0.5 layout) -- 100 lines per kind, then
//                            every 100th event; CTXRAW = the 16 dwords, first 40 events
//   RING idx t_ms out_w out_r valid snap#  -- one line per ring snapshot appended to
//                            <console folder>\<game>_xma_ring<idx>.raw (contexts 0-3; taken on
//                            each poll and on a 20 ms host tick once a context was kicked,
//                            only when out_w or 4 sampled dwords changed; 400 per context)
//   CLEAR / SUM              past the first 400 accesses: one line per CLEAR write and a
//                            per-second summary, ceiling 4000; the first non-zero 0x1818 read
// Contexts live at 0xE0000000 + (reg1800 & 0x1FFFFFFF) - 0x1000 + idx*64 (C1FE6000 -> E1FE5000,
// the addresses XMAReleaseContext was given in run 5). Guest memory is read only after
// VirtualQuery says COMMIT and through a noinline copy under __try
// ([[reference_clangcl_try_inline_load]]). The tick thread and the guest threads never wait
// on each other: a per-context busy flag is TRY-acquired and the loser skips.
extern "C" uintptr_t zz_guest_base(void);           // zz_import.cpp

static volatile uint32_t g_base1800;
static volatile LONG g_kicked;                      // contexts 0-31 ever kicked (bit mask)
static volatile LONG g_ckick, g_clock, g_cclear, g_cpoll, g_cpollnz, g_ccreate;
static volatile LONG g_nctx[3], g_nctxraw, g_nextra, g_nzfirst;

extern "C" void zz_xma_note_create(void) { InterlockedIncrement(&g_ccreate); }   // zz_save.cpp observe path

__declspec(noinline) static void zz_copy_raw(uint32_t* d, const volatile uint32_t* s, size_t ndw)
{
    for (size_t i = 0; i < ndw; i++) d[i] = s[i];
}
static bool zz_copy(uint32_t* d, uintptr_t s, size_t ndw)
{
    __try { zz_copy_raw(d, (const volatile uint32_t*)s, ndw); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool zz_rd(uintptr_t base, uint32_t va, size_t n)
{
    for (uint32_t off = 0; off < n; off += 0x1000)   // every page the read touches
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void*)(base + ((va + off) & ~0xFFFu)), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((const void*)(base + va + n - 1), &mbi, sizeof(mbi))) return false;
    return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

// A physical address -> a readable guest VA: the E alias first, then A, then C.
static uint32_t zz_phys_va(uintptr_t base, uint32_t phys, size_t n, char* win)
{
    phys &= 0x1FFFFFFFu;
    if (phys >= 0x1000u && zz_rd(base, 0xE0000000u + phys - 0x1000u, n)) { *win = 'E'; return 0xE0000000u + phys - 0x1000u; }
    if (zz_rd(base, 0xA0000000u + phys, n)) { *win = 'A'; return 0xA0000000u + phys; }
    if (zz_rd(base, 0xC0000000u + phys, n)) { *win = 'C'; return 0xC0000000u + phys; }
    *win = '-'; return 0;
}

// The 16 dwords of context idx, guest order (bswapped). false = not readable yet.
static bool zz_ctx_read(int idx, uint32_t d[16], uint32_t* va_out)
{
    const uintptr_t base = zz_guest_base();
    const uint32_t r = g_base1800;
    if (!base || !r) return false;
    const uint32_t va = 0xE0000000u + ((r & 0x1FFFFFFFu) - 0x1000u) + (uint32_t)idx * 64u;
    *va_out = va;
    if (!zz_rd(base, va, 64) || !zz_copy(d, base + va, 16)) return false;
    for (int i = 0; i < 16; i++) d[i] = _byteswap_ulong(d[i]);
    return true;
}

static const char* const kCtxKind[3] = { "KICK", "LOCK", "POLL" };

static void zz_ctx_log(int idx, int kind)
{
    const LONG n = InterlockedIncrement(&g_nctx[kind]);
    if (!(n <= 100 || (n % 100) == 0)) return;
    uint32_t d[16], va = 0;
    if (!zz_ctx_read(idx, d, &va)) { if (n <= 100) zz_wlog(&g_xl, "CTX %d @%s va=%08X not readable  #%ld", idx, kCtxKind[kind], va, (long)n); return; }
    // Emu reads/writes the context through the register's own window address (0x85CE0 /
    // 0x85DB0: guest base + reg1800 + idx*64 = the 0xC view); the game through the E view.
    // cview: 1 = the C view holds the same 64 bytes, 0 = it differs, -1 = not readable.
    int cview = -1;
    {
        uint32_t c[16];
        const uint32_t cva = g_base1800 + (uint32_t)idx * 64u;
        if (zz_rd(zz_guest_base(), cva, 64) && zz_copy(c, zz_guest_base() + cva, 16))
        {
            for (int i = 0; i < 16; i++) c[i] = _byteswap_ulong(c[i]);
            cview = memcmp(c, d, sizeof(c)) == 0;
        }
    }
    static const unsigned kRate[4] = { 24000, 32000, 44100, 48000 };
    zz_wlog(&g_xl, "CTX %d @%s va=%08X cview=%d  pkts0=%u v0=%u v1=%u blocks=%u out_w=%u | pkts1=%u sub=%u rate=%u st=%u out_valid=%u | "
            "rd=%u cur_in=%u in0=%08X in1=%08X out=%08X out_r=%u | raw-VA readable in0=%d out=%d  #%ld",
            idx, kCtxKind[kind], va, cview, d[0] & 0xFFF, (d[0] >> 20) & 1, (d[0] >> 21) & 1, (d[0] >> 22) & 31, d[0] >> 27,
            d[1] & 0xFFF, (d[1] >> 20) & 0x7F, kRate[(d[1] >> 27) & 3], (d[1] >> 29) & 1, d[1] >> 31,
            d[2], d[4] >> 31, d[5], d[6], d[7], d[9] & 31,
            d[5] ? (int)zz_rd(zz_guest_base(), d[5], 4) : -1, d[7] ? (int)zz_rd(zz_guest_base(), d[7], 4) : -1, (long)n);
    if (InterlockedIncrement(&g_nctxraw) <= 40)
        zz_wlog(&g_xl, "CTXRAW %d @%s %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                idx, kCtxKind[kind], d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
}

// One raw file per context and view, appended with single WriteFile calls (the zz_wlog
// pattern, no header).
struct ZzRaw { volatile HANDLE h; volatile LONG state; };
static ZzRaw g_raw[2][4];
static void zz_raw_write(int view, int idx, const void* p, DWORD n)
{
    ZzRaw* R = &g_raw[view][idx];
    if (R->state != 2)
    {
        if (InterlockedCompareExchange(&R->state, 1, 0) != 0) return;   // someone else is opening it
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE,
                    ZZ_TITLE "_xma_%sring%d.raw",
                    view ? "emu" : "", idx);
        R->h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        InterlockedExchange(&R->state, 2);
    }
    if (R->h == INVALID_HANDLE_VALUE || !R->h) return;
    DWORD w; WriteFile(R->h, p, n, &w, nullptr);
}

// TWO views of each output ring (s4 A5, Emu disassembly): the game reads its ring through a
// physical window (view 0: the E alias of dw7, then A, then C -- RING lines), but Emu's KICK
// handler (0x86364-0x863A8) hands its decoder `[Emu+0x1436D8] + dw7`, i.e. the RAW physical
// address used as a guest VA (view 1: EMURING lines, <game>_xma_emuring<idx>.raw). If Emu
// writes PCM only to view 1, the game never sees it.
struct ZzRingSnap { volatile LONG busy; int have; uint32_t out, w, samp[4]; LONG nsnap, nbad; uint32_t buf[2048]; };
static ZzRingSnap g_rs[2][4];

static void zz_ring_view(int view, int idx, const uint32_t d[16], char why)
{
    ZzRingSnap& S = g_rs[view][idx];
    if (S.nsnap >= 400 || InterlockedCompareExchange(&S.busy, 1, 0) != 0) return;
    const uintptr_t base = zz_guest_base();
    const uint32_t blocks = (d[0] >> 22) & 31, w = d[0] >> 27, r = d[9] & 31, valid = d[1] >> 31, out = d[7];
    const uint32_t size = blocks * 256u;
    char win = view ? 'R' : '-';
    uint32_t va = 0;
    if (size && out) va = view ? (zz_rd(base, out, size) ? out : 0) : zz_phys_va(base, out, size, &win);
    if (!va || !zz_copy(S.buf, base + va, size / 4))
    {
        if (size && out && ++S.nbad <= 8)
            zz_wlog(&g_xl, "%sRING %d out=%08X size=%u: not readable through %s (%c)", view ? "EMU" : "", idx, out, size,
                    view ? "the raw-physical guest VA Emu uses" : "the E/A/C windows", why);
    }
    else
    {
        const uint32_t q = size / 16;
        const uint32_t smp[4] = { S.buf[0], S.buf[q], S.buf[2 * q], S.buf[3 * q] };
        if (!S.have || S.out != out || S.w != w || memcmp(smp, S.samp, sizeof(smp)))
        {
            S.have = 1; S.out = out; S.w = w; memcpy(S.samp, smp, sizeof(smp));
            const LONG k = ++S.nsnap;
            zz_raw_write(view, idx, S.buf, size);
            zz_wlog(&g_xl, "%sRING %d t=%llu out_w=%u out_r=%u valid=%u snap=%ld size=%u out=%08X via %c%08X rate=%u st=%u (%c)",
                    view ? "EMU" : "", idx, (unsigned long long)(GetTickCount64() - g_xl.t0), w, r, valid, (long)k, size, out,
                    win, va, (d[1] >> 27) & 3, (d[1] >> 29) & 1, why);
        }
    }
    InterlockedExchange(&S.busy, 0);
}

static void zz_ring_check(int idx, char why)
{
    uint32_t d[16], cva = 0;
    if (!zz_ctx_read(idx, d, &cva)) return;
    zz_ring_view(0, idx, d, why);
    zz_ring_view(1, idx, d, why);
}

static inline void zz_xma_note(char rw, uint32_t addr, uint32_t hw)
{
    const LONG n = InterlockedIncrement(&g_xn);
    if (n == 1 && InterlockedExchange(&g_first, 1) == 0) zz_xma_first();
    if (n <= ZZ_XMA_MAX)
        zz_wlog(&g_xl, "%c %08X hw=%08X be=%08X  +%04X  #%ld", rw, addr, hw, _byteswap_ulong(hw),
                addr - ZZ_XMA_LO, (long)n);
    else if (rw == 'W' && addr - 0x7FEA1A80u < 0x10u && InterlockedIncrement(&g_nextra) <= 4000)
        zz_wlog(&g_xl, "CLEAR %08X hw=%08X  #%ld", addr, hw, (long)n);
    else if ((n % 5000) == 0)
        zz_wlog(&g_xl, "... %ld XMA accesses so far (last %c %08X hw=%08X)", (long)n, rw, addr, hw);
}

// KICK (0x1940..) / LOCK (0x1A40..) words: each set bit of the LE word is context word*32 + bit.
static void zz_xma_ctx_event(uint32_t addr, uint32_t hw, int kind)
{
    const uint32_t word = (addr - (kind == 0 ? 0x7FEA1940u : 0x7FEA1A40u)) / 4;
    for (uint32_t b = 0; b < 32; b++)
        if (hw & (1u << b))
        {
            const int idx = (int)(word * 32 + b);
            if (kind == 0 && idx < 32) _InterlockedOr(&g_kicked, (LONG)(1u << idx));
            zz_ctx_log(idx, kind);
            if (idx < 4) zz_ring_check(idx, kind == 0 ? 'K' : 'L');
        }
}

// s6 Plan B (zz_xma.cpp): when our decoder is on, the XMA block's writes are consumed here
// (never reach Emu, so Emu's model stays idle) and 0x1818 answers 0 ("hardware is not on one
// of yours": the poll compares reg ^ 0x200 with each entry's index). 0x1800, the context pool
// base Emu's XMACreateContext hands out, is still read from Emu.
extern "C" int  zz_xcd_on(void);
extern "C" void zz_xcd_set_base(uint32_t reg1800);
extern "C" void zz_xcd_kick(unsigned idx);
extern "C" void zz_xcd_lock(unsigned idx);
extern "C" void zz_xcd_clear(unsigned idx);
extern "C" void zz_xcd_reset_all(void);

static void zz_xcd_bits(uint32_t addr, uint32_t hw, uint32_t first, void (*fn)(unsigned))
{
    const uint32_t word = (addr - first) / 4;
    for (uint32_t b = 0; b < 32; b++)
        if (hw & (1u << b)) fn(word * 32 + b);
}

extern "C" uint32_t zz_mm_load32(uint32_t addr)
{
    if (__builtin_expect(addr == 0x7FEA1818u, 0) && zz_xcd_on())
    {
        zz_xma_note('R', addr, 0);
        InterlockedIncrement(&g_cpoll);
        return 0;
    }
    const uint32_t hw = reinterpret_cast<uint32_t(__fastcall*)(uint32_t)>(PrecompiledPointers[8])(addr);
    if (__builtin_expect(addr == 0x7FEA1800u, 0)) zz_xcd_set_base(hw);
    if (__builtin_expect(addr - ZZ_XMA_LO < ZZ_XMA_HI - ZZ_XMA_LO, 0))
    {
        zz_xma_note('R', addr, hw);
        if (addr == 0x7FEA1800u) g_base1800 = hw;
        else if (addr == 0x7FEA1818u)
        {
            InterlockedIncrement(&g_cpoll);
            if (hw)
            {
                InterlockedIncrement(&g_cpollnz);
                if (InterlockedExchange(&g_nzfirst, 1) == 0) zz_wlog(&g_xl, "FIRST NON-ZERO 0x1818 read: hw=%08X", hw);
            }
            for (int i = 0; i < 4; i++) { zz_ctx_log(i, 2); zz_ring_check(i, 'P'); }
        }
    }
    return __builtin_bswap32(hw);
}

extern "C" void zz_mm_store32(uint32_t addr, uint32_t value)
{
    const uint32_t hw = __builtin_bswap32(value);
    const bool xma = __builtin_expect(addr - ZZ_XMA_LO < ZZ_XMA_HI - ZZ_XMA_LO, 0);
    if (xma) zz_xma_note('W', addr, hw);
    if (xma && addr - 0x7FEA1800u < 0x400u && zz_xcd_on())
    {   // consumed: our register model
        if (addr - 0x7FEA1940u < 0x10u)      { InterlockedIncrement(&g_ckick);  zz_xcd_bits(addr, hw, 0x7FEA1940u, zz_xcd_kick);
                                               zz_xma_ctx_event(addr, hw, 0); }   // CTX line + ring snapshot (s4 instruments)
        else if (addr - 0x7FEA1A40u < 0x10u) { InterlockedIncrement(&g_clock);  zz_xcd_bits(addr, hw, 0x7FEA1A40u, zz_xcd_lock);
                                               zz_xma_ctx_event(addr, hw, 1); }
        else if (addr - 0x7FEA1A80u < 0x10u) { InterlockedIncrement(&g_cclear); zz_xcd_bits(addr, hw, 0x7FEA1A80u, zz_xcd_clear); }
        else if (addr == 0x7FEA1804u && hw)  zz_xcd_reset_all();
        return;
    }
    reinterpret_cast<void(__fastcall*)(uint32_t, uint32_t)>(PrecompiledPointers[9])(addr, hw);
    if (xma)
    {   // after the forward: the context as Emu left it
        if (addr - 0x7FEA1940u < 0x10u)      { InterlockedIncrement(&g_ckick);  zz_xma_ctx_event(addr, hw, 0); }
        else if (addr - 0x7FEA1A40u < 0x10u) { InterlockedIncrement(&g_clock);  zz_xma_ctx_event(addr, hw, 1); }
        else if (addr - 0x7FEA1A80u < 0x10u) InterlockedIncrement(&g_cclear);
    }
}

// Host thread: the 20 ms ring tick once a context has been kicked, and the per-second SUM.
static DWORD WINAPI zz_xma_tick_thread(LPVOID)
{
    ULONGLONG last = GetTickCount64();
    LONG p[6] = {};
    for (;;)
    {
        Sleep(20);
        if (!PPCFuncMappings[0].host_rva || !g_base1800) continue;
        for (int i = 0; i < 4; i++) if (g_kicked & (1 << i)) zz_ring_check(i, 'T');
        const ULONGLONG now = GetTickCount64();
        if (now - last < 1000) continue;
        last = now;
        const LONG c[6] = { g_ckick, g_clock, g_cclear, g_cpoll, g_cpollnz, g_ccreate };
        if (g_xn > ZZ_XMA_MAX && InterlockedIncrement(&g_nextra) <= 4000)
            zz_wlog(&g_xl, "SUM t=%llus kicks=%ld locks=%ld clears=%ld polls=%ld poll_nonzero=%ld creates=%ld  (totals %ld/%ld/%ld/%ld/%ld/%ld; "
                    "snaps game %ld/%ld/%ld/%ld emu %ld/%ld/%ld/%ld)",
                    (unsigned long long)((now - g_xl.t0) / 1000), (long)(c[0] - p[0]), (long)(c[1] - p[1]), (long)(c[2] - p[2]),
                    (long)(c[3] - p[3]), (long)(c[4] - p[4]), (long)(c[5] - p[5]),
                    (long)c[0], (long)c[1], (long)c[2], (long)c[3], (long)c[4], (long)c[5],
                    (long)g_rs[0][0].nsnap, (long)g_rs[0][1].nsnap, (long)g_rs[0][2].nsnap, (long)g_rs[0][3].nsnap,
                    (long)g_rs[1][0].nsnap, (long)g_rs[1][1].nsnap, (long)g_rs[1][2].nsnap, (long)g_rs[1][3].nsnap);
        memcpy(p, c, sizeof(p));
    }
}

// Host thread: the gate byte at t = 5 s and 30 s. Only the twin Emu actually initialised
// (PPCFuncMappings filled in by InitPrecompiledDll) writes, so an idle twin stays silent.
static DWORD WINAPI zz_xma_gate_thread(LPVOID)
{
    const DWORD at[2] = { 5000, 30000 };
    DWORD slept = 0;
    for (DWORD t : at)
    {
        Sleep(t - slept); slept = t;
        if (!PPCFuncMappings[0].host_rva) continue;
        const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
        HMODULE m8 = nullptr, m9 = nullptr; char n8[MAX_PATH] = "?", n9[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)PrecompiledPointers[8], &m8);
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)PrecompiledPointers[9], &m9);
        if (m8) GetModuleFileNameA(m8, n8, MAX_PATH);
        if (m9) GetModuleFileNameA(m9, n9, MAX_PATH);
        zz_wlog(&g_xl, "GATE t=%lus: xma worker gate = %u  (exe base %p)  XMA accesses so far %ld  "
                "PP8 in %s +%llX  PP9 in %s +%llX", t / 1000, zz_gate_byte(), (void*)exe, (long)g_xn,
                n8, (unsigned long long)(PrecompiledPointers[8] - (uintptr_t)m8),
                n9, (unsigned long long)(PrecompiledPointers[9] - (uintptr_t)m9));
    }
    return 0;
}

static int zz_xmamm_init()
{
    if (HANDLE h = CreateThread(nullptr, 0, zz_xma_gate_thread, nullptr, 0, nullptr)) CloseHandle(h);
    if (HANDLE h = CreateThread(nullptr, 0, zz_xma_tick_thread, nullptr, 0, nullptr)) CloseHandle(h);
    return 1;
}
static int g_xmamm_init = zz_xmamm_init();
