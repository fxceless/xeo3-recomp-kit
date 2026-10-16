// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// s9 pt8 -- the guest-kernel-call instrument, with the guest hot path taken
// completely off the log lock. See zz_import_hook.h for why the link register is the
// only place that can answer the question at all.
//
// WHAT RUN 7 MEASURED, and what it was NOT. Adding this instrument made the game boot
// about thirty times slower, and every theory about why was wrong when tested:
//
//   emission volume       32 lines in three minutes            innocent
//   watched-call rate     512 calls at most in that window     innocent
//   atomic contention     the counters are barely touched      innocent
//   ODS volume            3.6k lines, all D3D11 warnings       innocent
//   the thunk itself      benchmarked: +0.03 ns/call disarmed  innocent
//
// The cost was never per call. It was WHO TAKES THE LOG LOCK. zz_lock is a spin lock
// with Sleep(0), and zz_emit held it across a real disk sync; a sampler round writes
// about 200 lines and therefore performed about 200 syncs. Before this instrument
// only our own threads ever emitted, so that slowness never touched the game. The
// moment a guest thread could emit, the guest boot handshake began spinning on that
// lock behind the sampler, burning cores and starving the boot -- and our own worker
// with it, which is why run 7a produced ONE sampler round in 180 seconds.
//
// So the rule this file now follows: THE GUEST NEVER TOUCHES THE LOG. The hot path
// writes a fixed-size record into a lock-free ring and returns; the worker drains it.
// The flush policy in zz_crash.cpp was fixed at the same time, because a sampler
// round doing 200 disk syncs was a defect in its own right.
//
// s9 pt10 -- TWO ADDITIONS, both aimed at the ONE conjecture pt9 could not settle.
//
// pt9 measured VdSwap and a four-million-per-second NtWaitForSingleObjectEx falling
// off the same cliff in the same 2 s tick, and inferred -- without proof -- that one
// thread does both. The instrument could not decide it, for a flat reason: the record
// carried no thread id. It also kept only the FIRST eight calls per import, so the
// arguments of the LAST VdSwap before the freeze, the one that would say what the
// final present was handed, were never recorded either.
//
//   1. every record now carries the guest thread's HOST thread id, read straight
//      out of the TEB (one instruction, no call), so "same thread or not" is read
//      off the log instead of argued from a coincidence of timing.
//   2. every import keeps a ring of its LAST eight calls, overwritten in place by
//      the calling thread. It is dumped when that import STALLS (it was moving and
//      this tick it did not move at all) or when its rate COLLAPSES -- which is the
//      shape the spinner made, since it never went quiet, it fell to a residue.
//      A trigger on any import snapshots them all, so the freeze is photographed
//      whole rather than one counter at a time.
//
// The DISARMED cost is unchanged -- the watch table is still all-zero until a marker
// arms it. The ARMED cost is the number that mattered, because pt9 measured this path
// at four million calls a second, so it was benchmarked against the shipped pt8 object
// rather than reasoned about (test\bench_import2.cpp, which links the REAL file
// instead of keeping its own copy of it):
//
//                                        pt8        pt10
//     disarmed                        0.81 ns     0.82 ns
//     armed, one thread               2.68 ns     2.97 ns     +0.3 ns
//     armed, two threads/one import   5.4 ns     31.5 ns
//
// One thread at pt9's 4 M/s is 1.2% of a core: free. The contended figure is true
// sharing of one import's ring between two cores and is inherent to keeping a single
// ordered last-8 per import; it only binds if ONE import is driven at millions of
// calls a second by MORE THAN ONE thread, and pt9's records show a single call site
// per import. It is stated rather than hidden because it is the one shape that could
// slow the run, and pt8 is the standing lesson on what an instrument's own cost does
// to a launch.
#include <windows.h>
#include <intrin.h>

extern "C" unsigned char zz_imp_watch[256];
unsigned char zz_imp_watch[256];

extern "C" void zz_log_line(const char* s);

