// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Crackdown s6 (2026-10-14): OUR XMA decoder behind OUR register model (S4_PLAN §6 Plan B,
// with S5_FABLE_REVIEW §3's lazy input).
//
// WHY. Emu's XMA model decodes a whole input buffer at once and reports its valid bit clear
// after one ring (16 ms after the kick, run 6). The game's poll (sub_82A52710 82A527B0) then
// drops the buffer, its next KICK hands Emu "no input", the ring drains and the voice is
// released and re-created: 72 ms of sound, a gap, repeat ("cuts out randomly", launch 10).
// Real hardware keeps the buffer valid and advances the read offset packet by packet.
//
// HOW (the hardware's way, the context lives in guest memory): the game writes a voice's
// 64-byte context (at reg1800's E alias + idx*64), then KICKs it (0x7FEA1940 bit). While a
// context is kicked and not LOCKed (0x7FEA1A40), this file's host thread owns it: each pass
// reads the context, pulls ONE 2048-byte packet from the current input buffer into libavcodec's
// xma2 decoder (NFS s26's loader and decoder, work\ppc\zz_xma.cpp) only while the decoded-but-
// unwritten PCM is below one ring, writes s16 big-endian PCM into the output ring while there is
// room, and writes back dw0 (input valid bits, out write offset), dw1 (out valid), dw2 (input
// read offset, advanced per packet) and dw4 (current buffer). An input buffer's valid bit is
// cleared only after its LAST packet has been pulled. CLEAR (0x7FEA1A80) resets the decoder.
// A LOCK waits (spin, no host lock: guest-fiber rule) until this thread is off that context,
// so the game reads and edits a context we are not writing.
//
// zz_xmamm.cpp consumes the 0x7FEA1800 block's writes and the 0x1818 read when this is on
// (<game>_xma.on, and libav loaded); 0x1800 (the context pool base) still comes from Emu.
// Log: <console folder>\<game>_xma_dec.log. PCM dump: <game>_xma_dump.on ->
// <console folder>\<game>_xma_dump<idx>.raw (contexts 0-5, s16 BE).
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "zz_wlog.h"

extern "C" uintptr_t zz_guest_base(void);           // zz_import.cpp

static ZzWLog g_dl = { ZZ_TITLE "_xma_dec.log" };

// ---- libav surface (NFS s26, verbatim types) -----------------------------------------------
typedef void* (*pf_find_decoder_by_name)(const char*);
typedef void* (*pf_alloc_context3)(void*);
typedef int   (*pf_open2)(void*, void*, void*);
typedef int   (*pf_send_packet)(void*, void*);
typedef int   (*pf_receive_frame)(void*, void*);
typedef void  (*pf_free_context)(void**);
typedef void  (*pf_flush_buffers)(void*);
typedef void* (*pf_packet_alloc)(void);
typedef void  (*pf_packet_free)(void**);
typedef unsigned (*pf_version)(void);
typedef void* (*pf_frame_alloc)(void);
typedef void  (*pf_frame_free)(void**);
typedef void  (*pf_frame_unref)(void*);
typedef void* (*pf_malloc)(size_t);
typedef void* (*pf_opt_find)(void*, const char*, const char*, int, int);
typedef int   (*pf_opt_set_int)(void*, const char*, long long, int);
typedef int   (*pf_opt_set)(void*, const char*, const char*, int);
typedef void  (*pf_log_set_level)(int);

static struct {
    HMODULE avcodec, avutil;
    pf_find_decoder_by_name find_decoder_by_name;
    pf_alloc_context3 alloc_context3;
    pf_open2 open2;
    pf_send_packet send_packet;
    pf_receive_frame receive_frame;
    pf_free_context free_context;
    pf_flush_buffers flush_buffers;
    pf_packet_alloc packet_alloc;
    pf_packet_free packet_free;
    pf_version version;
    pf_frame_alloc frame_alloc;
    pf_frame_free frame_free;
    pf_frame_unref frame_unref;
    pf_malloc av_malloc;
    pf_opt_find opt_find;
    pf_opt_set_int opt_set_int;
    pf_opt_set opt_set;
    pf_log_set_level log_set_level;
    void* codec;
    int off_extradata, off_extradata_size;
} AV;

