// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Crackdown s6 (2026-10-14): NFS's predicated-tiling fix + MRT fix, ported (S6_GRAPHICS_PORT.md).
//
// WHY. Launch 10 (first time in game): the 3D scene draws only in a band, black below, HUD fine
// -- the NFS s16-s18 picture. The Xbox 360 renders a frame that does not fit EDRAM in bins; per
// bin the game writes PA_SC_WINDOW_OFFSET (reg 0x2080) and selects the bin with predication.
// Emu's WOFF callback (VGPUDX12+0x4F990) stores the offset at ctx+0x1F0/0x1F4 and marks the
// context dirty 0x80A but NOT bit 4 (viewport rebuild), and the replay (fn 0xF710) only
// re-applies a draw record's viewport when the record's dirty byte (rec+0x88) is set -- so every
// bin draws with bin 0's squashed viewport. NFS's fix, on the SAME VGPUDX12.dll (md5 3f002570):
//   hook A (VGPUDX12+0x5272C, the PM4 register-write funnel): capture the ctx; on reg 0x2080 set
//          viewport-dirty bit 4 in *[ctx+0xA8].
//   hook F (VGPUDX12+0x14F04, inside the draw-record builder, r14 = the new record): while a
//          specific bin is selected (BIN_SELECT_lo at regfile+0x31B8 not 0 and not select-all)
//          and the record's viewport is the full frame at origin, set the viewport origin to the
//          live per-bin window offset and force rec[0x88] = 1.
//   hook H (VGPUDX12+0x29050, surface create): log only -- prints the tile render-target sizes.
//   mrtfix: VGPUDX12+0xF87B `mov r9d,1` -> 0: OMSetRenderTargets honours the per-slot RTV array.
// NFS-specific constants (1280x720 x N, bin selects 02/08/20) are generalised per Fable: any
// specific bin select, any origin-(0,0) record of at least 1024x576.
//
// Markers (the console folder, <game>_-prefixed; the unprefixed zz_tile.on / zz_mrtfix.on there are
// the dash track's and are never read here): <game>_zz_tile.on (hooks + stamp),
// <game>_zz_tile_observe.on (hooks + log, no mutation), <game>_zz_mrtfix.on.
// s7 (2026-10-14, S7_GRAPHICS_PLAN Batch B, build 20): three INSTRUMENTS, no behaviour change, behind
// <game>_zz_tile_probe.on (needs zz_tile.on or _observe.on for the hooks to be installed):
//   B1 hook A also tallies the game's window scissor (0x2081/0x2082) and screen scissor (0x200E/0x200F)
//      writes as distinct (TL, BR, WOFF.y, BIN_SELECT lo) tuples;
//   B2 call-site thunks at 0xF8C5 (RSSetViewports) and 0xF8E2 (RSSetScissorRects) in the replay fn
//      0xF710 (NFS zz_vp.cpp / zz_tile.cpp hook B encoding): distinct viewport tuples and joint
//      (viewport TLy,H | scissor top,bottom) tuples as D3D12 actually receives them;
//   B3 a tap at the resolve handler entry fn 0xC080 (rdx = ctx+0x1E0; S7_FINDINGS A4): the resolve rect
//      (ctx+0xA24..0xA30, tile-local), dest regs 0x2318-0x231B, WOFF, window scissor, vtx_window_offset_enable.
// s8 (build 22, S8_GRAPHICS_PLAN): build 21's rsfix retired; rsfix2 (base + dest-y only) behind
// <game>_zz_tile_rsfix2.on; probe adds I1 readback census (0x79EA0), I2 one-shot config/rt-list read,
// I3 resolve-target map census (0x751C4).
// s9 (build 23, S9_PLAN §2): probe adds six entry taps (untile 0x78C8C, untile-src 0xC4BC, slots 0xC008, present
// 0x2964, shadow-write 0x52DB0, resolve dispatch 0xBE9C) and cfg entry 76 + the resolution-scale word. Instruments only.
// s10 (build 24, S10_PLAN §2): probe adds the direct-resolve route taps -- I-G target lookup 0x7CC6C, I-H dirty/placement
// 0x6B28C, I-J untile caller (I-A's 4th hit arg = entry rsp) + ORDER stamps, I-K untile dispatch args 0x73AD0, I-L shadow
// views 0xEAB0, I-M dispatch extents 0xE158, I-N executor 0x771E4 / 0x76EE4 -- and I2++ (the sample-select gate bytes and
// the global [[0x5ABCC0]+0x1C]) in the resolve tap. Instruments only.
// Log: <console folder>\<game>_tile.log, written ONLY by our own host thread (never from the
// VdSwap thunk: guest-fiber rule). Both twins load: a per-pid mutex makes one of them inert.
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>
#include <windows.h>
#include <intrin.h>

#define ZZ_FUNNEL_RVA 0x5272Cu
static const unsigned char ZZ_FUNNEL_SIG[8] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x6C };

static FILE*                    g_f;
static unsigned char* volatile  g_ctx;
static unsigned char*           g_vgpu;
static volatile LONG            g_evt;
static bool g_tile_on, g_mrtfix_on, g_observe, g_probe;
static void* zz_tile_alloc_near(uintptr_t near_addr, size_t n);

#define ZZ_TILE_RING 4096
struct TileRec
{
    unsigned frame, kind;       // kind 4 = SNAP, 6 = TEXC
    float    tlx, tly, w, h;
    int      woffX, woffY;
    unsigned raw, fixed, tid, wseq, wnz;
    void*    dst;
    unsigned evt;
};
static TileRec*      g_ring;
static volatile LONG g_widx;
static long          g_dumped;

extern "C" volatile long zz_swaps;     // zz_fps.cpp: bumped in the VdSwap thunk, read only here

// ---- hook A: PM4 register-write funnel (NFS zz_tile.cpp 97-189) -------------------------------
struct RegHit { unsigned frame, reg, val, tid, evt, prodHi, prodLo, maskLo, selLo; };
#define ZZ_WOFF_RING 64
static RegHit        g_woff_ring[ZZ_WOFF_RING];
static volatile LONG g_woff_widx, g_woff_total, g_woff_nonzero, g_copy_total;
static unsigned      g_copy_last[8]; static volatile LONG g_copy_lidx;
static void*         g_ctx_seen[4];  static volatile LONG g_ctx_seen_n;

// ---- s7 B1: scissor-register tuples (kind 'W' window 0x2081/82, 'S' screen 0x200E/0F) ----------
struct ScRegStat { unsigned kind, tl, br, sel; int wy; volatile LONG n; };
#define ZZ_SCREG_N 32
static ScRegStat     g_screg[ZZ_SCREG_N];
static volatile LONG g_screg_n, g_screg_over;
static void zz_screg_note(unsigned kind, unsigned tl, unsigned br, int wy, unsigned sel)
{
    LONG n = g_screg_n;
    for (LONG i = 0; i < n && i < ZZ_SCREG_N; i++)
    {
        ScRegStat& t = g_screg[i];
        if (t.kind == kind && t.tl == tl && t.br == br && t.wy == wy && t.sel == sel) { t.n++; return; }
    }
    if (n < ZZ_SCREG_N)
    {
        LONG i = InterlockedIncrement(&g_screg_n) - 1;
        if (i < ZZ_SCREG_N) { g_screg[i].kind = kind; g_screg[i].tl = tl; g_screg[i].br = br;
                              g_screg[i].wy = wy; g_screg[i].sel = sel; g_screg[i].n = 1; return; }
    }
    InterlockedIncrement(&g_screg_over);
}
// the first 64 scissor writes made while WOFF is nonzero, printed once
struct ScRegHit { unsigned frame, kind, tl, br, sel; int wx, wy; };
static ScRegHit      g_screg_first[64];
static volatile LONG g_screg_first_n;
static long          g_screg_first_dumped;

static int zz_sext15(unsigned v) { return (int)(v << 17) >> 17; }