// ---------------------------------------------------------------- the ring
//
// Power of two, so the index wraps by masking. A slot is claimed with one
// Interlocked increment, filled by that thread alone, and only then marked ready --
// the ready flag is the handoff to the worker.
#define ZZ_RING 256

typedef struct ZzImpRec
{
    volatile LONG      ready;
    unsigned int       idx;
    unsigned long      tid;
    unsigned long long lr, r3, r4, r5;
} ZzImpRec;

static ZzImpRec      g_ring[ZZ_RING];
static volatile LONG g_ringHead;
static volatile LONG g_ringTail;
static volatile LONG g_ringLost;

static const char*   g_impName[256];
static unsigned char g_impKnown[256];
static unsigned long g_impDrained[256];
static volatile LONG g_impArmed;

// ------------------------------------------------------- the LAST-calls ring
//
// One small ring per import, written by the guest thread on EVERY watched call and
// read by the worker only when something has gone wrong. There is no handoff flag
// here as there is on the first-8 ring, because a slot is overwritten continuously
// and a flag would only ever read as ready; instead each slot is stamped with the
// call's sequence number BEFORE and AFTER its fields, so the worker can tell a
// complete record from one it caught mid-write. That is the whole synchronisation:
// the reader never blocks the writer and the writer never waits for anything.
//
// ⚠ TWO CONTENTION DEFECTS, both found by BENCHMARK rather than by reading, and both
// fixed here. The first draft claimed a ring slot with its own InterlockedIncrement
// over a dense `LONG g_lastSeq[256]`:
//
//   * 64 of those counters share ONE cache line, so DIFFERENT imports false-shared
//     it. The two threads under study -- the presenter and the four-million-a-second
//     spinner -- would have ping-ponged a single line between cores at 4 M/s. An
//     instrument that manufactures the stall it is measuring is worse than no
//     instrument, and this is exactly the shape of pt8's 30x slowdown.
//   * the locked increment was redundant. Each import ALREADY counts its calls, so
//     the count IS the sequence number; claiming a slot separately bought nothing and
//     cost an atomic on the hottest path in the module.
//
// Measured, two threads on one import: 68.1 ns/call as first written, 31.5 ns/call
// after both fixes -- and 9.8 -> 3.0 ns/call single-threaded, which is the case that
// actually applies. Per-import state is now one cache-line-aligned block, each ring
// slot owns its own line (worth a further 42.9 -> 32.8 ns contended, measured), and
// the only atomic left on the guest path is the first-8 ring's slot claim, which
// stops after eight calls.
#define ZZ_LAST 8

typedef struct __declspec(align(64)) ZzImpLast
{
    volatile unsigned long seq0;
    unsigned long          tid;
    unsigned long long     lr, r3, r4, r5;
    volatile unsigned long seq1;
    unsigned char          pad[12];
} ZzImpLast;

typedef struct __declspec(align(64)) ZzImpHot
{
    unsigned long count;              // the call counter AND the ring's sequence
    unsigned char pad[60];            // nothing else shares this import's line
    ZzImpLast     last[ZZ_LAST];
} ZzImpHot;

static ZzImpHot g_hot[256];

// s9 pt12: the guest memory base, latched from the first watched call. Guest memory is
// stored BIG-ENDIAN (ppc_context.h: PPC_LOAD_U32 is bswap32 of *(base + addr)), so a
// read here must byteswap or it prints a plausible, wrong handle.
static volatile unsigned long long g_guestBase;

// ⚠ the load lives in its own noinline function ON PURPOSE. clang-cl's __except does
// NOT catch a fault from a load written INLINE in the try body -- it catches one
// inside a CALL made from it. That was measured in test\iso.cpp (3/3 caught via a
// call, died second-chance inline) and it was once live in zz_walk's leaf fallback.
// [[reference_clangcl_try_inline_load]]
__declspec(noinline) static unsigned int zz_imp_raw_load(unsigned long long hostAddr)
{
    return *(volatile unsigned int*)(uintptr_t)hostAddr;
}