#define AVF_DATA0       0
#define AVF_DATA1       8
#define AVF_NB_SAMPLES  112
#define AVF_FORMAT      116
#define AVP_DATA        24
#define AVP_SIZE        32
#define AV_SAMPLE_FMT_FLTP 8
#define AVERROR_EAGAIN  (-11)
#define AVERROR_EOF     (-541478725)

#define XMA_PACKET_BYTES   2048
#define XMA_PACKET_BITS    (XMA_PACKET_BYTES * 8)
#define XMA_BLOCK_BYTES    256
#define XMA_FRAME_SAMPLES  512
#define ZZ_XMA_STARVE_MS   15
#define ZZ_XMA_PRIME_SAMPLES (XMA_FRAME_SAMPLES + 64)
#define ZZ_XCD_N           64          // contexts handled (the game uses 0-3; 320 exist)

struct XCtx
{
    volatile LONG enabled, locked, busy, clear_req;
    void*     avctx;
    unsigned  av_channels, av_rate, channels;
    unsigned char* pend;
    unsigned  pend_cap, pend_head, pend_len;
    unsigned long long total_samples, last_input_ms;
    bool      flush_armed, primed;
    unsigned  n_pkts, n_kicks;
    HANDLE    dump;
    int       idx;
};
static XCtx g_x[ZZ_XCD_N];
static volatile uint32_t g_reg1800;
static HANDLE g_evt;
static int g_on;                         // marker present AND libav loaded
static int g_dump_on;
static volatile LONG g_st_pkts, g_st_frames, g_st_full, g_st_flush, g_st_err, g_st_kick, g_st_lock, g_st_clear, g_st_spin;
static void* g_frame; static void* g_pkt;
static unsigned char* g_scratch; static unsigned g_scratch_cap;

// s9 I-F (S9_PLAN §2): kick watchdog. zz_xcd_kick (guest fiber, lock-free) records the last 16 kicks;
// the stat reporter dumps them ONCE, the first second with kicks == 0 while the game swapped >= 5 frames.
extern "C" volatile long zz_swaps;                                     // zz_fps.cpp
extern "C" void zz_game_dump_run(void (*emit)(const char*));          // zz_import.cpp
struct XKickRec { volatile LONG idx, swaps; volatile DWORD tid; volatile ULONGLONG ms; };
static XKickRec      g_kring[16];
static volatile LONG g_kring_n;
static ULONGLONG     g_svc_ms;                                         // the last pass that serviced a context
static long          g_wd_swaps;
static int           g_wd_fired;