extern "C" void zz_tile_reg(unsigned char* ctx, unsigned regIdx, unsigned count, unsigned char* src)
{
    __try
    {
        g_ctx = ctx;
        LONG n = g_ctx_seen_n;
        int known = 0;
        for (LONG k = 0; k < n && k < 4; k++) if (g_ctx_seen[k] == ctx) { known = 1; break; }
        if (!known && n < 4)
        {
            LONG slot = InterlockedIncrement(&g_ctx_seen_n) - 1;
            if (slot < 4) g_ctx_seen[slot] = ctx;
        }
        if (count > 0x1000) return;
        unsigned scMask = 0, pWTL = 0, pWBR = 0, pSTL = 0, pSBR = 0, pWoff = 0;
        for (unsigned k = 0; k < count; k++)
        {
            unsigned reg = regIdx + k;
            if (g_probe)
            {
                if      (reg == 0x2081) { scMask |= 1;  pWTL  = _byteswap_ulong(*(unsigned*)(src + k * 4)); }
                else if (reg == 0x2082) { scMask |= 2;  pWBR  = _byteswap_ulong(*(unsigned*)(src + k * 4)); }
                else if (reg == 0x200E) { scMask |= 4;  pSTL  = _byteswap_ulong(*(unsigned*)(src + k * 4)); }
                else if (reg == 0x200F) { scMask |= 8;  pSBR  = _byteswap_ulong(*(unsigned*)(src + k * 4)); }
                else if (reg == 0x2080) { scMask |= 16; pWoff = _byteswap_ulong(*(unsigned*)(src + k * 4)); }
            }
            if (reg == 0x2080)
            {
                // THE FIX (NFS s18): mark the viewport dirty on EVERY window-offset write.
                if (!g_observe)
                {
                    unsigned char* db = *(unsigned char**)(ctx + 0xA8);
                    if (db) *(volatile unsigned*)db |= 4u;
                }
                unsigned val = _byteswap_ulong(*(unsigned*)(src + k * 4));
                InterlockedIncrement(&g_woff_total);
                if (val) InterlockedIncrement(&g_woff_nonzero);
                LONG i = (InterlockedIncrement(&g_woff_widx) - 1) & (ZZ_WOFF_RING - 1);
                g_woff_ring[i].frame = (unsigned)zz_swaps;
                g_woff_ring[i].reg = reg; g_woff_ring[i].val = val;
                g_woff_ring[i].tid = GetCurrentThreadId();
                g_woff_ring[i].evt = (unsigned)InterlockedIncrement(&g_evt);
                g_woff_ring[i].prodHi = *(unsigned*)(ctx + 0x90);
                g_woff_ring[i].prodLo = *(unsigned*)(ctx + 0x94);
                unsigned char* rf = *(unsigned char**)(ctx + 0xA0);
                g_woff_ring[i].maskLo = rf ? *(unsigned*)(rf + 0x31B0) : 0xDEADDEAD;
                g_woff_ring[i].selLo  = rf ? *(unsigned*)(rf + 0x31B8) : 0xDEADDEAD;
            }
            else if (reg >= 0x2300 && reg <= 0x233F)
            {
                unsigned val = _byteswap_ulong(*(unsigned*)(src + k * 4));
                InterlockedIncrement(&g_copy_total);
                LONG i = (InterlockedIncrement(&g_copy_lidx) - 1) & 7;
                g_copy_last[i] = (reg << 24) | (val & 0xFFFFFF);
            }
        }
        if (scMask & 15)
        {
            // the regfile holds the values from before this packet (the funnel stores after our tap)
            unsigned char* rf = *(unsigned char**)(ctx + 0xA0);
            if (!rf) return;
            int wx = *(int*)(ctx + 0x1F0), wy = *(int*)(ctx + 0x1F4);
            if (scMask & 16) { wx = zz_sext15(pWoff & 0x7FFF); wy = zz_sext15((pWoff >> 16) & 0x7FFF); }
            unsigned sel = *(unsigned*)(rf + 0x31B8);
            for (int w = 0; w < 2; w++)
            {
                unsigned have = w == 0 ? (scMask & 3) : ((scMask >> 2) & 3);
                if (!have) continue;
                unsigned tl = w == 0 ? ((have & 1) ? pWTL : *(unsigned*)(rf + 0xA264)) : ((have & 1) ? pSTL : *(unsigned*)(rf + 0xA098));
                unsigned br = w == 0 ? ((have & 2) ? pWBR : *(unsigned*)(rf + 0xA268)) : ((have & 2) ? pSBR : *(unsigned*)(rf + 0xA09C));
                unsigned kind = w == 0 ? 'W' : 'S';
                zz_screg_note(kind, tl, br, wy, sel);
                if ((wx || wy) && g_screg_first_n < 64)
                {
                    LONG i = InterlockedIncrement(&g_screg_first_n) - 1;
                    if (i < 64)
                    {
                        ScRegHit& h = g_screg_first[i];
                        h.frame = (unsigned)zz_swaps; h.kind = kind; h.tl = tl; h.br = br; h.sel = sel; h.wx = wx; h.wy = wy;
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void zz_tile_say(const char* fmt, ...)
{
    if (!g_f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_f, fmt, ap);
    va_end(ap);
    fputc('\n', g_f);
    fflush(g_f);
}

// ---- hook F: draw-record snapshot (NFS 298-463), stamp generalised (S6_GRAPHICS_PORT §3) -----
__declspec(align(64)) static volatile LONG g_snap_n;
__declspec(align(64)) static volatile LONG g_stamped, g_bin_other;

struct DimStat { unsigned w, h; volatile LONG n; };
static DimStat g_rec_dims[16];
static volatile LONG g_rec_dims_n;
static void zz_rec_dim_note(float w, float h)
{
    unsigned iw = (unsigned)w, ih = (unsigned)h;
    LONG n = g_rec_dims_n;
    for (LONG i = 0; i < n && i < 16; i++)
        if (g_rec_dims[i].w == iw && g_rec_dims[i].h == ih) { g_rec_dims[i].n++; return; }
    if (n < 16)
    {
        LONG i = InterlockedIncrement(&g_rec_dims_n) - 1;
        if (i < 16) { g_rec_dims[i].w = iw; g_rec_dims[i].h = ih; g_rec_dims[i].n = 1; }
    }
}

// up to 16 distinct (sel, selHi, mask, wx, wy) tuples seen while a bin is selected
struct TupleStat { unsigned sel, selHi, mask; int wx, wy; volatile LONG n; };
static TupleStat g_tuples[16];
static volatile LONG g_tuples_n;
static void zz_tuple_note(unsigned sel, unsigned selHi, unsigned mask, int wx, int wy)
{
    LONG n = g_tuples_n;
    for (LONG i = 0; i < n && i < 16; i++)
    {
        TupleStat& t = g_tuples[i];
        if (t.sel == sel && t.selHi == selHi && t.mask == mask && t.wx == wx && t.wy == wy) { t.n++; return; }
    }
    if (n < 16)
    {
        LONG i = InterlockedIncrement(&g_tuples_n) - 1;
        if (i < 16) { g_tuples[i].sel = sel; g_tuples[i].selHi = selHi; g_tuples[i].mask = mask;
                      g_tuples[i].wx = wx; g_tuples[i].wy = wy; g_tuples[i].n = 1; }
    }
}

extern "C" void zz_tile_snap(unsigned char* rec, unsigned char* ctx2)
{
    __try
    {
        LONG n = ++g_snap_n;
        unsigned char* ctx = g_ctx;
        if (!ctx) return;
        int wy = *(int*)(ctx + 0x1F4);
        int wx = *(int*)(ctx + 0x1F0);
        float* vp = (float*)(rec + 0x10);
        if ((n & 15) == 0) zz_rec_dim_note(vp[2], vp[3]);

        unsigned sel = 0, selHi = 0, mask = 0;
        unsigned char* rf = *(unsigned char**)(ctx + 0xA0);
        if (rf) { sel = *(unsigned*)(rf + 0x31B8); selHi = *(unsigned*)(rf + 0x31BC); mask = *(unsigned*)(rf + 0x31B0); }
        unsigned selLow = sel & 0x7FFFFFFFu;
        // a SPECIFIC bin is selected: select-all (lo FFFFFFFF) and "none yet" (0) excluded
        int binSel = (selLow != 0u && selLow != 0x7FFFFFFFu);
        int fullFrame = (vp[0] == 0.0f && vp[1] == 0.0f && vp[2] >= 1024.0f && vp[3] >= 576.0f);
        unsigned stamped = 0;
        if (!g_observe && binSel && fullFrame)
        {
            vp[0] = (float)wx;
            vp[1] = (float)wy;
            rec[0x88] = 1;              // block dirty: fn 0xF710 re-issues RSSetViewports for this draw
            stamped = 1;
            g_stamped++;
        }
        else if (binSel) g_bin_other++;
        if (binSel && (n & 3) == 0) zz_tuple_note(sel, selHi, mask, wx, wy);

        // log: every stamped record with a nonzero offset, every 4th unstamped bin draw, else sampled
        if (!(stamped && (wy || wx)) && !(binSel && !stamped && (n & 3) == 0) && (n & 63) != 0) return;
        if (!g_ring) return;
        LONG i = InterlockedIncrement(&g_widx);
        if (i > ZZ_TILE_RING) return;
        TileRec* r = &g_ring[i - 1];
        r->frame = (unsigned)zz_swaps; r->kind = 4;
        r->tlx = vp[0]; r->tly = vp[1]; r->w = vp[2]; r->h = vp[3];
        r->woffX = wx; r->woffY = wy;
        r->raw = sel; r->fixed = stamped;
        r->tid = GetCurrentThreadId();
        r->wseq = mask; r->wnz = selHi;
        r->dst = (void*)ctx2;
        r->evt = (unsigned)InterlockedIncrement(&g_evt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

#define ZZ_SNAP_RVA 0x14F04u
static const unsigned char ZZ_SNAP_SIG[8] = { 0x40,0x88,0x78,0x78, 0x48,0x8B,0x51,0x08 };

static bool zz_tile_install_snap(void)
{
    unsigned char* target = g_vgpu + ZZ_SNAP_RVA;
    if (memcmp(target, ZZ_SNAP_SIG, sizeof(ZZ_SNAP_SIG)) != 0) { zz_tile_say("# snap sig mismatch at +%X", ZZ_SNAP_RVA); return false; }
    unsigned char* t = (unsigned char*)zz_tile_alloc_near((uintptr_t)target, 128);
    if (!t) { zz_tile_say("# snap: no near trampoline"); return false; }
    // NFS byte for byte (s17 lesson: mid-function site, rsp == 0 mod 16, shadow 0x20)
    size_t i = 0;
    t[i++] = 0x40; t[i++] = 0x88; t[i++] = 0x78; t[i++] = 0x78;  // mov [rax+78h],dil
    t[i++] = 0x48; t[i++] = 0x8B; t[i++] = 0x51; t[i++] = 0x08;  // mov rdx,[rcx+8]
    t[i++] = 0x9C; t[i++] = 0x50; t[i++] = 0x51; t[i++] = 0x52;
    t[i++] = 0x41; t[i++] = 0x50; t[i++] = 0x41; t[i++] = 0x51; t[i++] = 0x41; t[i++] = 0x52; t[i++] = 0x41; t[i++] = 0x53;
    t[i++] = 0x49; t[i++] = 0x8B; t[i++] = 0xCE;                 // mov rcx, r14 (record)
    t[i++] = 0x48; t[i++] = 0x8B; t[i++] = 0xD6;                 // mov rdx, rsi (ctx2)
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xEC; t[i++] = 0x20;  // sub rsp, 0x20
    t[i++] = 0x48; t[i++] = 0xB8; *(void**)(t + i) = (void*)&zz_tile_snap; i += 8;
    t[i++] = 0xFF; t[i++] = 0xD0;                                // call rax
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xC4; t[i++] = 0x20;  // add rsp, 0x20
    t[i++] = 0x41; t[i++] = 0x5B; t[i++] = 0x41; t[i++] = 0x5A; t[i++] = 0x41; t[i++] = 0x59; t[i++] = 0x41; t[i++] = 0x58;
    t[i++] = 0x5A; t[i++] = 0x59; t[i++] = 0x58; t[i++] = 0x9D;
    t[i++] = 0xFF; t[i++] = 0x25; *(int*)(t + i) = 0; i += 4;    // jmp [rip+0] -> site+8
    *(void**)(t + i) = (void*)(target + 8); i += 8;
    intptr_t rel = (intptr_t)t - ((intptr_t)target + 5);
    if (rel > 0x7FFFFFFF || rel < -0x7FFFFFFF) { zz_tile_say("# snap: out of rel32 range"); return false; }
    DWORD old = 0;
    if (!VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old)) { zz_tile_say("# snap: VirtualProtect failed (%lu)", GetLastError()); return false; }
    target[0] = 0xE9; *(int*)(target + 1) = (int)rel;
    target[5] = 0x90; target[6] = 0x90; target[7] = 0x90;
    VirtualProtect(target, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 8);
    zz_tile_say("# snapshot tap installed at VGPUDX12+%X", ZZ_SNAP_RVA);
    return true;
}

// ---- hook H: surface create (NFS 748-860, log only) ----------------------------------------
static int zz_ok_mem(const void* p, size_t n)
{
    if (!p || ((uintptr_t)p & 3)) return 0;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof(mi))) return 0;
    if (mi.State != MEM_COMMIT) return 0;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if (!(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
    size_t avail = (size_t)((const char*)mi.BaseAddress + mi.RegionSize - (const char*)p);
    return avail >= n;
}

static volatile LONG g_texc_n;
struct ZzTexcStat { unsigned key, n; };
static ZzTexcStat    g_texc_stats[64];
static volatile LONG g_texc_stat_n;

extern "C" void zz_tile_texc(unsigned char* argbase, unsigned edx, unsigned fmt, unsigned r9d)
{
    __try
    {
        InterlockedIncrement(&g_texc_n);
        if (!zz_ok_mem(argbase + 0x28, 0x30)) return;
        unsigned a5 = *(unsigned*)(argbase + 0x28);
        unsigned a6 = *(unsigned*)(argbase + 0x30);
        unsigned a7 = *(unsigned*)(argbase + 0x38);
        unsigned a8 = *(unsigned*)(argbase + 0x40);
        unsigned key = ((fmt & 0x3F) << 8) | (a8 & 3);
        LONG n = g_texc_stat_n; int found = -1;
        for (LONG k = 0; k < n && k < 64; k++) if (g_texc_stats[k].key == key) { found = (int)k; break; }
        if (found >= 0) g_texc_stats[found].n++;
        else { LONG s = InterlockedIncrement(&g_texc_stat_n) - 1; if (s < 64) { g_texc_stats[s].key = key; g_texc_stats[s].n = 1; } }
        if (!g_ring) return;
        LONG i = InterlockedIncrement(&g_widx);
        if (i > ZZ_TILE_RING) return;
        TileRec* r = &g_ring[i - 1];
        r->frame = (unsigned)zz_swaps; r->kind = 6;
        r->tlx = r->tly = r->w = r->h = 0.0f;
        r->woffX = (int)a5; r->woffY = (int)a6;
        r->raw = fmt; r->fixed = 0;
        r->tid = GetCurrentThreadId();
        r->wseq = a8; r->wnz = a7;
        r->dst = (void*)(uintptr_t)((unsigned long long)edx << 32 | r9d);
        r->evt = (unsigned)InterlockedIncrement(&g_evt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

#define ZZ_TEXC_RVA 0x29050u
static const unsigned char ZZ_TEXC_SIG[5] = { 0x40,0x53,0x55,0x56,0x57 };

static bool zz_tile_install_texc(void)
{
    unsigned char* target = g_vgpu + ZZ_TEXC_RVA;
    if (memcmp(target, ZZ_TEXC_SIG, sizeof(ZZ_TEXC_SIG)) != 0)
    { zz_tile_say("# texc sig mismatch at +%X: %02X %02X %02X %02X %02X", ZZ_TEXC_RVA, target[0], target[1], target[2], target[3], target[4]); return false; }
    unsigned char* t = (unsigned char*)zz_tile_alloc_near((uintptr_t)target, 128);
    if (!t) { zz_tile_say("# texc: no near trampoline"); return false; }
    size_t i = 0;
    t[i++] = 0x9C; t[i++] = 0x50; t[i++] = 0x51; t[i++] = 0x52;
    t[i++] = 0x41; t[i++] = 0x50; t[i++] = 0x41; t[i++] = 0x51; t[i++] = 0x41; t[i++] = 0x52; t[i++] = 0x41; t[i++] = 0x53;
    t[i++] = 0x48; t[i++] = 0x8D; t[i++] = 0x4C; t[i++] = 0x24; t[i++] = 0x40;   // lea rcx,[rsp+0x40] = entry rsp
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xEC; t[i++] = 0x28;
    t[i++] = 0x48; t[i++] = 0xB8; *(void**)(t + i) = (void*)&zz_tile_texc; i += 8;
    t[i++] = 0xFF; t[i++] = 0xD0;
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xC4; t[i++] = 0x28;
    t[i++] = 0x41; t[i++] = 0x5B; t[i++] = 0x41; t[i++] = 0x5A; t[i++] = 0x41; t[i++] = 0x59; t[i++] = 0x41; t[i++] = 0x58;
    t[i++] = 0x5A; t[i++] = 0x59; t[i++] = 0x58; t[i++] = 0x9D;
    t[i++] = 0x40; t[i++] = 0x53; t[i++] = 0x55; t[i++] = 0x56; t[i++] = 0x57;   // displaced: push rbx/rbp/rsi/rdi
    t[i++] = 0xFF; t[i++] = 0x25; *(int*)(t + i) = 0; i += 4;
    *(void**)(t + i) = (void*)(target + 5); i += 8;
    intptr_t rel = (intptr_t)t - ((intptr_t)target + 5);
    if (rel > 0x7FFFFFFF || rel < -0x7FFFFFFF) { zz_tile_say("# texc: out of rel32 range"); return false; }
    DWORD old = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) { zz_tile_say("# texc: VirtualProtect failed (%lu)", GetLastError()); return false; }
    target[0] = 0xE9; *(int*)(target + 1) = (int)rel;
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    zz_tile_say("# texc probe installed at VGPUDX12+%X", ZZ_TEXC_RVA);
    return true;
}

// ---- s7 B2: D3D12 truth at the replay fn 0xF710 (call-site thunks, NFS zz_vp.cpp encoding) ----
// 0xF8C5 `call [rax+0xA8]` RSSetViewports (r8 = record viewport block), 0xF8E2 `call [rax+0xB0]`
// RSSetScissorRects (r8 = block+0x18, so the viewport just issued sits at r8-0x18).
// Hit paths: no I/O, no locks; racy distinct-tuple tables like zz_tuple_note.
struct VpStat { int tly, w, h; volatile LONG n; };
#define ZZ_VP_N 24
static VpStat        g_vpt[ZZ_VP_N];
static volatile LONG g_vpt_n, g_vpt_over, g_vp_calls;
struct ScStat { int vtly, vh, top, bottom, left, right; volatile LONG n; };
#define ZZ_SCT_N 32
static ScStat        g_sct[ZZ_SCT_N];
static volatile LONG g_sct_n, g_sct_over, g_sc_calls;

extern "C" void zz_tile_vp_hit(unsigned rva, unsigned kind, float* vp, unsigned count)
{
    (void)rva; (void)kind; (void)count;
    __try
    {
        g_vp_calls++;
        int tly = (int)vp[1], w = (int)vp[2], h = (int)vp[3];
        LONG n = g_vpt_n;
        for (LONG i = 0; i < n && i < ZZ_VP_N; i++)
            if (g_vpt[i].tly == tly && g_vpt[i].w == w && g_vpt[i].h == h) { g_vpt[i].n++; return; }
        if (n < ZZ_VP_N)
        {
            LONG i = InterlockedIncrement(&g_vpt_n) - 1;
            if (i < ZZ_VP_N) { g_vpt[i].tly = tly; g_vpt[i].w = w; g_vpt[i].h = h; g_vpt[i].n = 1; return; }
        }
        g_vpt_over++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

extern "C" void zz_tile_sc_hit(unsigned rva, unsigned kind, int* rc, unsigned count)
{
    (void)rva; (void)kind; (void)count;
    __try
    {
        g_sc_calls++;
        const float* vp = (const float*)((unsigned char*)rc - 0x18);
        int vtly = (int)vp[1], vh = (int)vp[3];
        int left = rc[0], top = rc[1], right = rc[2], bottom = rc[3];
        LONG n = g_sct_n;
        for (LONG i = 0; i < n && i < ZZ_SCT_N; i++)
        {
            ScStat& t = g_sct[i];
            if (t.vtly == vtly && t.vh == vh && t.top == top && t.bottom == bottom && t.left == left && t.right == right) { t.n++; return; }
        }
        if (n < ZZ_SCT_N)
        {
            LONG i = InterlockedIncrement(&g_sct_n) - 1;
            if (i < ZZ_SCT_N)
            {
                ScStat& t = g_sct[i];
                t.vtly = vtly; t.vh = vh; t.top = top; t.bottom = bottom; t.left = left; t.right = right; t.n = 1;
                return;
            }
        }
        g_sct_over++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

#define ZZ_VPSET_RVA 0xF8C5u
#define ZZ_SCSET_RVA 0xF8E2u
static const unsigned char ZZ_VPSET_SIG[6] = { 0xFF,0x90,0xA8,0x00,0x00,0x00 };   // call [rax+0xA8]
static const unsigned char ZZ_SCSET_SIG[6] = { 0xFF,0x90,0xB0,0x00,0x00,0x00 };   // call [rax+0xB0]

// NFS zz_tile.cpp zz_tile_install_vpset, byte for byte, parameterised by site and hit function.
static bool zz_tile_install_callsite(unsigned rva, const unsigned char* sig, void* hit, const char* what)
{
    unsigned char* site = g_vgpu + rva;
    if (memcmp(site, sig, 6) != 0)
    {
        zz_tile_say("# %s sig mismatch at +%X: %02X %02X %02X %02X %02X %02X",
                    what, rva, site[0], site[1], site[2], site[3], site[4], site[5]);
        return false;
    }
    unsigned char jmp_modrm = (unsigned char)((site[1] & 0xC7) | 0x20);  // call->jmp modrm
    int disp = *(int*)(site + 2);
    unsigned char* t = (unsigned char*)zz_tile_alloc_near((uintptr_t)site, 128);
    if (!t) { zz_tile_say("# %s: no near trampoline", what); return false; }
    size_t i = 0;
    t[i++] = 0x9C; t[i++] = 0x50; t[i++] = 0x51; t[i++] = 0x52;          // pushfq, rax, rcx, rdx
    t[i++] = 0x41; t[i++] = 0x50; t[i++] = 0x41; t[i++] = 0x51;          // r8, r9
    t[i++] = 0x41; t[i++] = 0x52; t[i++] = 0x41; t[i++] = 0x53;          // r10, r11
    t[i++] = 0x44; t[i++] = 0x8B; t[i++] = 0xCA;                         // mov r9d, edx (count)
    t[i++] = 0xBA; *(unsigned*)(t + i) = 0; i += 4;                      // mov edx, 0 (kind)
    t[i++] = 0xB9; *(unsigned*)(t + i) = rva; i += 4;                    // mov ecx, rva
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xEC; t[i++] = 0x28;          // sub rsp, 0x28
    t[i++] = 0x48; t[i++] = 0xB8; *(void**)(t + i) = hit; i += 8;        // mov rax, hit
    t[i++] = 0xFF; t[i++] = 0xD0;                                        // call rax
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xC4; t[i++] = 0x28;          // add rsp, 0x28
    t[i++] = 0x41; t[i++] = 0x5B; t[i++] = 0x41; t[i++] = 0x5A;
    t[i++] = 0x41; t[i++] = 0x59; t[i++] = 0x41; t[i++] = 0x58;
    t[i++] = 0x5A; t[i++] = 0x59; t[i++] = 0x58; t[i++] = 0x9D;
    t[i++] = 0xFF; t[i++] = jmp_modrm; *(int*)(t + i) = disp; i += 4;    // jmp [rax+disp]
    intptr_t rel = (intptr_t)t - ((intptr_t)site + 5);
    if (rel > 0x7FFFFFFF || rel < -0x7FFFFFFF) { zz_tile_say("# %s: out of rel32 range", what); return false; }
    DWORD old = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) { zz_tile_say("# %s: VirtualProtect failed (%lu)", what, GetLastError()); return false; }
    site[0] = 0xE8; *(int*)(site + 1) = (int)rel; site[5] = 0x90;       // call thunk; nop
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    zz_tile_say("# %s tap installed at VGPUDX12+%X (fn 0xF710 replay)", what, rva);
    return true;
}

// ---- s7 B3: resolve handler entry fn 0xC080 (S7_FINDINGS A4) ---------------------------------
// rdx = ctx+0x1E0: rect top/left/bottom/right at +0x844/+0x848/+0x84C/+0x850 (tile-local after WOFF when
// PA_SU_SC_MODE_CNTL bit 16 is set), Y shift at +0x85C; the handler's dest y = shift + rect top.
struct RsStat { unsigned base, pitch, info, ctrl, surf, wtl, wbr, sel, vtxen; int top, left, bottom, right, wy, ysh; volatile LONG n; };
#define ZZ_RS_N 40
static RsStat        g_rst[ZZ_RS_N];
static volatile LONG g_rst_n, g_rst_over, g_rs_calls;
static RsStat        g_rs_first[64];         // the first 64 resolves made with a nonzero WOFF
static unsigned      g_rs_first_frame[64];
static volatile LONG g_rs_first_n;
static long          g_rs_first_dumped;

// ---- s8 (build 22, S8_GRAPHICS_PLAN §2): rsfix2, marker <game>_zz_tile_rsfix2.on --------------
// Bin 1's main colour resolve (convert, ctrl 00100340) goes to base 1A419000 pitch 1280x240 = 480 rows past
// bin 0's 1A1C1000 1280x720 (launch 12). Emu keys its resolve-target buffer by (base, W) (fn 0x751C4), so
// bin 1 writes its own buffer and the final fetch of (1A1C1000, 1280) gets bin 0's, rows 480-719 never
// written. rsfix2 rewrites only the BASE (0xC115 -> parent) and the DEST-Y shift (0xC213 += rows); H stays
// the game's 240, so fn 0xC3E4's size is 720 or 240 rows (never above the parent's) and 0x751C4 reuses the
// parent's buffer. (Build 21's rsfix also rewrote H to 720 -> 1200-row sizes -> launch 13's death; retired.)
// Exact-parent gate: same W/fmt, parent H == H + rows, different bin, same or previous frame.
// Marker content = arm frame (default 0).
static bool g_rsfix2;
static long g_rsfix2_arm;
struct RxParent2 { unsigned base, W, H, fmt, sel, frame; };
static RxParent2     g_rxp2[8];
static volatile LONG g_rxp2_n;
static struct { DWORD tid; unsigned char* blk; unsigned base; int yadd; } g_rx2;
static volatile LONG g_rx2_decided, g_rx2_used, g_rx2_nomatch, g_rx2_notyet;
struct Rx2Pair { unsigned child, parent, W, H, rows; volatile LONG n; };
static Rx2Pair       g_rx2pair[8];
static volatile LONG g_rx2pair_n;

// ---- s8 I2: the runtime unknowns of the resolve path, read once at the first resolve hit ---------
static struct
{
    void *thisp, *g;
    unsigned n;                        // config vector element count
    unsigned long long e[6];           // entries 0xF8 0x210 0x218 0x300 0x398 0x3D0 (index = off/8)
    unsigned has;                      // bit k: e[k] read (index < n)
    int surf48, surf49;                // -1 = unreadable
    unsigned long long fmtmask;
    unsigned listn, listW[16]; unsigned long long listM[16];
    unsigned long long e260; int has260;            // s9 I2+: entry 76 (the present shortcut gate, 0x2A0A)
    unsigned scale28, scale60; int hasScale;        // s9 I2+: [[0x5ABCD8]+0x28] / +0x60 (0x229AC: +0x60 == 1 -> writeback off)
    // s10 I2++: read at the first convert resolve with copy sample select 4 (ctrl bits 4-6)
    unsigned s10frame, s10ctrl, s10base, s10b854, s10b858, s10glob; int s10globOk, s10siteOk;
} g_cfg;
static volatile LONG g_s10cfg_once, g_s10cfg_ready;
static long          g_s10cfg_printed;
static volatile LONG g_cfg_once, g_cfg_ready;
static long          g_cfg_printed;
static const unsigned ZZ_CFG_OFF[6] = { 0xF8, 0x210, 0x218, 0x300, 0x398, 0x3D0 };

// ---- s8 I1: readback census at fn 0x79EA0 (GetCopyableFootprints of width x height x format) -----
struct RbStat { unsigned w, h, fmt, dim, first; volatile LONG n; };
#define ZZ_RB_N 24
static RbStat        g_rbt[ZZ_RB_N];
static volatile LONG g_rbt_n, g_rbt_over, g_rb_calls;

// ---- s8 I3: resolve-target map census at fn 0x751C4(g, &{base, W}, size) ------------------------
struct RtStat { unsigned caller, base, W, size; volatile LONG n; };
#define ZZ_RT_N 32
static RtStat        g_rtt[ZZ_RT_N];
static volatile LONG g_rtt_n, g_rtt_over, g_rt_calls;

static unsigned zz_fmt_bpp(unsigned f)
{
    switch (f)
    {
    case 6: case 7: case 14: case 16: case 17: case 22: case 23: case 25: case 28: case 31: case 33: case 36: return 4;
    case 26: case 29: case 32: case 34: case 37: return 8;
    case 3: case 4: case 5: case 10: case 15: case 24: case 27: case 30: return 2;
    default: return 0;
    }
}

static void zz_rx2_pair_note(unsigned child, unsigned parent, unsigned W, unsigned H, unsigned rows)
{
    LONG n = g_rx2pair_n;
    for (LONG i = 0; i < n && i < 8; i++)
    {
        Rx2Pair& t = g_rx2pair[i];
        if (t.child == child && t.parent == parent && t.W == W && t.H == H && t.rows == rows) { t.n++; return; }
    }
    if (n < 8)
    {
        LONG i = InterlockedIncrement(&g_rx2pair_n) - 1;
        if (i < 8) { g_rx2pair[i].child = child; g_rx2pair[i].parent = parent; g_rx2pair[i].W = W;
                     g_rx2pair[i].H = H; g_rx2pair[i].rows = rows; g_rx2pair[i].n = 1; }
    }
}

static void zz_rx2_decide(unsigned char* blk, unsigned char* rf, int wx, int wy)
{
    g_rx2.tid = 0;
    unsigned ctrl = *(unsigned*)(rf + 0xACC0);
    if ((ctrl & 0x300000u) != 0x100000u) return;                     // convert resolves only (raw copies untouched)
    unsigned base = *(unsigned*)(rf + 0xACC4), pitch = *(unsigned*)(rf + 0xACC8), info = *(unsigned*)(rf + 0xACCC);
    unsigned fmt = (info >> 7) & 0x3F, W = pitch & 0x3FFF, H = (pitch >> 16) & 0x3FFF;
    unsigned sel = *(unsigned*)(rf + 0x31B8) & 0x7FFFFFFFu;
    int binSel = (sel != 0u && sel != 0x7FFFFFFFu);
    if (wx || !binSel || !((*(unsigned*)(rf + 0xA874) >> 16) & 1)) return;
    unsigned frame = (unsigned)zz_swaps;
    if (wy == 0)
    {
        // a bin-0 resolve: remember it as a possible parent (same base/W/fmt -> refresh H, sel, frame)
        LONG n = g_rxp2_n; if (n > 8) n = 8;
        for (LONG i = 0; i < n; i++)
        {
            RxParent2& p = g_rxp2[i];
            if (p.base == base && p.W == W && p.fmt == fmt) { p.H = H; p.sel = sel; p.frame = frame; return; }
        }
        LONG i = (InterlockedIncrement(&g_rxp2_n) - 1) & 7;
        g_rxp2[i].base = base; g_rxp2[i].W = W; g_rxp2[i].H = H; g_rxp2[i].fmt = fmt; g_rxp2[i].sel = sel; g_rxp2[i].frame = frame;
        if (g_rxp2_n > 8) g_rxp2_n = 8;
        return;
    }
    if (wy > 0) return;
    unsigned rows = (unsigned)(-wy), bpp = zz_fmt_bpp(fmt);
    if ((rows & 31) || !bpp || !H) { g_rx2_nomatch++; return; }
    if (zz_swaps < g_rsfix2_arm) { g_rx2_notyet++; return; }
    unsigned parent = base - rows * W * bpp;
    LONG n = g_rxp2_n; if (n > 8) n = 8;
    for (LONG i = 0; i < n; i++)
    {
        RxParent2& p = g_rxp2[i];
        if (p.base == parent && p.W == W && p.fmt == fmt && p.H == H + rows && p.sel != sel && (long)p.frame + 1 >= (long)frame)
        {
            g_rx2.blk = blk; g_rx2.base = parent; g_rx2.yadd = (int)rows;
            g_rx2.tid = GetCurrentThreadId();
            g_rx2_decided++;
            zz_rx2_pair_note(base, parent, W, H, rows);
            return;
        }
    }
    g_rx2_nomatch++;
}

extern "C" unsigned zz_rx2_base(unsigned char* blk, unsigned orig)
{ return (g_rx2.tid && g_rx2.blk == blk && g_rx2.tid == GetCurrentThreadId()) ? g_rx2.base : orig; }
extern "C" unsigned zz_rx2_ysh(unsigned char* blk, unsigned orig)
{
    if (g_rx2.tid && g_rx2.blk == blk && g_rx2.tid == GetCurrentThreadId())
    {
        g_rx2.tid = 0;                                                // consumed: one resolve only
        g_rx2_used++;
        return orig + (unsigned)g_rx2.yadd;
    }
    return orig;
}

// I1 hit: entry rsp of fn 0x79EA0 -> [+0x28] height, [+0x50] DXGI format; edx = dim, r9d = width
extern "C" void zz_tile_rb_hit(unsigned char* ersp, unsigned dim, unsigned r8d, unsigned width)
{
    (void)r8d;
    __try
    {
        g_rb_calls++;
        if (!zz_ok_mem(ersp + 0x28, 0x30)) return;
        unsigned h = *(unsigned*)(ersp + 0x28), fmt = *(unsigned*)(ersp + 0x50);
        LONG n = g_rbt_n;
        for (LONG i = 0; i < n && i < ZZ_RB_N; i++)
        {
            RbStat& t = g_rbt[i];
            if (t.w == width && t.h == h && t.fmt == fmt && t.dim == dim) { t.n++; return; }
        }
        if (n < ZZ_RB_N)
        {
            LONG i = InterlockedIncrement(&g_rbt_n) - 1;
            if (i < ZZ_RB_N) { g_rbt[i].w = width; g_rbt[i].h = h; g_rbt[i].fmt = fmt; g_rbt[i].dim = dim;
                               g_rbt[i].first = (unsigned)zz_swaps; g_rbt[i].n = 1; return; }
        }
        g_rbt_over++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// I3 hit: fn 0x751C4(g, key = &{base, W}, size); ersp = entry rsp, [ersp] = the caller's return address
extern "C" void zz_tile_rt_hit(unsigned char* g, unsigned* key, unsigned size, unsigned char* ersp)
{
    (void)g;
    __try
    {
        g_rt_calls++;
        // the key is a caller stack local in every site read (0xCD82 lea rdx,[rbp-0x80]); anything else is checked
        uintptr_t k = (uintptr_t)key, s = (uintptr_t)ersp;
        if (!(k >= s && k < s + 0x10000) && !zz_ok_mem(key, 8)) return;
        unsigned caller = (unsigned)(*(uintptr_t*)ersp - (uintptr_t)g_vgpu);
        unsigned base = key[0], W = key[1];
        LONG n = g_rtt_n;
        for (LONG i = 0; i < n && i < ZZ_RT_N; i++)
        {
            RtStat& t = g_rtt[i];
            if (t.caller == caller && t.base == base && t.W == W && t.size == size) { t.n++; return; }
        }
        if (n < ZZ_RT_N)
        {
            LONG i = InterlockedIncrement(&g_rtt_n) - 1;
            if (i < ZZ_RT_N) { g_rtt[i].caller = caller; g_rtt[i].base = base; g_rtt[i].W = W; g_rtt[i].size = size; g_rtt[i].n = 1; return; }
        }
        g_rtt_over++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// I2: read once (VGPU thread, first resolve), every pointer checked first (clang-cl __try misses inline loads)
static void zz_cfg_read(unsigned char* thisp)
{
    g_cfg.thisp = thisp;
    g_cfg.surf48 = g_cfg.surf49 = -1;
    if (!zz_ok_mem(g_vgpu + 0x5ABCD0, 8) || !zz_ok_mem(g_vgpu + 0x5ABCE0, 8)) return;
    unsigned char* g = *(unsigned char**)(g_vgpu + 0x5ABCD0);
    g_cfg.g = g;
    // 0xC42E mov rax,[rip -> 0x5ABCE0]; 0xC439 r9 = [rax+8] begin; 0xC43D rcx = [rax+0x10] end
    unsigned char* vec = *(unsigned char**)(g_vgpu + 0x5ABCE0);
    if (vec && zz_ok_mem(vec + 8, 16))
    {
        unsigned char* cb = *(unsigned char**)(vec + 8);
        unsigned char* ce = *(unsigned char**)(vec + 0x10);
        if (cb && ce > cb && (size_t)(ce - cb) < 0x100000)
        {
            g_cfg.n = (unsigned)((ce - cb) / 8);
            for (int k = 0; k < 6; k++)
                if (ZZ_CFG_OFF[k] / 8 < g_cfg.n && zz_ok_mem(cb + ZZ_CFG_OFF[k], 8))
                { g_cfg.e[k] = *(unsigned long long*)(cb + ZZ_CFG_OFF[k]); g_cfg.has |= 1u << k; }
            if (g_cfg.n > 76 && zz_ok_mem(cb + 0x260, 8)) { g_cfg.e260 = *(unsigned long long*)(cb + 0x260); g_cfg.has260 = 1; }
        }
    }
    if (zz_ok_mem(g_vgpu + 0x5ABCD8, 8))
    {
        unsigned char* sc = *(unsigned char**)(g_vgpu + 0x5ABCD8);
        if (sc && zz_ok_mem(sc + 0x28, 0x3C)) { g_cfg.scale28 = *(unsigned*)(sc + 0x28); g_cfg.scale60 = *(unsigned*)(sc + 0x60); g_cfg.hasScale = 1; }
    }
    if (!g || !zz_ok_mem(g + 0xC2698, 8)) return;
    unsigned char* surf = *(unsigned char**)(g + 0xC2698);
    if (surf && zz_ok_mem(surf + 0x48, 4)) { g_cfg.surf48 = surf[0x48]; g_cfg.surf49 = surf[0x49]; }
    // fn 0x723D4: rt = g+0xC24F0 inline; mask [rt+0x128]; list [rt+0x130]..[rt+0x138], 16-byte {W dword, mask qword @+8}
    unsigned char* rt = g + 0xC24F0;
    if (!zz_ok_mem(rt + 0x128, 0x18)) return;
    g_cfg.fmtmask = *(unsigned long long*)(rt + 0x128);
    unsigned char* lb = *(unsigned char**)(rt + 0x130);
    unsigned char* le = *(unsigned char**)(rt + 0x138);
    for (unsigned char* p = lb; p && p < le && g_cfg.listn < 16; p += 16)
    {
        if (!zz_ok_mem(p, 16)) break;
        g_cfg.listW[g_cfg.listn] = *(unsigned*)p;
        g_cfg.listM[g_cfg.listn] = *(unsigned long long*)(p + 8);
        g_cfg.listn++;
    }
}

// a 7-byte register read inside fn 0xC080 -> `call thunk; nop; nop`. argset = bytes that load rcx/edx
// (rcx = the ctx+0x1E0 block, edx = the original value), store = bytes that move eax into the target.
static bool zz_tile_install_rxsite(unsigned rva, const unsigned char* sig, const unsigned char* argset, size_t an,
                                   void* fn, const unsigned char* store, size_t sn, const char* what)
{
    unsigned char* site = g_vgpu + rva;
    if (memcmp(site, sig, 7) != 0)
    {
        zz_tile_say("# %s sig mismatch at +%X: %02X %02X %02X %02X %02X %02X %02X", what, rva,
                    site[0], site[1], site[2], site[3], site[4], site[5], site[6]);
        return false;
    }
    unsigned char* t = (unsigned char*)zz_tile_alloc_near((uintptr_t)site, 128);
    if (!t) { zz_tile_say("# %s: no near trampoline", what); return false; }
    // mid-function site (rsp == 0 mod 16): call -> 8; 8 pushes -> 8; sub 0x28 -> 0 before our call
    size_t i = 0;
    t[i++] = 0x9C; t[i++] = 0x50; t[i++] = 0x51; t[i++] = 0x52;
    t[i++] = 0x41; t[i++] = 0x50; t[i++] = 0x41; t[i++] = 0x51; t[i++] = 0x41; t[i++] = 0x52; t[i++] = 0x41; t[i++] = 0x53;
    memcpy(t + i, argset, an); i += an;
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xEC; t[i++] = 0x28;
    t[i++] = 0x48; t[i++] = 0xB8; *(void**)(t + i) = fn; i += 8;
    t[i++] = 0xFF; t[i++] = 0xD0;
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xC4; t[i++] = 0x28;
    memcpy(t + i, store, sn); i += sn;
    t[i++] = 0x41; t[i++] = 0x5B; t[i++] = 0x41; t[i++] = 0x5A; t[i++] = 0x41; t[i++] = 0x59; t[i++] = 0x41; t[i++] = 0x58;
    t[i++] = 0x5A; t[i++] = 0x59; t[i++] = 0x58; t[i++] = 0x9D;
    t[i++] = 0xC3;                                                    // ret -> site+5 (the nops)
    intptr_t rel = (intptr_t)t - ((intptr_t)site + 5);
    if (rel > 0x7FFFFFFF || rel < -0x7FFFFFFF) { zz_tile_say("# %s: out of rel32 range", what); return false; }
    DWORD old = 0;
    if (!VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &old)) { zz_tile_say("# %s: VirtualProtect failed (%lu)", what, GetLastError()); return false; }
    site[0] = 0xE8; *(int*)(site + 1) = (int)rel; site[5] = 0x90; site[6] = 0x90;
    VirtualProtect(site, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 7);
    zz_tile_say("# rsfix2 %s installed at VGPUDX12+%X", what, rva);
    return true;
}

// s8: base (0xC115) and dest-y (0xC213) only -- the pitch read at 0xC11E is NOT patched (H stays the game's)
static bool zz_tile_install_rsfix2(void)
{
    static const unsigned char sigB[7] = { 0x45,0x8B,0xA0,0xC4,0xAC,0x00,0x00 };   // mov r12d,[r8+0ACC4h]
    static const unsigned char sigY[7] = { 0x41,0x8B,0x80,0x5C,0x08,0x00,0x00 };   // mov eax,[r8+85Ch]
    static const unsigned char argB[10] = { 0x48,0x8B,0xCA, 0x41,0x8B,0x90,0xC4,0xAC,0x00,0x00 };  // rcx=rdx; edx=[r8+ACC4]
    static const unsigned char argY[10] = { 0x49,0x8B,0xC8, 0x41,0x8B,0x90,0x5C,0x08,0x00,0x00 };  // rcx=r8;  edx=[r8+85C]
    static const unsigned char stB[3] = { 0x44,0x8B,0xE0 };                                       // mov r12d,eax
    static const unsigned char stY[7] = { 0x89,0xC0, 0x48,0x89,0x44,0x24,0x30 };                  // mov eax,eax; mov [rsp+30h],rax (saved rax)
    unsigned char* b = g_vgpu;
    if (memcmp(b + 0xC115, sigB, 7) || memcmp(b + 0xC213, sigY, 7))
    { zz_tile_say("# rsfix2: site signature mismatch -- not installed"); return false; }
    // an installed base2 thunk with g_rsfix2 off is a pure pass-through, so a partial install is safe
    return zz_tile_install_rxsite(0xC115, sigB, argB, 10, (void*)&zz_rx2_base, stB, 3, "base2")
        && zz_tile_install_rxsite(0xC213, sigY, argY, 10, (void*)&zz_rx2_ysh,  stY, 7, "dest-y2");
}

// s8 I1/I3: a function-entry tap (entry rsp == 8 mod 16), the texc/ctxcap shape: 8 pushes, `extra` (sets the
// hit's arguments; rsp+0x40 = entry rsp), sub 0x28, call hit, add, pops, the displaced position-independent
// dn bytes (whole instructions; s9: dn > 5 pads site+5..site+dn with nop), jmp [rip] -> site+dn.
// sig = the bytes verified before patching (sig[0..dn) = the displaced bytes, sn >= dn).
static bool zz_tile_install_tap(unsigned rva, const unsigned char* sig, size_t sn, const unsigned char* extra, size_t en,
                                void* hit, const char* what, size_t dn = 5)
{
    unsigned char* target = g_vgpu + rva;
    if (dn < 5 || dn > sn || dn > 16) { zz_tile_say("# %s: bad displaced length %u", what, (unsigned)dn); return false; }
    if (memcmp(target, sig, sn) != 0)
    { zz_tile_say("# %s sig mismatch at +%X: %02X %02X %02X %02X %02X %02X %02X %02X", what, rva, target[0], target[1], target[2],
                  target[3], target[4], target[5], target[6], target[7]); return false; }
    unsigned char* t = (unsigned char*)zz_tile_alloc_near((uintptr_t)target, 128);
    if (!t) { zz_tile_say("# %s: no near trampoline", what); return false; }
    size_t i = 0;
    t[i++] = 0x9C; t[i++] = 0x50; t[i++] = 0x51; t[i++] = 0x52;
    t[i++] = 0x41; t[i++] = 0x50; t[i++] = 0x41; t[i++] = 0x51; t[i++] = 0x41; t[i++] = 0x52; t[i++] = 0x41; t[i++] = 0x53;
    if (en) { memcpy(t + i, extra, en); i += en; }
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xEC; t[i++] = 0x28;
    t[i++] = 0x48; t[i++] = 0xB8; *(void**)(t + i) = hit; i += 8;
    t[i++] = 0xFF; t[i++] = 0xD0;
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xC4; t[i++] = 0x28;
    t[i++] = 0x41; t[i++] = 0x5B; t[i++] = 0x41; t[i++] = 0x5A; t[i++] = 0x41; t[i++] = 0x59; t[i++] = 0x41; t[i++] = 0x58;
    t[i++] = 0x5A; t[i++] = 0x59; t[i++] = 0x58; t[i++] = 0x9D;
    memcpy(t + i, sig, dn); i += dn;                                  // displaced
    t[i++] = 0xFF; t[i++] = 0x25; *(int*)(t + i) = 0; i += 4;
    *(void**)(t + i) = (void*)(target + dn); i += 8;
    intptr_t rel = (intptr_t)t - ((intptr_t)target + 5);
    if (rel > 0x7FFFFFFF || rel < -0x7FFFFFFF) { zz_tile_say("# %s: out of rel32 range", what); return false; }
    DWORD old = 0;
    if (!VirtualProtect(target, dn, PAGE_EXECUTE_READWRITE, &old)) { zz_tile_say("# %s: VirtualProtect failed (%lu)", what, GetLastError()); return false; }
    target[0] = 0xE9; *(int*)(target + 1) = (int)rel;
    for (size_t k = 5; k < dn; k++) target[k] = 0x90;
    VirtualProtect(target, dn, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, dn);
    zz_tile_say("# %s tap installed at VGPUDX12+%X (displaced %u)", what, rva, (unsigned)dn);
    return true;
}

static bool zz_tile_install_rb(void)
{
    static const unsigned char sig[8] = { 0x48,0x89,0x5C,0x24,0x10, 0x55,0x56,0x57 };   // mov [rsp+10h],rbx; push rbp/rsi/rdi
    static const unsigned char ex[5]  = { 0x48,0x8D,0x4C,0x24,0x40 };                    // lea rcx,[rsp+0x40] = entry rsp
    return zz_tile_install_tap(0x79EA0u, sig, 8, ex, 5, (void*)&zz_tile_rb_hit, "readback (0x79EA0)");
}

static bool zz_tile_install_rt(void)
{
    static const unsigned char sig[9] = { 0x40,0x55, 0x53, 0x56, 0x57, 0x41,0x54, 0x41,0x56 }; // push rbp/rbx/rsi/rdi/r12/r14
    static const unsigned char ex[5]  = { 0x4C,0x8D,0x4C,0x24,0x40 };                    // lea r9,[rsp+0x40] = entry rsp
    return zz_tile_install_tap(0x751C4u, sig, 9, ex, 5, (void*)&zz_tile_rt_hit, "rtmap (0x751C4)");
}

// ---- s9 (build 23, S9_PLAN §2): the non-writeback read path, instruments only ---------------------
// I-A  untile fn 0x78C8C(this, ctx, desc, level) (texture vtable 0x3D9570 slot 2): the shadow base it untiles
//      is the fetch-constant holder's word (rdi = [this+8]+0x10: [rdi+4] base, [rdi+8] size, [rdi+0x1C] pitch
//      field, 0x790DD/0x79163); desc+0x38/3C/40 is what 0x79322 registers. Gates: [this+0xC8] (0x78CC6),
//      [this+0x10] byte (0x78D15), [this+0x158] (0x78D4C), [[this+8]+0x50] dword (0x78D5A).
// I-B  0xC4BC(shadow, &view, r8d = base + mip offset, r9d = size): the untile's source view.
// I-C  0xC008(g, view, desc): every resolve stores {view, desc+0x38} in slot 1 (g+0xD50/58) -- read today:
//      the W >= 800 test only decides the "tall" flag (W >= 800 AND H >= 400 -> g+0xD5C = 1); the resolve AFTER
//      a tall one shifts slot 1 into slot 2 (g+0xD60/68).
// I-D  present 0x2964(this, desc): the 0x2A04..0x2A4B branch computed from cfg[0x260], the slots and desc+4.
// I-E1 0x52DB0(shadow, &view, r8d base, r9d size): the resolve's shadow-write view (per-thread last base).
// I-E2 0xBE9C entry (rcx = entry rsp): the resolve dispatch's src/dest/extent/pitch arguments.
#define ZZ_S9_MAIN 0x1A1C1000u
#define ZZ_S9_BIN1 0x1A419000u
struct UtStat { unsigned fcbase, fcsize, fcpitch, dbase, dW, dH, gates, first, last; volatile LONG n; };
#define ZZ_UT_N 32
static UtStat        g_utt[ZZ_UT_N];
static volatile LONG g_utt_n, g_utt_over, g_ut_calls, g_ut_main, g_ut_bin1;
struct BsStat { unsigned base, size, first, last; volatile LONG n; };
#define ZZ_BS_N 24
static BsStat        g_ust[ZZ_BS_N];                     // I-B
static volatile LONG g_ust_n, g_ust_over, g_us_calls, g_us_main;
static BsStat        g_swt[ZZ_BS_N];                     // I-E1
static volatile LONG g_swt_n, g_swt_over, g_sw_calls;
struct SlStat { unsigned W, H, base; volatile LONG n; };
#define ZZ_SL_N 24
static SlStat        g_slt[ZZ_SL_N];
static volatile LONG g_slt_n, g_slt_over, g_sl_calls;
static unsigned char* volatile g_sl_g;
struct DpStat { unsigned base, sx, sy, dx, dy, w, h, pitch; volatile LONG n; };
#define ZZ_DP_N 24
static DpStat        g_dpt[ZZ_DP_N];
static volatile LONG g_dpt_n, g_dpt_over, g_dp_calls;
static struct { volatile DWORD tid; volatile unsigned base, size; } g_swlast[8];
static volatile LONG g_swlast_n;
// I-D, computed (not observed): which frame image 0x2964 would pick
static volatile LONG g_pr_calls, g_pr_cfg0, g_pr_slot1, g_pr_slot2, g_pr_null, g_pr_unread;
static volatile unsigned g_pr_front, g_pr_front8, g_pr_cfg260lo, g_pr_n;
struct FbStat { unsigned base; volatile LONG n; };
static FbStat        g_prf[8];
static volatile LONG g_prf_n;

static void zz_bs_note(BsStat* tab, volatile LONG* tn, volatile LONG* over, unsigned base, unsigned size)
{
    unsigned fr = (unsigned)zz_swaps;
    LONG n = *tn;
    for (LONG i = 0; i < n && i < ZZ_BS_N; i++)
        if (tab[i].base == base && tab[i].size == size) { tab[i].n++; tab[i].last = fr; return; }
    if (n < ZZ_BS_N)
    {
        LONG i = InterlockedIncrement(tn) - 1;
        if (i < ZZ_BS_N) { tab[i].base = base; tab[i].size = size; tab[i].first = tab[i].last = fr; tab[i].n = 1; return; }
    }
    (*over)++;
}

// ---- s10 (build 24, S10_PLAN §2): shared pieces -----------------------------------------------------
// A census row = up to 12 dword keys + count + first/last frame. Hits claim rows lock-free (the s9 pattern).
#define ZZ_K 12
struct KStat { unsigned k[ZZ_K]; unsigned first, last; volatile LONG n; };
struct KTab  { KStat* t; LONG cap; unsigned nk; volatile LONG n, over, calls; };
static void zz_k_note(KTab& tb, const unsigned* k)
{
    unsigned fr = (unsigned)zz_swaps;
    LONG n = tb.n; if (n > tb.cap) n = tb.cap;
    for (LONG i = 0; i < n; i++)
        if (memcmp(tb.t[i].k, k, tb.nk * 4) == 0) { tb.t[i].n++; tb.t[i].last = fr; return; }
    if (tb.n < tb.cap)
    {
        LONG i = InterlockedIncrement(&tb.n) - 1;
        if (i < tb.cap) { memcpy(tb.t[i].k, k, tb.nk * 4); tb.t[i].first = tb.t[i].last = fr; tb.t[i].n = 1; return; }
    }
    tb.over++;
}
#define ZZ_KTAB(name, cap, nk) static KStat name##_rows[cap]; static KTab name = { name##_rows, cap, nk, 0, 0, 0 }
ZZ_KTAB(g_s10g, 24, 11);    // I-G  target lookup 0x7CC6C
ZZ_KTAB(g_s10h, 24, 9);     // I-H  dirty 0x6B28C
ZZ_KTAB(g_s10j, 16, 4);     // I-J  untile callers 0x78C8C
ZZ_KTAB(g_s10k, 24, 12);    // I-K  untile dispatch 0x73AD0
ZZ_KTAB(g_s10l, 32, 5);     // I-L  shadow views 0xEAB0 (bases in [19000000, 20000000) only)
ZZ_KTAB(g_s10m, 16, 4);     // I-M  dispatches 0xE158
ZZ_KTAB(g_s10na, 16, 8);    // I-N  executor 0x771E4
ZZ_KTAB(g_s10nb, 16, 8);    // I-N  executor 0x76EE4
static volatile LONG g_s10l_main, g_s10l_bin1, g_s10l_1fd;

static uintptr_t g_vgpu_end;                          // g_vgpu + SizeOfImage
// the tapped function's caller: k[0] = RVA (in VGPUDX12) or the low 32 bits, k[1] = 1 when outside the image
static void zz_caller(unsigned char* ersp, unsigned* k)
{
    uintptr_t ra = *(uintptr_t*)ersp;                  // the entry rsp is the live stack: always readable
    if (ra >= (uintptr_t)g_vgpu && ra < g_vgpu_end) { k[0] = (unsigned)(ra - (uintptr_t)g_vgpu); k[1] = 0; }
    else { k[0] = (unsigned)ra; k[1] = 1; }
}
static const char* zz_caller_str(char* b, size_t n, unsigned lo, unsigned host)
{
    if (host) _snprintf_s(b, n, _TRUNCATE, "host:%08X", lo); else _snprintf_s(b, n, _TRUNCATE, "%05X", lo);
    return b;
}
// a pointer the caller keeps on its stack (desc / rect locals) is readable without a VirtualQuery
static int zz_ok_stack(const void* p, size_t n)
{
    uintptr_t here = (uintptr_t)_AddressOfReturnAddress(), q = (uintptr_t)p;
    if (q > here && q + n < here + 0x20000) return 1;
    return zz_ok_mem(p, n);
}

// I-J ORDER stamps: 4 consecutive frames from max(300, first bin-1 untile frame + 60) -- launch 16 saw bin 1 from
// f202, so this is the plan's f300..303 whenever bin 1 starts by f240, and slides later when the city comes later.
struct OrdRec { unsigned frame; char kind; unsigned a, b, c, d, e; };
#define ZZ_ORD_N 256
static OrdRec        g_ord[ZZ_ORD_N];
static volatile LONG g_ord_n, g_ord_over, g_ord_lo;
static long          g_ord_printed;
static void zz_ord_note(char kind, unsigned a, unsigned b, unsigned c, unsigned d, unsigned e)
{
    LONG lo = g_ord_lo;
    unsigned fr = (unsigned)zz_swaps;
    if (!lo || fr < (unsigned)lo || fr >= (unsigned)lo + 4) return;
    LONG i = InterlockedIncrement(&g_ord_n) - 1;
    if (i >= ZZ_ORD_N) { g_ord_over++; return; }
    OrdRec& r = g_ord[i]; r.frame = fr; r.a = a; r.b = b; r.c = c; r.d = d; r.e = e; r.kind = kind;
}

// s9 I-A + s10 I-J: 4th argument = the entry rsp (`lea r9,[rsp+0x40]`; the level is not used here)
extern "C" void zz_tile_ut_hit(unsigned char* thisp, unsigned char* ctx, unsigned char* desc, unsigned char* ersp)
{
    (void)ctx;
    __try
    {
        g_ut_calls++;
        unsigned ck[4] = {};
        zz_caller(ersp, ck);
        if (!zz_ok_mem(thisp, 0x160)) return;
        UtStat s = {};
        unsigned char* fc = *(unsigned char**)(thisp + 8);
        unsigned fc50 = 0;
        if (fc && zz_ok_mem(fc, 0x54))
        {
            s.fcbase = *(unsigned*)(fc + 0x14); s.fcsize = *(unsigned*)(fc + 0x18);
            s.fcpitch = ((*(unsigned*)(fc + 0x2C) >> 2) & 0x1FF) << 5;
            fc50 = *(unsigned*)(fc + 0x50);
        }
        unsigned dy = 0;
        if (desc && zz_ok_mem(desc + 0x20, 0x24))
        { s.dbase = *(unsigned*)(desc + 0x38); s.dW = *(unsigned*)(desc + 0x3C); s.dH = *(unsigned*)(desc + 0x40); dy = *(unsigned*)(desc + 0x24); }
        // gates bit0 [+0xC8]!=0 (else delegate), bit1 [+0x10] byte, bit2 [+0x158]!=0, bit3 [fc+0x50]!=0, bit4 fc readable
        s.gates = (*(void**)(thisp + 0xC8) ? 1u : 0u) | (thisp[0x10] ? 2u : 0u) | (*(void**)(thisp + 0x158) ? 4u : 0u) |
                  (fc50 ? 8u : 0u) | (fc ? 16u : 0u);
        if ((s.fcbase & ~0xFFFu) == ZZ_S9_MAIN || (s.dbase & ~0xFFFu) == ZZ_S9_MAIN) g_ut_main++;
        if ((s.fcbase & ~0xFFFu) == ZZ_S9_BIN1 || (s.dbase & ~0xFFFu) == ZZ_S9_BIN1) g_ut_bin1++;
        unsigned fr = (unsigned)zz_swaps;
        // s10 I-J: caller census (caller, fc base, desc base) + the ORDER window armed by the first bin-1 untile
        g_s10j.calls++;
        ck[2] = s.fcbase; ck[3] = s.dbase;
        zz_k_note(g_s10j, ck);
        if (!g_ord_lo && s.dbase == ZZ_S9_BIN1)
            InterlockedCompareExchange(&g_ord_lo, (LONG)(fr + 60 > 300 ? fr + 60 : 300), 0);
        zz_ord_note('U', ck[1] ? 0xFFFFFFFFu : ck[0], s.fcbase, s.dbase, dy, s.dH);
        LONG n = g_utt_n;
        for (LONG i = 0; i < n && i < ZZ_UT_N; i++)
        {
            UtStat& t = g_utt[i];
            if (t.fcbase == s.fcbase && t.fcsize == s.fcsize && t.dbase == s.dbase && t.dW == s.dW && t.dH == s.dH && t.gates == s.gates)
            { t.n++; t.last = fr; return; }
        }
        if (n < ZZ_UT_N)
        {
            LONG i = InterlockedIncrement(&g_utt_n) - 1;
            if (i < ZZ_UT_N) { s.first = s.last = fr; s.n = 1; g_utt[i] = s; return; }
        }
        g_utt_over++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

extern "C" void zz_tile_us_hit(unsigned char* shadow, unsigned char* view, unsigned base, unsigned size)
{
    (void)shadow; (void)view;
    __try
    {
        g_us_calls++;
        if ((base & ~0xFFFu) == ZZ_S9_MAIN) g_us_main++;
        zz_bs_note(g_ust, &g_ust_n, &g_ust_over, base, size);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

extern "C" void zz_tile_sl_hit(unsigned char* g, unsigned char* view, unsigned char* desc, unsigned r9d)
{
    (void)view; (void)r9d;
    __try
    {
        g_sl_calls++;
        g_sl_g = g;
        if (!desc || !zz_ok_mem(desc + 0x10, 0x2C)) return;
        unsigned W = *(unsigned*)(desc + 0x10), H = *(unsigned*)(desc + 0x14), base = *(unsigned*)(desc + 0x38);
        LONG n = g_slt_n;
        for (LONG i = 0; i < n && i < ZZ_SL_N; i++)
            if (g_slt[i].W == W && g_slt[i].H == H && g_slt[i].base == base) { g_slt[i].n++; return; }
        if (n < ZZ_SL_N)
        {
            LONG i = InterlockedIncrement(&g_slt_n) - 1;
            if (i < ZZ_SL_N) { g_slt[i].W = W; g_slt[i].H = H; g_slt[i].base = base; g_slt[i].n = 1; return; }
        }
        g_slt_over++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

extern "C" void zz_tile_pr_hit(unsigned char* thisp, unsigned char* desc, unsigned long long r8, unsigned long long r9)
{
    (void)r8; (void)r9;
    __try
    {
        g_pr_calls++;
        if (!thisp || !desc || !zz_ok_mem(desc, 12) || !zz_ok_mem(thisp + 0xD50, 0x20) || !zz_ok_mem(g_vgpu + 0x5ABCE0, 8))
        { g_pr_unread++; return; }
        unsigned front = *(unsigned*)(desc + 4);
        g_pr_front = front; g_pr_front8 = *(unsigned*)(desc + 8);
        LONG fn = g_prf_n; bool seen = false;
        for (LONG i = 0; i < fn && i < 8; i++) if (g_prf[i].base == front) { g_prf[i].n++; seen = true; break; }
        if (!seen && fn < 8) { LONG i = InterlockedIncrement(&g_prf_n) - 1; if (i < 8) { g_prf[i].base = front; g_prf[i].n = 1; } }
        // 0x29E7: rax = [0x5ABCE0]; rdx = [rax+8], n = ([rax+0x10] - rdx) / 8; 0x2A04 n > 0x4C; 0x2A0A [rdx+0x260] != 0
        unsigned char* vec = *(unsigned char**)(g_vgpu + 0x5ABCE0);
        if (!vec || !zz_ok_mem(vec + 8, 16)) { g_pr_unread++; return; }
        unsigned char* cb = *(unsigned char**)(vec + 8);
        unsigned char* ce = *(unsigned char**)(vec + 0x10);
        unsigned n = (cb && ce > cb) ? (unsigned)((ce - cb) / 8) : 0;
        g_pr_n = n;
        if (n <= 0x4C || !zz_ok_mem(cb + 0x260, 8)) { g_pr_cfg0++; return; }
        unsigned long long c260 = *(unsigned long long*)(cb + 0x260);
        g_pr_cfg260lo = (unsigned)c260;
        if (!c260) { g_pr_cfg0++; return; }
        void* pick;
        if ((front & 0xFFFFF000u) == *(unsigned*)(thisp + 0xD68))
        {
            // 0x2A24: cmp byte [[0x5ABCC0]+0xC8041], 0 -> je (rdi stays 0) / 0x2A2D rdi = slot 2's view
            unsigned char* r9p = zz_ok_mem(g_vgpu + 0x5ABCC0, 8) ? *(unsigned char**)(g_vgpu + 0x5ABCC0) : nullptr;
            bool on = false;
            if (r9p && zz_ok_mem(r9p + 0xC8040, 4)) on = r9p[0xC8041] != 0;
            pick = on ? *(void**)(thisp + 0xD60) : nullptr;
            if (pick) { g_pr_slot2++; return; }
        }
        else
        {
            pick = *(void**)(thisp + 0xD50);
            if (pick) { g_pr_slot1++; return; }
        }
        g_pr_null++;                                                 // rdi == 0 -> 0x2A4B the 0x321C lookup
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

extern "C" void zz_tile_sw_hit(unsigned char* shadow, unsigned char* view, unsigned base, unsigned size)
{
    (void)shadow; (void)view;
    __try
    {
        g_sw_calls++;
        zz_bs_note(g_swt, &g_swt_n, &g_swt_over, base, size);
        DWORD tid = GetCurrentThreadId();
        LONG n = g_swlast_n; if (n > 8) n = 8;
        for (LONG i = 0; i < n; i++)
            if (g_swlast[i].tid == tid) { g_swlast[i].base = base; g_swlast[i].size = size; return; }
        LONG i = InterlockedIncrement(&g_swlast_n) - 1;
        if (i < 8) { g_swlast[i].base = base; g_swlast[i].size = size; g_swlast[i].tid = tid; }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// rcx = entry rsp of 0xBE9C: [+0x28] src y, [+0x30] dest x, [+0x38] dest y, [+0x58] width, [+0x60] height,
// [+0x68] pitch W (the caller's 0xCF95..0xCF60 stores, +8); r9d = src x
extern "C" void zz_tile_dp_hit(unsigned char* ersp, unsigned long long rdx, unsigned long long r8, unsigned sx)
{
    (void)rdx; (void)r8;
    __try
    {
        g_dp_calls++;
        if (!zz_ok_mem(ersp + 0x28, 0x48)) return;
        DpStat s = {};
        s.sx = sx; s.sy = *(unsigned*)(ersp + 0x28); s.dx = *(unsigned*)(ersp + 0x30); s.dy = *(unsigned*)(ersp + 0x38);
        s.w = *(unsigned*)(ersp + 0x58); s.h = *(unsigned*)(ersp + 0x60); s.pitch = *(unsigned*)(ersp + 0x68);
        s.base = 0xFFFFFFFFu;
        DWORD tid = GetCurrentThreadId();
        LONG ln = g_swlast_n; if (ln > 8) ln = 8;
        for (LONG i = 0; i < ln; i++) if (g_swlast[i].tid == tid) { s.base = g_swlast[i].base; break; }
        LONG n = g_dpt_n;
        for (LONG i = 0; i < n && i < ZZ_DP_N; i++)
        {
            DpStat& t = g_dpt[i];
            if (t.base == s.base && t.sx == s.sx && t.sy == s.sy && t.dx == s.dx && t.dy == s.dy && t.w == s.w && t.h == s.h &&
                t.pitch == s.pitch) { t.n++; return; }
        }
        if (n < ZZ_DP_N)
        {
            LONG i = InterlockedIncrement(&g_dpt_n) - 1;
            if (i < ZZ_DP_N) { s.n = 1; g_dpt[i] = s; return; }
        }
        g_dpt_over++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static int zz_tile_install_s9(void)
{
    static const unsigned char sigUT[13] = { 0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 }; // push rbp..r15
    static const unsigned char sigUS[12] = { 0x4C,0x8B,0xDC, 0x49,0x89,0x5B,0x08, 0x57, 0x48,0x83,0xEC,0x70 };   // mov r11,rsp; mov [r11+8],rbx
    static const unsigned char sigSL[10] = { 0x41,0x81,0x78,0x10,0x20,0x03,0x00,0x00, 0x73,0x24 };             // cmp dword [r8+10h],320h; jae
    static const unsigned char sigPR[8]  = { 0x48,0x8B,0xC4, 0x48,0x89,0x58,0x18, 0x55 };                      // mov rax,rsp; mov [rax+18h],rbx
    static const unsigned char sigSW[10] = { 0x48,0x89,0x5C,0x24,0x08, 0x57, 0x48,0x83,0xEC,0x70 };            // mov [rsp+8],rbx; push rdi
    static const unsigned char exDP[5]   = { 0x48,0x8D,0x4C,0x24,0x40 };                                       // lea rcx,[rsp+0x40] = entry rsp
    static const unsigned char exUT[5]   = { 0x4C,0x8D,0x4C,0x24,0x40 };                                       // s10 I-J: lea r9,[rsp+0x40]
    int m = 0;
    m |= zz_tile_install_tap(0x78C8Cu, sigUT, 13, exUT, 5,    (void*)&zz_tile_ut_hit, "s9 I-A untile (0x78C8C) + s10 I-J caller", 5) ? 1 : 0;
    m |= zz_tile_install_tap(0x0C4BCu, sigUS, 12, nullptr, 0, (void*)&zz_tile_us_hit, "s9 I-B untile-src (0xC4BC)", 7)         ? 2 : 0;
    m |= zz_tile_install_tap(0x0C008u, sigSL, 10, nullptr, 0, (void*)&zz_tile_sl_hit, "s9 I-C slots (0xC008)", 8)              ? 4 : 0;
    m |= zz_tile_install_tap(0x02964u, sigPR, 8,  nullptr, 0, (void*)&zz_tile_pr_hit, "s9 I-D present (0x2964)", 7)            ? 8 : 0;
    m |= zz_tile_install_tap(0x52DB0u, sigSW, 10, nullptr, 0, (void*)&zz_tile_sw_hit, "s9 I-E1 shadow-write (0x52DB0)", 5)     ? 16 : 0;
    m |= zz_tile_install_tap(0x0BE9Cu, sigUT, 13, exDP, 5,    (void*)&zz_tile_dp_hit, "s9 I-E2 resolve-dispatch (0xBE9C)", 5)  ? 32 : 0;
    return m;
}

// ---- s10 (build 24, S10_PLAN §2): the direct-resolve route, instruments only ---------------------------
// I-G fn 0x7CC6C(this, rdx base, r8d fmt, r9d pitch W, [e+0x28] W, [e+0x30] rows, [e+0x38] byte, [e+0x40] al,
//     [e+0x48] left, [e+0x50] dest y, [e+0x58] &rect{l,t,r,b}, [e+0x60] &out_x, [e+0x68] &out_y)   (e = entry rsp)
extern "C" void zz_tile_s10g_hit(unsigned char* ersp, unsigned long long base, unsigned long long fmt, unsigned long long pw)
{
    __try
    {
        g_s10g.calls++;
        unsigned k[ZZ_K] = {};
        k[0] = (unsigned)base; k[1] = (unsigned)fmt; k[2] = (unsigned)pw;
        k[3] = *(unsigned*)(ersp + 0x28); k[4] = *(unsigned*)(ersp + 0x30);
        k[5] = *(unsigned*)(ersp + 0x48); k[6] = *(unsigned*)(ersp + 0x50);
        unsigned* rc = *(unsigned**)(ersp + 0x58);
        if (rc && zz_ok_stack(rc, 16)) { k[7] = rc[0]; k[8] = rc[1]; k[9] = rc[2]; k[10] = rc[3]; }
        else k[7] = k[8] = k[9] = k[10] = 0xFFFFFFFFu;
        zz_k_note(g_s10g, k);
        zz_ord_note('G', k[0], k[3], k[4], k[6], k[5]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// I-H fn 0x6B28C(g, &desc): desc +0x10 w, +0x14 h, +0x20 dest x, +0x24 dest y, +0x30 sample select, +0x34 target idx,
//     +0x38 base, +0x3C W, +0x40 H
extern "C" void zz_tile_s10h_hit(unsigned char* g, unsigned char* desc, unsigned long long r8, unsigned long long r9)
{
    (void)g; (void)r8; (void)r9;
    __try
    {
        g_s10h.calls++;
        if (!desc || !zz_ok_stack(desc, 0x50)) return;
        unsigned k[ZZ_K] = {};
        k[0] = *(unsigned*)(desc + 0x38); k[1] = *(unsigned*)(desc + 0x3C); k[2] = *(unsigned*)(desc + 0x40);
        k[3] = *(unsigned*)(desc + 0x10); k[4] = *(unsigned*)(desc + 0x14);
        k[5] = *(unsigned*)(desc + 0x20); k[6] = *(unsigned*)(desc + 0x24);
        k[7] = *(unsigned*)(desc + 0x34); k[8] = *(unsigned*)(desc + 0x30);
        zz_k_note(g_s10h, k);
        zz_ord_note('D', k[0], k[6], k[7], k[4], k[5]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// I-K fn 0x73AD0(ctx, rdx = &[this+0x150], r8, r9d = pitch*32*s, [e+0x28] height*s, [e+0x30] &rect{x,y,w,h},
//     [e+0x50] row count, [e+0x58] macro start, [e+0x88] (left-dest x)*s, [e+0x90] (top-dest y)*s, [e+0x98] level)
extern "C" void zz_tile_s10k_hit(unsigned char* ersp, unsigned char* p150, unsigned long long r8, unsigned long long r9)
{
    (void)r8;
    __try
    {
        g_s10k.calls++;
        unsigned k[ZZ_K] = {};
        unsigned char* thisp = p150 ? p150 - 0x150 : nullptr;
        k[0] = 0xFFFFFFFFu;
        if (thisp && zz_ok_mem(thisp + 8, 8))
        {
            unsigned char* fc = *(unsigned char**)(thisp + 8);
            if (fc && zz_ok_mem(fc + 0x14, 4)) k[0] = *(unsigned*)(fc + 0x14);
        }
        unsigned* rc = *(unsigned**)(ersp + 0x30);
        if (rc && zz_ok_stack(rc, 16)) { k[1] = rc[0]; k[2] = rc[1]; k[3] = rc[2]; k[4] = rc[3]; }
        else k[1] = k[2] = k[3] = k[4] = 0xFFFFFFFFu;
        k[5] = *(unsigned*)(ersp + 0x50); k[6] = *(unsigned*)(ersp + 0x58);
        k[7] = *(unsigned*)(ersp + 0x88); k[8] = *(unsigned*)(ersp + 0x90);
        k[9] = (unsigned)r9; k[10] = *(unsigned*)(ersp + 0x28); k[11] = *(unsigned*)(ersp + 0x98);
        zz_k_note(g_s10k, k);
        zz_ord_note('K', k[0], k[2], k[4], k[5], k[6]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// I-L fn 0xEAB0(shadow, rdx = &view, r8d = base, r9d = size, [e+0x28] kind) -- only bases in [19000000, 20000000)
extern "C" void zz_tile_s10l_hit(unsigned char* ersp, unsigned long long view, unsigned long long base, unsigned long long size)
{
    (void)view;
    __try
    {
        g_s10l.calls++;
        unsigned b = (unsigned)base;
        if (b < 0x19000000u || b >= 0x20000000u) return;
        unsigned k[ZZ_K] = {};
        k[0] = b; k[1] = (unsigned)size; k[2] = *(unsigned*)(ersp + 0x28);
        zz_caller(ersp, k + 3);
        zz_k_note(g_s10l, k);
        unsigned pg = b & ~0xFFFu;
        if (pg == ZZ_S9_MAIN) g_s10l_main++; else if (pg == ZZ_S9_BIN1) g_s10l_bin1++; else if (pg == 0x1FD41000u) g_s10l_1fd++;
        if (pg == ZZ_S9_MAIN || pg == ZZ_S9_BIN1) zz_ord_note('L', b, k[1], k[2], k[4] ? 0xFFFFFFFFu : k[3], 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// I-M fn 0xE158(cmdlist, edx = groups x, r8d = groups y) = cmdlist->vtbl+0x70 (Dispatch)
extern "C" void zz_tile_s10m_hit(unsigned char* ersp, unsigned long long gx, unsigned long long gy, unsigned long long r9)
{
    (void)r9;
    __try
    {
        g_s10m.calls++;
        unsigned k[ZZ_K] = {};
        zz_caller(ersp, k);
        k[2] = (unsigned)gx; k[3] = (unsigned)gy;
        zz_k_note(g_s10m, k);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// I-N the executor fn 0x771E4(obj, desc, r8b, r9b) / fn 0x76EE4(obj, desc, out): the desc layout of I-H
static void zz_s10n_note(KTab& tb, char kind, unsigned char* desc)
{
    tb.calls++;
    if (!desc || !zz_ok_stack(desc, 0x50)) return;
    unsigned k[ZZ_K] = {};
    k[0] = *(unsigned*)(desc + 0x38); k[1] = *(unsigned*)(desc + 0x20); k[2] = *(unsigned*)(desc + 0x24);
    k[3] = *(unsigned*)(desc + 0x10); k[4] = *(unsigned*)(desc + 0x14); k[5] = *(unsigned*)(desc + 0x34);
    k[6] = *(unsigned*)(desc + 0x30); k[7] = *(unsigned*)(desc + 0x40);
    zz_k_note(tb, k);
    zz_ord_note(kind, k[0], k[2], k[5], k[4], k[1]);
}
extern "C" void zz_tile_s10na_hit(unsigned char* obj, unsigned char* desc, unsigned long long r8, unsigned long long r9)
{
    (void)obj; (void)r8; (void)r9;
    __try { zz_s10n_note(g_s10na, 'N', desc); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
extern "C" void zz_tile_s10nb_hit(unsigned char* obj, unsigned char* desc, unsigned long long r8, unsigned long long r9)
{
    (void)obj; (void)r8; (void)r9;
    __try { zz_s10n_note(g_s10nb, 'n', desc); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static int zz_tile_install_s10(void)
{
    // entry bytes read from VGPUDX12 3f002570 (S10_PLAN "Verified today"); every displaced run is whole instructions
    static const unsigned char sigG[11] = { 0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57 };         // push rbp/rbx/rsi/rdi | r12..
    static const unsigned char sigH[11] = { 0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x57 };       // mov [rsp+10h],rbx
    static const unsigned char sigK[13] = { 0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 }; // push rbp/rbx/rsi/rdi | r12..
    static const unsigned char sigL[11] = { 0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08, 0x48,0x89,0x68,0x10 };       // mov rax,rsp; mov [rax+8],rbx (7)
    static const unsigned char sigM[10] = { 0x48,0x89,0x5C,0x24,0x08, 0x57, 0x48,0x83,0xEC,0x20 };            // mov [rsp+8],rbx
    static const unsigned char sigNa[8] = { 0x48,0x89,0x5C,0x24,0x18, 0x55,0x56,0x57 };                       // mov [rsp+18h],rbx
    static const unsigned char sigNb[8] = { 0x48,0x89,0x5C,0x24,0x20, 0x55,0x56,0x57 };                       // mov [rsp+20h],rbx
    static const unsigned char exE[5]   = { 0x48,0x8D,0x4C,0x24,0x40 };                                       // lea rcx,[rsp+0x40] = entry rsp
    int m = 0;
    m |= zz_tile_install_tap(0x7CC6Cu, sigG, 11, exE, 5,      (void*)&zz_tile_s10g_hit,  "s10 I-G target-lookup (0x7CC6C)", 5)   ? 0x01 : 0;
    m |= zz_tile_install_tap(0x6B28Cu, sigH, 11, nullptr, 0,  (void*)&zz_tile_s10h_hit,  "s10 I-H dirty (0x6B28C)", 5)           ? 0x02 : 0;
    m |= zz_tile_install_tap(0x73AD0u, sigK, 13, exE, 5,      (void*)&zz_tile_s10k_hit,  "s10 I-K untile-dispatch (0x73AD0)", 5) ? 0x04 : 0;
    m |= zz_tile_install_tap(0x0EAB0u, sigL, 11, exE, 5,      (void*)&zz_tile_s10l_hit,  "s10 I-L shadow-views (0xEAB0)", 7)     ? 0x08 : 0;
    m |= zz_tile_install_tap(0x0E158u, sigM, 10, exE, 5,      (void*)&zz_tile_s10m_hit,  "s10 I-M dispatches (0xE158)", 5)       ? 0x10 : 0;
    m |= zz_tile_install_tap(0x771E4u, sigNa, 8, nullptr, 0,  (void*)&zz_tile_s10na_hit, "s10 I-N executor (0x771E4)", 5)        ? 0x20 : 0;
    m |= zz_tile_install_tap(0x76EE4u, sigNb, 8, nullptr, 0,  (void*)&zz_tile_s10nb_hit, "s10 I-N executor (0x76EE4)", 5)        ? 0x40 : 0;
    return m;
}

extern "C" void zz_tile_resolve_hit(unsigned char* blk, unsigned char* thisp)
{
    __try
    {
        g_rs_calls++;
        unsigned char* ctx = blk - 0x1E0;
        unsigned char* rf = *(unsigned char**)(ctx + 0xA0);
        if (!rf) return;
        if (g_rsfix2) zz_rx2_decide(blk, rf, *(int*)(ctx + 0x1F0), *(int*)(ctx + 0x1F4));
        if (!g_probe) return;
        if (!g_cfg_once && InterlockedCompareExchange(&g_cfg_once, 1, 0) == 0) { zz_cfg_read(thisp); g_cfg_ready = 1; }
        if (!g_s10cfg_once)
        {
            // s10 I2++: 0xC23E `cmp byte [r8+0x858], 0` (r8 = blk, 0xC1D4) -> 0x588E1 `mov rcx,[rip+0x5533D8]` (= RVA 0x5ABCC0)
            // `mov edx,[rcx+0x1C]`; 0xC27C `cmp dword [r8+0x854], 0`
            unsigned c = *(unsigned*)(rf + 0xACC0);
            if ((c & 0x300000u) == 0x100000u && ((c >> 4) & 7) == 4 && InterlockedCompareExchange(&g_s10cfg_once, 1, 0) == 0)
            {
                static const unsigned char site[7] = { 0x48,0x8B,0x0D,0xD8,0x33,0x55,0x00 };
                g_cfg.s10frame = (unsigned)zz_swaps; g_cfg.s10ctrl = c; g_cfg.s10base = *(unsigned*)(rf + 0xACC4);
                g_cfg.s10b854 = *(unsigned*)(blk + 0x854); g_cfg.s10b858 = blk[0x858];
                g_cfg.s10siteOk = memcmp(g_vgpu + 0x588E1u, site, 7) == 0;
                if (zz_ok_mem(g_vgpu + 0x5ABCC0u, 8))
                {
                    unsigned char* gp = *(unsigned char**)(g_vgpu + 0x5ABCC0u);
                    if (gp && zz_ok_mem(gp + 0x1C, 4)) { g_cfg.s10glob = *(unsigned*)(gp + 0x1C); g_cfg.s10globOk = 1; }
                }
                g_s10cfg_ready = 1;
            }
        }
        RsStat s;
        s.top = *(int*)(blk + 0x844); s.left = *(int*)(blk + 0x848);
        s.bottom = *(int*)(blk + 0x84C); s.right = *(int*)(blk + 0x850);
        s.ysh = *(int*)(blk + 0x85C);
        s.wy = *(int*)(ctx + 0x1F4);
        int wx = *(int*)(ctx + 0x1F0);
        s.ctrl = *(unsigned*)(rf + 0xACC0); s.base = *(unsigned*)(rf + 0xACC4);
        s.pitch = *(unsigned*)(rf + 0xACC8); s.info = *(unsigned*)(rf + 0xACCC);
        s.surf = *(unsigned*)(rf + 0xA060);
        s.wtl = *(unsigned*)(rf + 0xA264); s.wbr = *(unsigned*)(rf + 0xA268);
        s.vtxen = (*(unsigned*)(rf + 0xA874) >> 16) & 1;
        s.sel = *(unsigned*)(rf + 0x31B8);
        s.n = 1;
        if ((s.wy || wx) && g_rs_first_n < 64)
        {
            LONG i = InterlockedIncrement(&g_rs_first_n) - 1;
            if (i < 64) { g_rs_first[i] = s; g_rs_first_frame[i] = (unsigned)zz_swaps; }
        }
        LONG n = g_rst_n;
        for (LONG i = 0; i < n && i < ZZ_RS_N; i++)
        {
            RsStat& t = g_rst[i];
            if (t.base == s.base && t.top == s.top && t.bottom == s.bottom && t.left == s.left && t.right == s.right &&
                t.wy == s.wy && t.sel == s.sel && t.pitch == s.pitch && t.vtxen == s.vtxen) { t.n++; return; }
        }
        if (n < ZZ_RS_N)
        {
            LONG i = InterlockedIncrement(&g_rst_n) - 1;
            if (i < ZZ_RS_N) { g_rst[i] = s; return; }
        }
        g_rst_over++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

#define ZZ_RESOLVE_RVA 0xC080u
static const unsigned char ZZ_RESOLVE_SIG[6] = { 0x48,0x89,0x5C,0x24,0x18, 0x55 };   // mov [rsp+18h],rbx; push rbp

static bool zz_tile_install_resolve(void)
{
    unsigned char* target = g_vgpu + ZZ_RESOLVE_RVA;
    if (memcmp(target, ZZ_RESOLVE_SIG, sizeof(ZZ_RESOLVE_SIG)) != 0)
    { zz_tile_say("# resolve sig mismatch at +%X: %02X %02X %02X %02X %02X %02X", ZZ_RESOLVE_RVA, target[0], target[1], target[2], target[3], target[4], target[5]); return false; }
    unsigned char* t = (unsigned char*)zz_tile_alloc_near((uintptr_t)target, 128);
    if (!t) { zz_tile_say("# resolve: no near trampoline"); return false; }
    // function entry (rsp == 8 mod 16): the ctxcap encoding, with rcx = rdx (the ctx+0x1E0 block)
    size_t i = 0;
    t[i++] = 0x9C; t[i++] = 0x50; t[i++] = 0x51; t[i++] = 0x52;
    t[i++] = 0x41; t[i++] = 0x50; t[i++] = 0x41; t[i++] = 0x51; t[i++] = 0x41; t[i++] = 0x52; t[i++] = 0x41; t[i++] = 0x53;
    t[i++] = 0x48; t[i++] = 0x8B; t[i++] = 0xCA;                                 // mov rcx, rdx
    t[i++] = 0x48; t[i++] = 0x8B; t[i++] = 0x54; t[i++] = 0x24; t[i++] = 0x28;   // mov rdx,[rsp+28h] = saved rcx (this)
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xEC; t[i++] = 0x28;
    t[i++] = 0x48; t[i++] = 0xB8; *(void**)(t + i) = (void*)&zz_tile_resolve_hit; i += 8;
    t[i++] = 0xFF; t[i++] = 0xD0;
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xC4; t[i++] = 0x28;
    t[i++] = 0x41; t[i++] = 0x5B; t[i++] = 0x41; t[i++] = 0x5A; t[i++] = 0x41; t[i++] = 0x59; t[i++] = 0x41; t[i++] = 0x58;
    t[i++] = 0x5A; t[i++] = 0x59; t[i++] = 0x58; t[i++] = 0x9D;
    t[i++] = 0x48; t[i++] = 0x89; t[i++] = 0x5C; t[i++] = 0x24; t[i++] = 0x18;   // displaced: mov [rsp+18h],rbx
    t[i++] = 0xFF; t[i++] = 0x25; *(int*)(t + i) = 0; i += 4;
    *(void**)(t + i) = (void*)(target + 5); i += 8;
    intptr_t rel = (intptr_t)t - ((intptr_t)target + 5);
    if (rel > 0x7FFFFFFF || rel < -0x7FFFFFFF) { zz_tile_say("# resolve: out of rel32 range"); return false; }
    DWORD old = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) { zz_tile_say("# resolve: VirtualProtect failed (%lu)", GetLastError()); return false; }
    target[0] = 0xE9; *(int*)(target + 1) = (int)rel;
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    zz_tile_say("# resolve tap installed at VGPUDX12+%X (fn 0xC080 entry)", ZZ_RESOLVE_RVA);
    return true;
}

// ---- trampolines + hook A install (NFS 1552-1624) -----------------------------------------
static void* zz_tile_alloc_near(uintptr_t near_addr, size_t n)
{
    const uintptr_t GRAN = 0x10000;
    uintptr_t base = near_addr & ~(GRAN - 1);
    for (uintptr_t delta = GRAN; delta < 0x4000000; delta += GRAN)
        for (int dir = 0; dir < 2; dir++)
        {
            void* p = VirtualAlloc((void*)(dir ? base + delta : base - delta), n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    return nullptr;
}

static bool zz_tile_install_ctxcap(void)
{
    unsigned char* target = g_vgpu + ZZ_FUNNEL_RVA;
    if (memcmp(target, ZZ_FUNNEL_SIG, sizeof(ZZ_FUNNEL_SIG)) != 0)
    { zz_tile_say("# funnel sig mismatch at +%X: %02X %02X %02X %02X %02X", ZZ_FUNNEL_RVA, target[0], target[1], target[2], target[3], target[4]); return false; }
    unsigned char* t = (unsigned char*)zz_tile_alloc_near((uintptr_t)target, 128);
    if (!t) { zz_tile_say("# ctxcap: no near trampoline"); return false; }
    size_t i = 0;
    t[i++] = 0x9C; t[i++] = 0x50; t[i++] = 0x51; t[i++] = 0x52;
    t[i++] = 0x41; t[i++] = 0x50; t[i++] = 0x41; t[i++] = 0x51; t[i++] = 0x41; t[i++] = 0x52; t[i++] = 0x41; t[i++] = 0x53;
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xEC; t[i++] = 0x28;
    t[i++] = 0x48; t[i++] = 0xB8; *(void**)(t + i) = (void*)&zz_tile_reg; i += 8;
    t[i++] = 0xFF; t[i++] = 0xD0;
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xC4; t[i++] = 0x28;
    t[i++] = 0x41; t[i++] = 0x5B; t[i++] = 0x41; t[i++] = 0x5A; t[i++] = 0x41; t[i++] = 0x59; t[i++] = 0x41; t[i++] = 0x58;
    t[i++] = 0x5A; t[i++] = 0x59; t[i++] = 0x58; t[i++] = 0x9D;
    t[i++] = 0x48; t[i++] = 0x89; t[i++] = 0x5C; t[i++] = 0x24; t[i++] = 0x08;   // displaced: mov [rsp+8],rbx
    t[i++] = 0xFF; t[i++] = 0x25; *(int*)(t + i) = 0; i += 4;
    *(void**)(t + i) = (void*)(target + 5); i += 8;
    intptr_t rel = (intptr_t)t - ((intptr_t)target + 5);
    if (rel > 0x7FFFFFFF || rel < -0x7FFFFFFF) { zz_tile_say("# ctxcap: out of rel32 range"); return false; }
    DWORD old = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) { zz_tile_say("# ctxcap: VirtualProtect failed (%lu)", GetLastError()); return false; }
    target[0] = 0xE9; *(int*)(target + 1) = (int)rel;
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    zz_tile_say("# reg tally installed at VGPUDX12+%X (PM4 funnel entry)", ZZ_FUNNEL_RVA);
    return true;
}

// ---- the log, from our own thread (NFS zz_tile_frame 1383-1549, trimmed) ---------------------
static void zz_tile_drain(void)
{
    if (!g_ring) return;
    LONG end = g_widx; if (end > ZZ_TILE_RING) end = ZZ_TILE_RING;
    for (long i = g_dumped; i < end; i++)
    {
        TileRec* r = &g_ring[i];
        if (r->kind == 6)
            zz_tile_say("TEXC   evt=%-7u f%-5u fmt=%02X a8=%u a5=%d a6=%d a7=%08X tid=%u",
                        r->evt, r->frame, r->raw & 0x3F, r->wseq & 3, r->woffX, r->woffY, r->wnz, r->tid);
        else
            zz_tile_say("SNAP   %s evt=%-7u f%-5u vp=(%.0f,%.0f %.0fx%.0f) woff=(%d,%d) sel=%08X selHi=%08X mask=%08X tid=%u",
                        r->fixed ? "FIX " : "MISS", r->evt, r->frame, r->tlx, r->tly, r->w, r->h,
                        r->woffX, r->woffY, r->raw, r->wnz, r->wseq, r->tid);
    }
    g_dumped = end;
}

// s9: one table per instrument, one tuple per line; per-frame rates over the last summary window
static void zz_tile_summary_s9(long frame)
{
    static long pf, put_m, put_b, pus_m;
    long df = frame - pf; if (df < 1) df = 1;
    long utm = g_ut_main, utb = g_ut_bin1, usm = g_us_main;
    LONG un = g_utt_n; if (un > ZZ_UT_N) un = ZZ_UT_N;
    zz_tile_say("# f%-6ld untile (0x78C8C): calls=%ld tuples=%ld (overflow %ld) | %08X: %ld (%.2f/frame) %08X: %ld (%.2f/frame) over %ld frames"
                " -- gates b0 [+C8] b1 [+10] b2 [+158] b3 [fc+50] b4 fc",
                frame, (long)g_ut_calls, (long)un, (long)g_utt_over, ZZ_S9_MAIN, utm, (double)(utm - put_m) / df,
                ZZ_S9_BIN1, utb, (double)(utb - put_b) / df, df);
    for (LONG k = 0; k < un; k++)
    {
        UtStat& t = g_utt[k];
        zz_tile_say("#   ut fc base=%08X size=%08X pitch=%u | desc base=%08X W=%u H=%u | gates=%02X n%ld f%u..f%u",
                    t.fcbase, t.fcsize, t.fcpitch, t.dbase, t.dW, t.dH, t.gates, (long)t.n, t.first, t.last);
    }
    LONG bn = g_ust_n; if (bn > ZZ_BS_N) bn = ZZ_BS_N;
    zz_tile_say("# f%-6ld untile-src (0xC4BC): calls=%ld tuples=%ld (overflow %ld) | %08X: %ld (%.2f/frame)",
                frame, (long)g_us_calls, (long)bn, (long)g_ust_over, ZZ_S9_MAIN, usm, (double)(usm - pus_m) / df);
    for (LONG k = 0; k < bn; k++)
        zz_tile_say("#   us base=%08X size=%u n%ld f%u..f%u", g_ust[k].base, g_ust[k].size, (long)g_ust[k].n, g_ust[k].first, g_ust[k].last);
    pf = frame; put_m = utm; put_b = utb; pus_m = usm;

    LONG sn = g_slt_n; if (sn > ZZ_SL_N) sn = ZZ_SL_N;
    zz_tile_say("# f%-6ld slots (0xC008): calls=%ld tuples=%ld (overflow %ld)", frame, (long)g_sl_calls, (long)sn, (long)g_slt_over);
    for (LONG k = 0; k < sn; k++)
        zz_tile_say("#   sl W=%u H=%u base=%08X n%ld", g_slt[k].W, g_slt[k].H, g_slt[k].base, (long)g_slt[k].n);
    unsigned char* g = g_sl_g;
    if (g && zz_ok_mem(g + 0xD50, 0x20))
        zz_tile_say("#   live slot1=%p/%08X slot2=%p/%08X flag=%u", *(void**)(g + 0xD50), *(unsigned*)(g + 0xD58),
                    *(void**)(g + 0xD60), *(unsigned*)(g + 0xD68), (unsigned)g[0xD5C]);
    else zz_tile_say("#   live slots unreadable (g=%p)", g);

    LONG fn = g_prf_n; if (fn > 8) fn = 8;
    char line[400]; int off = 0;
    for (LONG k = 0; k < fn; k++)
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " %08X:n%ld", g_prf[k].base, (long)g_prf[k].n);
    zz_tile_say("# f%-6ld present (0x2964): calls=%ld COMPUTED branch: cfg0/absent=%ld slot1=%ld slot2=%ld null->lookup=%ld unread=%ld"
                " | cfg n=%u [260]lo=%08X | last front=%08X +8=%08X | fronts:%s",
                frame, (long)g_pr_calls, (long)g_pr_cfg0, (long)g_pr_slot1, (long)g_pr_slot2, (long)g_pr_null, (long)g_pr_unread,
                g_pr_n, g_pr_cfg260lo, g_pr_front, g_pr_front8, fn ? line : " none");

    LONG wn = g_swt_n; if (wn > ZZ_BS_N) wn = ZZ_BS_N;
    zz_tile_say("# f%-6ld shadow-write (0x52DB0): calls=%ld tuples=%ld (overflow %ld)", frame, (long)g_sw_calls, (long)wn, (long)g_swt_over);
    for (LONG k = 0; k < wn; k++)
        zz_tile_say("#   sw base=%08X size=%u n%ld f%u..f%u", g_swt[k].base, g_swt[k].size, (long)g_swt[k].n, g_swt[k].first, g_swt[k].last);
    LONG dn = g_dpt_n; if (dn > ZZ_DP_N) dn = ZZ_DP_N;
    zz_tile_say("# f%-6ld resolve-dispatch (0xBE9C): calls=%ld tuples=%ld (overflow %ld) -- base = this thread's last 0x52DB0 base",
                frame, (long)g_dp_calls, (long)dn, (long)g_dpt_over);
    for (LONG k = 0; k < dn; k++)
    {
        DpStat& t = g_dpt[k];
        zz_tile_say("#   dp base=%08X src (%u,%u) dest (%u,%u) %ux%u pitch=%u n%ld", t.base, t.sx, t.sy, t.dx, t.dy, t.w, t.h, t.pitch, (long)t.n);
    }
}

// s10: one table per instrument, one tuple per line
static LONG zz_k_rows(KTab& tb) { LONG n = tb.n; return n > tb.cap ? tb.cap : n; }
static void zz_tile_summary_s10(long frame)
{
    static long pf, pl_m, pl_b, pl_f;
    long df = frame - pf; if (df < 1) df = 1;
    char c1[24];
    LONG n = zz_k_rows(g_s10g);
    zz_tile_say("# f%-6ld target-lookup (0x7CC6C): calls=%ld tuples=%ld (overflow %ld)", frame, (long)g_s10g.calls, (long)n, (long)g_s10g.over);
    for (LONG i = 0; i < n; i++)
    {
        unsigned* k = g_s10g.t[i].k;
        zz_tile_say("#   tg base=%08X fmt=%u pitchW=%u W=%u rows=%u left=%u desty=%u rect {%d,%d,%d,%d} n%ld f%u..f%u", k[0], k[1], k[2], k[3],
                    k[4], k[5], k[6], (int)k[7], (int)k[8], (int)k[9], (int)k[10], (long)g_s10g.t[i].n, g_s10g.t[i].first, g_s10g.t[i].last);
    }
    n = zz_k_rows(g_s10h);
    zz_tile_say("# f%-6ld dirty (0x6B28C): calls=%ld tuples=%ld (overflow %ld) -- the placement the untile is handed", frame,
                (long)g_s10h.calls, (long)n, (long)g_s10h.over);
    for (LONG i = 0; i < n; i++)
    {
        unsigned* k = g_s10h.t[i].k;
        zz_tile_say("#   dh base=%08X W=%u H=%u | rect %ux%u dest (%u,%u) | idx=%u ss=%u n%ld f%u..f%u", k[0], k[1], k[2], k[3], k[4], k[5], k[6],
                    k[7], k[8], (long)g_s10h.t[i].n, g_s10h.t[i].first, g_s10h.t[i].last);
    }
    n = zz_k_rows(g_s10j);
    zz_tile_say("# f%-6ld untile-callers (0x78C8C): calls=%ld tuples=%ld (overflow %ld)", frame, (long)g_s10j.calls, (long)n, (long)g_s10j.over);
    for (LONG i = 0; i < n; i++)
    {
        unsigned* k = g_s10j.t[i].k;
        zz_tile_say("#   uj caller=%s fc=%08X desc=%08X n%ld f%u..f%u", zz_caller_str(c1, sizeof(c1), k[0], k[1]), k[2], k[3],
                    (long)g_s10j.t[i].n, g_s10j.t[i].first, g_s10j.t[i].last);
    }
    n = zz_k_rows(g_s10k);
    zz_tile_say("# f%-6ld untile-dispatch (0x73AD0): calls=%ld tuples=%ld (overflow %ld)", frame, (long)g_s10k.calls, (long)n, (long)g_s10k.over);
    for (LONG i = 0; i < n; i++)
    {
        unsigned* k = g_s10k.t[i].k;
        zz_tile_say("#   uk fc=%08X rect {%d,%d,%d,%d} rows=%u macro=%u t88=%d t90=%d pitch=%u height=%u level=%u n%ld f%u..f%u", k[0], (int)k[1],
                    (int)k[2], (int)k[3], (int)k[4], k[5], k[6], (int)k[7], (int)k[8], k[9], k[10], k[11], (long)g_s10k.t[i].n,
                    g_s10k.t[i].first, g_s10k.t[i].last);
    }
    n = zz_k_rows(g_s10l);
    long lm = g_s10l_main, lb = g_s10l_bin1, lf = g_s10l_1fd;
    zz_tile_say("# f%-6ld shadow-views (0xEAB0): calls=%ld tuples=%ld (overflow %ld) | %08X %.2f/frame %08X %.2f/frame 1FD41000 %.2f/frame"
                " over %ld frames -- caller = the return address inside 0xEAB0's caller (0x52DB0.. / 0xC4BC.. = the known wrappers)",
                frame, (long)g_s10l.calls, (long)n, (long)g_s10l.over, ZZ_S9_MAIN, (double)(lm - pl_m) / df, ZZ_S9_BIN1,
                (double)(lb - pl_b) / df, (double)(lf - pl_f) / df, df);
    for (LONG i = 0; i < n; i++)
    {
        unsigned* k = g_s10l.t[i].k;
        zz_tile_say("#   sv base=%08X size=%08X kind=%u caller=%s n%ld f%u..f%u", k[0], k[1], k[2], zz_caller_str(c1, sizeof(c1), k[3], k[4]),
                    (long)g_s10l.t[i].n, g_s10l.t[i].first, g_s10l.t[i].last);
    }
    pf = frame; pl_m = lm; pl_b = lb; pl_f = lf;
    n = zz_k_rows(g_s10m);
    zz_tile_say("# f%-6ld dispatches (0xE158): calls=%ld tuples=%ld (overflow %ld)", frame, (long)g_s10m.calls, (long)n, (long)g_s10m.over);
    for (LONG i = 0; i < n; i++)
    {
        unsigned* k = g_s10m.t[i].k;
        zz_tile_say("#   dm caller=%s groups %ux%u n%ld f%u..f%u", zz_caller_str(c1, sizeof(c1), k[0], k[1]), k[2], k[3], (long)g_s10m.t[i].n,
                    g_s10m.t[i].first, g_s10m.t[i].last);
    }
    KTab* ex[2] = { &g_s10na, &g_s10nb };
    zz_tile_say("# f%-6ld executor (0x771E4/0x76EE4): calls=%ld/%ld tuples=%ld/%ld (overflow %ld/%ld)", frame, (long)g_s10na.calls,
                (long)g_s10nb.calls, (long)zz_k_rows(g_s10na), (long)zz_k_rows(g_s10nb), (long)g_s10na.over, (long)g_s10nb.over);
    for (int s = 0; s < 2; s++)
    {
        n = zz_k_rows(*ex[s]);
        for (LONG i = 0; i < n; i++)
        {
            unsigned* k = ex[s]->t[i].k;
            zz_tile_say("#   ex %s base=%08X dest (%u,%u) rect %ux%u H=%u idx=%u ss=%u n%ld f%u..f%u", s ? "76EE4" : "771E4", k[0], k[1], k[2],
                        k[3], k[4], k[7], k[5], k[6], (long)ex[s]->t[i].n, ex[s]->t[i].first, ex[s]->t[i].last);
        }
    }
}

// s10: the one-shot I2++ line and the I-J ORDER stamps, from the per-frame drain
static void zz_tile_drain_s10(void)
{
    if (g_s10cfg_ready && !g_s10cfg_printed)
    {
        g_s10cfg_printed = 1;
        zz_tile_say("# cfg s10: first sample-select-4 convert resolve f%u ctrl=%08X base=%08X | [blk+854]=%08X [blk+858]=%02X | "
                    "[[5ABCC0]+1C]=%s%X (site 588E1 %s) -- 858 != 0 takes the sample select from the global (0->0 1->4 2->6)",
                    g_cfg.s10frame, g_cfg.s10ctrl, g_cfg.s10base, g_cfg.s10b854, g_cfg.s10b858, g_cfg.s10globOk ? "" : "?",
                    g_cfg.s10glob, g_cfg.s10siteOk ? "bytes match" : "BYTES DIFFER");
    }
    LONG lo = g_ord_lo;
    if (!lo || g_ord_printed || (long)zz_swaps < lo + 4) return;
    g_ord_printed = 1;
    LONG n = g_ord_n; if (n > ZZ_ORD_N) n = ZZ_ORD_N;
    zz_tile_say("# ORDER f%ld..f%ld: %ld stamps (overflow %ld) -- U untile(caller fc base desty H) G lookup(base W rows desty left)"
                " D dirty(base desty idx h destx) K untile-dispatch(fc recty recth rows macro) L view(base size kind caller)"
                " N/n executor 771E4/76EE4(base desty idx h destx)", (long)lo, (long)lo + 3, (long)n, (long)g_ord_over);
    for (LONG i = 0; i < n; i++)
    {
        OrdRec& r = g_ord[i];
        zz_tile_say("ORDER f%u %c %08X %08X %08X %08X %08X", r.frame, r.kind, r.a, r.b, r.c, r.d, r.e);
    }
}

static void zz_tile_summary(long frame)
{
    char line[600]; int off = 0;
    LONG dn = g_rec_dims_n; if (dn > 16) dn = 16;
    for (LONG k = 0; k < dn; k++)
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " %ux%u:n%ld", g_rec_dims[k].w, g_rec_dims[k].h, (long)g_rec_dims[k].n);
    zz_tile_say("# f%-6ld stamped=%ld bin-draws-not-stamped=%ld snaps=%ld | rec dims (1/16 sampled):%s",
                frame, (long)g_stamped, (long)g_bin_other, (long)g_snap_n, dn ? line : " none");
    off = 0;
    LONG tn = g_tuples_n; if (tn > 16) tn = 16;
    for (LONG k = 0; k < tn; k++)
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " [sel=%08X hi=%08X mask=%08X woff=%d,%d n%ld]",
                           g_tuples[k].sel, g_tuples[k].selHi, g_tuples[k].mask, g_tuples[k].wx, g_tuples[k].wy, (long)g_tuples[k].n);
    zz_tile_say("# f%-6ld bin tuples (1/4 sampled):%s", frame, tn ? line : " none");
    off = 0;
    LONG xn = g_texc_stat_n; if (xn > 64) xn = 64;
    for (LONG k = 0; k < xn; k++)
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " f%02X/a8=%u:n%u", g_texc_stats[k].key >> 8, g_texc_stats[k].key & 3, g_texc_stats[k].n);
    zz_tile_say("# f%-6ld texc=%ld fmt/a8:%s", frame, (long)g_texc_n, xn ? line : " none");
    zz_tile_say("# f%-6ld WOFF(0x2080): total=%ld nonzero=%ld | copy(0x23xx)=%ld | ctxs=%ld [%p %p %p %p]",
                frame, (long)g_woff_total, (long)g_woff_nonzero, (long)g_copy_total,
                (long)g_ctx_seen_n, g_ctx_seen[0], g_ctx_seen[1], g_ctx_seen[2], g_ctx_seen[3]);
    off = 0; int shown = 0;
    LONG w = g_woff_widx;
    for (LONG k = 0; k < ZZ_WOFF_RING && k < w && shown < 8; k++)
    {
        RegHit* h = &g_woff_ring[(w - 1 - k) & (ZZ_WOFF_RING - 1)];
        if (!h->val) continue;
        shown++;
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " f%u:%08X[prod=%08X%08X mask=%08X sel=%08X]",
                           h->frame, h->val, h->prodHi, h->prodLo, h->maskLo, h->selLo);
    }
    zz_tile_say("# last NONZERO WOFF writes:%s", shown ? line : " none");
    if (g_rsfix2)
    {
        zz_tile_say("# f%-6ld rsfix2: decided=%ld used=%ld nomatch=%ld notyet=%ld arm=%ld", frame, (long)g_rx2_decided,
                    (long)g_rx2_used, (long)g_rx2_nomatch, (long)g_rx2_notyet, g_rsfix2_arm);
        LONG pn = g_rxp2_n; if (pn > 8) pn = 8;
        for (LONG k = 0; k < pn; k++)
            zz_tile_say("#   parent %08X W=%u H=%u fmt=%u sel=%08X f%u", g_rxp2[k].base, g_rxp2[k].W, g_rxp2[k].H,
                        g_rxp2[k].fmt, g_rxp2[k].sel, g_rxp2[k].frame);
        LONG qn = g_rx2pair_n; if (qn > 8) qn = 8;
        for (LONG k = 0; k < qn; k++)
            zz_tile_say("#   pair %08X -> %08X W=%u H=%u rows=%u n%ld", g_rx2pair[k].child, g_rx2pair[k].parent, g_rx2pair[k].W,
                        g_rx2pair[k].H, g_rx2pair[k].rows, (long)g_rx2pair[k].n);
    }
    if (!g_probe) return;

    // s8 I2 (once) / I1 / I3
    if (g_cfg_ready && !g_cfg_printed)
    {
        g_cfg_printed = 1;
        char cf[6][24];
        for (int k = 0; k < 6; k++)
            if (g_cfg.has & (1u << k)) _snprintf_s(cf[k], sizeof(cf[k]), _TRUNCATE, "%llX", g_cfg.e[k]);
            else strcpy_s(cf[k], sizeof(cf[k]), "absent");
        off = 0;
        for (unsigned k = 0; k < g_cfg.listn; k++)
            off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " (W=%u m=%llX)", g_cfg.listW[k], g_cfg.listM[k]);
        zz_tile_say("# cfg: this=%p g=%p n=%u [F8]=%s [210]=%s [218]=%s [300]=%s [398]=%s [3D0]=%s | surf+48=%d +49=%d | fmtmask=%016llX list=%u:%s",
                    g_cfg.thisp, g_cfg.g, g_cfg.n, cf[0], cf[1], cf[2], cf[3], cf[4], cf[5], g_cfg.surf48, g_cfg.surf49,
                    g_cfg.fmtmask, g_cfg.listn, g_cfg.listn ? line : " none");
        if (g_cfg.has260) zz_tile_say("# cfg s9: [260]=%llX (entry 76) | scale [0x5ABCD8]+28=%s%X +60=%s%X", g_cfg.e260,
                                      g_cfg.hasScale ? "" : "?", g_cfg.scale28, g_cfg.hasScale ? "" : "?", g_cfg.scale60);
        else zz_tile_say("# cfg s9: [260]=absent (n=%u <= 76) | scale [0x5ABCD8]+28=%s%X +60=%s%X", g_cfg.n,
                         g_cfg.hasScale ? "" : "?", g_cfg.scale28, g_cfg.hasScale ? "" : "?", g_cfg.scale60);
    }
    zz_tile_summary_s9(frame);
    zz_tile_summary_s10(frame);
    LONG bn = g_rbt_n; if (bn > ZZ_RB_N) bn = ZZ_RB_N;
    zz_tile_say("# f%-6ld readback (0x79EA0): calls=%ld tuples=%ld (overflow %ld)", frame, (long)g_rb_calls, (long)bn, (long)g_rbt_over);
    for (LONG k = 0; k < bn; k++)
        zz_tile_say("#   rb %ux%u fmt=%u dim=%u n%ld first=f%u", g_rbt[k].w, g_rbt[k].h, g_rbt[k].fmt, g_rbt[k].dim, (long)g_rbt[k].n, g_rbt[k].first);
    LONG tn2 = g_rtt_n; if (tn2 > ZZ_RT_N) tn2 = ZZ_RT_N;
    zz_tile_say("# f%-6ld rtmap (0x751C4): calls=%ld tuples=%ld (overflow %ld) -- caller 0CD92 = resolve, 78C3C/7910B = fetch",
                frame, (long)g_rt_calls, (long)tn2, (long)g_rtt_over);
    for (LONG k = 0; k < tn2; k++)
        zz_tile_say("#   rt caller=%05X base=%08X W=%u size=%u n%ld", g_rtt[k].caller, g_rtt[k].base, g_rtt[k].W, g_rtt[k].size, (long)g_rtt[k].n);

    // s7 B1-B3: one tuple per line (the 600-byte line buffer would truncate a table)
    LONG sn = g_screg_n; if (sn > ZZ_SCREG_N) sn = ZZ_SCREG_N;
    zz_tile_say("# f%-6ld scissor-reg tuples: %ld (overflow %ld) -- W = window 0x2081/82 (TL bit31 = offset disable), S = screen 0x200E/0F",
                frame, (long)sn, (long)g_screg_over);
    for (LONG k = 0; k < sn; k++)
    {
        ScRegStat& t = g_screg[k];
        if (t.kind == 'W')
            zz_tile_say("#   sc %c TL=%08X BR=%08X (x%u..%u y%u..%u dis=%u) woff.y=%d sel=%08X n%ld", (char)t.kind, t.tl, t.br,
                        t.tl & 0x3FFF, t.br & 0x3FFF, (t.tl >> 16) & 0x3FFF, (t.br >> 16) & 0x3FFF, t.tl >> 31, t.wy, t.sel, (long)t.n);
        else
            zz_tile_say("#   sc %c TL=%08X BR=%08X (x%d..%d y%d..%d) woff.y=%d sel=%08X n%ld", (char)t.kind, t.tl, t.br,
                        zz_sext15(t.tl & 0x7FFF), zz_sext15(t.br & 0x7FFF), zz_sext15((t.tl >> 16) & 0x7FFF), zz_sext15((t.br >> 16) & 0x7FFF),
                        t.wy, t.sel, (long)t.n);
    }
    LONG vn = g_vpt_n; if (vn > ZZ_VP_N) vn = ZZ_VP_N;
    zz_tile_say("# f%-6ld D3D12 RSSetViewports (0xF8C5): calls=%ld tuples=%ld (overflow %ld)", frame, (long)g_vp_calls, (long)vn, (long)g_vpt_over);
    for (LONG k = 0; k < vn; k++)
        zz_tile_say("#   vp TLy=%d %dx%d n%ld", g_vpt[k].tly, g_vpt[k].w, g_vpt[k].h, (long)g_vpt[k].n);
    LONG cn = g_sct_n; if (cn > ZZ_SCT_N) cn = ZZ_SCT_N;
    zz_tile_say("# f%-6ld D3D12 RSSetScissorRects (0xF8E2): calls=%ld tuples=%ld (overflow %ld) -- with the viewport issued just before",
                frame, (long)g_sc_calls, (long)cn, (long)g_sct_over);
    for (LONG k = 0; k < cn; k++)
        zz_tile_say("#   sc rows %d..%d cols %d..%d | vp TLy=%d H=%d n%ld", g_sct[k].top, g_sct[k].bottom, g_sct[k].left, g_sct[k].right,
                    g_sct[k].vtly, g_sct[k].vh, (long)g_sct[k].n);
    LONG rn = g_rst_n; if (rn > ZZ_RS_N) rn = ZZ_RS_N;
    zz_tile_say("# f%-6ld resolves (fn 0xC080, mode 2): calls=%ld tuples=%ld (overflow %ld) -- rect is tile-local, dest y = ysh + top",
                frame, (long)g_rs_calls, (long)rn, (long)g_rst_over);
    for (LONG k = 0; k < rn; k++)
    {
        RsStat& t = g_rst[k];
        zz_tile_say("#   rs base=%08X pitch=%08X info=%08X ctrl=%08X surf=%08X rect rows %d..%d cols %d..%d ysh=%d woff.y=%d vtxen=%u wsc=%08X/%08X sel=%08X n%ld",
                    t.base, t.pitch, t.info, t.ctrl, t.surf, t.top, t.bottom, t.left, t.right, t.ysh, t.wy, t.vtxen, t.wtl, t.wbr, t.sel, (long)t.n);
    }
}

static void zz_tile_drain_probe(void)
{
    if (!g_probe) return;
    zz_tile_drain_s10();
    LONG e = g_screg_first_n; if (e > 64) e = 64;
    for (long i = g_screg_first_dumped; i < e; i++)
    {
        ScRegHit& h = g_screg_first[i];
        zz_tile_say("SCREG  f%-5u %c TL=%08X BR=%08X woff=(%d,%d) sel=%08X", h.frame, (char)h.kind, h.tl, h.br, h.wx, h.wy, h.sel);
    }
    g_screg_first_dumped = e;
    e = g_rs_first_n; if (e > 64) e = 64;
    for (long i = g_rs_first_dumped; i < e; i++)
    {
        RsStat& t = g_rs_first[i];
        zz_tile_say("RESOLV f%-5u base=%08X pitch=%08X info=%08X ctrl=%08X surf=%08X rect rows %d..%d cols %d..%d ysh=%d woff.y=%d vtxen=%u wsc=%08X/%08X sel=%08X",
                    g_rs_first_frame[i], t.base, t.pitch, t.info, t.ctrl, t.surf, t.top, t.bottom, t.left, t.right, t.ysh, t.wy, t.vtxen, t.wtl, t.wbr, t.sel);
    }
    g_rs_first_dumped = e;
}

static void zz_tile_install(void)
{
    g_f = _fsopen(ZZ_TITLE "_tile.log", "w", _SH_DENYNO);
    HMODULE vgpu = GetModuleHandleA("VGPUDX12.dll");
    if (!vgpu) { zz_tile_say("# VGPUDX12 not loaded -- nothing installed"); return; }
    g_vgpu = (unsigned char*)vgpu;
    {
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_vgpu + ((IMAGE_DOS_HEADER*)g_vgpu)->e_lfanew);
        g_vgpu_end = (uintptr_t)g_vgpu + nt->OptionalHeader.SizeOfImage;
    }
    g_ring = (TileRec*)VirtualAlloc(nullptr, sizeof(TileRec) * ZZ_TILE_RING, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int a = 0, f = 0, h = 0, m = 0;
    if (g_tile_on || g_observe)
    {
        a = zz_tile_install_ctxcap();
        f = zz_tile_install_snap();
        h = zz_tile_install_texc();
    }
    int pv = 0, ps = 0, pr = 0;
    if (g_probe && (g_tile_on || g_observe))
    {
        pv = zz_tile_install_callsite(ZZ_VPSET_RVA, ZZ_VPSET_SIG, (void*)&zz_tile_vp_hit, "vpset");
        ps = zz_tile_install_callsite(ZZ_SCSET_RVA, ZZ_SCSET_SIG, (void*)&zz_tile_sc_hit, "scset");
    }
    int rx = 0, rb = 0, rt = 0, s9 = 0, s10 = 0;
    if ((g_probe || g_rsfix2) && (g_tile_on || g_observe))
    {
        // the rsfix2 decision is made in the entry tap, so install the sites only once the tap is in
        pr = zz_tile_install_resolve();
        if (pr && g_rsfix2 && !g_observe) rx = zz_tile_install_rsfix2();
    }
    if (!rx) { g_rsfix2 = false; g_rx2.tid = 0; }    // no sites (or a partial install): never arm a decision
    if (g_probe && (g_tile_on || g_observe))
    {
        rb = zz_tile_install_rb();
        rt = zz_tile_install_rt();
        s9 = zz_tile_install_s9();
        s10 = zz_tile_install_s10();
    }
    zz_tile_say("# s8 probe=%d rsfix2=%d (arm f%ld) | vpset=%d scset=%d resolve=%d rsfix2-sites=%d readback=%d rtmap=%d",
                (int)g_probe, (int)g_rsfix2, g_rsfix2_arm, pv, ps, pr, rx, rb, rt);
    zz_tile_say("# s9 probe=%d taps=%02X (of 3F: 01 untile 02 untile-src 04 slots 08 present 10 shadow-write 20 dispatch)",
                (int)g_probe, s9);
    zz_tile_say("# s10 probe=%d taps=%02X (of 7F: 01 target-lookup 02 dirty 04 untile-dispatch 08 shadow-views 10 dispatches"
                " 20 executor-771E4 40 executor-76EE4; I-J rides s9's 01) image=%08X bytes",
                (int)g_probe, s10, (unsigned)(g_vgpu_end - (uintptr_t)g_vgpu));
    if (g_mrtfix_on)
    {
        unsigned char* site = g_vgpu + 0xF87Bu;
        static const unsigned char sig[6] = { 0x41,0xB9,0x01,0x00,0x00,0x00 };
        if (memcmp(site, sig, 6) == 0)
        {
            DWORD old = 0;
            if (VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old))
            {
                site[2] = 0x00;
                VirtualProtect(site, 6, old, &old);
                FlushInstructionCache(GetCurrentProcess(), site, 6);
                m = 1;
                zz_tile_say("# mrtfix: OMSetRenderTargets RTsSingleHandleToDescriptorRange forced FALSE at VGPUDX12+F87B");
            }
            else zz_tile_say("# mrtfix: VirtualProtect failed (%lu)", GetLastError());
        }
        else zz_tile_say("# mrtfix sig mismatch at +F87B: %02X %02X %02X %02X %02X %02X", site[0], site[1], site[2], site[3], site[4], site[5]);
    }
    zz_tile_say("# zz_tile_cd: tile=%d observe=%d mrtfix=%d | ctxcap=%d snap=%d texc=%d mrt=%d",
                (int)g_tile_on, (int)g_observe, (int)g_mrtfix_on, a, f, h, m);
}

static DWORD WINAPI zz_tile_cd_thread(LPVOID)
{
    for (int i = 0; i < 2400 && !GetModuleHandleA("VGPUDX12.dll"); i++) Sleep(50);
    zz_tile_install();
    long last = 0, lastSummary = 0;
    for (;;)
    {
        Sleep(50);
        long fr = zz_swaps;
        if (fr == last) continue;
        last = fr;
        zz_tile_drain();
        zz_tile_drain_probe();
        if (fr / 300 != lastSummary) { lastSummary = fr / 300; zz_tile_summary(fr); }
    }
}

static int zz_tile_cd_init()
{
    g_tile_on   = GetFileAttributesA(ZZ_TITLE "_zz_tile.on") != INVALID_FILE_ATTRIBUTES;
    g_observe   = GetFileAttributesA(ZZ_TITLE "_zz_tile_observe.on") != INVALID_FILE_ATTRIBUTES;
    g_mrtfix_on = GetFileAttributesA(ZZ_TITLE "_zz_mrtfix.on") != INVALID_FILE_ATTRIBUTES;
    g_probe     = GetFileAttributesA(ZZ_TITLE "_zz_tile_probe.on") != INVALID_FILE_ATTRIBUTES;
    g_rsfix2    = GetFileAttributesA(ZZ_TITLE "_zz_tile_rsfix2.on") != INVALID_FILE_ATTRIBUTES;
    if (g_rsfix2)
    {
        FILE* mf = nullptr;
        if (fopen_s(&mf, ZZ_TITLE "_zz_tile_rsfix2.on", "r") == 0 && mf)
        {
            if (fscanf_s(mf, "%ld", &g_rsfix2_arm) != 1) g_rsfix2_arm = 0;
            fclose(mf);
        }
    }
    if (!g_tile_on && !g_observe && !g_mrtfix_on) return 0;
    char nm[64];
    _snprintf_s(nm, sizeof(nm), _TRUNCATE, "Local\\" ZZ_TITLE "_zz_tile_owner_%08lX", GetCurrentProcessId());
    HANDLE own = CreateMutexA(nullptr, FALSE, nm);
    if (own && GetLastError() == ERROR_ALREADY_EXISTS) return 0;     // the twin owns it
    if (HANDLE h = CreateThread(nullptr, 0, zz_tile_cd_thread, nullptr, 0, nullptr)) CloseHandle(h);
    return 1;
}
static int g_tile_cd_init = zz_tile_cd_init();