static int zz_imp_read_guest_u32(unsigned long long guestPtr, unsigned int* out)
{
    const unsigned long long b = g_guestBase;
    if (!b || !guestPtr) return 0;
    unsigned int v = 0;
    __try { v = zz_imp_raw_load(b + (unsigned int)guestPtr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    *out = _byteswap_ulong(v);
    return 1;
}

// The imports whose r3 is a PHANDLE out-parameter. "NtCr" is enough to select them and
// nothing else in the watch set begins that way.
static int zz_imp_is_create(const char* n)
{
    return n && n[0] == 'N' && n[1] == 't' && n[2] == 'C' && n[3] == 'r';
}

// Per-import tick state for the stall/collapse triggers, worker-owned.
static unsigned long g_impPrevDelta[256];
static unsigned char g_impEverFired[256];
static int           g_snapshots;
static unsigned long long g_impTick;

// A snapshot is 8 imports x 8 records at worst, so an unbounded trigger could bury
// the log in a repeating stall. Four is enough to hold the freeze and its aftermath.
#define ZZ_SNAP_MAX 4

// The thread id WITHOUT a call. On x64 the TEB is at gs:0 and its CLIENT_ID sits at
// +0x40, so UniqueThread -- what GetCurrentThreadId returns -- is the dword at
// gs:0x48. GetCurrentThreadId does exactly this read, but as an exported function
// behind an import thunk; at four million calls a second that indirection is the one
// cost in this path worth removing. The fixture checks the two against each other,
// because an offset that is wrong here would not fail, it would name the wrong
// thread and every conclusion drawn from the launch would inherit it.
static __forceinline unsigned long zz_imp_tid(void)
{
    return (unsigned long)__readgsdword(0x48);
}

// The set the question needs, and nothing else. VdSwap answers whether the guest is
// still presenting -- on the sibling title that was the whole difference between a
// guest that stopped drawing and an emulator that stopped showing. The wait calls
// answer who is blocked and on what. KeSetEvent answers whether anything signals it.
static int zz_imp_wanted(const char* n)
{
    static const char* const WANT[] = {
        "VdSwap",
        "KeWaitForSingleObject",
        "KeWaitForMultipleObjects",
        "NtWaitForSingleObjectEx",
        "NtWaitForMultipleObjectsEx",
        "KeSetEvent",
        "KePulseEvent",
        "NtSetEvent",

        // --- s9 pt12 additions, each for one open question ------------------
        //
        // VdGetSystemCommandBuffer SPLITS THE HYPOTHESIS. pt11 found the present is
        // guarded: sub_82B9CD68 reads [r31+21532] and, if it is NON-ZERO, diverts and
        // skips VdSwap entirely. This import is called UNCONDITIONALLY in that same
        // function, three instructions BEFORE the test, and it is called nowhere else
        // in the module. So after the freeze:
        //     it keeps ticking  -> the present function still runs; the FLAG is set
        //                          and the guard is skipping the swap.
        //     it stops too      -> the render loop never reaches the function at all.
        // Nothing else separates those, and they lead opposite ways.
        "VdGetSystemCommandBuffer",

        // WHO UN-SIGNALS 0xF8000028. The spinner polls one handle seven million times
        // a second and its wait returns INSTANTLY for 27.5 s -- so that object is
        // signalled -- and then starts timing out. Something clears it at the freeze.
        // NtClearEvent takes the handle directly in r3, so if it fires on 0xF8000028
        // it names the culprit outright. The release/cancel calls cover the same
        // question for the other object types.
        "NtClearEvent",
        "NtReleaseSemaphore",
        "NtReleaseMutant",
        "NtCancelTimer",
        "NtClose",

        // WHAT 0xF8000028 IS. Each of these returns a handle through a PHANDLE in r3;
        // the worker dereferences it at drain time (see zz_imp_is_create), by which
        // point the call has completed and the out-parameter is filled in. Matching
        // the handle to its creator names both the object TYPE and the guest code
        // that made it.
        "NtCreateEvent",
        "NtCreateSemaphore",
        "NtCreateMutant",
        "NtCreateTimer",
        0
    };
    for (int i = 0; WANT[i]; i++)
    {
        const char* a = WANT[i];
        const char* b = n;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return 1;
    }
    return 0;
}

// --------------------------------------------- the guest path, and only this
//
// No lock, no formatting, no file handle, no OutputDebugString. An unwanted name
// disarms its own slot on first sight and is never seen again; a wanted one costs a
// plain increment and, for the first few calls per index, one ring write.
extern "C" void zz_imp_log(const char* name, unsigned int idx,
                           unsigned long long lr, unsigned long long r3,
                           unsigned long long r4, unsigned long long r5,
                           unsigned long long base)
{
    if (idx >= 256 || !name) return;

    // One store, once. The guest base never changes, and the worker needs it to read
    // guest memory at drain time.
    if (!g_guestBase) g_guestBase = base;

    if (!g_impKnown[idx])
    {
        g_impKnown[idx] = 1;
        if (!zz_imp_wanted(name)) { zz_imp_watch[idx] = 0; return; }
        g_impName[idx] = name;
    }
    if (!zz_imp_watch[idx]) return;

    ZzImpHot* H = &g_hot[idx];
    const unsigned long c = ++H->count;
    const unsigned long tid = zz_imp_tid();

    // s9 pt10: the LAST call, always. This runs on every watched call including the
    // spinner's four million a second, so it is deliberately seven plain stores and
    // NO atomic -- no branch on the count either, because the whole value of this ring
    // is that it holds the final call before a freeze, and a ring that stopped
    // recording after eight would hold the first eight instead, which pt9 already had.
    {
        ZzImpLast* L = &H->last[(c - 1) & (ZZ_LAST - 1)];
        L->seq0 = c;
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        L->tid = tid; L->lr = lr; L->r3 = r3; L->r4 = r4; L->r5 = r5;
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        L->seq1 = c;
    }

    // Records are for the first few calls of each import; after that the COUNT is the
    // measurement -- for VdSwap the count IS the frame rate, and its standing still
    // is the finding.
    if (c > 8) return;

    const LONG slot = InterlockedIncrement(&g_ringHead) - 1;
    if (slot - g_ringTail >= ZZ_RING) { InterlockedIncrement(&g_ringLost); return; }
    ZzImpRec* r = &g_ring[slot & (ZZ_RING - 1)];
    r->idx = idx; r->tid = tid; r->lr = lr; r->r3 = r3; r->r4 = r4; r->r5 = r5;
    InterlockedExchange(&r->ready, 1);
}

// ------------------------------------------------------------ the worker path
static void zz_imp_hex(char* o, int* k, unsigned long long v, int digits)
{
    static const char* H = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) o[(*k)++] = H[(v >> (i * 4)) & 0xF];
}
static void zz_imp_dec(char* o, int* k, unsigned long long v)
{
    char t[24];
    int n = 0;
    if (!v) t[n++] = (char)48;
    while (v) { t[n++] = (char)(48 + (v % 10)); v /= 10; }
    while (n) o[(*k)++] = t[--n];
}
static void zz_imp_str(char* o, int* k, const char* s)
{
    while (*s) o[(*k)++] = *s++;
}

// s9 pt10: the last-calls dump, worker-side. Reads each slot's two sequence stamps
// around its fields and prints what it caught -- a slot whose stamps disagree was
// being overwritten as it was read and says so, rather than printing a record half
// of which belongs to another call. Slots are printed oldest first, so the LAST line
// of an import's block is its final call.
static void zz_imp_dump_last(int idx, const char* why)
{
    const unsigned long head = g_hot[idx].count;
    if (!head) return;

    {
        char b[192];
        int k = 0;
        zz_imp_str(b, &k, "ZZIMP last ");
        zz_imp_str(b, &k, g_impName[idx] ? g_impName[idx] : "unknown");
        zz_imp_str(b, &k, "  ");
        zz_imp_str(b, &k, why);
        zz_imp_str(b, &k, "  after ");
        zz_imp_dec(b, &k, g_hot[idx].count);
        zz_imp_str(b, &k, " calls");
        b[k] = 0;
        zz_log_line(b);
    }

    unsigned long first = (head > ZZ_LAST) ? (head - ZZ_LAST) : 0;
    for (unsigned long s = first + 1; s <= head; s++)
    {
        ZzImpLast* L = &g_hot[idx].last[(s - 1) & (ZZ_LAST - 1)];
        const unsigned long a = L->seq0;
        const unsigned long tid = L->tid;
        const unsigned long long lr = L->lr, r3 = L->r3, r4 = L->r4, r5 = L->r5;
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        const unsigned long z = L->seq1;

        char b[224];
        int k = 0;
        zz_imp_str(b, &k, "  ZZIMPLAST ");
        zz_imp_str(b, &k, g_impName[idx] ? g_impName[idx] : "unknown");
        zz_imp_str(b, &k, " #");
        zz_imp_dec(b, &k, s);
        if (a != z || a != s)
        {
            zz_imp_str(b, &k, "  <torn, seq ");
            zz_imp_dec(b, &k, a);
            zz_imp_str(b, &k, "/");
            zz_imp_dec(b, &k, z);
            zz_imp_str(b, &k, ">");
            b[k] = 0;
            zz_log_line(b);
            continue;
        }
        zz_imp_str(b, &k, "  tid=");
        zz_imp_hex(b, &k, tid, 8);
        zz_imp_str(b, &k, " lr=0x");
        zz_imp_hex(b, &k, lr, 8);
        zz_imp_str(b, &k, " r3=0x");
        zz_imp_hex(b, &k, r3, 8);
        zz_imp_str(b, &k, " r4=0x");
        zz_imp_hex(b, &k, r4, 8);
        zz_imp_str(b, &k, " r5=0x");
        zz_imp_hex(b, &k, r5, 8);
        b[k] = 0;
        zz_log_line(b);
    }
}

// Called from the worker tick. Everything that formats or writes happens HERE, on a
// thread the game never waits for.
extern "C" void zz_imp_drain(void)
{
    if (!g_impArmed) return;

    for (int guard = 0; guard < ZZ_RING; guard++)
    {
        const LONG tail = g_ringTail;
        if (tail == g_ringHead) break;
        ZzImpRec* r = &g_ring[tail & (ZZ_RING - 1)];
        if (!r->ready) break;

        char b[224];
        int k = 0;
        zz_imp_str(b, &k, "ZZIMP ");
        zz_imp_str(b, &k, g_impName[r->idx] ? g_impName[r->idx] : "unknown");
        zz_imp_str(b, &k, "  tid=");
        zz_imp_hex(b, &k, r->tid, 8);
        zz_imp_str(b, &k, "  lr=0x");
        zz_imp_hex(b, &k, r->lr, 8);
        zz_imp_str(b, &k, "  r3=0x");
        zz_imp_hex(b, &k, r->r3, 8);
        zz_imp_str(b, &k, " r4=0x");
        zz_imp_hex(b, &k, r->r4, 8);
        zz_imp_str(b, &k, " r5=0x");
        zz_imp_hex(b, &k, r->r5, 8);
        // s9 pt12: for a create, r3 is a PHANDLE and the call has long since completed
        // by the time the worker gets here -- so this is the handle it actually
        // produced. If the read fails, SAY so; a printed zero would read like an answer.
        if (zz_imp_is_create(g_impName[r->idx]))
        {
            unsigned int h = 0;
            zz_imp_str(b, &k, "  -> handle=");
            if (zz_imp_read_guest_u32(r->r3, &h)) { zz_imp_str(b, &k, "0x"); zz_imp_hex(b, &k, h, 8); }
            else                                   zz_imp_str(b, &k, "<unreadable>");
        }
        b[k] = 0;
        zz_log_line(b);

        InterlockedExchange(&r->ready, 0);
        InterlockedExchange(&g_ringTail, tail + 1);
    }

    // s9 pt10: the tick now carries a WALL CLOCK. Until this line existed the only
    // timestamps in the log came from the PC sampler, every fifth tick, so the pt9
    // timeline had to interpolate rate lines two seconds apart between them -- and
    // the whole finding was which two counters died in the SAME tick. A tick that
    // dates itself removes the interpolation from the answer.
    ++g_impTick;
    {
        char b[96];
        int k = 0;
        zz_imp_str(b, &k, "ZZIMP tick ");
        zz_imp_dec(b, &k, g_impTick);
        zz_imp_str(b, &k, "  t=");
        zz_imp_dec(b, &k, GetTickCount64());
        zz_imp_str(b, &k, " ms");
        b[k] = 0;
        zz_log_line(b);
    }

    // One rate line per watched import per tick, and only when it moved. For VdSwap
    // this is the frame counter, and a line that stops appearing is the answer.
    //
    // s9 pt10: the deltas are also the TRIGGER. Two shapes end a run, and pt9 saw
    // both in one tick:
    //   STALL     -- it was moving and this tick it did not move at all (VdSwap).
    //   COLLAPSE  -- it kept being called but fell by more than 16x from a rate that
    //                was large to begin with (NtWaitForSingleObjectEx, 8 M to 236).
    // A stall alone would have missed the spinner entirely, because the spinner never
    // went quiet. Either one on ANY import photographs them ALL, since the question
    // is what the rest of the system was doing at that instant.
    int trigger = 0;
    const char* why = 0;
    const char* whoName = 0;
    for (int i = 0; i < 256; i++)
    {
        if (!zz_imp_watch[i] || !g_impName[i]) continue;
        const unsigned long now = g_hot[i].count;
        const unsigned long delta = now - g_impDrained[i];
        const unsigned long prev = g_impPrevDelta[i];

        if (delta)
        {
            g_impEverFired[i] = 1;
            char b[160];
            int k = 0;
            zz_imp_str(b, &k, "ZZIMP rate ");
            zz_imp_str(b, &k, g_impName[i]);
            zz_imp_str(b, &k, "  +");
            zz_imp_dec(b, &k, delta);
            zz_imp_str(b, &k, " this tick, ");
            zz_imp_dec(b, &k, now);
            zz_imp_str(b, &k, " total");
            b[k] = 0;
            zz_log_line(b);
        }
        else if (g_impEverFired[i] && prev)
        {
            // The silence is the measurement, so say it out loud. pt9 had to read
            // this off the ABSENCE of a line, which is only legible once you already
            // know the tick cadence.
            char b[160];
            int k = 0;
            zz_imp_str(b, &k, "ZZIMP quiet ");
            zz_imp_str(b, &k, g_impName[i]);
            zz_imp_str(b, &k, "  +0 this tick, ");
            zz_imp_dec(b, &k, now);
            zz_imp_str(b, &k, " total (was +");
            zz_imp_dec(b, &k, prev);
            zz_imp_str(b, &k, ")");
            b[k] = 0;
            zz_log_line(b);
        }

        if (!trigger && g_impEverFired[i] && prev)
        {
            if (!delta)                                  { trigger = 1; why = "STALLED"; whoName = g_impName[i]; }
            else if (prev >= 1000 && delta < prev / 16)  { trigger = 1; why = "COLLAPSED"; whoName = g_impName[i]; }
        }

        g_impPrevDelta[i] = delta;
        g_impDrained[i] = now;
    }

    if (trigger && g_snapshots < ZZ_SNAP_MAX)
    {
        g_snapshots++;
        char b[192];
        int k = 0;
        zz_imp_str(b, &k, "ZZIMP SNAPSHOT ");
        zz_imp_dec(b, &k, (unsigned long long)g_snapshots);
        zz_imp_str(b, &k, " of ");
        zz_imp_dec(b, &k, ZZ_SNAP_MAX);
        zz_imp_str(b, &k, "  triggered by ");
        zz_imp_str(b, &k, whoName ? whoName : "unknown");
        zz_imp_str(b, &k, " ");
        zz_imp_str(b, &k, why ? why : "");
        zz_imp_str(b, &k, "  tick ");
        zz_imp_dec(b, &k, g_impTick);
        b[k] = 0;
        zz_log_line(b);
        for (int i = 0; i < 256; i++)
        {
            if (!g_impName[i]) continue;
            zz_imp_dump_last(i, "at snapshot");
        }
    }

    const LONG lost = InterlockedExchange(&g_ringLost, 0);
    if (lost)
    {
        char b[96];
        int k = 0;
        zz_imp_str(b, &k, "ZZIMP ring overflow, ");
        zz_imp_dec(b, &k, (unsigned long long)lost);
        zz_imp_str(b, &k, " records dropped");
        b[k] = 0;
        zz_log_line(b);
    }
}

extern "C" void zz_imp_arm(int on)
{
    if (InterlockedExchange(&g_impArmed, on ? 1 : 0) != 0) return;
    if (!on) return;
    for (int i = 0; i < 256; i++) zz_imp_watch[i] = 1;
}

// Test seams for the fixture, called by nothing in Emu.
extern "C" int  zz_selftest_import(const char* name) { return zz_imp_wanted(name); }
extern "C" unsigned char* zz_selftest_import_table(void) { return zz_imp_watch; }
extern "C" long zz_selftest_import_pending(void) { return g_ringHead - g_ringTail; }

// s9 pt10 seams. The tid one exists because a wrong TEB offset does not fail, it
// names the wrong thread -- so the fixture compares this against the documented API
// on a thread whose id it already knows.
extern "C" unsigned long zz_selftest_import_tid(void) { return zz_imp_tid(); }
extern "C" long zz_selftest_import_lastseq(unsigned int idx)
{
    return (idx < 256) ? (long)g_hot[idx].count : -1;
}
extern "C" void zz_selftest_import_dump_last(unsigned int idx, const char* why)
{
    if (idx < 256) zz_imp_dump_last((int)idx, why ? why : "selftest");
}

// ---------------------------------------------------------------------------
// Crackdown s1 (2026-10-14): the auto-logon's host side. See zz_import_hook.h for the
// guest side and for why. Armed at module load by <console folder>\<game>_autologon.on
// (cwd = the console folder). The marker may contain a hex XUID; empty = the xuid= line of
// xeo3_profile.txt (zz_save.cpp), the same player the local-save layer reports.
// ---------------------------------------------------------------------------
// The guest memory base (set by the import thunk, zz_import_hook.h) and an optional
// game-specific dump appended to fault records and to the XMA watchdog's report. A per-game
// file registers it at static init; examples\crackdown\zz_lock.cpp adds its D3D lock ring:
//     static int r = (zz_game_dump = my_dump, 1);
extern "C" uintptr_t zz_g_base = 0;
extern "C" uintptr_t zz_guest_base(void) { return zz_g_base; }
typedef void (*zz_game_dump_fn)(void (*emit)(const char*));
extern "C" zz_game_dump_fn zz_game_dump = nullptr;
extern "C" void zz_game_dump_run(void (*emit)(const char*)) { if (zz_game_dump) zz_game_dump(emit); }

extern "C" volatile unsigned char zz_autologon_pending = 0;
static volatile LONG g_autoClaimed = 0;
extern "C" unsigned long long zz_profile_xuid(void);   // zz_save.cpp
static unsigned long long g_autoXuid = 0;
static unsigned long long g_autoT0 = 0;

static void zz_auto_log(const char* s)
{
    HANDLE h = CreateFileA(ZZ_TITLE "_autologon.log", FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0, n = 0;
    while (s[n]) n++;
    WriteFile(h, s, n, &w, NULL);
    CloseHandle(h);
}

static void zz_auto_hex(char* out, unsigned long long v, int digits)
{
    for (int k = digits - 1; k >= 0; k--, v >>= 4) out[k] = "0123456789ABCDEF"[v & 0xF];
    out[digits] = 0;
}

static int zz_autologon_init(void)
{
    g_autoT0 = GetTickCount64();
    g_autoXuid = zz_profile_xuid();
    HANDLE h = CreateFileA(ZZ_TITLE "_autologon.on", GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;          // marker absent: never armed
    char buf[64] = {0};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    unsigned long long v = 0;
    int digits = 0;
    for (DWORD k = 0; k < got; k++) {
        char ch = buf[k];
        int d = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
              : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
        if (d < 0) { if (digits) break; continue; }
        v = (v << 4) | (unsigned)d;
        digits++;
    }
    if (digits == 16) g_autoXuid = v;
    char line[160] = "ZZAUTO armed: will XamUserLogonEx(0x";
    char hx[17]; zz_auto_hex(hx, g_autoXuid, 16);
    lstrcatA(line, hx);
    lstrcatA(line, ", flags 0x120424) before the first XamUserGetSigninState\r\n");
    zz_auto_log(line);
    zz_autologon_pending = 1;
    return 1;
}
static int g_autoInit = zz_autologon_init();

extern "C" int zz_autologon_claim(const char* import_name, unsigned long long* xuid)
{
    if (lstrcmpA(import_name, "XamUserGetSigninState") != 0) return 0;
    if (InterlockedCompareExchange(&g_autoClaimed, 1, 0) != 0) return 0;
    zz_autologon_pending = 0;                          // one attempt per process
    *xuid = g_autoXuid;
    return 1;
}

extern "C" void zz_autologon_done(unsigned int hr, unsigned int sp)
{
    char line[160] = "ZZAUTO XamUserLogonEx -> 0x";
    char hx[17];
    zz_auto_hex(hx, hr, 8);           lstrcatA(line, hx);
    lstrcatA(line, hr == 0 ? " (OK)  scratch sp=0x" : " (FAILED)  scratch sp=0x");
    zz_auto_hex(hx, sp, 8);           lstrcatA(line, hx);
    lstrcatA(line, "  t+");
    zz_auto_hex(hx, GetTickCount64() - g_autoT0, 8); lstrcatA(line, hx);
    lstrcatA(line, " ms(hex)\r\n");
    zz_auto_log(line);
}

// run 2: wait for the asynchronous XamUserLogonEx on the XOVERLAPPED in the scratch
// frame (guest memory, big-endian): +0 InternalLow (997 = ERROR_IO_PENDING until done),
// +0x18 dwExtendedError (the task's real HRESULT). Logs all three numbers; returns the
// final InternalLow (0 = signed in). Bounded at 30 s so a stuck logon cannot hang the
// game thread forever -- a timeout is itself the answer.
static unsigned int zz_be32(const unsigned char* p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}
extern "C" unsigned int zz_autologon_wait(unsigned int immediate, const unsigned char* xov)
{
    const ULONGLONG t0 = GetTickCount64();
    unsigned int low = zz_be32(xov);
    // run 3 fix: measured in run 2, the call returns 0 IMMEDIATELY with the logon still
    // pending (InternalLow 0x3E5) -- exactly what the dash log shows ("-> 0"). So wait on
    // InternalLow alone, whatever the immediate result was; returning early let xam write
    // its completion into stack the game had already reused.
    while ((immediate == 0u || immediate == 997u) && low == 997u && GetTickCount64() - t0 < 30000) {
        Sleep(10);
        low = zz_be32((const unsigned char*)xov);
    }
    char line[200] = "ZZAUTO async: immediate=0x";
    char hx[17];
    zz_auto_hex(hx, immediate, 8);             lstrcatA(line, hx);
    lstrcatA(line, " InternalLow=0x");          zz_auto_hex(hx, low, 8); lstrcatA(line, hx);
    lstrcatA(line, " ExtendedError=0x");        zz_auto_hex(hx, zz_be32(xov + 0x18), 8); lstrcatA(line, hx);
    lstrcatA(line, " waited(hex ms)=");         zz_auto_hex(hx, GetTickCount64() - t0, 8); lstrcatA(line, hx);
    lstrcatA(line, "\r\n");
    zz_auto_log(line);
    if (low == 997u) zz_auto_log("ZZAUTO !! still pending after 30 s -- the frame is being released with the logon in flight\r\n");
    return (immediate == 0u || immediate == 997u) ? low : immediate;
}