// ---- guest memory ------------------------------------------------------------------------
__declspec(noinline) static void zz_mv(void* d, const void* s, size_t n) { memcpy(d, s, n); }
static bool zz_safe_mv(void* d, const void* s, size_t n)
{
    __try { zz_mv(d, s, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool zz_ok(uintptr_t base, uint32_t va, size_t n, bool write)
{
    for (size_t off = 0;; off += 0x1000)
    {
        if (off >= n) off = n - 1;
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((const void*)(base + va + off), &m, sizeof(m))) return false;
        if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        if (write && !(m.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) return false;
        if (off == n - 1) return true;
    }
}
// A guest pointer the game stored (a window VA like 0xDE83D000, or a bare physical address) ->
// a guest VA we can touch: the pointer itself, then the E / A / C alias of the same RAM.
static uint32_t zz_resolve(uintptr_t base, uint32_t p, size_t n, bool write)
{
    if (p >= 0xA0000000u && zz_ok(base, p, n, write)) return p;
    const uint32_t ph = p >= 0xE0000000u ? (p & 0x1FFFFFFFu) + 0x1000u : (p & 0x1FFFFFFFu);
    if (ph >= 0x1000u && zz_ok(base, 0xE0000000u + ph - 0x1000u, n, write)) return 0xE0000000u + ph - 0x1000u;
    if (zz_ok(base, 0xA0000000u + ph, n, write)) return 0xA0000000u + ph;
    if (zz_ok(base, 0xC0000000u + ph, n, write)) return 0xC0000000u + ph;
    return 0;
}

// ---- pending PCM -----------------------------------------------------------------------------
static void zz_pend_append(XCtx* x, const unsigned char* p, unsigned n)
{
    if (x->pend_head && x->pend_head + x->pend_len + n > x->pend_cap)
    {
        memmove(x->pend, x->pend + x->pend_head, x->pend_len);
        x->pend_head = 0;
    }
    if (x->pend_head + x->pend_len + n > x->pend_cap)
    {
        unsigned nc = x->pend_cap ? x->pend_cap * 2 : 65536;
        while (nc < x->pend_head + x->pend_len + n) nc *= 2;
        unsigned char* np = (unsigned char*)realloc(x->pend, nc);
        if (!np) return;
        x->pend = np; x->pend_cap = nc;
    }
    memcpy(x->pend + x->pend_head + x->pend_len, p, n);
    x->pend_len += n;
    if (g_dump_on && !x->dump && x->idx < 6)
    {   // opened lazily: only the twin that actually decodes ever gets here
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE,
                    ZZ_TITLE "_xma_dump%d.raw", x->idx);
        x->dump = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (x->dump == INVALID_HANDLE_VALUE) x->dump = (HANDLE)1;   // 1 = tried, failed
    }
    if (x->dump && x->dump != (HANDLE)1) { DWORD w; WriteFile(x->dump, p, n, &w, nullptr); }
}
static void zz_pend_prime(XCtx* x)
{
    unsigned char z[ZZ_XMA_PRIME_SAMPLES * 2 * 2];
    memset(z, 0, sizeof(z));
    zz_pend_append(x, z, ZZ_XMA_PRIME_SAMPLES * x->channels * 2);
    x->total_samples = ZZ_XMA_PRIME_SAMPLES;
    x->primed = true;
}

// ---- decoder (NFS s26) --------------------------------------------------------------------
static bool zz_av_open(XCtx* x, unsigned rate)
{
    if (x->avctx && x->av_channels == x->channels && x->av_rate == rate) return true;
    if (x->avctx) { AV.free_context(&x->avctx); x->avctx = nullptr; }
    void* c = AV.alloc_context3(AV.codec);
    if (!c) return false;
    int r1 = AV.opt_set_int(c, "ar", rate, 0);
    int r2 = AV.opt_set(c, "ch_layout", x->channels == 2 ? "stereo" : "mono", 0);
    int r3 = AV.opt_set_int(c, "block_align", XMA_PACKET_BYTES, 0);
    unsigned char* ed = (unsigned char*)AV.av_malloc(34 + 64);
    if (!ed) { AV.free_context(&c); return false; }
    memset(ed, 0, 34 + 64);
    ed[0] = 1;
    *(void**)((char*)c + AV.off_extradata) = ed;
    *(int*)((char*)c + AV.off_extradata_size) = 34;
    int r = AV.open2(c, AV.codec, nullptr);
    if (r1 || r2 || r3 || r)
    {
        zz_wlog(&g_dl, "avcodec_open2 FAILED (%d %d %d -> %d) ch=%u rate=%u", r1, r2, r3, r, x->channels, rate);
        AV.free_context(&c);
        InterlockedIncrement(&g_st_err);
        return false;
    }
    x->avctx = c; x->av_channels = x->channels; x->av_rate = rate;
    return true;
}

static void zz_av_drain(XCtx* x)
{
    for (;;)
    {
        int r = AV.receive_frame(x->avctx, g_frame);
        if (r) { if (r != AVERROR_EAGAIN && r != AVERROR_EOF) InterlockedIncrement(&g_st_err); break; }
        int ns  = *(int*)((char*)g_frame + AVF_NB_SAMPLES);
        int fmt = *(int*)((char*)g_frame + AVF_FORMAT);
        const float* L = *(const float**)((char*)g_frame + AVF_DATA0);
        const float* R = *(const float**)((char*)g_frame + AVF_DATA1);
        if (ns > 0 && fmt == AV_SAMPLE_FMT_FLTP && L && (x->channels == 1 || R))
        {
            unsigned nb = (unsigned)ns * x->channels * 2;
            if (nb > g_scratch_cap)
            {
                unsigned nc = g_scratch_cap ? g_scratch_cap : 65536; while (nc < nb) nc *= 2;
                unsigned char* np = (unsigned char*)realloc(g_scratch, nc);
                if (!np) { AV.frame_unref(g_frame); break; }
                g_scratch = np; g_scratch_cap = nc;
            }
            unsigned char* o = g_scratch;
            for (int i = 0; i < ns; i++)
                for (unsigned ch = 0; ch < x->channels; ch++)
                {
                    float f = ch ? R[i] : L[i];
                    int v = (int)(f * 32767.0f + (f >= 0 ? 0.5f : -0.5f));
                    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                    *o++ = (unsigned char)((unsigned)v >> 8);
                    *o++ = (unsigned char)v;
                }
            zz_pend_append(x, g_scratch, nb);
            x->total_samples += (unsigned)ns;
            InterlockedIncrement(&g_st_frames);
        }
        else if (fmt != AV_SAMPLE_FMT_FLTP) InterlockedIncrement(&g_st_err);
        AV.frame_unref(g_frame);
    }
}

static void zz_av_packet(XCtx* x, const unsigned char* pkt2048)
{
    unsigned char buf[XMA_PACKET_BYTES + 64];
    memcpy(buf, pkt2048, XMA_PACKET_BYTES);
    memset(buf + XMA_PACKET_BYTES, 0, 64);
    *(void**)((char*)g_pkt + AVP_DATA) = buf;
    *(int*)((char*)g_pkt + AVP_SIZE) = XMA_PACKET_BYTES;
    int r = AV.send_packet(x->avctx, g_pkt);
    if (r) { InterlockedIncrement(&g_st_err); if (r != AVERROR_EAGAIN) AV.flush_buffers(x->avctx); }
    zz_av_drain(x);
    x->flush_armed = true;
    x->n_pkts++; InterlockedIncrement(&g_st_pkts);
}

static void zz_av_flush(XCtx* x)
{
    if (!x->avctx || !x->flush_armed) return;
    AV.send_packet(x->avctx, nullptr);
    zz_av_drain(x);
    AV.flush_buffers(x->avctx);
    unsigned rem = (unsigned)(x->total_samples % XMA_FRAME_SAMPLES);
    if (rem)
    {
        unsigned pad = (XMA_FRAME_SAMPLES - rem) * x->channels * 2;
        unsigned char z[XMA_FRAME_SAMPLES * 2 * 2];
        memset(z, 0, pad);
        zz_pend_append(x, z, pad);
        x->total_samples += XMA_FRAME_SAMPLES - rem;
    }
    x->flush_armed = false;
    InterlockedIncrement(&g_st_flush);
    zz_pend_prime(x);
}

static void zz_x_reset(XCtx* x)
{
    if (x->avctx) AV.flush_buffers(x->avctx);
    x->pend_len = x->pend_head = 0;
    x->total_samples = 0;
    x->flush_armed = false;
    x->primed = false;
}

// ---- one service pass over one kicked, unlocked context (host thread, busy held) -----------
static void zz_x_service(int idx, XCtx* x)
{
    const uintptr_t base = zz_guest_base();
    const uint32_t reg = g_reg1800;
    if (!base || !reg) return;
    const uint32_t cva = 0xE0000000u + ((reg & 0x1FFFFFFFu) - 0x1000u) + (uint32_t)idx * 64u;
    uint32_t raw[16], d[16];
    if (!zz_ok(base, cva, 64, true) || !zz_safe_mv(raw, (const void*)(base + cva), 64)) return;
    for (int i = 0; i < 16; i++) d[i] = _byteswap_ulong(raw[i]);

    unsigned pk[2] = { d[0] & 0xFFF, d[1] & 0xFFF };
    unsigned v[2]  = { (d[0] >> 20) & 1, (d[0] >> 21) & 1 };
    const unsigned blocks = (d[0] >> 22) & 31;
    unsigned w = d[0] >> 27, ovalid = d[1] >> 31;
    const unsigned rate_e = (d[1] >> 27) & 3, stereo = (d[1] >> 29) & 1;
    unsigned rd = d[2] & 0x3FFFFFF, cur = d[4] >> 31;
    const uint32_t in[2] = { d[5], d[6] }, out = d[7];
    const unsigned r = d[9] & 31;
    static const unsigned rates[4] = { 24000, 32000, 44100, 48000 };

    x->channels = stereo ? 2 : 1;
    if (!x->primed) zz_pend_prime(x);
    const unsigned cap = blocks * XMA_BLOCK_BYTES;
    if (!cap || !out) return;
    const uint32_t outva = zz_resolve(base, out, cap, true);
    if (!zz_av_open(x, rates[rate_e])) return;

    for (int guard = 0; guard < 64; guard++)
    {
        // 1. ring <- pending
        if (outva && ovalid && x->pend_len >= XMA_BLOCK_BYTES)
        {
            unsigned free_b = ((w == r) ? blocks : ((r + blocks - w) % blocks)) * XMA_BLOCK_BYTES;
            unsigned n = x->pend_len - (x->pend_len % XMA_BLOCK_BYTES);
            if (n > free_b) n = free_b;
            if (n)
            {
                unsigned wpos = (w * XMA_BLOCK_BYTES) % cap;
                unsigned first = cap - wpos; if (first > n) first = n;
                zz_safe_mv((void*)(base + outva + wpos), x->pend + x->pend_head, first);
                if (n > first) zz_safe_mv((void*)(base + outva), x->pend + x->pend_head + first, n - first);
                x->pend_head += n; x->pend_len -= n;
                if (!x->pend_len) x->pend_head = 0;
                w = (w + n / XMA_BLOCK_BYTES) % blocks;
                if (w == r) { ovalid = 0; InterlockedIncrement(&g_st_full); }
            }
        }
        // 2. pending <- ONE packet, while pending is below one ring (lazy input)
        if (x->pend_len < cap)
        {
            unsigned c = cur;
            if (!v[c] && v[c ^ 1]) c ^= 1;
            if (v[c])
            {
                cur = c;
                const unsigned p = rd / XMA_PACKET_BITS;
                if (p >= pk[c] || !in[c]) { v[c] = 0; cur = c ^ 1; rd = 32; continue; }
                const uint32_t iva = zz_resolve(base, in[c] + p * XMA_PACKET_BYTES, XMA_PACKET_BYTES, false);
                unsigned char pkt[XMA_PACKET_BYTES];
                if (!iva || !zz_safe_mv(pkt, (const void*)(base + iva), XMA_PACKET_BYTES))
                {
                    InterlockedIncrement(&g_st_err);
                    v[c] = 0; cur = c ^ 1; rd = 32;      // unreadable input: drop the buffer like an error
                    continue;
                }
                zz_av_packet(x, pkt);
                if (p + 1 >= pk[c]) { v[c] = 0; cur = c ^ 1; rd = 32; }   // the LAST packet was pulled
                else rd = (p + 1) * XMA_PACKET_BITS + 32;
                x->last_input_ms = GetTickCount64();
                continue;
            }
            // 3. starved: no input, nothing pending, ring wants more -> flush libav's held tail
            if (x->flush_armed && x->pend_len < XMA_BLOCK_BYTES && ovalid &&
                GetTickCount64() - x->last_input_ms >= ZZ_XMA_STARVE_MS)
            {
                zz_av_flush(x);
                continue;
            }
        }
        break;
    }

    // write back the hardware-owned fields, only the dwords that changed
    uint32_t n0 = (d[0] & ~((1u << 20) | (1u << 21) | (31u << 27))) | (v[0] << 20) | (v[1] << 21) | (w << 27);
    uint32_t n1 = (d[1] & 0x7FFFFFFFu) | (ovalid << 31);
    uint32_t n2 = (d[2] & ~0x3FFFFFFu) | (rd & 0x3FFFFFFu);
    uint32_t n4 = (d[4] & 0x7FFFFFFFu) | (cur << 31);
    volatile uint32_t* g = (volatile uint32_t*)(base + cva);
    if (n0 != d[0]) g[0] = _byteswap_ulong(n0);
    if (n1 != d[1]) g[1] = _byteswap_ulong(n1);
    if (n2 != d[2]) g[2] = _byteswap_ulong(n2);
    if (n4 != d[4]) g[4] = _byteswap_ulong(n4);
}

static void zz_xcd_wd_emit(const char* s) { zz_wlog(&g_dl, "%s", s); }

// I-F: once, from this thread (the stat reporter), the first stat second with no kick while frames advance
static void zz_xcd_watchdog(ULONGLONG now, long sw)
{
    g_wd_fired = 1;
    zz_wlog(&g_dl, "WATCHDOG kicks=0 this second, swaps %ld -> %ld (+%ld); last service pass %llu ms ago; %ld kicks recorded",
            g_wd_swaps, sw, sw - g_wd_swaps, (unsigned long long)(now - g_svc_ms), (long)g_kring_n);
    const LONG n = g_kring_n;
    for (LONG k = (n > 16 ? n - 16 : 0); k < n; k++)
    {
        const XKickRec& r = g_kring[k & 15];
        zz_wlog(&g_dl, "  kick #%ld ctx=%ld swap=%ld tid=%lu %llu ms ago", (long)k, (long)r.idx, (long)r.swaps,
                (unsigned long)r.tid, (unsigned long long)(now - r.ms));
    }
    for (int i = 0; i < ZZ_XCD_N; i++)
    {
        const XCtx* x = &g_x[i];
        if (!x->n_kicks && !x->enabled && !x->locked && !x->busy) continue;
        zz_wlog(&g_dl, "  ctx %d enabled=%ld locked=%ld busy=%ld clear_req=%ld kicks=%u pkts=%u pend=%u primed=%d armed=%d last_input %llu ms ago",
                i, (long)x->enabled, (long)x->locked, (long)x->busy, (long)x->clear_req, x->n_kicks, x->n_pkts, x->pend_len,
                (int)x->primed, (int)x->flush_armed, x->last_input_ms ? (unsigned long long)(now - x->last_input_ms) : 0ull);
    }
    zz_game_dump_run(zz_xcd_wd_emit);
}

static DWORD WINAPI zz_xcd_thread(LPVOID)
{
    ULONGLONG last = GetTickCount64();
    for (;;)
    {
        WaitForSingleObject(g_evt, 2);
        for (int i = 0; i < ZZ_XCD_N; i++)
        {
            XCtx* x = &g_x[i];
            if (!x->enabled && !x->clear_req) continue;
            InterlockedExchange(&x->busy, 1);
            if (x->clear_req) { zz_x_reset(x); InterlockedExchange(&x->clear_req, 0); }
            if (x->enabled && !x->locked) { zz_x_service(i, x); g_svc_ms = GetTickCount64(); }
            InterlockedExchange(&x->busy, 0);
        }
        const ULONGLONG now = GetTickCount64();
        if (now - last >= 1000)
        {
            last = now;
            LONG p = InterlockedExchange(&g_st_pkts, 0), f = InterlockedExchange(&g_st_frames, 0);
            LONG fu = InterlockedExchange(&g_st_full, 0), fl = InterlockedExchange(&g_st_flush, 0);
            LONG e = InterlockedExchange(&g_st_err, 0), k = InterlockedExchange(&g_st_kick, 0);
            LONG l = InterlockedExchange(&g_st_lock, 0), c = InterlockedExchange(&g_st_clear, 0);
            LONG s = InterlockedExchange(&g_st_spin, 0);
            if (p || f || k || l || c || e)
            {
                unsigned en = 0; for (int i = 0; i < ZZ_XCD_N; i++) en += g_x[i].enabled ? 1 : 0;
                zz_wlog(&g_dl, "stat kicks=%ld locks=%ld clears=%ld enabled=%u pkts=%ld frames=%ld ringfull=%ld flush=%ld err=%ld lockspin_ms=%ld",
                        (long)k, (long)l, (long)c, en, (long)p, (long)f, (long)fu, (long)fl, (long)e, (long)s);
            }
            const long sw = zz_swaps;
            if (!g_wd_fired && k == 0 && g_kring_n > 0 && sw - g_wd_swaps >= 5) zz_xcd_watchdog(now, sw);
            g_wd_swaps = sw;
        }
    }
}

// ---- the register-model entry points (zz_xmamm.cpp, guest-fiber context: no host locks) ---
extern "C" int zz_xcd_on(void) { return g_on; }
extern "C" void zz_xcd_set_base(uint32_t reg1800) { g_reg1800 = reg1800; }

static void zz_xcd_wait_off(XCtx* x)
{
    // Dekker with full barriers: we set locked, the thread sets busy then re-reads locked.
    const ULONGLONG t0 = GetTickCount64();
    while (x->busy)
    {
        YieldProcessor();
        if (GetTickCount64() - t0 > 50) { InterlockedIncrement(&g_st_spin); break; }
    }
}

extern "C" void zz_xcd_kick(unsigned idx)
{
    if (idx >= ZZ_XCD_N) return;
    XCtx* x = &g_x[idx];
    x->n_kicks++;
    InterlockedIncrement(&g_st_kick);
    XKickRec& kr = g_kring[(InterlockedIncrement(&g_kring_n) - 1) & 15];
    kr.idx = (LONG)idx; kr.swaps = zz_swaps; kr.tid = GetCurrentThreadId(); kr.ms = GetTickCount64();
    InterlockedExchange(&x->locked, 0);
    InterlockedExchange(&x->enabled, 1);
    SetEvent(g_evt);
}
extern "C" void zz_xcd_lock(unsigned idx)
{
    if (idx >= ZZ_XCD_N) return;
    XCtx* x = &g_x[idx];
    InterlockedIncrement(&g_st_lock);
    InterlockedExchange(&x->locked, 1);
    zz_xcd_wait_off(x);
}
extern "C" void zz_xcd_clear(unsigned idx)
{
    if (idx >= ZZ_XCD_N) return;
    XCtx* x = &g_x[idx];
    InterlockedIncrement(&g_st_clear);
    InterlockedExchange(&x->locked, 1);
    zz_xcd_wait_off(x);
    InterlockedExchange(&x->enabled, 0);
    InterlockedExchange(&x->clear_req, 1);
    SetEvent(g_evt);
}
extern "C" void zz_xcd_reset_all(void)
{
    for (unsigned i = 0; i < ZZ_XCD_N; i++) zz_xcd_clear(i);
}

// ---- init --------------------------------------------------------------------------------------
static bool zz_av_load()
{
    const char* dirs[] = { "", "C:\\Program Files\\obs-studio\\bin\\64bit\\" };
    for (const char* d : dirs)
    {
        char p[MAX_PATH];
        _snprintf_s(p, sizeof(p), _TRUNCATE, "%savutil-59.dll", d);
        AV.avutil = LoadLibraryExA(p, nullptr, d[0] ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
        if (!AV.avutil) continue;
        _snprintf_s(p, sizeof(p), _TRUNCATE, "%savcodec-61.dll", d);
        AV.avcodec = LoadLibraryExA(p, nullptr, d[0] ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
        if (AV.avcodec) { zz_wlog(&g_dl, "libav loaded from \"%s\"", d[0] ? d : "<the console folder>"); break; }
        FreeLibrary(AV.avutil); AV.avutil = nullptr;
    }
    if (!AV.avcodec) { zz_wlog(&g_dl, "avcodec-61.dll not found -- decoder OFF, err=%lu", GetLastError()); return false; }
#define GP(mod, field, name) do { AV.field = (decltype(AV.field))GetProcAddress(AV.mod, name); if (!AV.field) { zz_wlog(&g_dl, "missing %s -- OFF", name); return false; } } while (0)
    GP(avcodec, find_decoder_by_name, "avcodec_find_decoder_by_name");
    GP(avcodec, alloc_context3, "avcodec_alloc_context3");
    GP(avcodec, open2, "avcodec_open2");
    GP(avcodec, send_packet, "avcodec_send_packet");
    GP(avcodec, receive_frame, "avcodec_receive_frame");
    GP(avcodec, free_context, "avcodec_free_context");
    GP(avcodec, flush_buffers, "avcodec_flush_buffers");
    GP(avcodec, packet_alloc, "av_packet_alloc");
    GP(avcodec, packet_free, "av_packet_free");
    GP(avcodec, version, "avcodec_version");
    GP(avutil, frame_alloc, "av_frame_alloc");
    GP(avutil, frame_free, "av_frame_free");
    GP(avutil, frame_unref, "av_frame_unref");
    GP(avutil, av_malloc, "av_malloc");
    GP(avutil, opt_find, "av_opt_find");
    GP(avutil, opt_set_int, "av_opt_set_int");
    GP(avutil, opt_set, "av_opt_set");
    GP(avutil, log_set_level, "av_log_set_level");
#undef GP
    unsigned v = AV.version();
    if ((v >> 16) != 61) { zz_wlog(&g_dl, "avcodec major %u != 61 -- offsets unverified, OFF", v >> 16); return false; }
    AV.log_set_level(16);
    AV.codec = AV.find_decoder_by_name("xma2");
    if (!AV.codec) { zz_wlog(&g_dl, "no xma2 decoder in this avcodec -- OFF"); return false; }
    void* probe = AV.alloc_context3(AV.codec);
    if (!probe) return false;
    void* o_flags2 = AV.opt_find(probe, "flags2", nullptr, 0, 0);
    if (!o_flags2) { AV.free_context(&probe); zz_wlog(&g_dl, "AVOption flags2 not found -- OFF"); return false; }
    int off_flags2 = *(int*)((char*)o_flags2 + 16);
    AV.off_extradata = (off_flags2 + 4 + 7) & ~7;
    AV.off_extradata_size = AV.off_extradata + 8;
    zz_wlog(&g_dl, "avcodec %u.%u.%u flags2=%d extradata=%d/%d%s", v >> 16, (v >> 8) & 0xFF, v & 0xFF, off_flags2,
            AV.off_extradata, AV.off_extradata_size, off_flags2 != 68 ? "  (WARNING: != 68 seen offline)" : "");
    AV.free_context(&probe);
    g_frame = AV.frame_alloc();
    g_pkt = AV.packet_alloc();
    return g_frame && g_pkt;
}

static int zz_xcd_init()
{
    if (GetFileAttributesA(ZZ_TITLE "_xma.on") == INVALID_FILE_ATTRIBUTES) return 0;
    zz_wlog(&g_dl, ZZ_TITLE "_xma.on present -- our XMA decoder behind our register model (lazy input)");
    if (!zz_av_load()) return 0;
    g_dump_on = GetFileAttributesA(ZZ_TITLE "_xma_dump.on") != INVALID_FILE_ATTRIBUTES;
    for (int i = 0; i < ZZ_XCD_N; i++) g_x[i].idx = i;
    g_evt = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!g_evt) return 0;
    if (HANDLE h = CreateThread(nullptr, 0, zz_xcd_thread, nullptr, 0, nullptr)) CloseHandle(h); else return 0;
    g_on = 1;
    zz_wlog(&g_dl, "ON (dump %s)", g_dump_on ? "on" : "off");
    return 1;
}
static int g_xcd_init = zz_xcd_init();
