// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Fable II: the FAULT NAMER -- a passive, process-wide vectored exception handler.
//
// WHY THIS EXISTS
// ---------------
// s4b got further than any previous build (music.bnk + all three texture banks
// streaming, GUI init) and then died like this:
//
//   (5274) GENERAL Unhandled exception c0000005 in the emulator at 00007FFF5B5C9340
//          (0000000000000000, 0000000000000000). Emu at 00007FF769A00000
//
// Three things make that message useless on its own, and each is why this file
// exists:
//   1. "in the emulator" is Emu's WORDING, not an attribution. It does not name
//      the containing module -- unlike the s1 form ("of Module <base>") that made
//      resolve_fault.py possible. Emu.exe itself is 1.2 MB at 0x7FF769A00000, so
//      the fault is provably NOT in Emu.exe; it is in one of the ~60 other images
//      in that process, and nothing on disk says which.
//   2. ExceptionInformation (0,0) is a READ OF HOST ADDRESS 0 -- a genuine host
//      null, not guest VA 0 (that was s1's 0x100000000).
//   3. Thread 5274 appears NOWHERE ELSE in 1954 log lines and the log continues
//      ~300 lines after the fatal. One thread died; the game carried on.
//
// A zero-launch ASLR replay was tried first (a probe script) and came
// back INCONCLUSIVE: its own known-answer check failed, because Emu_v5.exe maps at
// 0x7FF770A30000 today while the s4b run recorded 0x7FF769A00000. So the address
// cannot be attributed after the fact, and it has to be attributed AS IT HAPPENS.
//
// WHAT THIS DOES
// --------------
// Registers a vectored handler at the FRONT of the chain, which therefore sees
// every exception first-chance BEFORE any frame-based SEH Emu may use to catch it
// and park the thread (which is exactly what the s4b log shows happening). For a
// fault it records: tid, code, the faulting PC as MODULE + RVA, the two info
// words, and a real stack walk of the FAULTING context.
//
// It ALWAYS returns EXCEPTION_CONTINUE_SEARCH. It is an observer: it cannot
// swallow a fault, cannot change control flow, and costs nothing when nothing
// faults.
//
// DESIGN DECISIONS THAT ARE NOT ARBITRARY
// ---------------------------------------
// * DEDUPE, NOT A FIRST-N CAP. The plausible flood here is benign first-chance
//   AVs that Emu HANDLES (residency/write-watch tricks are undecidable from
//   outside a closed binary). A flat "log the first 64" cap would let that noise
//   exhaust the budget and leave the real fault unlogged -- an instrument that
//   fails SILENTLY, which is precisely the s4-pt2 lesson (an alias rule that only
//   widened could only ever excuse, never reject). So: full detail for every NEW
//   site, counters for repeats, and the counters are dumped alongside every
//   detailed record because there is no clean shutdown to print a summary at.
// * NULL-ISH FAULTS ARE ALWAYS DETAILED (info[1] < 0x10000, up to 4 per site),
//   because that is the signature of the fault being hunted, and a benign
//   residency AV faults on a real backing address, never on 0.
// * NO CRT IN THE HANDLER. Raw CreateFile/WriteFile and hand-rolled formatting:
//   no heap, no CRT locks, nothing that can deadlock against a faulting thread.
// * STACK OVERFLOW (0xC00000FD) TAKES A MINIMAL PATH: one short line, no unwind,
//   no formatting beyond hex. Doing real work on an overflowed stack double-faults
//   and would kill the process before Emu's own report -- the instrument would
//   destroy the measurement.
// * THE MODULE TABLE IS SNAPSHOT BY A WORKER THREAD, never by the handler. The
//   handler must not touch the loader lock: a faulting thread can already hold it.
//   The worker also refreshes every 2 s, so modules that load late (XAudio,
//   avcodec) are still named, and it re-dumps the table whenever the count changes.
//
// Log: <game>_crash.log in the working directory (= the the console folder console
// folder, same as <game>_switchmiss.log and <game>_setjmp.log).
#include <windows.h>
#include <tlhelp32.h>       // s9 pt4: thread enumeration for the PC sampler.
#include <d3d12.h>          // s6 pt4: the DRED interfaces and their uuids ONLY.
#include <d3d12sdklayers.h> // s6 pt5: ID3D12InfoQueue. Declarations only -- this
                     // does NOT make the debug layer load; if it is absent the QI
                     // simply fails and one line says so.
                     // Nothing here links d3d12.lib -- D3D12GetDebugInterface
                     // is reached through GetProcAddress on an already-loaded
                     // d3d12.dll, because this can run under the loader lock.

extern "C" IMAGE_DOS_HEADER __ImageBase;   // this module's own PE image

// ---------------------------------------------------------------------------
// module snapshot (written by the worker thread, read lock-free by the handler)
// ---------------------------------------------------------------------------
#define ZZ_MAX_MODS 320
#define ZZ_NAME_LEN 72

struct ZzMod
{
    unsigned long long base;
    unsigned long long size;
    char               name[ZZ_NAME_LEN];
};

// Double-buffered: the worker fills the inactive table, then publishes it with a
// single interlocked store. The handler reads whichever is published and never
// takes a lock, so it cannot be blocked by the worker or vice versa.
static ZzMod        g_mods[2][ZZ_MAX_MODS];
static volatile LONG g_modCount[2];
static volatile LONG g_live = -1;          // index of the published table, -1 = none

typedef BOOL (WINAPI *PFN_EnumProcessModules)(HANDLE, HMODULE*, DWORD, LPDWORD);
typedef struct { LPVOID base; DWORD size; LPVOID entry; } ZZ_MODINFO;   // = MODULEINFO
typedef BOOL (WINAPI *PFN_GetModuleInformation)(HANDLE, HMODULE, ZZ_MODINFO*, DWORD);

static PFN_EnumProcessModules   g_enumMods;
static PFN_GetModuleInformation g_modInfo;

// ---------------------------------------------------------------------------
// output: raw handle + hand-rolled formatting, no CRT
// ---------------------------------------------------------------------------
static volatile HANDLE g_log = INVALID_HANDLE_VALUE;
static volatile LONG   g_logInit;
static volatile LONG   g_wlock;

static void zz_lock()   { while (InterlockedCompareExchange(&g_wlock, 1, 0) != 0) Sleep(0); }
static void zz_unlock() { InterlockedExchange(&g_wlock, 0); }

static void zz_open()
{
    if (InterlockedCompareExchange(&g_logInit, 1, 0) != 0)
    {
        while (g_logInit < 2) Sleep(0);
        return;
    }
    // s9 pt8: ONE LOG PER PROCESS, not one per machine.
    //
    // The owner mutex below is per-PID by design -- it exists to keep the second DLL
    // twin inside the SAME process inert. It says nothing about a second PROCESS, and
    // Emu runs as a parent and a child (the -dvd handover). Both load this module,
    // both claim their own per-PID ownership, and both used to open the same
    // "<game>_crash.log" with CREATE_ALWAYS -- so the later one TRUNCATED the earlier
    // one. That is not a theory: run 7b banked a 29 KB crash log from a parent that
    // exited at ~1 min while a child ran on for another eight minutes, and the child
    // data was nowhere.
    //
    // So the FIRST process in this session keeps the familiar name and any later one
    // writes <game>_crash_<pid>.log. A cross-process named mutex is the discriminator,
    // because that is the thing the per-PID owner mutex cannot be.
    char path[64];
    int  k = 0;
    const char* base = ZZ_TITLE "_crash";
    while (base[k]) { path[k] = base[k]; k++; }

    HANDLE first = CreateMutexA(NULL, FALSE, "Local\\zz_" ZZ_TITLE "_log_primary");
    if (first && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        static const char* HEXD = "0123456789ABCDEF";
        const unsigned long pid = GetCurrentProcessId();
        path[k++] = '_';
        for (int i = 7; i >= 0; i--) path[k++] = HEXD[(pid >> (i * 4)) & 0xF];
    }
    const char* ext = ".log";
    for (int i = 0; ext[i]; i++) path[k++] = ext[i];
    path[k] = 0;

    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_log = h;
    InterlockedExchange(&g_logInit, 2);
}

// s9 pt8: the flush is no longer per line, and that is a correctness-preserving
// change with a very large performance consequence.
//
// THE MEASUREMENT THAT FORCED IT. Run 7 booted ~30x slower than run 6 with only the
// import instrument added, and every theory about why was wrong when measured: the
// emission volume (32 lines in three minutes), the watched-call frequency (512 calls
// at most), atomic contention, and ODS volume were all innocent, and a benchmark of
// the thunk itself put the disarmed path at +0.03 ns/call. What actually changed is
// WHO TAKES THIS LOCK. Before the import instrument only our own threads ever
// emitted; after it, guest threads emit too -- and zz_lock is a SPIN lock with
// Sleep(0), while zz_emit held it across WriteFile AND FlushFileBuffers, a real disk
// sync. A sampler round writes ~200 lines, so it performed ~200 disk syncs while
// every guest thread that touched a watched import spun on the lock burning a core.
// That starved the boot and our own worker with it -- one sampler round in 180 s.
//
// FlushFileBuffers survives a process KILL either way: the data is already in the
// OS cache, and only a machine crash could lose it. So the routine path buffers and
// the flush is time-boxed; the fault path still forces one, because that is the case
// where the process may not survive to flush later.
static ULONGLONG g_lastFlush;

static void zz_emit_ex(const char* buf, int len, int urgent)
{
    zz_open();
    if (g_log == INVALID_HANDLE_VALUE || (!len && !urgent))
        return;
    DWORD wrote = 0;
    zz_lock();
    if (len) WriteFile(g_log, buf, (DWORD)len, &wrote, NULL);
    const ULONGLONG now = GetTickCount64();
    if (urgent || (now - g_lastFlush) >= 250)
    {
        FlushFileBuffers(g_log);
        g_lastFlush = now;
    }
    zz_unlock();
}

static void zz_emit(const char* buf, int len) { zz_emit_ex(buf, len, 0); }

// The fault path forces one: a crash record is exactly the case where the
// process may be gone before the next time-boxed flush comes round.
static void zz_log_flush(void) { zz_emit_ex("", 0, 1); }

struct ZzBuf
{
    char buf[2048];
    int  n;
};

static void b_ch(ZzBuf* b, char c)
{
    if (b->n < (int)sizeof(b->buf) - 1) b->buf[b->n++] = c;
}

static void b_str(ZzBuf* b, const char* s)
{
    while (s && *s) b_ch(b, *s++);
}

static void b_hex(ZzBuf* b, unsigned long long v, int digits)
{
    static const char* HEX = "0123456789ABCDEF";
    if (digits <= 0)
    {
        digits = 1;
        for (unsigned long long t = v >> 4; t; t >>= 4) digits++;
    }
    for (int i = digits - 1; i >= 0; i--)
        b_ch(b, HEX[(v >> (i * 4)) & 0xF]);
}

static void b_dec(ZzBuf* b, unsigned long long v)
{
    char tmp[24];
    int  n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 23);
    while (n) b_ch(b, tmp[--n]);
}

static void b_flush(ZzBuf* b)
{
    zz_emit(b->buf, b->n);
    b->n = 0;
}

// s9 pt7: the one entry point zz_import.cpp uses. Kept here because this file owns
// the log handle and its lock; the import instrument stays free of both.
extern "C" void zz_log_line(const char* s)
{
    ZzBuf b; b.n = 0;
    b_str(&b, s);
    b_str(&b, "\r\n");
    b_flush(&b);
}

// Crackdown s4 A4: one VirtualQuery line for a host address (no lock, no CRT), and the game's
// own extra dump if it registered one (zz_import.cpp: zz_game_dump).
extern "C" void zz_game_dump_run(void (*emit)(const char*));
extern "C" uintptr_t zz_guest_base(void);
static void zz_page_line(ZzBuf* b, const char* what, unsigned long long host, unsigned long long guest)
{
    MEMORY_BASIC_INFORMATION mbi;
    b_str(b, "  ");
    b_str(b, what);
    if (guest) { b_str(b, " guest 0x"); b_hex(b, guest, 8); }
    b_str(b, ": ");
    if (!VirtualQuery((LPCVOID)host, &mbi, sizeof(mbi))) { b_str(b, "VirtualQuery failed\r\n"); return; }
    b_str(b, mbi.State == MEM_COMMIT ? "COMMIT" : mbi.State == MEM_RESERVE ? "RESERVE" : mbi.State == MEM_FREE ? "FREE" : "?");
    b_str(b, " state=0x");   b_hex(b, mbi.State, 0);
    b_str(b, " prot=0x");    b_hex(b, mbi.Protect, 0);
    b_str(b, (mbi.Protect & 0xFF) == PAGE_NOACCESS ? " (NOACCESS)" : (mbi.Protect & 0xFF) == PAGE_READWRITE ? " (RW)"
             : (mbi.Protect & 0xFF) == PAGE_READONLY ? " (RO)" : "");
    if (mbi.Protect & PAGE_GUARD) b_str(b, " +GUARD");
    b_str(b, " type=0x");    b_hex(b, mbi.Type, 0);
    b_str(b, mbi.Type == MEM_PRIVATE ? " (PRIVATE)" : mbi.Type == MEM_MAPPED ? " (MAPPED)" : "");
    b_str(b, " allocbase=0x"); b_hex(b, (unsigned long long)mbi.AllocationBase, 0);
    b_str(b, " region=0x");  b_hex(b, (unsigned long long)mbi.RegionSize, 0);
    b_str(b, "\r\n");
}

extern "C" void zz_imp_arm(int on);
extern "C" void zz_imp_drain(void);

// ---------------------------------------------------------------------------
// address -> module, from the snapshot only (no loader calls, ever)
// ---------------------------------------------------------------------------
static const ZzMod* zz_mod_for(unsigned long long addr)
{
    const LONG live = g_live;
    if (live < 0) return NULL;
    const LONG n = g_modCount[live];
    for (LONG i = 0; i < n && i < ZZ_MAX_MODS; i++)
    {
        const ZzMod* m = &g_mods[live][i];
        if (addr >= m->base && addr < m->base + m->size)
            return m;
    }
    return NULL;
}

static void b_addr(ZzBuf* b, unsigned long long addr)
{
    const ZzMod* m = zz_mod_for(addr);
    if (m)
    {
        b_str(b, m->name);
        b_str(b, "+0x");
        b_hex(b, addr - m->base, 0);
        b_str(b, " (0x");
        b_hex(b, addr, 16);
        b_ch(b, ')');
    }
    else
    {
        // No module contains it: either the snapshot is not published yet, or the
        // address really is in private/JIT memory -- which is itself a diagnosis.
        b_str(b, "<no module> (0x");
        b_hex(b, addr, 16);
        b_ch(b, ')');
    }
}

// ---------------------------------------------------------------------------
// dedupe table
// ---------------------------------------------------------------------------
#define ZZ_MAX_SITES 192

struct ZzSite
{
    unsigned long      code;
    unsigned long long base;      // containing module base, 0 if unknown
    unsigned long long rva;       // or the absolute PC when base is 0
    volatile LONG      hits;
    volatile LONG      guest;     // of those hits, how many were guest-window AVs
    volatile LONG      detailed;
};

static ZzSite       g_sites[ZZ_MAX_SITES];
static volatile LONG g_siteCount;
static volatile LONG g_benign[8];         // counters for the noisy, expected codes
static volatile LONG g_detailTotal;
static volatile LONG g_guestHits;         // Emu's guest page-fault machinery, expected

static const unsigned long ZZ_BENIGN[8] = {
    0xE06D7363,   // C++ throw
    0x406D1388,   // SetThreadName
    0x4000001F,   // STATUS_WX86_BREAKPOINT
    0x4000001E,   // STATUS_WX86_SINGLE_STEP
    0x80000003,   // breakpoint
    0x80000004,   // single step
    0x40010006,   // OutputDebugString
    0x000006BA    // RPC unavailable (raised, not fatal)
};

static int zz_benign_index(unsigned long code)
{
    for (int i = 0; i < 8; i++)
        if (ZZ_BENIGN[i] == code) return i;
    return -1;
}

// Returns the site, creating it if new and `create` allows it. *isNew tells the
// caller whether this is the first time this exact (code, module, rva) has been
// seen.
//
// ⚠ A FULL TABLE REPORTS *isNew = 1, ON PURPOSE. The alternative -- returning
// "not new" -- would mean that once the table fills, a genuinely novel fault is
// silently dropped, which is the s5 failure with a different cause. Erring toward
// detailing costs a few extra records; erring the other way costs the record.
static ZzSite* zz_site(unsigned long code, unsigned long long base, unsigned long long rva,
                       int* isNew, int create)
{
    *isNew = 0;
    const LONG n = g_siteCount;
    for (LONG i = 0; i < n && i < ZZ_MAX_SITES; i++)
    {
        if (g_sites[i].code == code && g_sites[i].base == base && g_sites[i].rva == rva)
            return &g_sites[i];
    }
    if (!create)
        return NULL;                 // guest-window AVs never consume a table slot
    const LONG idx = InterlockedIncrement(&g_siteCount) - 1;
    if (idx >= ZZ_MAX_SITES)
    {
        *isNew = 1;
        return NULL;
    }
    g_sites[idx].code = code;
    g_sites[idx].base = base;
    g_sites[idx].rva  = rva;
    *isNew = 1;
    return &g_sites[idx];
}

// ---------------------------------------------------------------------------
// the stack walk, isolated in its own function so __try can wrap it
// (clang-cl refuses __try in a function that needs C++ object unwinding, and a
//  corrupt .pdata chain or an unreadable return slot must not kill the process)
// ---------------------------------------------------------------------------
// ⚠ THE RISKY DEREFERENCE MUST SIT BEHIND A CALL, NOT INLINE IN THE __try BODY.
// Measured with test\iso.cpp: clang-cl's __except catches a fault raised inside a
// CALL made from the __try body (3/3), but a fault on a load written INLINE in
// that body is NOT caught and kills the process. The leaf fallback below reads
// [rsp] on a guess, which is precisely the read most likely to fault -- inline, it
// would turn the instrument into the thing that destroys the run.
__declspec(noinline) static DWORD64 zz_read_u64(DWORD64 addr)
{
    return *(volatile DWORD64*)addr;
}

// Reads one qword through the call form, reporting failure instead of raising.
// (Keeping the __try in its own tiny function, as everywhere else in this file.)
static int zz_try_read_u64(DWORD64 addr, DWORD64* out)
{
    __try { *out = zz_read_u64(addr); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// A window of qwords, 4 per line, with the byte-level view that makes a packed
// struct legible (a D3D12_RESOURCE_DESC1 is 16 dwords, not 8 qwords).
static void zz_dump_window(ZzBuf* b, const char* what, DWORD64 base, int qwords)
{
    // ⚠ FLUSH, DO NOT RESET. This began as `b->n = 0` and silently DISCARDED the
    // caller's unflushed content -- which in zz_veh is the whole record header and
    // the register file. The 17:47 launch therefore produced 80 records with no
    // "ZZCRASH # tid= code=" line and no registers: the memory windows survived and
    // the identifying line did not. Losing rbx cost the heap pointer.
    b_flush(b);
    b_str(b, "  mem @");
    b_str(b, what);
    b_str(b, " 0x");
    b_hex(b, base, 16);
    b_str(b, ":\r\n");
    b_flush(b);

    for (int i = 0; i < qwords; i += 4)
    {
        b->n = 0;
        b_str(b, "    +");
        b_hex(b, (unsigned long long)(i * 8), 3);
        b_str(b, "  ");
        for (int j = 0; j < 4; j++)
        {
            DWORD64 v = 0;
            if (zz_try_read_u64(base + (DWORD64)(i + j) * 8, &v))
                b_hex(b, (unsigned long long)v, 16);
            else
                b_str(b, "????????????????");
            b_ch(b, ' ');
        }
        b_str(b, "\r\n");
        b_flush(b);
    }
}

// ---------------------------------------------------------------------------
// naming an OBJECT at fault time (s6)
// ---------------------------------------------------------------------------
// s6 pt1 settled everything about the VGPU fault except the one thing that picks
// the fix: WHAT the heap is. Every argument of the refused CreatePlacedResource2
// is ordinary -- probe1/2/3/3b are 39 standalone cases and not one reproduces
// DXGI_ERROR_INVALID_CALL from an argument -- so the object is the last variable.
//
// A memory window cannot answer it: an ID3D12Heap is a vtable pointer and opaque
// state. What answers it is WHERE THE VTABLE LIVES (a real one is in D3D12Core.dll;
// a VGPU wrapper would be in VGPUDX12.dll) and, for a real one, its GetDesc.
static int zz_name_has(const char* name, const char* needle)
{
    for (int i = 0; name[i]; i++)
    {
        int j = 0;
        while (needle[j] && name[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static void zz_dump_object(ZzBuf* b, const char* what, DWORD64 obj)
{
    b_flush(b);
    b_str(b, "  obj ");
    b_str(b, what);
    b_str(b, " = 0x");
    b_hex(b, obj, 16);
    if (!obj) { b_str(b, "  (null)\r\n"); b_flush(b); return; }

    DWORD64 vt = 0;
    if (!zz_try_read_u64(obj, &vt))
    {
        b_str(b, "  <unreadable>\r\n");
        b_flush(b);
        return;
    }
    b_str(b, "\r\n    vtable ");
    b_addr(b, vt);
    b_str(b, "\r\n");
    b_flush(b);

    // The first slots are enough to recognise a class: for any COM object [0..2]
    // are QueryInterface/AddRef/Release, and the module they sit in is the answer
    // we actually came for.
    for (int i = 0; i < 10; i++)
    {
        DWORD64 fn = 0;
        if (!zz_try_read_u64(vt + (DWORD64)i * 8, &fn)) break;
        b->n = 0;
        b_str(b, "      [");
        b_dec(b, (unsigned long long)i);
        b_str(b, "] ");
        b_addr(b, fn);
        b_str(b, "\r\n");
        b_flush(b);
    }
}

// D3D12_HEAP_DESC, laid out by hand so this file keeps needing only <windows.h>.
struct ZzHeapDesc
{
    unsigned long long SizeInBytes;       // +0x00
    unsigned int       Type;              // +0x08  D3D12_HEAP_TYPE
    unsigned int       CPUPageProperty;   // +0x0C
    unsigned int       MemoryPool;        // +0x10
    unsigned int       CreationNodeMask;  // +0x14
    unsigned int       VisibleNodeMask;   // +0x18
    unsigned int       pad0;              // +0x1C
    unsigned long long Alignment;         // +0x20
    unsigned int       Flags;             // +0x28  D3D12_HEAP_FLAGS
    unsigned int       pad1;              // +0x2C
};

// ⚠ Behind a CALL, like every risky operation in this file: clang-cl's __except
// does not catch a fault written inline in the __try body (measured, test\iso.cpp).
// A >8-byte struct return uses the hidden-out-pointer form, so the ABI signature
// really is (this, out).
__declspec(noinline) static void zz_call_getdesc(DWORD64 self, DWORD64 fn, void* out)
{
    typedef void* (*ZzPfnGetDesc)(void*, void*);
    ((ZzPfnGetDesc)fn)((void*)self, out);
}

static int zz_try_getdesc(DWORD64 self, DWORD64 fn, void* out)
{
    __try { zz_call_getdesc(self, fn, out); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Calling a function pointer out of a fault handler is the one thing in this file
// that could make the instrument worse than the bug it is measuring, so the gate
// is deliberately narrow and it fires AT MOST ONCE per process:
//   - the object's vtable and its slot 8 must both resolve to a module,
//   - it must be the SAME module,
//   - and that module must be D3D12Core -- i.e. we only ever call GetDesc on
//     something that really is a runtime object.
// When the gate refuses, the reason it prints is itself the finding: a heap whose
// vtable is not in D3D12Core is not a heap.
static volatile LONG g_heapDescOnce;

static void zz_dump_heap_desc(ZzBuf* b, DWORD64 heap)
{
    if (!heap) return;

    DWORD64 vt = 0, fn = 0;
    if (!zz_try_read_u64(heap, &vt)) return;
    if (!zz_try_read_u64(vt + 8 * 8, &fn)) return;      // ID3D12Heap::GetDesc = slot 8

    const ZzMod* mv = zz_mod_for(vt);
    const ZzMod* mf = zz_mod_for(fn);

    b_flush(b);
    if (!mv || !mf || mv != mf)
    {
        b_str(b, "    heap GetDesc SKIPPED: vtable and slot 8 do not share a module\r\n");
        b_flush(b);
        return;
    }
    if (!zz_name_has(mv->name, "D3D12Core") && !zz_name_has(mv->name, "d3d12core"))
    {
        b_str(b, "    heap GetDesc SKIPPED: vtable is in ");
        b_str(b, mv->name);
        b_str(b, ", not D3D12Core -- THIS IS NOT AN ID3D12Heap\r\n");
        b_flush(b);
        return;
    }
    if (InterlockedCompareExchange(&g_heapDescOnce, 1, 0) != 0)
    {
        b_str(b, "    heap GetDesc skipped: already called once this process\r\n");
        b_flush(b);
        return;
    }

    ZzHeapDesc d;
    for (int i = 0; i < (int)sizeof(d); i++) ((char*)&d)[i] = 0;

    if (!zz_try_getdesc(heap, fn, &d))
    {
        b_str(b, "    heap GetDesc FAULTED\r\n");
        b_flush(b);
        return;
    }

    b_str(b, "    heap GetDesc: SizeInBytes=0x");
    b_hex(b, d.SizeInBytes, 0);
    b_str(b, "  Alignment=0x");
    b_hex(b, d.Alignment, 0);
    b_str(b, "  Flags=0x");
    b_hex(b, (unsigned long long)d.Flags, 0);
    b_str(b, "\r\n      Type=");
    b_dec(b, (unsigned long long)d.Type);
    b_str(b, " CPUPageProperty=");
    b_dec(b, (unsigned long long)d.CPUPageProperty);
    b_str(b, " MemoryPool=");
    b_dec(b, (unsigned long long)d.MemoryPool);
    b_str(b, " nodes=");
    b_hex(b, (unsigned long long)d.CreationNodeMask, 0);
    b_ch(b, '/');
    b_hex(b, (unsigned long long)d.VisibleNodeMask, 0);
    b_str(b, "\r\n");
    b_flush(b);
}


// Test seam. The object namer only ever runs on a VGPUDX12 fault, and no fixture
// can force `rbx` at a real fault without hand-written assembly -- so the FUNCTION
// is exercised here with a PLANTED object whose answers are known, and the three
// lines that wire rbx into it stay small enough to check by eye. Exported so
// zz_crash_test.exe can call it; nothing in Emu ever does.
// [[feedback_null_results_verify_the_search]]
extern "C" void zz_selftest_dump_object(const char* what, void* obj)
{
    ZzBuf b;
    b.n = 0;
    zz_dump_object(&b, what, (DWORD64)obj);
    zz_dump_heap_desc(&b, (DWORD64)obj);
    b_flush(&b);
}

// ---------------------------------------------------------------------------
// THE DEVICE WATCH (s6 pt4)
// ---------------------------------------------------------------------------
// s6 pt3 established that the stencil-scratch AV is a CASUALTY: the D3D12 device
// was already removed, with reason DXGI_ERROR_INVALID_CALL, before the refused
// CreatePlacedResource2. So the question is no longer "why was that call refused"
// but "what killed the device, and when".
//
// VGPU's device is reachable from this module with no cooperation from Emu:
//
//     [VGPUDX12.dll + 0x5ABCC0]        -> VGPU's global state
//     [state + 0x28]                   -> the ID3D12Device
//
// which is the same field the 43 failure paths call GetDeviceRemovedReason on, and
// the same one two other sites call GetCopyableFootprints on (slot 38, argument for
// argument -- that is how the object was proven to be a device without assuming the
// slot number). Polling it from the worker brackets the guilty submission in TIME,
// against the boot log. DRED, if it can be armed early enough, names it outright.
static const unsigned long ZZ_VGPU_STATE_RVA  = 0x5ABCC0;
static const unsigned long ZZ_VGPU_DEVICE_OFF = 0x28;

// Crackdown (2026-10-14): markers are TITLE-SCOPED. This file is Fable II's, and its
// s7-s9 VGPU fixes (zz_edram_init / zz_cast_fix / zz_map_clamp) and instruments
// (zz_dred / zz_pcsample / zz_import_log) read marker files from the cwd -- which is
// the shared the console folder folder, where Fable II's markers are all present. Read
// unprefixed, every one of them would be live for Crackdown's first launch. So this
// copy looks for "<game>_<name>" instead (e.g. <game>_zz_edram_init.on): all off
// until Crackdown's own fault record asks for one, and an A/B is still one file. The
// announce lines below still print the unprefixed name. zz_time.cpp's zz_mftb.on is
// NOT routed through here and stays shared on purpose.
static int zz_marker(const char* name)
{
    static const char prefix[] = ZZ_TITLE "_";
    char path[96];
    size_t n = 0;
    for (const char* p = prefix; *p && n < sizeof(path) - 1; ++p) path[n++] = *p;
    for (const char* p = name;   *p && n < sizeof(path) - 1; ++p) path[n++] = *p;
    path[n] = 0;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// The device pointer, with NO call made through it. The raw form is for the
// static initialiser, which runs before the module snapshot is published and so
// cannot do the D3D12Core sanity check.
static DWORD64 zz_vgpu_device_raw(void)
{
    HMODULE m = GetModuleHandleA("VGPUDX12.dll");
    if (!m) return 0;
    DWORD64 state = 0, dev = 0;
    if (!zz_try_read_u64((DWORD64)m + ZZ_VGPU_STATE_RVA, &state) || !state) return 0;
    if (!zz_try_read_u64(state + ZZ_VGPU_DEVICE_OFF, &dev) || !dev) return 0;
    return dev;
}

// The checked form: a D3D12 object is only ever called through once its vtable has
// been positively attributed to a D3D12 module.
//
// ⚠ s7 pt3, and the reason this now accepts TWO modules: with the debug layer on,
// VGPU's device pointer is the LAYER'S WRAPPER, whose vtable lives in
// D3D12SDKLayers.dll. The original gate accepted D3D12Core only, so it refused the
// wrapper -- and that silently blinded the entire device watch during the one launch
// that had the layer on: no "device found" line, no infoqueue arm (so the 1024
// message default stood and ate the tail), no removal line, no dump. The gate and
// the debug layer were mutually exclusive as coded.
//
// Accepting the wrapper is still a POSITIVE identification and not a widening: the
// wrapper class exists only when the layer is loaded, and nothing else in this
// process puts a vtable in D3D12SDKLayers.dll.
// [[reference_widening_rules_only_excuse]]
// ⚠ CASE-INSENSITIVE, and the fixture is why. The first version of this used the
// existing case-sensitive zz_name_has with the needle "D3D12SDKLayers", and the
// module is called `d3d12SDKLayers.dll` on this machine -- lowercase d3d12. The
// gate would have refused the debug-layer wrapper for a second time, the launch
// would have produced no ZZDEV lines at all, and it would have looked exactly like
// a build that was never installed. Windows file names are not case-sensitive and
// this comparison must not be either.
static int zz_ci_has(const char* name, const char* needle)
{
    for (int i = 0; name[i]; i++)
    {
        int j = 0;
        while (needle[j])
        {
            char a = name[i + j], n = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (n >= 'A' && n <= 'Z') n = (char)(n - 'A' + 'a');
            if (a != n) break;
            j++;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

static int zz_is_d3d12_module_name(const char* name)
{
    return zz_ci_has(name, "d3d12core") || zz_ci_has(name, "d3d12sdklayers");
}

static int zz_is_d3d12_vtable(DWORD64 vt)
{
    const ZzMod* mv = zz_mod_for(vt);
    if (!mv) return 0;
    return zz_is_d3d12_module_name(mv->name);
}

// Test seam, exported for the fixture and called by nothing in Emu. The widened
// gate is the one piece of s8 that a launch cannot re-prove cheaply -- if it is
// wrong in EITHER direction the run produces no ZZDEV lines at all and looks like
// a build that was never installed -- so both directions are machine-checked here.
extern "C" void zz_selftest_d3d12_gate(const char* name)
{
    ZzBuf b;
    b.n = 0;
    b_str(&b, "ZZGATE \"");
    b_str(&b, name);
    b_str(&b, "\" -> ");
    b_str(&b, zz_is_d3d12_module_name(name) ? "ACCEPTED\r\n" : "refused\r\n");
    b_flush(&b);
}

static DWORD64 zz_vgpu_device(void)
{
    const DWORD64 dev = zz_vgpu_device_raw();
    if (!dev) return 0;
    DWORD64 vt = 0;
    if (!zz_try_read_u64(dev, &vt) || !vt) return 0;
    if (!zz_is_d3d12_vtable(vt)) return 0;
    return dev;
}

__declspec(noinline) static HRESULT zz_call_gdrr(DWORD64 self, DWORD64 fn)
{
    typedef HRESULT (*ZzPfnGdrr)(void*);
    return ((ZzPfnGdrr)fn)((void*)self);
}

static int zz_try_gdrr(DWORD64 self, DWORD64 fn, HRESULT* out)
{
    __try { *out = zz_call_gdrr(self, fn); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ID3D12Device::GetDeviceRemovedReason is vtable slot 37 -- the same 0x128 the
// VGPU failure paths call. Gated exactly like the GetDesc call: the vtable and the
// slot must resolve to one module before anything is called.
static int zz_device_removed_reason(DWORD64 dev, HRESULT* out)
{
    DWORD64 vt = 0, fn = 0;
    if (!zz_try_read_u64(dev, &vt)) return 0;
    if (!zz_try_read_u64(vt + 37 * 8, &fn)) return 0;
    const ZzMod* mv = zz_mod_for(vt);
    const ZzMod* mf = zz_mod_for(fn);
    if (!mv || !mf || mv != mf) return 0;
    return zz_try_gdrr(dev, fn, out);
}

// --- DRED ------------------------------------------------------------------
// Auto-breadcrumbs and page-fault data must be ARMED BEFORE THE DEVICE IS CREATED,
// so this is attempted from the static initialiser -- which runs under the loader
// lock, hence GetModuleHandle and never LoadLibrary. Opt-in via a "zz_dred.on"
// marker in the console folder (the same convention as zz_mftb.on), so the ordinary
// play build is never slowed down and the experiment is undone by deleting a file.
// [[feedback_restore_config_after_experiments]]
// 0 = untried (keep trying: d3d12.dll may not be loaded yet)
// 1 = settled (armed, or failed in a way retrying cannot fix)
// 2 = disabled (no marker)
// Settled states are SILENT. This is called from the worker every 2 s, and an
// instrument that reprints the same line hundreds of times buries the one line
// the run exists to produce.
static volatile LONG g_dredState;
static volatile LONG g_dredWaitLogged;

static void zz_dred_enable(const char* when)
{
    if (g_dredState) return;          // settled, and silent

    ZzBuf b;
    b.n = 0;
    b_str(&b, "ZZDEV dred(");
    b_str(&b, when);
    b_str(&b, "): ");

    if (!zz_marker("zz_dred.on"))
    {
        if (InterlockedCompareExchange(&g_dredState, 2, 0) == 0)
        {
            b_str(&b, "marker zz_dred.on absent, not arming (said once)\r\n");
            b_flush(&b);
        }
        return;
    }

    // Report the window we are in: DRED is only useful if the device does not exist yet.
    const DWORD64 devNow = zz_vgpu_device_raw();
    b_str(&b, "VGPUDX12=");
    b_str(&b, GetModuleHandleA("VGPUDX12.dll") ? "loaded" : "not loaded");
    b_str(&b, " d3d12=");
    b_str(&b, GetModuleHandleA("d3d12.dll") ? "loaded" : "not loaded");
    b_str(&b, " device=");
    if (devNow) { b_str(&b, "0x"); b_hex(&b, devNow, 16); b_str(&b, " ALREADY CREATED (too late)"); }
    else        { b_str(&b, "not yet (good)"); }
    b_str(&b, " -- ");

    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    if (!d3d12)
    {
        // Retryable: leave the state untried so a later tick can arm it, but say
        // so only once.
        if (InterlockedCompareExchange(&g_dredWaitLogged, 1, 0) == 0)
        {
            b_str(&b, "d3d12.dll not loaded yet; will retry each tick (said once)\r\n");
            b_flush(&b);
        }
        return;
    }

    PFN_D3D12_GET_DEBUG_INTERFACE p =
        (PFN_D3D12_GET_DEBUG_INTERFACE)GetProcAddress(d3d12, "D3D12GetDebugInterface");
    if (!p)
    {
        InterlockedExchange(&g_dredState, 1);
        b_str(&b, "no D3D12GetDebugInterface export\r\n");
        b_flush(&b);
        return;
    }

    ID3D12DeviceRemovedExtendedDataSettings* s = nullptr;
    HRESULT hr = E_FAIL;
    __try { hr = p(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), (void**)&s); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hr = E_FAIL; s = nullptr; }

    if (FAILED(hr) || !s)
    {
        InterlockedExchange(&g_dredState, 1);
        b_str(&b, "D3D12GetDebugInterface(DREDSettings) hr=0x");
        b_hex(&b, (unsigned long long)(unsigned)hr, 8);
        b_str(&b, "\r\n");
        b_flush(&b);
        return;
    }

    InterlockedExchange(&g_dredState, 1);
    __try
    {
        s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        s->Release();
        b_str(&b, "ARMED (breadcrumbs + page faults FORCED_ON)\r\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        b_str(&b, "FAULTED while arming\r\n");
    }
    b_flush(&b);
}

// Walks DRED's output. Every deref is inside the __try, and the walk is capped, so
// a corrupt list costs a truncated dump and nothing else.
static volatile LONG g_dredDumped;

static void zz_dred_dump(DWORD64 dev)
{
    if (!dev) return;
    if (InterlockedCompareExchange(&g_dredDumped, 1, 0) != 0) return;

    ZzBuf b;
    b.n = 0;
    b_str(&b, "ZZDEV DRED:\r\n");
    b_flush(&b);

    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    HRESULT hr = E_FAIL;
    __try { hr = ((IUnknown*)dev)->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData),
                                                  (void**)&dred); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hr = E_FAIL; dred = nullptr; }

    if (FAILED(hr) || !dred)
    {
        b.n = 0;
        b_str(&b, "  no ID3D12DeviceRemovedExtendedData (hr=0x");
        b_hex(&b, (unsigned long long)(unsigned)hr, 8);
        b_str(&b, ") -- DRED was not armed before device creation\r\n");
        b_flush(&b);
        return;
    }

    __try
    {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc;
        bc.pHeadAutoBreadcrumbNode = nullptr;
        HRESULT h1 = dred->GetAutoBreadcrumbsOutput(&bc);

        b.n = 0;
        b_str(&b, "  breadcrumbs hr=0x");
        b_hex(&b, (unsigned long long)(unsigned)h1, 8);
        b_str(&b, "\r\n");
        b_flush(&b);

        const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode;
        for (int node = 0; n && node < 8; node++)
        {
            b.n = 0;
            b_str(&b, "    node ");
            b_dec(&b, (unsigned long long)node);
            b_str(&b, " queue=");
            b_str(&b, n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "?");
            b_str(&b, " list=");
            b_str(&b, n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "?");
            b_str(&b, " ops=");
            b_dec(&b, (unsigned long long)n->BreadcrumbCount);
            b_str(&b, " lastCompleted=");
            b_dec(&b, n->pLastBreadcrumbValue ? (unsigned long long)*n->pLastBreadcrumbValue : 0);
            b_str(&b, "\r\n");
            b_flush(&b);

            // The ops AROUND the last completed one are the interesting window:
            // the first op after it is the one that did not finish.
            const UINT last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            UINT from = (last > 4) ? last - 4 : 0;
            UINT to = last + 8;
            if (to > n->BreadcrumbCount) to = n->BreadcrumbCount;
            for (UINT i = from; i < to && n->pCommandHistory; i++)
            {
                b.n = 0;
                b_str(&b, "      op[");
                b_dec(&b, (unsigned long long)i);
                b_str(&b, "] = ");
                b_dec(&b, (unsigned long long)n->pCommandHistory[i]);
                if (i == last) b_str(&b, "   <-- last COMPLETED; the next one is the suspect");
                b_str(&b, "\r\n");
                b_flush(&b);
            }
            n = n->pNext;
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT pf;
        pf.PageFaultVA = 0;
        pf.pHeadExistingAllocationNode = nullptr;
        pf.pHeadRecentFreedAllocationNode = nullptr;
        HRESULT h2 = dred->GetPageFaultAllocationOutput(&pf);
        b.n = 0;
        b_str(&b, "  pagefault hr=0x");
        b_hex(&b, (unsigned long long)(unsigned)h2, 8);
        b_str(&b, " VA=0x");
        b_hex(&b, (unsigned long long)pf.PageFaultVA, 16);
        b_str(&b, "\r\n");
        b_flush(&b);

        dred->Release();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        b.n = 0;
        b_str(&b, "  DRED walk FAULTED (output truncated)\r\n");
        b_flush(&b);
    }
}

// --- the debug layer's own account (s6 pt5) --------------------------------
// DRED came back EMPTY: breadcrumbs S_OK with no nodes, page fault S_OK with
// VA 0.
//
// ⚠ s7: the reading this comment originally carried -- "therefore the GPU
// executed no command lists at all" -- is WITHDRAWN. Breadcrumb nodes for
// COMPLETED lists are retained on a budget, and a CPU-side removal snapshots no
// GPU fault, so an empty node list is consistent with any amount of prior work
// (which is what the white screen and 18 s of streaming say happened).
// What survives stands on the removal REASON alone: DXGI_ERROR_INVALID_CALL
// means the runtime killed the device over a CPU-side API call.
//
// The only thing that names such a call in words is the debug layer, and if it is
// loaded then ID3D12InfoQueue hangs off the very device pointer we already have.
// Without it this prints one line saying so and costs nothing, so it is safe to
// ship whether or not the layer is ever installed.
static volatile LONG g_infoqDumped;

static void zz_infoqueue_dump(DWORD64 dev)
{
    if (!dev) return;
    if (InterlockedCompareExchange(&g_infoqDumped, 1, 0) != 0) return;

    ZzBuf b;
    b.n = 0;
    b_str(&b, "ZZDEV InfoQueue:\r\n");
    b_flush(&b);

    ID3D12InfoQueue* q = nullptr;
    HRESULT hr = E_FAIL;
    __try { hr = ((IUnknown*)dev)->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&q); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hr = E_FAIL; q = nullptr; }

    if (FAILED(hr) || !q)
    {
        b.n = 0;
        b_str(&b, "  no ID3D12InfoQueue (hr=0x");
        b_hex(&b, (unsigned long long)(unsigned)hr, 8);
        b_str(&b, ") -- the debug layer is not loaded; drop the Agility 1.618.5 "
                  "d3d12SDKLayers.dll next to D3D12Core.dll and force it on for "
                  "emu_v9.exe with d3dconfig\r\n");
        b_flush(&b);
        return;
    }

    __try
    {
        const UINT64 n = q->GetNumStoredMessages();
        b.n = 0;
        b_str(&b, "  stored messages: ");
        b_dec(&b, (unsigned long long)n);
        b_str(&b, "\r\n");
        b_flush(&b);

        // Whether anything was LOST is part of the measurement, not a detail: the
        // queue discards NEW messages when it is full, so a non-zero discard count
        // means the fatal message may never have been stored at all and the run's
        // conclusion has to say so. The denied count is the storage filter doing
        // its job (INFO/MESSAGE refused at arm time) and should be large.
        const UINT64 disc = q->GetNumMessagesDiscardedByMessageCountLimit();
        const UINT64 deny = q->GetNumMessagesDeniedByStorageFilter();
        b.n = 0;
        b_str(&b, "  discarded by count limit: ");
        b_dec(&b, (unsigned long long)disc);
        b_str(&b, "   denied by storage filter: ");
        b_dec(&b, (unsigned long long)deny);
        b_str(&b, "\r\n");
        b_flush(&b);

        // The LAST messages are the interesting ones: the invalid call that removed
        // the device is at or near the end. 64 rather than 24, which is affordable
        // now that INFO/MESSAGE never reach storage.
        UINT64 first = (n > 64) ? n - 64 : 0;
        static char msgbuf[8192];
        for (UINT64 i = first; i < n; i++)
        {
            SIZE_T len = 0;
            if (FAILED(q->GetMessage(i, NULL, &len)) || !len || len > sizeof(msgbuf))
                continue;
            if (FAILED(q->GetMessage(i, (D3D12_MESSAGE*)msgbuf, &len)))
                continue;

            const D3D12_MESSAGE* m = (const D3D12_MESSAGE*)msgbuf;
            b.n = 0;
            b_str(&b, "    [");
            b_dec(&b, (unsigned long long)i);
            b_str(&b, "] sev=");
            b_dec(&b, (unsigned long long)m->Severity);
            b_str(&b, " cat=");
            b_dec(&b, (unsigned long long)m->Category);
            b_str(&b, " id=");
            b_dec(&b, (unsigned long long)m->ID);
            b_str(&b, "\r\n      ");
            b_str(&b, m->pDescription ? m->pDescription : "(no description)");
            b_str(&b, "\r\n");
            b_flush(&b);
        }
        q->Release();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        b.n = 0;
        b_str(&b, "  InfoQueue walk FAULTED (output truncated)\r\n");
        b_flush(&b);
    }
}

// --- arming that queue, the moment the device is first seen (s7 pt2) --------
// The trap this exists to remove: the info queue stores 1024 messages by default,
// and when it is FULL it discards the NEW ones, not the old ones. With the debug
// layer live across 18 s of boot, the fatal message at the END is exactly the one
// that would be dropped -- the dump would then be 24 lines of startup chatter and
// the launch would be wasted.
//
// So: refuse INFO and MESSAGE at storage time (CORRUPTION/ERROR/WARNING are kept)
// and raise the limit to 65536. ONE SHOT, like every other arm in this file: a
// per-tick retry would reprint the "layer not loaded" line hundreds of times and
// bury the line the run exists to produce (the pt4 flood lesson).
static volatile LONG g_infoqArmed;

static void zz_infoqueue_arm(DWORD64 dev)
{
    if (!dev) return;
    if (InterlockedCompareExchange(&g_infoqArmed, 1, 0) != 0) return;

    ZzBuf b;

    // Same guarded QI as the dump: the virtual call is made FROM the __try body,
    // which is the form clang-cl's __except actually catches (a load written
    // inline in that body is not). [[reference_clangcl_try_inline_load]]
    ID3D12InfoQueue* q = nullptr;
    HRESULT hr = E_FAIL;
    __try { hr = ((IUnknown*)dev)->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&q); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hr = E_FAIL; q = nullptr; }

    if (FAILED(hr) || !q)
    {
        b.n = 0;
        b_str(&b, "ZZDEV no ID3D12InfoQueue at arm time (hr=0x");
        b_hex(&b, (unsigned long long)(unsigned)hr, 8);
        b_str(&b, ") -- debug layer not loaded\r\n");
        b_flush(&b);
        return;
    }

    // Built outside the __try and field by field, in the style of the DRED
    // page-fault struct: no memset, no aggregate init, nothing that could pull
    // C++ unwinding into a function that has to carry __try.
    D3D12_MESSAGE_SEVERITY sev[2];
    sev[0] = D3D12_MESSAGE_SEVERITY_INFO;
    sev[1] = D3D12_MESSAGE_SEVERITY_MESSAGE;

    D3D12_INFO_QUEUE_FILTER f;
    f.AllowList.NumCategories = 0;
    f.AllowList.pCategoryList = nullptr;
    f.AllowList.NumSeverities = 0;
    f.AllowList.pSeverityList = nullptr;
    f.AllowList.NumIDs        = 0;
    f.AllowList.pIDList       = nullptr;
    f.DenyList.NumCategories  = 0;
    f.DenyList.pCategoryList  = nullptr;
    f.DenyList.NumSeverities  = 2;
    f.DenyList.pSeverityList  = sev;
    f.DenyList.NumIDs         = 0;
    f.DenyList.pIDList        = nullptr;

    __try
    {
        q->PushStorageFilter(&f);

        // s9 phase A: TURN OFF break-on-error.
        //
        // Every run since s7 has ended AT the first D3D12 ERROR, because the
        // layer breaks there -- so the message that comes AFTER the first one
        // has never been seen. #342 stops the run; whatever removes the device
        // in retail is behind it. With the break off the layer REPORTS and
        // keeps going, and one launch maps every remaining wall instead of only
        // the next one. [[feedback_logs_first_theories_second]] CORRUPTION is
        // turned off for the same reason -- a corruption break would hide the
        // tail just as effectively.
        q->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        q->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);

        q->SetMessageCountLimit(65536);
        q->Release();

        b.n = 0;
        b_str(&b, "ZZDEV infoqueue armed (filter WARNING+, limit 65536, "
                  "break-on-error OFF)\r\n");
        b_flush(&b);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        b.n = 0;
        b_str(&b, "ZZDEV infoqueue arm FAULTED (queue left at its defaults)\r\n");
        b_flush(&b);
    }
}

// --- the FIX: initialize what VGPU forgot (s8, opt-in: zz_edram_init.on) ----
//
// THE BUG, named in English by the debug layer in s7 pt3 and reproduced verbatim
// standalone in s8 phase P (test\d3d12_probe6.cpp case 1):
//
//   EXECUTION ERROR #1422 RENDER_TARGET_OR_DEPTH_STENCIL_RESOUCE_NOT_INITIALIZED
//   Resource (...:'EDRAM alias texture'), Subresource (0) is not initialized but
//   is used in Function (CGraphicsCommandList::SetComputeRootDescriptorTable)
//
// D3D12 requires a PLACED resource carrying ALLOW_RENDER_TARGET or
// ALLOW_DEPTH_STENCIL to be initialized by a Discard, Clear or Copy before any
// other use. VGPU's compute resolve path skips that, the runtime enforces it at
// ExecuteCommandLists by REMOVING THE DEVICE, and everything after -- including
// the AV at VGPUDX12+0x19340 -- is the casualty.
//
// WHY A CREATE-INTERPOSE AND NOT A ONE-SHOT SWEEP. s8 phase M walked the callers
// of the creating function (VGPUDX12+0x2892C) up to 0x22AC0, which is a
// get-or-create cache -- lookup, and on a MISS create. The EDRAM surfaces are
// therefore made lazily while rendering, so there is no startup moment at which a
// worker could find them all. Interposing the create is the only place that sees
// every one of them, and it has a free ordering guarantee: a resource cannot be
// used before its create returns, so an init submitted before we return is
// submitted before any use of it.
//
// Phase P measured what the init has to be:
//   * ONE Discard per resource is enough -- case 3 used A, then an aliased B, then
//     A again, each after an aliasing barrier, with no re-init and no #1422. So no
//     per-activation tracking is needed.
//   * Submitting from another thread onto the same queue preserves the ordering
//     (case 4), and a separate queue with a fence wait also works (case 5), which
//     is the fallback used here when VGPU's queue cannot be identified.
//   * Phase M measured the initial layout/state as RENDER_TARGET, which already
//     permits DiscardResource, so the barrier sandwich degenerates to a bare
//     Discard for the resource that actually kills the boot.
//
// Keyed on desc.Flags, NEVER on the resource name: phase M found FOUR named placed
// creates in VGPU and a fix hardcoded to one of them would be wrong by construction.
static const unsigned long ZZ_VGPU_QUEUE_OFF = 0x1238;   // phase M: 0x1C0 + 0x1078

// device vtable slots, fixed by the D3D12 headers and confirmed against this
// binary in s6 pt2 (the refused call is `call [r11+0x268]`, 0x268/8 = 77)
#define ZZ_SLOT_CPR   29
#define ZZ_SLOT_CPR2  77
// The device vtable is COPIED, and the copy must be COMPLETE: a truncated copy
// leaves zeroes in the high slots, and any later call through one of them becomes a
// jump to 0. ID3D12Device14 has ~93 slots, so 256 is generous -- and arming is
// REFUSED outright unless all 256 read cleanly, rather than silently truncated.
#define ZZ_VT_SLOTS   256

typedef HRESULT (WINAPI *ZzPfnCPR)(void*, void*, UINT64, const D3D12_RESOURCE_DESC*,
                                   D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                   REFIID, void**);
typedef HRESULT (WINAPI *ZzPfnCPR2)(void*, void*, UINT64, const D3D12_RESOURCE_DESC1*,
                                    D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*,
                                    UINT32, const DXGI_FORMAT*, REFIID, void**);

// 0 = untried, 1 = ARMED, 2 = disabled (no marker), 3 = failed to arm.
// Every settled state is silent, because this is reached from a per-tick path and
// an instrument that reprints one line hundreds of times buries the run's finding.
static volatile LONG      g_fixState;
static volatile LONG      g_fixAnnounced;
static CRITICAL_SECTION   g_fixCs;
static ID3D12CommandQueue*        g_fixQueue;      // VGPU's DIRECT queue, or ours
static int                        g_fixOwnQueue;   // 1 = ours, so we must fence-wait
static ID3D12CommandAllocator*    g_fixAlloc;
static ID3D12GraphicsCommandList* g_fixList;
static ID3D12GraphicsCommandList7* g_fixList7;   // for LAYOUT barriers; may be null
static ID3D12Fence*               g_fixFence;
static UINT64                     g_fixFenceVal;
static int                        g_fixPending;
static void**                     g_fixVt;
static ZzPfnCPR                   g_origCPR;
static ZzPfnCPR2                  g_origCPR2;
static volatile LONG              g_fixDone;       // resources initialized
static volatile LONG              g_fixFailed;

// VGPU's one DIRECT queue (phase M: exactly one CreateCommandQueue in the module,
// desc all zeroes, stored at [g_vgpu+0x1C0+0x1078]). Gated exactly like the device.
static DWORD64 zz_vgpu_queue(void)
{
    HMODULE m = GetModuleHandleA("VGPUDX12.dll");
    if (!m) return 0;
    DWORD64 state = 0, q = 0, vt = 0;
    if (!zz_try_read_u64((DWORD64)m + ZZ_VGPU_STATE_RVA, &state) || !state) return 0;
    if (!zz_try_read_u64(state + ZZ_VGPU_QUEUE_OFF, &q) || !q) return 0;
    if (!zz_try_read_u64(q, &vt) || !vt) return 0;
    if (!zz_is_d3d12_vtable(vt)) return 0;
    return q;
}

// The object's vtable POINTER is replaced, not the shared vtable itself, so only
// this one device object is affected. A single aligned pointer store is atomic, so
// a concurrent call through the old vtable is either fully before or fully after.
__declspec(noinline) static int zz_write_ptr(void* dst, void* val)
{
    __try { *(void**)dst = val; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Everything risky in one guarded body: the allocator reset, the recording, the
// submit and the signal are all virtual calls made FROM the __try body, which is
// the form clang-cl's __except actually catches.
// [[reference_clangcl_try_inline_load]]
// mode: ZZ_MODE_BARE   the resource already sits where Discard is legal
//       ZZ_MODE_LEGACY a legacy state sandwich (created with an InitialState)
//       ZZ_MODE_EB     an enhanced-barrier LAYOUT sandwich (created with an
//                      InitialLayout that does not permit Discard -- s8 pt4 found
//                      VGPU makes RT resources in layout 0 COMMON as well as
//                      layout 2 RENDER_TARGET, and the COMMON ones were the whole
//                      of the remaining #1422)
// discardWhere: the state (legacy) or layout (eb) to sit in for the Discard --
// RENDER_TARGET for an RT resource, DEPTH_WRITE / DEPTH_STENCIL_WRITE for a
// depth-stencil-only one, which cannot legally reach RENDER_TARGET at all.
#define ZZ_MODE_BARE   0
#define ZZ_MODE_LEGACY 1
#define ZZ_MODE_EB     2

__declspec(noinline) static int zz_edram_submit(ID3D12Resource* res, int mode,
                                                unsigned before,
                                                unsigned discardWhere,
                                                int isDepth)
{
    __try
    {
        // fence-wait only before REUSING the allocator, never to make VGPU wait
        if (g_fixPending)
        {
            for (int i = 0; i < 2000 && g_fixFence->GetCompletedValue() < g_fixFenceVal; i++)
                Sleep(1);
            g_fixPending = 0;
        }

        g_fixAlloc->Reset();
        g_fixList->Reset(g_fixAlloc, NULL);

        D3D12_RESOURCE_BARRIER bar;
        D3D12_TEXTURE_BARRIER tb;
        D3D12_BARRIER_GROUP grp;

        if (mode == ZZ_MODE_LEGACY)
        {
            bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bar.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            bar.Transition.pResource = res;
            bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bar.Transition.StateBefore = (D3D12_RESOURCE_STATES)before;
            bar.Transition.StateAfter = (D3D12_RESOURCE_STATES)discardWhere;
            g_fixList->ResourceBarrier(1, &bar);
        }
        else if (mode == ZZ_MODE_EB)
        {
            // built field by field, no aggregate init, so nothing drags C++
            // unwinding into a function that has to carry __try
            tb.SyncBefore = D3D12_BARRIER_SYNC_NONE;
            tb.SyncAfter = isDepth ? D3D12_BARRIER_SYNC_DEPTH_STENCIL
                                   : D3D12_BARRIER_SYNC_RENDER_TARGET;
            tb.AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
            tb.AccessAfter = isDepth ? D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE
                                     : D3D12_BARRIER_ACCESS_RENDER_TARGET;
            tb.LayoutBefore = (D3D12_BARRIER_LAYOUT)before;
            tb.LayoutAfter = (D3D12_BARRIER_LAYOUT)discardWhere;
            tb.pResource = res;
            tb.Subresources.IndexOrFirstMipLevel = 0xffffffff;   // all subresources
            tb.Subresources.NumMipLevels = 0;
            tb.Subresources.FirstArraySlice = 0;
            tb.Subresources.NumArraySlices = 0;
            tb.Subresources.FirstPlane = 0;
            tb.Subresources.NumPlanes = 0;
            tb.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

            grp.Type = D3D12_BARRIER_TYPE_TEXTURE;
            grp.NumBarriers = 1;
            grp.pTextureBarriers = &tb;
            g_fixList7->Barrier(1, &grp);
        }

        g_fixList->DiscardResource(res, NULL);

        if (mode == ZZ_MODE_LEGACY)
        {
            bar.Transition.StateBefore = (D3D12_RESOURCE_STATES)discardWhere;
            bar.Transition.StateAfter = (D3D12_RESOURCE_STATES)before;
            g_fixList->ResourceBarrier(1, &bar);
        }
        else if (mode == ZZ_MODE_EB)
        {
            // and put the layout back, so VGPU's own tracking is undisturbed
            tb.SyncBefore = tb.SyncAfter;
            tb.SyncAfter = D3D12_BARRIER_SYNC_NONE;
            tb.AccessBefore = tb.AccessAfter;
            tb.AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS;
            tb.LayoutBefore = (D3D12_BARRIER_LAYOUT)discardWhere;
            tb.LayoutAfter = (D3D12_BARRIER_LAYOUT)before;
            g_fixList7->Barrier(1, &grp);
        }

        g_fixList->Close();

        ID3D12CommandList* lists[1];
        lists[0] = g_fixList;
        g_fixQueue->ExecuteCommandLists(1, lists);
        g_fixFenceVal = g_fixFenceVal + 1;
        g_fixQueue->Signal(g_fixFence, g_fixFenceVal);
        g_fixPending = 1;

        // On OUR OWN queue there is no shared submission order with VGPU, so the
        // init has to be COMPLETE before we return (phase P case 5). On VGPU's own
        // queue the ordering is free and nothing is waited on.
        if (g_fixOwnQueue)
        {
            for (int i = 0; i < 2000 && g_fixFence->GetCompletedValue() < g_fixFenceVal; i++)
                Sleep(1);
            g_fixPending = 0;
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Discard is only legal from RENDER_TARGET / DEPTH_WRITE / UNORDERED_ACCESS. When
// the resource is already in one of those (which phase M says the EDRAM alias is),
// no barrier is needed at all.
static int zz_state_allows_discard(unsigned s)
{
    return (s & (D3D12_RESOURCE_STATE_RENDER_TARGET |
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                 D3D12_RESOURCE_STATE_DEPTH_WRITE)) != 0;
}

static int zz_layout_allows_discard(unsigned l)
{
    return l == (unsigned)D3D12_BARRIER_LAYOUT_RENDER_TARGET ||
           l == (unsigned)D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS ||
           l == (unsigned)D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
}

// isLayout: 0 = sl is a D3D12_RESOURCE_STATES, 1 = sl is a D3D12_BARRIER_LAYOUT.
static void zz_edram_init(void* res, unsigned flags, unsigned sl, int isLayout)
{
    if (g_fixState != 1 || !res) return;

    const int direct = isLayout ? zz_layout_allows_discard(sl)
                                : zz_state_allows_discard(sl);
    const int isDepth = (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? 0 : 1;

    int mode = ZZ_MODE_BARE;
    if (!direct) mode = isLayout ? ZZ_MODE_EB : ZZ_MODE_LEGACY;

    // ⚠ s8 pt4: this branch used to be an unconditional SKIP for the enhanced-barrier
    // case, and the launch proved it was the whole of the remaining bug -- VGPU makes
    // RT resources in layout 0 COMMON as well as layout 2 RENDER_TARGET, and the one
    // resource still tripping #1422 was, byte for byte, one of the three this had
    // skipped. It now runs a LAYOUT sandwich instead, and only genuinely gives up
    // when the runtime has no ID3D12GraphicsCommandList7 to issue one with -- which
    // it says out loud, because a silent skip looks exactly like a fix that worked.
    if (mode == ZZ_MODE_EB && !g_fixList7)
    {
        if (InterlockedIncrement(&g_fixFailed) <= 8)
        {
            ZzBuf b; b.n = 0;
            b_str(&b, "ZZFIX edram-init: SKIPPED 0x");
            b_hex(&b, (unsigned long long)(DWORD64)res, 16);
            b_str(&b, " -- layout ");
            b_dec(&b, sl);
            b_str(&b, " needs a layout barrier and there is no "
                      "ID3D12GraphicsCommandList7\r\n");
            b_flush(&b);
        }
        return;
    }

    // an RT resource is discarded from RENDER_TARGET; a DEPTH-STENCIL-ONLY one
    // cannot legally reach that state at all, so it goes through DEPTH_WRITE
    unsigned discardWhere;
    if (isLayout)
        discardWhere = isDepth ? (unsigned)D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE
                               : (unsigned)D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    else
        discardWhere = isDepth ? (unsigned)D3D12_RESOURCE_STATE_DEPTH_WRITE
                               : (unsigned)D3D12_RESOURCE_STATE_RENDER_TARGET;

    EnterCriticalSection(&g_fixCs);
    const int ok = zz_edram_submit((ID3D12Resource*)res, mode, sl, discardWhere, isDepth);
    LeaveCriticalSection(&g_fixCs);

    const LONG n = InterlockedIncrement(&g_fixDone);
    if (n <= 32)                       // one line per resource, capped: no floods
    {
        ZzBuf b; b.n = 0;
        b_str(&b, ok ? "ZZFIX edram-init: discarded 0x" : "ZZFIX edram-init: FAULTED 0x");
        b_hex(&b, (unsigned long long)(DWORD64)res, 16);
        b_str(&b, "  flags=0x");
        b_hex(&b, flags, 2);
        b_str(&b, isLayout ? "  layout=" : "  state=0x");
        if (isLayout) b_dec(&b, sl); else b_hex(&b, sl, 4);
        b_str(&b, mode == ZZ_MODE_BARE   ? "  (bare Discard)"
                : mode == ZZ_MODE_EB     ? "  (layout sandwich)"
                                         : "  (state sandwich)");
        b_str(&b, "\r\n");
        b_flush(&b);
    }
}

#define ZZ_RTDS (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | \
                 D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)

// --- the SECOND fix: legalize the cast VGPU wants (s9, opt-in: zz_cast_fix.on) --
//
// THE BUG, named by the debug layer in s8 pt5 and replicated in s8 pt7:
//
//   D3D12 ERROR: ID3D12Device::CreateUnorderedAccessView: The Format (0x11,
//   R32G32_UINT) is invalid ... not a fully qualified Format castable from the
//   Format of the Resource (0x9, R16G16B16A16_TYPELESS).
//   [ STATE_CREATION ERROR #342 ]
//
// s9 phase P (test\d3d12_probe7.cpp) then measured the two things that matter and
// were only conjecture before:
//
//   * case 1, layer OFF: that CreateUnorderedAccessView call REMOVES THE DEVICE
//     by itself -- GetDeviceRemovedReason = 0x887A0001 DXGI_ERROR_INVALID_CALL,
//     with no submit, no dispatch and no ExecuteCommandLists anywhere near it.
//     So the retail 0x19340 death is not a DEFERRED consequence of #342: the
//     device is already dead when VGPU makes its next create, and that create's
//     failure is what walks into VGPU's no-error-path.
//   * case 3: `pCastableFormats = { R32G32_UINT }` on CreatePlacedResource2 makes
//     the identical CreateUAV succeed, layer on and off, and the dispatch through
//     it runs clean. Case 4: the castable list does NOT change the allocation size
//     or alignment, so every placed offset VGPU has already computed stays valid.
//
// s9 phase M placed the resource inside our existing interpose: the EDRAM surface
// is a PLACED texture created through CreatePlacedResource2 (device slot 77, which
// s8 measured VGPU choosing at run time), its format comes from VGPU's EDRAM group
// table at rva 0x4E3810 whose entry 55 is exactly DXGI_FORMAT 9, and the UAV is
// built by the helper at VGPUDX12+0x76750 from a format held in the surface object.
// VGPU passes NumCastableFormats = 0, pCastableFormats = NULL.
//
// So the fix is to APPEND, never replace: whatever VGPU someday supplies is passed
// through and ours is added after it.
static volatile LONG g_castOn;          // 1 = marker present
static volatile LONG g_castAnnounced;
static volatile LONG g_castDone;
static volatile LONG g_castSkipped;

// 8 formats for the one mapped resource, plus room for whatever VGPU supplies
// itself, plus slack. Every write is bounded; a list that would overflow is
// truncated rather than smeared across the stack.
#define ZZ_CAST_MAX 24

// ⚠ s9 pt2 -- THE RULE THE FIRST VERSION OF THIS GOT WRONG, and the launch found
// it in one run. An explicit `pCastableFormats` list does not EXTEND the default
// cast legality, it REPLACES it. With `{R32G32_UINT}` alone on the list, VGPU's
// very next view of the same resource -- R16G16B16A16_FLOAT, which had been legal
// for free because it is a fully-typed member of the resource's OWN typeless
// family -- was refused, and removed the device:
//
//   D3D12 ERROR #342: CreateUnorderedAccessView: The view format (0xa,
//   R16G16B16A16_FLOAT) was not specified in the list of castable formats. The
//   resource ...:'HostTexture-DX12TextureTranslateDesc' was created with an
//   explicit format cast list.
//
// Reproduced standalone afterwards in probe7 case 5, message for message. So the
// list must name EVERY format the resource will ever be viewed as: the whole
// R16G16B16A16 family that was free before, plus the 64-bpp reinterpretations that
// are the reason for doing this at all. probe7 case 6 proved the create accepts all
// eight for both resource-flag combinations the launches logged (RT|UAV and RT),
// that UAVs in both 0x11 and 0x0A then succeed, that an SRV succeeds, and that the
// allocation size and alignment are unchanged -- so VGPU's placed offsets stay valid.
//
// Fills `out` and returns the count; 0 means "not a format we map", and such a
// create is passed through untouched, which is exactly the old behaviour.
static UINT32 zz_cast_list_for(DXGI_FORMAT f, DXGI_FORMAT* out, UINT32 cap)
{
    if (f != DXGI_FORMAT_R16G16B16A16_TYPELESS) return 0;
    UINT32 n = 0;
    if (n < cap) out[n++] = DXGI_FORMAT_R16G16B16A16_FLOAT;   // 0x0A the launch's
    if (n < cap) out[n++] = DXGI_FORMAT_R16G16B16A16_UNORM;   // 0x0B
    if (n < cap) out[n++] = DXGI_FORMAT_R16G16B16A16_UINT;    // 0x0C
    if (n < cap) out[n++] = DXGI_FORMAT_R16G16B16A16_SNORM;   // 0x0D
    if (n < cap) out[n++] = DXGI_FORMAT_R16G16B16A16_SINT;    // 0x0E
    if (n < cap) out[n++] = DXGI_FORMAT_R32G32_FLOAT;         // 0x10
    if (n < cap) out[n++] = DXGI_FORMAT_R32G32_UINT;          // 0x11 the 64-bpp cast
    if (n < cap) out[n++] = DXGI_FORMAT_R32G32_SINT;          // 0x12
    return n;
}

// Test seam for the fixture, called by nothing in Emu. A map that is wrong in
// either direction produces a silent run -- either a cast is not legalized (and the
// device dies exactly as before) or an unrelated create is handed a castable list it
// never needed, which under the replace-not-extend rule STRIPS the casts it used to
// have for free. Both directions are machine-checked.
extern "C" unsigned zz_selftest_cast(unsigned fmt)
{
    DXGI_FORMAT list[ZZ_CAST_MAX];
    const UINT32 n = zz_cast_list_for((DXGI_FORMAT)fmt, list, ZZ_CAST_MAX);
    ZzBuf b; b.n = 0;
    b_str(&b, "ZZCASTMAP 0x");
    b_hex(&b, fmt, 2);
    b_str(&b, " -> ");
    if (!n) b_str(&b, "none");
    for (UINT32 i = 0; i < n; ++i)
    {
        if (i) b_str(&b, " ");
        b_str(&b, "0x");
        b_hex(&b, (unsigned)list[i], 2);
    }
    b_str(&b, "\r\n");
    b_flush(&b);
    return n;
}

// --- the THIRD fix: clamp an over-declared Map range (s9 pt3, zz_map_clamp.on) --
//
// THE BUG, read out of the binary after the s9 pt2 launch got past 18.2 s for the
// first time and died at 29.4 s on the ONE remaining error:
//
//   D3D12 ERROR #828 MAP_INVALIDRANGE: ID3D12Resource::Map: pReadRange is not
//   NULL, not empty, and specifies an area past the subresource extent.
//   Begin is 0, End is 6594560, and subresource 0 has only 6451200 bytes.
//
// The whole chain, static, from VGPUDX12+0x77D8C (the CPU-resolve download path):
//
//   +0782EF  lea   r9, [rbp+0x40]         ; ppData
//   +0782F3  mov   qword [rbp+0x40], 0    ; *ppData = 0, pre-zeroed
//   +0782FB  lea   r8, [rbp+0x98]         ; pReadRange
//   +078302  mov   qword [rbp+0x98], 0    ; Begin = 0
//   +078317  mov   qword [rbp+0xa0], rax  ; End = [struct+0x14] -- the DESTINATION
//                                         ;   surface's SLICE PITCH, not this
//                                         ;   buffer's size. That is the bug.
//   +078328  call  qword [rax+0x40]       ; ID3D12Resource::Map (slot 8)
//   +078357  mov   rax, qword [rbp+0x40]  ; the pointer -- HRESULT NEVER CHECKED
//   +078376  call  r11                    ; -> the copy loop at 0x328C14, arg6 = rax
//
// Map fails, D3D12 leaves *ppData untouched, and the still-zero pointer is handed
// to the 64-bpp untile loop at 0x328C14 as its SOURCE. It faults on the first read:
//
//   +328C60  lea   rcx, [r8+4]            ; r8 = 0 -> rcx = 4
//   +328C73  movzx eax, word [rcx-4]      ; READ of address 0   <-- the AV
//
// Same class as s5 pt6: a failed call falls unconditionally into the success path.
//
// WHY CLAMPING IS LOSSLESS, and not a guess. The copy loop's own counters say what
// it actually reads: arg1 = 0x460 = 1120 elements of 8 bytes = 8960 bytes a row,
// arg2 = 0x2D0 = 720 rows, arg3 = 1 slice, source row stride r15 = 0x2300 = 8960.
// 720 x 8960 = 6,451,200 -- EXACTLY the buffer's size. The 16 extra rows in the
// declared range (6,594,560 / 8960 = 736 = align(720,32)) are never touched. The
// range is over-DECLARED, not over-read, so clamping End to the subresource size
// removes an invalid argument and changes nothing that is read.
//
// WHY HERE AND NOT ELSEWHERE. The buffer is `CpuResolveScratchDownload` (name at
// rva 0x478C88, used at +078072), created at +0780A9 through the heap-table thunk
// 0x182C8, which tail-jumps to the heap object's vtable slot 2 = VGPUDX12+0x19214,
// which calls **CreatePlacedResource2** at +0x1932D. So it is a PLACED BUFFER and
// it already passes through the slot-77 thunk this file owns -- no new device slot
// is needed. Fixing VGPU's size computation would mean patching VGPUDX12 itself;
// enlarging the buffer at create time would mean guessing an alignment that is not
// visible at create time. Clamping at the API boundary is the only route that is
// both precise and provably lossless.
static volatile LONG g_mapOn;
static volatile LONG g_mapAnnounced;
static volatile LONG g_mapArmed;
static volatile LONG g_mapClamped;
static volatile LONG g_mapArmFail;

// ID3D12Resource3 has ~18 slots; 128 is a wide margin, and the copy REFUSES to arm
// unless all 128 read cleanly -- a partial copy would leave a jump to 0 in the high
// slots, the same rule the device vtable copy has followed since s8.
#define ZZ_RES_VT   128
#define ZZ_RES_MAX  256
#define ZZ_SLOT_MAP 8

typedef HRESULT (WINAPI *ZzPfnMap)(void*, UINT, const D3D12_RANGE*, void**);

// The vtable copy lives INSIDE the record, so the record is reachable from the
// object's own vtable pointer by subtraction -- no table, no lock, and no way to
// read a stale entry after a resource is freed and its address reused.
typedef struct ZzResHook
{
    UINT64   size;            // subresource 0 size (a buffer's Width)
    ZzPfnMap orig;
    void*    vt[ZZ_RES_VT];
} ZzResHook;

static ZzResHook g_resHooks[ZZ_RES_MAX];

// Pure, and therefore testable without a D3D12 object at all. Returns 1 when the
// range had to be clamped. Only an END past the extent is out of contract; every
// other range is passed through byte for byte.
static int zz_clamp_range(UINT64 size, UINT64 inBegin, UINT64 inEnd,
                          UINT64* outBegin, UINT64* outEnd)
{
    if (!size || inEnd <= size) return 0;
    *outEnd = size;
    *outBegin = (inBegin > size) ? size : inBegin;
    return 1;
}

// Test seam for the fixture, called by nothing in Emu.
extern "C" unsigned zz_selftest_map_clamp(unsigned long long size,
                                          unsigned long long begin,
                                          unsigned long long end)
{
    UINT64 b2 = 0, e2 = 0;
    const int did = zz_clamp_range(size, begin, end, &b2, &e2);
    ZzBuf b; b.n = 0;
    b_str(&b, "ZZMAPCLAMP size=");
    b_dec(&b, size);
    b_str(&b, " [");
    b_dec(&b, begin);
    b_str(&b, ",");
    b_dec(&b, end);
    b_str(&b, "] -> ");
    if (did)
    {
        b_str(&b, "[");
        b_dec(&b, b2);
        b_str(&b, ",");
        b_dec(&b, e2);
        b_str(&b, "]\r\n");
    }
    else
    {
        b_str(&b, "untouched\r\n");
    }
    b_flush(&b);
    return (unsigned)did;
}

static ZzResHook* zz_hook_of(void* self)
{
    DWORD64 vt = 0;
    if (!zz_try_read_u64((DWORD64)self, &vt) || !vt) return NULL;
    const DWORD64 base = (DWORD64)&g_resHooks[0];
    const DWORD64 lim  = base + sizeof(g_resHooks);
    const DWORD64 off  = (DWORD64)((char*)&g_resHooks[0].vt[0] - (char*)&g_resHooks[0]);
    if (vt < base + off) return NULL;
    const DWORD64 rec = vt - off;
    if (rec < base || rec >= lim) return NULL;
    if (((rec - base) % sizeof(ZzResHook)) != 0) return NULL;
    return (ZzResHook*)rec;
}

static HRESULT WINAPI zz_thunk_map(void* self, UINT sub, const D3D12_RANGE* rr,
                                   void** ppData)
{
    ZzResHook* h = zz_hook_of(self);
    if (!h || !h->orig) return E_FAIL;      // unreachable: we got here through h->vt

    D3D12_RANGE clamped;
    const D3D12_RANGE* use = rr;
    if (rr)
    {
        UINT64 nb = 0, ne = 0;
        if (zz_clamp_range(h->size, (UINT64)rr->Begin, (UINT64)rr->End, &nb, &ne))
        {
            clamped.Begin = (SIZE_T)nb;
            clamped.End   = (SIZE_T)ne;
            use = &clamped;
            if (InterlockedIncrement(&g_mapClamped) <= 16)
            {
                ZzBuf b; b.n = 0;
                b_str(&b, "ZZMAP clamp: 0x");
                b_hex(&b, (unsigned long long)(DWORD64)self, 16);
                b_str(&b, " sub ");
                b_dec(&b, sub);
                b_str(&b, " read range [");
                b_dec(&b, (unsigned long long)rr->Begin);
                b_str(&b, ",");
                b_dec(&b, (unsigned long long)rr->End);
                b_str(&b, "] past the ");
                b_dec(&b, h->size);
                b_str(&b, "-byte extent -> [");
                b_dec(&b, nb);
                b_str(&b, ",");
                b_dec(&b, ne);
                b_str(&b, "]\r\n");
                b_flush(&b);
            }
        }
    }
    return h->orig(self, sub, use, ppData);
}

// Called from the create thunks, on a BUFFER only: Map is the only call this
// touches and a texture with an UNKNOWN layout cannot legally be mapped anyway.
// Per-object, so no other resource is affected; a single aligned pointer store, so
// a concurrent call is either fully before it or fully after.
__declspec(noinline) static void zz_map_arm(void* res, UINT64 size)
{
    if (!g_mapOn || !res || !size) return;

    const LONG idx = InterlockedIncrement(&g_mapArmed) - 1;
    if (idx >= ZZ_RES_MAX)
    {
        if (idx == ZZ_RES_MAX)          // say it once, then never again
        {
            ZzBuf b; b.n = 0;
            b_str(&b, "ZZMAP arm: pool of ");
            b_dec(&b, (unsigned long long)ZZ_RES_MAX);
            b_str(&b, " exhausted -- later buffers are NOT clamped (said once)\r\n");
            b_flush(&b);
        }
        return;
    }

    ZzResHook* h = &g_resHooks[idx];
    DWORD64 vt = 0;
    if (!zz_try_read_u64((DWORD64)res, &vt) || !vt)
    {
        InterlockedIncrement(&g_mapArmFail);
        return;
    }

    int n = 0;
    for (; n < ZZ_RES_VT; n++)
    {
        DWORD64 s = 0;
        if (!zz_try_read_u64(vt + (DWORD64)n * 8, &s)) break;
        h->vt[n] = (void*)s;
    }
    if (n != ZZ_RES_VT)
    {
        if (InterlockedIncrement(&g_mapArmFail) <= 4)
        {
            ZzBuf b; b.n = 0;
            b_str(&b, "ZZMAP arm FAILED: only ");
            b_dec(&b, (unsigned long long)n);
            b_str(&b, " readable vtable slots on 0x");
            b_hex(&b, (unsigned long long)(DWORD64)res, 16);
            b_str(&b, " (a partial copy would leave a jump to 0)\r\n");
            b_flush(&b);
        }
        return;
    }

    h->size = size;
    h->orig = (ZzPfnMap)h->vt[ZZ_SLOT_MAP];
    h->vt[ZZ_SLOT_MAP] = (void*)&zz_thunk_map;

    if (!zz_write_ptr(res, (void*)&h->vt[0]))
    {
        if (InterlockedIncrement(&g_mapArmFail) <= 4)
        {
            ZzBuf b; b.n = 0;
            b_str(&b, "ZZMAP arm FAILED: the resource vtable pointer is not writable\r\n");
            b_flush(&b);
        }
        return;
    }

    if (idx < 8)                        // one line for the first few, then silence
    {
        ZzBuf b; b.n = 0;
        b_str(&b, "ZZMAP armed on buffer 0x");
        b_hex(&b, (unsigned long long)(DWORD64)res, 16);
        b_str(&b, "  size ");
        b_dec(&b, size);
        b_str(&b, "  vtable ");
        b_addr(&b, vt);
        b_str(&b, "\r\n");
        b_flush(&b);
    }
}

static void zz_map_announce(void)
{
    if (InterlockedCompareExchange(&g_mapAnnounced, 1, 0) != 0) return;
    ZzBuf b; b.n = 0;
    if (zz_marker("zz_map_clamp.on"))
    {
        InterlockedExchange(&g_mapOn, 1);
        b_str(&b, "ZZMAP marker zz_map_clamp.on present -- will clamp over-declared "
                  "Map read ranges on placed buffers (needs zz_edram_init.on for "
                  "the interpose)\r\n");
    }
    else
    {
        b_str(&b, "ZZMAP marker zz_map_clamp.on absent, clamp not armed "
                  "(said once)\r\n");
    }
    b_flush(&b);
}

static HRESULT WINAPI zz_thunk_cpr(void* self, void* heap, UINT64 off,
                                   const D3D12_RESOURCE_DESC* desc,
                                   D3D12_RESOURCE_STATES state,
                                   const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv)
{
    // The legacy call has no castable-formats parameter at all, so a create that
    // needs one cannot be fixed here -- it would have to be upgraded to
    // CreatePlacedResource2. Phase M measured VGPU taking the slot-77 branch on
    // this machine, so this is expected never to fire; it SAYS SO rather than
    // passing silently, because a silent skip looks exactly like a fix that worked.
    DXGI_FORMAT probe[ZZ_CAST_MAX];
    if (g_castOn && desc && zz_cast_list_for(desc->Format, probe, ZZ_CAST_MAX) != 0 &&
        InterlockedIncrement(&g_castSkipped) <= 8)
    {
        ZzBuf b; b.n = 0;
        b_str(&b, "ZZFIX cast: NOT FIXED -- fmt 0x");
        b_hex(&b, (unsigned)desc->Format, 2);
        b_str(&b, " arrived on CreatePlacedResource (slot 29), which has no "
                  "castable-formats parameter\r\n");
        b_flush(&b);
    }

    const HRESULT hr = g_origCPR(self, heap, off, desc, state, cv, riid, ppv);
    if (hr >= 0 && ppv && *ppv && desc && (desc->Flags & ZZ_RTDS))
        zz_edram_init(*ppv, (unsigned)desc->Flags, (unsigned)state, 0);
    if (hr >= 0 && ppv && *ppv && desc &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        zz_map_arm(*ppv, desc->Width);   // a buffer's subresource 0 size IS Width
    return hr;
}

static HRESULT WINAPI zz_thunk_cpr2(void* self, void* heap, UINT64 off,
                                    const D3D12_RESOURCE_DESC1* desc,
                                    D3D12_BARRIER_LAYOUT layout,
                                    const D3D12_CLEAR_VALUE* cv, UINT32 n,
                                    const DXGI_FORMAT* fmts, REFIID riid, void** ppv)
{
    HRESULT hr;
    DXGI_FORMAT mine[ZZ_CAST_MAX];
    UINT32 mineN = 0;
    if (g_castOn && desc) mineN = zz_cast_list_for(desc->Format, mine, ZZ_CAST_MAX);

    if (mineN == 0)
    {
        hr = g_origCPR2(self, heap, off, desc, layout, cv, n, fmts, riid, ppv);
    }
    else
    {
        // Pass through anything VGPU supplied (today: nothing), then append every
        // format of ours it has not already named. Appending rather than replacing
        // matters twice over now: the list REPLACES the default legality, so
        // dropping one of VGPU's own entries would take a cast away from it.
        DXGI_FORMAT merged[ZZ_CAST_MAX];
        UINT32 m = 0;
        if (fmts)
        {
            for (UINT32 i = 0; i < n && m < ZZ_CAST_MAX; ++i)
                merged[m++] = fmts[i];
        }
        for (UINT32 j = 0; j < mineN && m < ZZ_CAST_MAX; ++j)
        {
            int already = 0;
            for (UINT32 k = 0; k < m; ++k)
                if (merged[k] == mine[j]) { already = 1; break; }
            if (!already) merged[m++] = mine[j];
        }

        hr = g_origCPR2(self, heap, off, desc, layout, cv, m, merged, riid, ppv);

        // If the castable list is what broke a create that used to work, put it
        // back the way VGPU asked for it rather than turning a working boot into a
        // failing one -- and say which happened.
        if (hr < 0)
        {
            const HRESULT first = hr;
            hr = g_origCPR2(self, heap, off, desc, layout, cv, n, fmts, riid, ppv);
            if (InterlockedIncrement(&g_castSkipped) <= 8)
            {
                ZzBuf b; b.n = 0;
                b_str(&b, "ZZFIX cast: REFUSED fmt 0x");
                b_hex(&b, (unsigned)desc->Format, 2);
                b_str(&b, " +castable ");
                b_dec(&b, m);
                b_str(&b, " fmts, hr=0x");
                b_hex(&b, (unsigned long long)(unsigned)first, 8);
                b_str(&b, " -- retried without the list, hr=0x");
                b_hex(&b, (unsigned long long)(unsigned)hr, 8);
                b_str(&b, "\r\n");
                b_flush(&b);
            }
        }
        else if (InterlockedIncrement(&g_castDone) <= 32)
        {
            ZzBuf b; b.n = 0;
            b_str(&b, "ZZFIX cast: fmt 0x");
            b_hex(&b, (unsigned)desc->Format, 2);
            b_str(&b, " +castable ");
            b_dec(&b, m);
            b_str(&b, " (");
            for (UINT32 i = 0; i < m; ++i)
            {
                if (i) b_str(&b, " ");
                b_str(&b, "0x");
                b_hex(&b, (unsigned)merged[i], 2);
            }
            b_str(&b, ") at 0x");
            b_hex(&b, (unsigned long long)(DWORD64)(ppv ? *ppv : 0), 16);
            b_str(&b, "\r\n");
            b_flush(&b);
        }
    }

    if (hr >= 0 && ppv && *ppv && desc && (desc->Flags & ZZ_RTDS))
        zz_edram_init(*ppv, (unsigned)desc->Flags, (unsigned)layout, 1);
    if (hr >= 0 && ppv && *ppv && desc &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        zz_map_arm(*ppv, desc->Width);   // a buffer's subresource 0 size IS Width
    return hr;
}

__declspec(noinline) static int zz_edram_build(DWORD64 dev)
{
    ID3D12Device* d = (ID3D12Device*)dev;
    __try
    {
        if (FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             __uuidof(ID3D12CommandAllocator),
                                             (void**)&g_fixAlloc)) || !g_fixAlloc)
            return 0;
        if (FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_fixAlloc,
                                        NULL, __uuidof(ID3D12GraphicsCommandList),
                                        (void**)&g_fixList)) || !g_fixList)
            return 0;
        g_fixList->Close();

        // optional: only needed for resources whose InitialLayout forbids Discard
        if (FAILED(g_fixList->QueryInterface(__uuidof(ID3D12GraphicsCommandList7),
                                             (void**)&g_fixList7)))
            g_fixList7 = NULL;

        if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                  (void**)&g_fixFence)) || !g_fixFence)
            return 0;

        const DWORD64 q = zz_vgpu_queue();
        if (q)
        {
            g_fixQueue = (ID3D12CommandQueue*)q;
            g_fixOwnQueue = 0;
        }
        else
        {
            D3D12_COMMAND_QUEUE_DESC qd;
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            qd.Priority = 0;
            qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            qd.NodeMask = 0;
            if (FAILED(d->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
                                             (void**)&g_fixQueue)) || !g_fixQueue)
                return 0;
            g_fixOwnQueue = 1;
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Called ONCE, from the first sighting of the device. Everything is built before
// the vtable pointer is swapped, so a thunk can never run against half-built state.
static void zz_edram_arm(DWORD64 dev)
{
    if (!dev) return;
    if (g_fixState != 0) return;

    ZzBuf b;
    DWORD64 vt = 0;
    if (!zz_try_read_u64(dev, &vt) || !vt) { InterlockedExchange(&g_fixState, 3); return; }

    void** copy = (void**)VirtualAlloc(NULL, ZZ_VT_SLOTS * 8,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!copy)
    {
        InterlockedExchange(&g_fixState, 3);
        b.n = 0; b_str(&b, "ZZFIX arm FAILED: no memory for the vtable copy\r\n"); b_flush(&b);
        return;
    }

    int n = 0;
    for (; n < ZZ_VT_SLOTS; n++)
    {
        DWORD64 s = 0;
        if (!zz_try_read_u64(vt + (DWORD64)n * 8, &s)) break;
        copy[n] = (void*)s;
    }
    if (n != ZZ_VT_SLOTS)
    {
        InterlockedExchange(&g_fixState, 3);
        b.n = 0;
        b_str(&b, "ZZFIX arm FAILED: only ");
        b_dec(&b, (unsigned long long)n);
        b_str(&b, " readable vtable slots, need all ");
        b_dec(&b, (unsigned long long)ZZ_VT_SLOTS);
        b_str(&b, " (a partial copy would leave a jump to 0 in the high slots)\r\n");
        b_flush(&b);
        return;
    }

    g_origCPR  = (ZzPfnCPR)copy[ZZ_SLOT_CPR];
    g_origCPR2 = (ZzPfnCPR2)copy[ZZ_SLOT_CPR2];
    copy[ZZ_SLOT_CPR]  = (void*)&zz_thunk_cpr;
    copy[ZZ_SLOT_CPR2] = (void*)&zz_thunk_cpr2;
    g_fixVt = copy;

    if (!zz_edram_build(dev))
    {
        InterlockedExchange(&g_fixState, 3);
        b.n = 0; b_str(&b, "ZZFIX arm FAILED: could not build the init command list\r\n");
        b_flush(&b);
        return;
    }

    InitializeCriticalSection(&g_fixCs);
    InterlockedExchange(&g_fixState, 1);      // thunks are live from the swap on

    if (!zz_write_ptr((void*)dev, (void*)copy))
    {
        InterlockedExchange(&g_fixState, 3);
        b.n = 0; b_str(&b, "ZZFIX arm FAILED: the device vtable pointer is not writable\r\n");
        b_flush(&b);
        return;
    }

    b.n = 0;
    b_str(&b, "ZZFIX armed on device 0x");
    b_hex(&b, dev, 16);
    b_str(&b, "  vtable ");
    b_addr(&b, vt);
    b_str(&b, " -> copy 0x");
    b_hex(&b, (unsigned long long)(DWORD64)copy, 16);
    b_str(&b, " (");
    b_dec(&b, (unsigned long long)n);
    b_str(&b, " slots), CreatePlacedResource + CreatePlacedResource2 interposed, cl7=");
    b_str(&b, g_fixList7 ? "yes" : "NO");
    b_str(&b, ", init on ");
    b_str(&b, g_fixOwnQueue ? "OUR OWN queue (fence-waited)\r\n"
                            : "VGPU's DIRECT queue\r\n");
    b_flush(&b);
}

// One line per run, said from the static initialiser, so the inert case is
// observable rather than indistinguishable from a build that lacks the fix.
static void zz_edram_announce(void)
{
    if (InterlockedCompareExchange(&g_fixAnnounced, 1, 0) != 0) return;
    ZzBuf b; b.n = 0;
    if (zz_marker("zz_edram_init.on"))
    {
        b_str(&b, "ZZFIX marker zz_edram_init.on present -- will interpose "
                  "CreatePlacedResource/2 at first device sighting\r\n");
    }
    else
    {
        InterlockedExchange(&g_fixState, 2);
        b_str(&b, "ZZFIX marker zz_edram_init.on absent, fix not armed (said once)\r\n");
    }
    b_flush(&b);
}

// The cast fix has its OWN marker so the two fixes can be toggled independently --
// but it rides on the same interpose, so it can only act when the init fix armed
// it. Both markers are expected present for a normal run.
static void zz_cast_announce(void)
{
    if (InterlockedCompareExchange(&g_castAnnounced, 1, 0) != 0) return;
    ZzBuf b; b.n = 0;
    if (zz_marker("zz_cast_fix.on"))
    {
        InterlockedExchange(&g_castOn, 1);
        b_str(&b, "ZZCAST marker zz_cast_fix.on present -- will append castable "
                  "formats to CreatePlacedResource2 (needs zz_edram_init.on for "
                  "the interpose)\r\n");
    }
    else
    {
        b_str(&b, "ZZCAST marker zz_cast_fix.on absent, cast fix not armed "
                  "(said once)\r\n");
    }
    b_flush(&b);
}

// --- the poll --------------------------------------------------------------
static volatile LONG g_devSeen;
static volatile LONG g_devDead;
static unsigned long long g_devFirstMs;

static void zz_device_tick(void)
{
    if (g_devDead) return;

    const DWORD64 dev = zz_vgpu_device();
    if (!dev)
    {
        // Not up yet -- and while it is not up, DRED can still be armed.
        zz_dred_enable("worker");
        return;
    }

    ZzBuf b;
    if (InterlockedCompareExchange(&g_devSeen, 1, 0) == 0)
    {
        g_devFirstMs = GetTickCount64();
        b.n = 0;
        b_str(&b, "ZZDEV VGPU device found at 0x");
        b_hex(&b, dev, 16);
        b_str(&b, "  t=");
        b_dec(&b, g_devFirstMs);
        b_str(&b, " ms\r\n");
        b_flush(&b);

        // First sight of the device is the earliest point WE control, and the
        // queue has to be widened before 18 s of boot chatter fills it.
        zz_infoqueue_arm(dev);

        // s8: and it is also the earliest point at which the create-interpose can
        // go in. The EDRAM surfaces are made lazily, ~18 s into the boot, so a
        // hook installed within the first worker tick is far ahead of them.
        zz_edram_arm(dev);
    }

    HRESULT hr = 0;
    if (!zz_device_removed_reason(dev, &hr)) return;
    if (hr == 0) return;                       // S_OK: still alive

    InterlockedExchange(&g_devDead, 1);
    const unsigned long long now = GetTickCount64();
    b.n = 0;
    b_str(&b, "ZZDEV *** DEVICE REMOVED *** reason=0x");
    b_hex(&b, (unsigned long long)(unsigned)hr, 8);
    b_str(&b, "  t=");
    b_dec(&b, now);
    b_str(&b, " ms  (");
    b_dec(&b, now - g_devFirstMs);
    b_str(&b, " ms after it appeared; the poll is every 250 ms, so the kill is in "
              "that window)\r\n");
    b_flush(&b);

    zz_dred_dump(dev);
    zz_infoqueue_dump(dev);
}

// s9 pt5: every frame of every walk gets its GUEST function name. Defined
// below with the sampler's resolver; declared here because zz_walk is older
// than both and every stack this file prints -- fault records included --
// is worth naming, not just the sampler's.
static int  zz_guest_for_host(unsigned long long rip, unsigned* guestAddr,
                              unsigned* delta);
static void zz_b_guest(ZzBuf* b, unsigned long long pc);

static void zz_walk(ZzBuf* b, const CONTEXT* src)
{
    __try
    {
        __declspec(align(16)) CONTEXT ctx = *src;   // RtlVirtualUnwind MUTATES it

        for (int frame = 0; frame < 64; frame++)
        {
            if (!ctx.Rip) break;

            b_str(b, "    #");
            if (frame < 10) b_ch(b, '0');
            b_dec(b, (unsigned long long)frame);
            b_ch(b, ' ');
            b_addr(b, ctx.Rip);
            zz_b_guest(b, ctx.Rip);   // s9 pt5: name the guest function, every frame

            DWORD64           imageBase = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &imageBase, NULL);
            if (!rf)
            {
                // A leaf with no .pdata. The return address is at [rsp] BY
                // CONVENTION only, so this frame and everything above it are a
                // guess and are labelled as such rather than quietly trusted.
                if (frame == 0)
                {
                    b_str(b, "   [leaf, no pdata -- next frame is a GUESS]\r\n");
                    ctx.Rip  = zz_read_u64(ctx.Rsp);   // via a call: see zz_read_u64
                    ctx.Rsp += 8;
                    continue;
                }
                b_str(b, "   [no pdata -- walk stops here]\r\n");
                break;
            }

            b_str(b, "\r\n");

            PVOID  handlerData = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, rf,
                             &ctx, &handlerData, &establisher, NULL);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        b_str(b, "    [stack walk faulted -- frames above are all we have]\r\n");
    }
}

// ---------------------------------------------------------------------------
// the handler
// ---------------------------------------------------------------------------
static __declspec(thread) int t_depth;   // reentrancy: an AV inside us must not recurse

// Is this PC inside THIS module (the recompiled guest code)? Read straight from
// our own PE headers, so it needs no snapshot and cannot be stale.
static int zz_pc_in_self(unsigned long long pc)
{
    const unsigned char* b = (const unsigned char*)&__ImageBase;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(b + __ImageBase.e_lfanew);
    const unsigned long long lo = (unsigned long long)b;
    const unsigned long long hi = lo + nt->OptionalHeader.SizeOfImage;
    return pc >= lo && pc < hi;
}

// Flushes PER LINE rather than building one buffer: 192 sites would overflow a
// 2 KB buffer and the tally would silently truncate -- and a truncated tally is
// exactly the kind of quiet, plausible-looking output this project has been
// burned by twice.
static void zz_tally(ZzBuf* b, const char* why)
{
    b_flush(b);          // flush, never reset -- see the note in zz_dump_window
    b_str(b, "  TALLY (");
    b_str(b, why);
    b_str(b, ") sites:\r\n");
    b_flush(b);

    const LONG n = g_siteCount;
    for (LONG i = 0; i < n && i < ZZ_MAX_SITES; i++)
    {
        b->n = 0;
        b_str(b, "    code=0x");
        b_hex(b, g_sites[i].code, 8);
        b_ch(b, ' ');
        if (g_sites[i].base)
        {
            const ZzMod* m = zz_mod_for(g_sites[i].base);
            b_str(b, m ? m->name : "<gone>");
            b_str(b, "+0x");
            b_hex(b, g_sites[i].rva, 0);
        }
        else
        {
            b_str(b, "<no module> 0x");
            b_hex(b, g_sites[i].rva, 16);
        }
        b_str(b, " hits=");
        b_dec(b, (unsigned long long)g_sites[i].hits);
        if (g_sites[i].guest)
        {
            b_str(b, " (guest-window=");
            b_dec(b, (unsigned long long)g_sites[i].guest);
            b_str(b, ", expected)");
        }
        b_str(b, "\r\n");
        b_flush(b);
    }

    b->n = 0;
    b_str(b, "  TALLY guest-window AVs (Emu page-fault machinery, expected): ");
    b_dec(b, (unsigned long long)g_guestHits);
    b_str(b, "\r\n  TALLY benign:");
    for (int i = 0; i < 8; i++)
    {
        if (!g_benign[i]) continue;
        b_str(b, " 0x");
        b_hex(b, ZZ_BENIGN[i], 8);
        b_ch(b, '=');
        b_dec(b, (unsigned long long)g_benign[i]);
    }
    b_str(b, "\r\n");
    b_flush(b);
}

// Sum of everything counted, so the worker can tell whether anything happened
// since its last tally without holding a lock.
//
// GUEST-WINDOW HITS ARE EXCLUDED, and that is the point: they never stop, so
// including them means the "counters have gone quiet" condition is never true and
// the periodic tally never fires -- which is half of why the s5 run ended with no
// tally covering its last events.
static unsigned long long zz_counter_sig()
{
    unsigned long long s = 0;
    const LONG n = g_siteCount;
    for (LONG i = 0; i < n && i < ZZ_MAX_SITES; i++)
        s += (unsigned long long)(g_sites[i].hits - g_sites[i].guest);
    for (int i = 0; i < 8; i++)
        s += (unsigned long long)g_benign[i];
    return s;
}

static LONG CALLBACK zz_veh(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    if (t_depth)
        return EXCEPTION_CONTINUE_SEARCH;
    t_depth++;

    const EXCEPTION_RECORD* er   = ep->ExceptionRecord;
    const unsigned long     code = er->ExceptionCode;
    const unsigned long long pc  = (unsigned long long)er->ExceptionAddress;

    const int bi = zz_benign_index(code);
    if (bi >= 0)
    {
        InterlockedIncrement(&g_benign[bi]);
        t_depth--;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // STACK OVERFLOW: one short line, nothing else. See the header comment.
    if (code == 0xC00000FD)
    {
        ZzBuf sb;
        sb.n = 0;
        b_str(&sb, "ZZCRASH STACK_OVERFLOW tid=");
        b_hex(&sb, (unsigned long long)GetCurrentThreadId(), 4);
        b_str(&sb, " pc=0x");
        b_hex(&sb, pc, 16);
        b_str(&sb, " (no unwind attempted)\r\n");
        b_flush(&sb);
        t_depth--;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // THE GUEST WINDOW. Measured in the s5 launch: ~4,000 first-chance AVs, all
    // inside our own recompiled code, all faulting on addresses like 0x17FEA1800
    // and 0x1FFCDF200 -- the 4-8 GB range where Emu maps GUEST memory. That is
    // Emu's ordinary page-fault machinery for guest accesses, raised and handled
    // thousands of times a second, and it is NOT a defect in this module.
    //
    // They are counted and never detailed, and deliberately kept out of BOTH the
    // detail budget and the quiet-period signature. In s5 they exhausted a global
    // 64-record cap before the real fault (VGPUDX12+0x19340) ever arrived, and
    // past the cap no TALLY is written either -- so the one record the whole
    // instrument existed to produce was the one it dropped. An instrument that
    // silently stops recording is worse than one that is noisy.
    // [[feedback_null_results_verify_the_search]]
    // ⚠ THE RANGE IS DERIVED, NOT ASSUMED. It began as a hardcoded
    // [0x100000000, 0x200000000) taken from one run's observed addresses, and the
    // very next launch reported ZERO guest-window AVs because Emu had mapped guest
    // memory somewhere else -- a filter that silently stops filtering.
    //
    // ppc_context.h compiles every guest access as `base + ea`, and PPC_FUNC's
    // preserve_none ABI carries that base in R15. So at a fault inside our own
    // recompiled code, R15 IS the guest memory base, straight from the machine
    // state. Guest address spaces are 4 GB, hence the extent.
    const unsigned long long ZZ_GUEST_LO =
        zz_pc_in_self(pc) ? (unsigned long long)ep->ContextRecord->R15 : 0;
    const unsigned long long ZZ_GUEST_HI =
        ZZ_GUEST_LO ? ZZ_GUEST_LO + 0x100000000ull : 0;

    const ZzMod*             m    = zz_mod_for(pc);
    const unsigned long long base = m ? m->base : 0;
    const unsigned long long rva  = m ? pc - m->base : pc;

    const int isAv    = (code == 0xC0000005);
    const unsigned long long info1 = (isAv && er->NumberParameters >= 2)
                                     ? (unsigned long long)er->ExceptionInformation[1] : ~0ull;
    const int isGuest   = (isAv && info1 >= ZZ_GUEST_LO && info1 < ZZ_GUEST_HI);
    const int isNullish = (isAv && info1 < 0x10000);

    int     isNew = 0;
    ZzSite* site  = zz_site(code, base, rva, &isNew, !isGuest);
    if (site)
    {
        InterlockedIncrement(&site->hits);
        if (isGuest) InterlockedIncrement(&site->guest);
    }

    if (isGuest)
    {
        // Expected by design. Counted only, and it does not spend any budget.
        InterlockedIncrement(&g_guestHits);
        t_depth--;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Detail if: never seen before, OR it looks like the fault being hunted (a
    // read/write of a null-ish address) and it has not been detailed four times.
    // NO GLOBAL CAP on new sites: outside the guest window, faults are rare, and
    // s5 proved that a global cap is a way to lose the only record that matters.
    // The per-site cap alone bounds a hot repeated site.
    const int detail = isNew || (isNullish && site && site->detailed < 4);

    if (!detail)
    {
        t_depth--;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (site) InterlockedIncrement(&site->detailed);
    const LONG serial = InterlockedIncrement(&g_detailTotal);

    ZzBuf b;
    b.n = 0;
    b_str(&b, "\r\nZZCRASH #");
    b_dec(&b, (unsigned long long)serial);
    b_str(&b, " tid=");
    b_hex(&b, (unsigned long long)GetCurrentThreadId(), 4);   // hex, to match Emu's "(5274)"
    b_str(&b, " code=0x");
    b_hex(&b, code, 8);
    b_str(&b, isNew ? " site=NEW" : " site=repeat");
    b_str(&b, "\r\n  pc   = ");
    b_addr(&b, pc);
    b_str(&b, "\r\n");

    if (er->NumberParameters >= 2)
    {
        const unsigned long long a0 = (unsigned long long)er->ExceptionInformation[0];
        b_str(&b, "  info = (0x");
        b_hex(&b, a0, 0);
        b_str(&b, ", 0x");
        b_hex(&b, info1, 0);
        b_str(&b, ")");
        if (isAv)
        {
            b_str(&b, a0 == 0 ? "  READ of 0x" : (a0 == 1 ? "  WRITE to 0x" : "  DEP/EXEC at 0x"));
            b_hex(&b, info1, 0);
            if (isNullish) b_str(&b, "   <-- NULL-ISH, the s4b signature");
        }
        b_str(&b, "\r\n");
        // Crackdown s4 A4: the page state AT THE FAULT (run 5's 0xDFCC5000 read died with no
        // record of whether the C view was reserved, free or no-access), and for a fault on a
        // guest physical-window address, the E/A/C aliases of the same physical page and the
        // game's extra dump (Crackdown: its last 64 D3D locks). The guest base is the one the import
        // thunks captured (zz_import.cpp), NOT R15: run 5's fatal record had R15 = 0x820EE1A4 inside
        // sub_825AA008 (R15 carries the base only across calls), so the fault was detailed as a
        // non-guest AV -- the isGuest test above never matched it.
        if (isAv)
        {
            zz_page_line(&b, "page", info1, 0);
            const unsigned long long gb = (unsigned long long)zz_guest_base();
            if (gb && info1 >= gb + 0xA0000000ull && info1 < gb + 0x100000000ull)
            {
                const unsigned long long ga = info1 - gb;
                const unsigned long long ph = ga >= 0xE0000000ull ? (ga & 0x1FFFFFFFull) + 0x1000 : (ga & 0x1FFFFFFFull);
                if (ph >= 0x1000) zz_page_line(&b, "E alias", gb + 0xE0000000ull + ph - 0x1000, 0xE0000000ull + ph - 0x1000);
                zz_page_line(&b, "A alias", gb + 0xA0000000ull + ph, 0xA0000000ull + ph);
                zz_page_line(&b, "C alias", gb + 0xC0000000ull + ph, 0xC0000000ull + ph);
                b_flush(&b);
                zz_game_dump_run(zz_log_line);
            }
        }
    }

    // The integer register file at the fault. Costs nothing (no memory is read)
    // and it is what turns "a null pointer was dereferenced" into "WHICH pointer".
    // s5 pt2 is the worked example: the faulting instruction is
    // `mov rax,[rcx]` after `mov rcx,[rbp-0x40]` in VGPUDX12, reached through
    // `call [r11+0x268]` -- so r11 names the vtable that was dispatched through
    // and rbx names the object it came from. Without the registers those are
    // unknowable from the log alone.
    {
        static const char* RN[16] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                      "r8 ","r9 ","r10","r11","r12","r13","r14","r15" };
        const DWORD64* R = &ep->ContextRecord->Rax;   // CONTEXT lays these out in order
        for (int row = 0; row < 4; row++)
        {
            b_str(&b, "  ");
            for (int col = 0; col < 4; col++)
            {
                const int i = row * 4 + col;
                b_str(&b, RN[i]);
                b_str(&b, "=");
                b_hex(&b, (unsigned long long)R[i], 16);
                b_str(&b, col == 3 ? "\r\n" : "  ");
            }
        }
    }

    // Memory windows around rsp and rbp. Every read goes through zz_read_u64, i.e.
    // through a CALL inside a __try (see the note there) -- an unreadable address
    // costs a "??" and nothing else.
    //
    // Why these two windows specifically: a fault's arguments almost always live in
    // them. The VGPU case is the worked example -- the D3D12_RESOURCE_DESC1 that
    // CreatePlacedResource2 rejected sits at [rbp-0x20], the null out-slot at
    // [rbp-0x40], the heap offset at [rsp+0x58], and the call's stack arguments at
    // [rsp+0x20..0x48]. None of that is recoverable from registers alone.
    zz_dump_window(&b, "rbp", ep->ContextRecord->Rbp - 0x80, 32);
    zz_dump_window(&b, "rsp", ep->ContextRecord->Rsp,        32);


    // s6: when the fault is inside VGPUDX12, NAME the objects the refused call was
    // made on. From FABLE2_NOTES s5 pt6, statically: rbx is element 9 of VGPU's
    // 14-slot heap table at g_vgpu+0x37338, rbx+0x08 is the ID3D12Device10 the call
    // was dispatched on, and rbx+0x10 is the ID3D12Heap CreatePlacedResource2
    // refused with DXGI_ERROR_INVALID_CALL. Those three objects are the only
    // remaining unknown in the whole diagnosis.
    {
        const ZzMod* mfault = zz_mod_for(pc);
        if (mfault && zz_name_has(mfault->name, "VGPUDX12"))
        {
            const DWORD64 rbx = ep->ContextRecord->Rbx;
            // NOT always a heap-table element: that reading is specific to the
            // 0x19340 create site. At 0x328C73 (s9 pt3) rbx is the copy loop's
            // DESTINATION pointer, and reading its zeroed memory as "a heap with a
            // null device" invented a bug that was not there. Say what is known.
            zz_dump_object(&b, "rbx (heap-table element 9 at the 0x19340 site; "
                               "just a pointer elsewhere)", rbx);

            // ...and the element's OWN fields, which is where the number that
            // most likely explains the refusal lives. ResourceHeap's lazy heap
            // creator (VGPUDX12+0x25CDC, vtable slot 1) builds its D3D12_HEAP_DESC
            // as: SizeInBytes = [this+0x48], Type = [this+0x18], and Alignment =
            // 4 MB when [this+0x20] == 0xB else 64 KB. So [rbx+0x48] IS the heap's
            // size, and the resource needs 0x6B0000 + 0xE0000 = 0x790000 of it.
            // This also covers the case where the GetDesc gate refuses to fire.
            zz_dump_window(&b, "rbx (ResourceHeap fields; +0x48 = heap SizeInBytes)",
                           rbx, 16);

            DWORD64 dev = 0, heap = 0;
            if (zz_try_read_u64(rbx + 0x08, &dev))
                zz_dump_object(&b, "[rbx+0x08] device", dev);
            if (zz_try_read_u64(rbx + 0x10, &heap))
            {
                zz_dump_object(&b, "[rbx+0x10] heap", heap);
                zz_dump_heap_desc(&b, heap);
            }

            // ⚠ s9 pt3 -- THIS USED TO LIVE INSIDE THE `heap` BRANCH ABOVE, and
            // that cost the s9 pt2 launch its whole message history. The rbx
            // reading is only valid at the 0x19340 site; at the 0x328C73 site rbx
            // is a destination POINTER whose +0x10 is zero, so the branch was
            // skipped and NO InfoQueue and NO DRED dump were written for a fault
            // that had 800+ banked messages sitting behind it. The dump must not
            // depend on a guess about one register: get the device the way every
            // other part of this file gets it, and dump unconditionally.
            // [[feedback_verify_instruments_before_doubting_user]]
            {
                DWORD64 d2 = zz_vgpu_device();
                if (!d2) d2 = dev;
                if (d2)
                {
                    HRESULT rr = 0;
                    if (zz_device_removed_reason(d2, &rr))
                    {
                        b_flush(&b);
                        b_str(&b, "    GetDeviceRemovedReason = 0x");
                        b_hex(&b, (unsigned long long)(unsigned)rr, 8);
                        b_str(&b, (rr == 0) ? "  (S_OK -- the device is ALIVE)\r\n"
                                            : "  (the device is GONE)\r\n");
                        b_flush(&b);
                    }
                    zz_dred_dump(d2);
                    zz_infoqueue_dump(d2);
                }
            }
        }
    }

    b_str(&b, "  stack:\r\n");
    b_flush(&b);          // the header is on disk before the risky part starts

    b.n = 0;
    zz_walk(&b, ep->ContextRecord);
    b_flush(&b);

    // s9 pt8: the routine path is now time-boxed, so the record that matters most
    // forces its own flush -- this is the one case where the process may not be
    // around when the next flush would have come.
    zz_log_flush();
    zz_tally(&b, "at fault");

    t_depth--;
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// the module-snapshot worker
// ---------------------------------------------------------------------------
static void zz_dump_mods(LONG live)
{
    ZzBuf b;
    b.n = 0;
    b_str(&b, "ZZCRASH modules (");
    b_dec(&b, (unsigned long long)g_modCount[live]);
    b_str(&b, ")\r\n");
    b_flush(&b);

    const LONG n = g_modCount[live];
    for (LONG i = 0; i < n; i++)
    {
        b.n = 0;
        b_str(&b, "  0x");
        b_hex(&b, g_mods[live][i].base, 16);
        b_str(&b, " .. 0x");
        b_hex(&b, g_mods[live][i].base + g_mods[live][i].size, 16);
        b_str(&b, "  size=0x");
        b_hex(&b, g_mods[live][i].size, 0);
        b_str(&b, "  ");
        b_str(&b, g_mods[live][i].name);
        b_str(&b, "\r\n");
        b_flush(&b);
    }
}

extern "C" IMAGE_DOS_HEADER __ImageBase;

// --- host RIP -> GUEST address, at run time (s9 pt4) -------------------------
//
// A sample that says `xeo3_....dll+0x1F2C480` names nothing. The guest function is
// what names it, and this module already carries the table: the recompiler emits
// PPCFuncMappings[] (90,757 entries for this title) and Emu fills in each entry's
// host_rva when it calls InitPrecompiledDll. So the reverse lookup is available in
// process, and needs no .map file -- which matters, because build_fable2.ps1 emits
// no map at all and the one in ppc\ is from the s3 build and would resolve every
// address to the wrong function. [[feedback_verify_the_artifact_not_the_build_log]]
//
// Declared here rather than by including ppc_context.h: this file is deliberately
// standalone so the fixture can compile it with nothing else, and the fixture
// supplies its own small table to check the search against known answers.
struct ZzFuncMapping { unsigned int guest_rva; unsigned int host_rva; };
extern "C" ZzFuncMapping PPCFuncMappings[];
extern int HostFuncsCount;

#define ZZ_GUEST_BASE  0x82000000u
// A hit further than this from the function's start is not a resolution, it is a
// coincidence -- the search would otherwise attribute any address above the last
// function to that function, forever.
#define ZZ_GUEST_SPAN  0x100000u

// Greatest BODY rva <= rva. Linear, because the table is ordered by GUEST address
// and nothing promises the host order; 90k comparisons a sample is nothing next to
// suspending a thread. Returns 1 and fills the outputs on a hit.
//
// Crackdown s3 A4: host_rva is the WEAK WRAPPER `sub_X`, and the linker does not keep a
// wrapper beside its body `__imp__sub_X` (crackdown.map: wrapper sub_825AA008 at RVA
// 0x2C194D0, its body at 0x2C17EE0) -- so "greatest wrapper <= pc" named the PREVIOUS
// function for every pc inside a body: run 4's fatal read was printed as
// sub_825A9E7C+h0EA2 and is really sub_825AA008+0x6F2. A wrapper is `E9 rel32` =
// `jmp __imp__sub_X`; where the compiler folded a trivial body into the wrapper itself (or
// a strong PPC_FUNC override replaced it) the code starts there. So each entry's body rva
// = the jmp target when byte 0 is 0xE9, else the wrapper's own rva, decoded on the fly
// (no table, no allocation: this runs inside the fault handler). The "+h" delta is then a
// real offset into the body.
// Measured on build 14 (62,204 wrappers): 28,305 are `E9`, every target is the function's own
// __imp__ body; 5 are `50 E8 rel32` (push rax; call body), every target again its own body;
// the other ~33,900 have the body folded in (no separate __imp__ symbol), plus strong overrides.
static unsigned int zz_body_rva(unsigned long long base, unsigned int h)
{
    const unsigned char* p = (const unsigned char*)(base + h);
    int rel;
    if (p[0] == 0xE9) { __builtin_memcpy(&rel, p + 1, 4); return (unsigned int)((long long)h + 5 + rel); }
    if (p[0] == 0x50 && p[1] == 0xE8) { __builtin_memcpy(&rel, p + 2, 4); return (unsigned int)((long long)h + 6 + rel); }
    return h;
}

static int zz_guest_for_host(unsigned long long rip, unsigned* guestAddr,
                             unsigned* delta)
{
    const unsigned long long base = (unsigned long long)&__ImageBase;
    if (rip < base) return 0;
    const unsigned long long off = rip - base;
    if (off > 0xFFFFFFFFull) return 0;
    const unsigned int rva = (unsigned int)off;

    int n = HostFuncsCount;
    if (n <= 0) return 0;

    unsigned int bestHost  = 0;
    unsigned int bestGuest = 0;
    int          found     = 0;
    for (int i = 0; i < n; i++)
    {
        const unsigned int w = PPCFuncMappings[i].host_rva;
        if (!w) continue;                       // not filled in yet: Emu populates
        const unsigned int h = zz_body_rva(base, w);
        if (h > rva) continue;                  // these at InitPrecompiledDll time
        if (!found || h > bestHost)
        {
            bestHost  = h;
            bestGuest = PPCFuncMappings[i].guest_rva;
            found     = 1;
        }
    }
    if (!found) return 0;
    const unsigned int d = rva - bestHost;
    if (d > ZZ_GUEST_SPAN) return 0;
    *guestAddr = ZZ_GUEST_BASE + bestGuest;
    *delta     = d;
    return 1;
}

// Test seam for the fixture, called by nothing in Emu.
extern "C" unsigned zz_selftest_guest(unsigned long long rip)
{
    unsigned g = 0, d = 0;
    const int ok = zz_guest_for_host(rip, &g, &d);
    ZzBuf b; b.n = 0;
    b_str(&b, "ZZGUEST host+0x");
    b_hex(&b, rip - (unsigned long long)&__ImageBase, 8);
    b_str(&b, " -> ");
    if (ok)
    {
        b_str(&b, "sub_");
        b_hex(&b, g, 8);
        b_str(&b, "+h");
        b_hex(&b, d, 4);
        b_str(&b, "\r\n");
    }
    else
    {
        b_str(&b, "unresolved\r\n");
    }
    b_flush(&b);
    return ok ? g : 0;
}

// Appends "  guest=sub_82XXXXXX+0xNN" when the address is ours to name, and
// nothing at all when it is not -- a frame in ntdll or VGPU must stay bare
// rather than be given a guest name it does not have.
static volatile LONG g_walkSawGuest;   // s9 pt6: did this walk touch guest code?

static void zz_b_guest(ZzBuf* b, unsigned long long pc)
{
    unsigned g = 0, d = 0;
    if (!zz_guest_for_host(pc, &g, &d)) return;
    InterlockedExchange(&g_walkSawGuest, 1);
    b_str(b, "  guest=sub_");
    b_hex(b, g, 8);
    // ⚠ the offset is a HOST byte offset into the recompiled function, NOT a
    // guest instruction offset -- one PPC instruction becomes several host
    // ones. The NAME is exact; the number only locates code within it. The
    // "+h" says so, because "sub_8236C1E0+0x6DF" read as a guest address sends
    // the next reader to the wrong instruction entirely.
    b_str(b, "+h");
    b_hex(b, d, 4);
}

// --- the INSTRUMENT for the white-screen hang (s9 pt4, zz_pcsample.on) --------
//
// s9 pt3's build got the game past every D3D12 wall: launch 3 ran ~14 hours with
// ZERO D3D12 errors and no fault. But the ODS capture says the GPU work STOPS at
// 22:19:45.398 -- about 30 s in, the same moment run 2 died -- while the XMA
// thread and music streaming run all night. So this is a HANG in one thread, not
// a crash and not a slow frame, and nothing in the logs says which thread or where.
//
// A sampling profiler answers that in one launch, and it is the only instrument
// that can: the guest side is 40 MB of recompiled code with no entry point we have
// identified, so no amount of static reading names the function the game is stuck
// in. Periodically: enumerate this process's threads, capture each one's RIP, and
// attribute it to a module. A thread parked in ntdll is BLOCKED on something; a
// thread whose RIP sits in this module is EXECUTING guest code, and its RVA
// resolves offline against ppc\fable2.map to the `sub_82XXXXXX` the game is
// spinning in.
//
// RULES, because suspending threads is the one thing here that can deadlock a
// process rather than just fail to measure:
//   * NOTHING IS LOGGED BETWEEN SuspendThread AND ResumeThread. Formatting takes
//     the log critical section and the CRT's; a suspended thread holding either
//     would deadlock us against ourselves. Contexts are captured into a local
//     array, every thread is resumed, and only then is a line written.
//   * our own thread is never suspended (tid compare), and neither is any thread
//     we cannot open.
//   * bounded everywhere: at most ZZ_PC_THREADS threads a round, at most
//     ZZ_PC_ROUNDS rounds for the whole run, and then it says it has stopped.
//   * every call is inside a __try, made FROM the __try body.
//     [[reference_clangcl_try_inline_load]]
#define ZZ_PC_THREADS 48
#define ZZ_PC_ROUNDS  60
#define ZZ_PC_WALKS   2        // walks per round for threads already in guest code
#define ZZ_PC_WALKS_ALL 10     // total walks per round, PARKED threads included
#define ZZ_PC_KNOWN   32       // tids remembered as GUEST threads across rounds
// ⚠ the known set gets its OWN budget, not the whole cap. If it could fill the
// cap, a process with many guest threads would walk the same known ones every
// round and the rotation below would never run -- which is the very failure
// this pass exists to remove, reintroduced by its own fix. The fixture starts
// 13 guest threads precisely to make that happen if the budget is not reserved.
#define ZZ_PC_WALKS_KNOWN 4    // of ZZ_PC_WALKS_ALL, reserved for known guests

static volatile LONG g_pcOn;
static volatile LONG g_pcAnnounced;
static volatile LONG g_pcRounds;

// s9 pt6: run 5 walked 10 threads out of 48 and re-walked the SAME six host
// idlers in all eight rounds, because pass 2 took the first unwalked threads in
// enumeration order and that order never changes. 38 threads -- including the
// one we are hunting -- were never walked at all. Two things fix it: REMEMBER
// which tids ever showed guest code and always walk those, and ROTATE through
// the rest so every thread is covered within a few rounds.
static DWORD              g_pcGuestTids[ZZ_PC_KNOWN];
static unsigned long long g_pcGuestBorn[ZZ_PC_KNOWN];
static volatile LONG  g_pcGuestN;
static unsigned       g_pcRotate;

typedef struct ZzPcSample
{
    DWORD              tid;
    // ⚠ s9 pt6: a TID IS NOT A STABLE IDENTITY. Windows recycles thread ids, and
    // run 5 proved it inside five minutes: tid 0E70 was sampled on THREE stacks
    // 300 MB apart and tid 9CE0 on two, which is what made a short-lived thread
    // look like a mostly-parked one that occasionally ran guest code. The
    // creation time makes the identity exact and costs one call on a handle we
    // already hold.
    unsigned long long born;
    unsigned long long rip;
    unsigned long long rsp;
    int                got;
    int                walked;
    CONTEXT            ctx;
} ZzPcSample;

// "interesting" = our own recompiled code (the guest is executing) or VGPUDX12
// (the emulator's GPU thread). Everything else is a system thread parked in a wait,
// and its single PC already says so.
static int zz_pc_interesting(unsigned long long pc)
{
    if (zz_pc_in_self(pc)) return 1;
    const ZzMod* m = zz_mod_for(pc);
    return m && zz_name_has(m->name, "VGPUDX12");
}

// One thread's control registers, captured with the thread stopped. Returns 1 on
// success. The thread is ALWAYS resumed, including on the fault path.
__declspec(noinline) static int zz_pc_capture(DWORD tid, ZzPcSample* out)
{
    HANDLE th = NULL;
    __try
    {
        // THREAD_QUERY_INFORMATION is what GetThreadTimes needs -- without it the
        // call fails and every creation time comes back ZERO, which is a silent
        // loss of the identity rather than a visible one. The fixture caught
        // exactly that. Ask for it, but never let it cost us the thread: if the
        // wider open is refused, fall back to the access this actually requires
        // and carry on without the identity.
        th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                        THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!th)
            th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { th = NULL; }
    if (!th) return 0;

    int ok = 0;
    __try
    {
        if (SuspendThread(th) != (DWORD)-1)
        {
            out->ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &out->ctx))
            {
                FILETIME c, e, k, u;
                if (GetThreadTimes(th, &c, &e, &k, &u))
                    out->born = ((unsigned long long)c.dwHighDateTime << 32) |
                                 (unsigned long long)c.dwLowDateTime;
                out->rip = (unsigned long long)out->ctx.Rip;
                out->rsp = (unsigned long long)out->ctx.Rsp;
                ok = 1;
            }
            ResumeThread(th);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }

    CloseHandle(th);
    return ok;
}

// One thread stack, with the tid repeated so each walk is readable on its own
// now that a round can carry several of them.
static int zz_pc_is_known(DWORD tid, unsigned long long born)
{
    const LONG k = g_pcGuestN;
    for (LONG i = 0; i < k && i < ZZ_PC_KNOWN; i++)
        if (g_pcGuestTids[i] == tid && g_pcGuestBorn[i] == born) return 1;
    return 0;
}

static void zz_pc_walk_one(ZzBuf* b, ZzPcSample* smp)
{
    smp->walked = 1;
    InterlockedExchange(&g_walkSawGuest, 0);
    b->n = 0;
    b_str(b, "    stack of tid=");
    b_hex(b, (unsigned long long)smp->tid, 4);
    b_str(b, ":\r\n");
    b_flush(b);
    b->n = 0;
    zz_walk(b, &smp->ctx);
    b_flush(b);

    // a thread whose stack contains OUR code is a guest thread; remember it so
    // later rounds always re-walk it instead of rediscovering it by luck
    if (g_walkSawGuest && !zz_pc_is_known(smp->tid, smp->born))
    {
        const LONG k = InterlockedIncrement(&g_pcGuestN) - 1;
        if (k < ZZ_PC_KNOWN)
        {
            g_pcGuestTids[k] = smp->tid;
            g_pcGuestBorn[k] = smp->born;
            b->n = 0;
            b_str(b, "    ZZPC tid=");
            b_hex(b, (unsigned long long)smp->tid, 4);
            b_str(b, " is a GUEST thread -- it will be walked every round from now on\r\n");
            b_flush(b);
        }
        else InterlockedDecrement(&g_pcGuestN);
    }
}

// One round: capture first, resume everything, then write. Never the other way.
static void zz_pc_round(void)
{
    if (!g_pcOn) return;

    const LONG round = InterlockedIncrement(&g_pcRounds);
    if (round > ZZ_PC_ROUNDS)
    {
        if (round == ZZ_PC_ROUNDS + 1)
        {
            ZzBuf b; b.n = 0;
            b_str(&b, "ZZPC sampler stopped after ");
            b_dec(&b, (unsigned long long)ZZ_PC_ROUNDS);
            b_str(&b, " rounds (said once)\r\n");
            b_flush(&b);
        }
        return;
    }

    static ZzPcSample s[ZZ_PC_THREADS];   // worker-thread only, so no lock needed
    int n = 0;

    const DWORD self = GetCurrentThreadId();
    const DWORD pid  = GetCurrentProcessId();

    HANDLE snap = INVALID_HANDLE_VALUE;
    __try { snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { snap = INVALID_HANDLE_VALUE; }
    if (snap == INVALID_HANDLE_VALUE)
    {
        ZzBuf b; b.n = 0;
        b_str(&b, "ZZPC round FAILED: no thread snapshot\r\n");
        b_flush(&b);
        return;
    }

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    int more = 0;
    __try { more = Thread32First(snap, &te) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { more = 0; }

    while (more && n < ZZ_PC_THREADS)
    {
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
        {
            s[n].tid = te.th32ThreadID;
            s[n].rip = 0;
            s[n].rsp = 0;
            s[n].walked = 0;
            s[n].born   = 0;
            s[n].got = zz_pc_capture(te.th32ThreadID, &s[n]);
            n++;
        }
        te.dwSize = sizeof(te);
        __try { more = Thread32Next(snap, &te) ? 1 : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { more = 0; }
    }
    CloseHandle(snap);

    // -- every thread is running again from here; NOW it is safe to format --
    ZzBuf b; b.n = 0;
    b_str(&b, "ZZPC round ");
    b_dec(&b, (unsigned long long)round);
    b_str(&b, "  t=");
    b_dec(&b, (unsigned long long)GetTickCount64());
    b_str(&b, " ms  ");
    b_dec(&b, (unsigned long long)n);
    b_str(&b, " threads\r\n");
    b_flush(&b);

    int walks = 0;
    for (int i = 0; i < n; i++)
    {
        b.n = 0;
        b_str(&b, "  ZZPC tid=");
        b_hex(&b, (unsigned long long)s[i].tid, 4);
        if (!s[i].got)
        {
            b_str(&b, "  <not sampled>\r\n");
            b_flush(&b);
            continue;
        }
        b_str(&b, "  rip=");
        b_addr(&b, s[i].rip);
        b_str(&b, "  rsp=0x");
        b_hex(&b, s[i].rsp, 16);
        // the low half of the creation time is enough to tell a recycled tid
        // apart from the thread that held it before
        b_str(&b, "  born=");
        b_hex(&b, s[i].born & 0xFFFFFFFFull, 8);
        zz_b_guest(&b, s[i].rip);   // a host RVA names nothing; the guest does
        b_str(&b, "\r\n");
        b_flush(&b);

        // A single PC names the module; the CHAIN names the caller, and for a
        // thread stopped in our own recompiled code that is what identifies the
        // guest function. Bounded to a couple per round: the walk reads another
        // thread's stack, which is only stable because the thread is hung -- which
        // is the very thing being measured, so it is never assumed, just bounded.
        // s9 pt5: the walk used to be limited to threads whose RIP was already in
        // guest or VGPU code, which is exactly the threads that need it LEAST -- a
        // running thread's leaf already names it. The threads that say nothing are
        // the PARKED ones, and their stacks are where the guest return address that
        // chose the wait lives. So round 1 walks the interesting ones, and a second
        // pass then walks the rest until the (larger) cap is used up.
        if (walks < ZZ_PC_WALKS && zz_pc_interesting(s[i].rip))
        {
            walks++;
            zz_pc_walk_one(&b, &s[i]);
        }
    }

    // second pass: threads ALREADY KNOWN to be guest threads. Once the one we
    // are hunting has been identified it must be re-walked every round, so its
    // wait can be watched over time instead of caught once by luck.
    int known = 0;
    for (int i = 0; i < n && walks < ZZ_PC_WALKS_ALL && known < ZZ_PC_WALKS_KNOWN; i++)
    {
        if (!s[i].got || s[i].walked) continue;
        if (!zz_pc_is_known(s[i].tid, s[i].born)) continue;
        walks++; known++;
        zz_pc_walk_one(&b, &s[i]);
    }

    // third pass: everything else, ROTATING. Run 5 proved why: taking the first
    // unwalked threads in enumeration order walks the same six host idlers every
    // round forever, and 38 of 48 threads were never walked at all. Starting at a
    // moving offset covers every thread within a few rounds instead of never.
    if (n > 0)
    {
        const unsigned start = g_pcRotate % (unsigned)n;
        int stepped = 0;
        for (int k = 0; k < n && walks < ZZ_PC_WALKS_ALL; k++)
        {
            const int i = (int)((start + (unsigned)k) % (unsigned)n);
            if (!s[i].got || s[i].walked) continue;
            walks++;
            stepped++;
            zz_pc_walk_one(&b, &s[i]);
        }
        g_pcRotate += (unsigned)(stepped ? stepped : 1);
    }
}

// Test seam for the fixture, called by nothing in Emu. The arithmetic in this
// instrument is trivial; the RISK is entirely in suspending live threads and
// resuming them again, and that cannot be checked by inspection -- a missing
// ResumeThread deadlocks the process, and a log written while a thread is
// suspended deadlocks it against our own critical section. So the fixture runs a
// REAL round against its own threads and then keeps running, which is the only
// evidence that matters.
// Lets the fixture put its OWN observations into the same log the criteria are
// checked against, so a check can cover something only the fixture can see -- like
// whether a suspended thread ever started again.
extern "C" void zz_selftest_note(const char* s)
{
    ZzBuf b; b.n = 0;
    b_str(&b, s);
    b_str(&b, "\r\n");
    b_flush(&b);
}

extern "C" void zz_selftest_pcsample(void)
{
    InterlockedExchange(&g_pcOn, 1);
    zz_pc_round();
    InterlockedExchange(&g_pcOn, 0);
    ZzBuf b; b.n = 0;
    b_str(&b, "ZZPC selftest returned -- the process is still running\r\n");
    b_flush(&b);
}

static void zz_pc_announce(void)
{
    if (InterlockedCompareExchange(&g_pcAnnounced, 1, 0) != 0) return;
    ZzBuf b; b.n = 0;
    if (zz_marker("zz_pcsample.on"))
    {
        InterlockedExchange(&g_pcOn, 1);
        b_str(&b, "ZZPC marker zz_pcsample.on present -- will sample every thread's "
                  "program counter periodically\r\n");
    }
    else
    {
        b_str(&b, "ZZPC marker zz_pcsample.on absent, sampler not armed "
                  "(said once)\r\n");
    }
    b_flush(&b);
}

static DWORD WINAPI zz_worker(LPVOID)
{
    // Written from the worker, never from DllMain: it is the first thing in the
    // file and it states the two bases every later address is judged against --
    // ours, and the exe's, which Emu also prints in its own fatal line
    // ("Emu at 00007FF769A00000"), so the two logs can be lined up.
    {
        ZzBuf hb;
        hb.n = 0;
        b_str(&hb, "ZZCRASH owner instance; this module base=0x");
        b_hex(&hb, (unsigned long long)&__ImageBase, 16);
        b_str(&hb, "  exe base=0x");
        b_hex(&hb, (unsigned long long)GetModuleHandleA(NULL), 16);
        b_str(&hb, "\r\n");
        b_flush(&hb);
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    g_enumMods = (PFN_EnumProcessModules)GetProcAddress(k32, "K32EnumProcessModules");
    g_modInfo  = (PFN_GetModuleInformation)GetProcAddress(k32, "K32GetModuleInformation");
    if (!g_enumMods || !g_modInfo)
        return 0;

    LONG               lastCount = -1;
    unsigned           pcTick    = 0;
    unsigned long long lastSig   = 0;
    int                quiet     = 0;
    for (;;)
    {
        const LONG fill = (g_live == 0) ? 1 : 0;      // fill the table nobody is reading

        HMODULE mods[ZZ_MAX_MODS];
        DWORD   needed = 0;
        if (g_enumMods(GetCurrentProcess(), mods, (DWORD)sizeof(mods), &needed))
        {
            DWORD count = needed / (DWORD)sizeof(HMODULE);
            if (count > ZZ_MAX_MODS) count = ZZ_MAX_MODS;

            LONG out = 0;
            for (DWORD i = 0; i < count; i++)
            {
                ZZ_MODINFO mi;
                if (!g_modInfo(GetCurrentProcess(), mods[i], &mi, (DWORD)sizeof(mi)))
                    continue;

                char path[MAX_PATH];
                DWORD len = GetModuleFileNameA(mods[i], path, MAX_PATH);
                const char* leaf = path;
                for (DWORD c = 0; c < len; c++)
                    if (path[c] == '\\' || path[c] == '/') leaf = path + c + 1;

                ZzMod* dst = &g_mods[fill][out];
                dst->base = (unsigned long long)mi.base;
                dst->size = (unsigned long long)mi.size;
                int j = 0;
                for (; leaf[j] && j < ZZ_NAME_LEN - 1; j++) dst->name[j] = leaf[j];
                dst->name[j] = 0;
                out++;
            }

            g_modCount[fill] = out;
            InterlockedExchange(&g_live, fill);       // publish

            if (out != lastCount)
            {
                // The set changed (first snapshot, or something loaded late such
                // as XAudio / avcodec). Re-dump so the log always holds a module
                // table taken close in time to whatever faults next.
                zz_dump_mods(fill);
                lastCount = out;
            }
        }

        // A tally is only written next to a detail record, and the LAST thing to
        // happen in a run may be counted rather than detailed -- or the process
        // may be killed outright. So the worker re-tallies whenever the counters
        // have moved and then gone quiet for ~10 s, which means the file on disk
        // is current even if Emu never gets to exit.
        const unsigned long long sig = zz_counter_sig();
        if (sig != lastSig) { lastSig = sig; quiet = 0; }
        else if (sig && ++quiet == 5)
        {
            ZzBuf tb;
            tb.n = 0;
            zz_tally(&tb, "periodic");
        }

        // s6 pt4: is VGPU's D3D12 device still alive? The stencil-scratch AV is a
        // casualty of the device dying earlier, so the first tick at which this
        // stops being S_OK brackets the guilty submission against the boot log.
        // 8 device checks per module-snapshot tick. The whole point is to bracket
        // the MOMENT of death, and a 2 s window is a poor bracket; 250 ms costs one
        // guarded read plus one no-argument call per tick.
        for (int i = 0; i < 8; i++) { zz_device_tick(); Sleep(250); }

        // s9 pt4: one PC sample of every thread every 5th tick (~10 s). The
        // whole point is a HANG, so a coarse period is the right one -- and a
        // sampler that ran every tick would suspend threads 30 times a minute
        // for the life of the process.
        // s9 pt8: the import ring is drained HERE, on the worker, because the
        // guest must never touch the log lock. Every tick, not every 5th: the
        // ring is 256 records and the rate lines are the measurement.
        zz_imp_drain();

        if ((++pcTick % 5) == 0) zz_pc_round();
    }
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------
// AddVectoredExceptionHandler(1, ...) puts us at the FRONT of the chain, ahead of
// anything Emu registered before our DLL loaded. The module snapshot is NOT taken
// here: this runs under the loader lock during DLL_PROCESS_ATTACH, so the work is
// handed to a thread that cannot start until the lock is released.
//
// --- Crackdown s5: VGPU's sampler table holds 16, the shaders ask for up to 32 ---
// (opt-in: <game>_zz_sampler32.on)
//
// THE BUG (launch 9, dump Emu.exe.32808, fast-fail 0xC0000409 subcode 2 =
// STACK_COOKIE_CHECK_FAILURE on VGPU's render thread, at Crackdown's first cutscene).
// VGPUDX12+0x14A05 computes the sampler count as 32 - lzcnt(vs_mask | ps_mask) -- up to
// 32, the Xbox 360's full set -- and passes it (r9d) to VGPUDX12+0x153C8, the function
// that builds the draw's SAMPLER descriptor table. That function gathers one CPU handle
// per sampler (the first qword of each 16-byte record at src+0x80) into a 16-qword array
// on its stack ([rsp+0x70], cookie at [rsp+0xF0]), keeps a 16-slot cache in its object
// (+0x160..+0x1DF, count at +0x1E0), and on a miss allocates `count` descriptors
// (VGPUDX12+0x46C24 on ctx+0x300) and calls ID3D12Device::CopyDescriptors (vtable 0xB8,
// heap type 1 = SAMPLER, device at VGPUDX12+0x5F49B8). No bound check: the dump's frame
// shows count = 17 at [rsp+0x60], and handle #17 overwrote the cookie.
//
// THE FIX: replace the function (a 14-byte jmp at its entry) with this copy of it, same
// arguments (rcx obj, rdx out GPU handle, r8 src, r9d count, [rsp+0x28] ctx), same
// fields, same return (1), with room for 64 and the object's 16-slot cache used only for
// tables of <= 16 (a larger one is rebuilt every call and leaves the cache invalid). The
// entry bytes are checked against the build this was read from before patching; any
// mismatch leaves VGPU untouched and says so.
#define ZZ_SAMP_RVA    0x153C8u
#define ZZ_SAMP_ALLOC  0x46C24u
#define ZZ_SAMP_DEVICE 0x5F49B8u
static unsigned char* g_sampVgpu;
static volatile LONG g_sampCalls, g_sampBig, g_sampMax;

static int zz_sampler_table(unsigned char* obj, UINT64* out, unsigned char* src, UINT count, unsigned char* ctx)
{
    UINT64 local[64];
    InterlockedIncrement(&g_sampCalls);
    if (count > 16)
    {
        const LONG nb = InterlockedIncrement(&g_sampBig);
        LONG mx = g_sampMax;
        while ((LONG)count > mx && InterlockedCompareExchange(&g_sampMax, (LONG)count, mx) != mx) mx = g_sampMax;
        if (nb <= 8 || (nb % 1000) == 0)
        {
            ZzBuf b; b.n = 0;
            b_str(&b, "ZZSAMP sampler table of "); b_dec(&b, count);
            b_str(&b, " (> VGPU's 16) built by the replacement, #"); b_dec(&b, (unsigned long long)nb);
            b_str(&b, "\r\n"); b_flush(&b);
        }
        if (count > 64) count = 64;              // never seen; the masks are 32 bits
    }
    for (UINT i = 0; i < count; i++) local[i] = *(UINT64*)(src + 0x80 + (size_t)i * 16);

    UINT*   cnt   = (UINT*)(obj + 0x1E0);
    UINT64* cache = (UINT64*)(obj + 0x160);
    int changed = *cnt != count;
    *cnt = count;
    unsigned char* ring = ctx + 0x300;
    const UINT64 epoch = *(UINT64*)(ring + 0x60);
    if (count <= 16)
    {
        for (UINT i = 0; i < count; i++)
            if (cache[i] != local[i]) { cache[i] = local[i]; changed = 1; }
    }
    else
    {
        changed = 1;
        *cnt = 0xFFFFFFFFu;                      // the cache cannot describe this table
    }
    if (!changed && *(UINT64*)(obj + 0x1F8) == epoch)
    {
        unsigned char* heap = *(unsigned char**)(obj + 0x1E8);
        *out = *(UINT64*)(heap + 0x20) + (UINT)(*(UINT*)(heap + 0x10) * *(UINT*)(obj + 0x1F0));
        return 1;
    }

    struct { unsigned char* heap; UINT idx; UINT extra; } t = { 0, 0, 0 };
    reinterpret_cast<void(*)(void*, void*, UINT)>(g_sampVgpu + ZZ_SAMP_ALLOC)(ring, &t, count);
    UINT64 dest = *(UINT64*)(t.heap + 0x18) + (UINT)(t.idx * *(UINT*)(t.heap + 0x10));
    UINT   dsz  = count;
    ID3D12Device* dev = *(ID3D12Device**)(g_sampVgpu + ZZ_SAMP_DEVICE);
    D3D12_CPU_DESCRIPTOR_HANDLE dst; dst.ptr = (SIZE_T)dest;
    dev->CopyDescriptors(1, &dst, &dsz, count, (const D3D12_CPU_DESCRIPTOR_HANDLE*)local, NULL,
                         D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    *(unsigned char**)(obj + 0x1E8) = t.heap;
    *(UINT*)(obj + 0x1F0) = t.idx;
    *(UINT*)(obj + 0x1F4) = t.extra;
    *(UINT64*)(obj + 0x1F8) = *(UINT64*)(ctx + 0x360);
    *out = *(UINT64*)(t.heap + 0x20) + (UINT)(t.idx * *(UINT*)(t.heap + 0x10));
    return 1;
}

static void zz_sampler_arm(void)
{
    ZzBuf b; b.n = 0;
    if (!zz_marker("zz_sampler32.on"))
    {
        b_str(&b, "ZZSAMP marker zz_sampler32.on absent, VGPU's 16-sampler table untouched (said once)\r\n");
        b_flush(&b);
        return;
    }
    unsigned char* v = (unsigned char*)GetModuleHandleA("VGPUDX12.dll");
    // The entry as read from the console folder\VGPUDX12.dll (md5 3f002570): mov [rsp+10h],rbx /
    // push rbp,rsi,rdi,r14,r15 / lea rbp,[rsp-2Fh] / sub rsp,100h; and the alloc and the
    // device load it relies on (call 46C24 at 154D8, mov rcx,[5F49B8] at 15518).
    static const unsigned char want[] = { 0x48,0x89,0x5C,0x24,0x10, 0x55, 0x56, 0x57, 0x41,0x56, 0x41,0x57,
                                          0x48,0x8D,0x6C,0x24,0xD1, 0x48,0x81,0xEC,0x00,0x01,0x00,0x00 };
    int ok = v != NULL;
    for (unsigned i = 0; ok && i < sizeof(want); i++) ok = v[ZZ_SAMP_RVA + i] == want[i];
    if (ok) ok = v[0x154D8] == 0xE8 && (INT32)(*(INT32*)(v + 0x154D9)) + 0x154DD == (INT32)ZZ_SAMP_ALLOC;
    if (ok) ok = v[0x15518] == 0x48 && v[0x15519] == 0x8B && v[0x1551A] == 0x0D &&
                 (INT32)(*(INT32*)(v + 0x1551B)) + 0x1551F == (INT32)ZZ_SAMP_DEVICE;
    if (!ok)
    {
        b_str(&b, v ? "ZZSAMP marker present but VGPUDX12+0x153C8 is not the function this fix was read from -- NOT patched\r\n"
                    : "ZZSAMP marker present but VGPUDX12.dll is not loaded -- NOT patched\r\n");
        b_flush(&b);
        return;
    }
    g_sampVgpu = v;
    unsigned char jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };   // jmp qword ptr [rip+0]
    *(UINT64*)(jmp + 6) = (UINT64)(uintptr_t)&zz_sampler_table;
    DWORD old = 0;
    if (!VirtualProtect(v + ZZ_SAMP_RVA, sizeof(jmp), PAGE_EXECUTE_READWRITE, &old))
    {
        b_str(&b, "ZZSAMP VirtualProtect failed -- NOT patched\r\n");
        b_flush(&b);
        return;
    }
    for (unsigned i = 0; i < sizeof(jmp); i++) v[ZZ_SAMP_RVA + i] = jmp[i];
    VirtualProtect(v + ZZ_SAMP_RVA, sizeof(jmp), old, &old);
    FlushInstructionCache(GetCurrentProcess(), v + ZZ_SAMP_RVA, sizeof(jmp));
    b_str(&b, "ZZSAMP marker zz_sampler32.on present -- VGPUDX12+0x153C8 (sampler table, 16 slots) replaced; "
              "tables up to 64 samplers\r\n");
    b_flush(&b);
}

// ⚠ THIS CODE IS IN THE PROCESS TWICE. Emu loads both members of every recompiled
// pair -- the s4b log shows `xeo3_30e4545c_..._ba901301.dll` AND its `_no.dll`
// twin loaded, and they are byte-identical copies, so both carry this file with
// their own statics. Left alone that would mean two vectored handlers (every
// fault logged twice, each instance's dedupe blind to the other's) and two
// CREATE_ALWAYS opens of the same log, the second TRUNCATING the first. So the
// first instance to load claims ownership through a per-process named mutex and
// the other instance stays completely inert.
static int zz_crash_install()
{
    char nm[64];
    int  k = 0;
    const char* pfx = "Local\\zz_crash_owner_";
    while (pfx[k]) { nm[k] = pfx[k]; k++; }
    const unsigned long pid = GetCurrentProcessId();
    static const char* HEXD = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) nm[k++] = HEXD[(pid >> (i * 4)) & 0xF];
    nm[k] = 0;

    HANDLE own = CreateMutexA(NULL, FALSE, nm);
    if (own && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;                    // the twin got here first; do nothing at all

    AddVectoredExceptionHandler(1, zz_veh);

    // Before anything else: DRED must be armed BEFORE the device exists, and
    // this static initialiser is the earliest point we control. It only ever
    // uses GetModuleHandle (loader lock), and it reports which window it was
    // in, so "too late" is measured rather than assumed.
    zz_dred_enable("static-init");

    // s8: says once whether the EDRAM-init fix is armed for this run. The arming
    // itself waits for the device; this only reads the marker, so the inert build
    // is one line and nothing else.
    zz_edram_announce();

    // s9: the same, for the cast fix. Read here rather than in the thunk so a
    // marker file appearing mid-run cannot change behaviour half way through.
    zz_cast_announce();

    // s9 pt3: and for the Map clamp. Same one-shot, same reason.
    zz_map_announce();

    // s9 pt4: and the PC sampler.
    zz_pc_announce();

    // Crackdown s5: VGPU's 16-slot sampler table (patched here, before the guest draws).
    zz_sampler_arm();

    // s9 pt7: the guest-kernel-call instrument. Announced and armed here so an
    // unarmed build is one line and a byte-zero table.
    {
        ZzBuf b; b.n = 0;
        const int on = zz_marker("zz_import_log.on") ? 1 : 0;
        zz_imp_arm(on);
        b_str(&b, on ? "ZZIMP marker zz_import_log.on present -- watching guest "
                       "kernel calls (VdSwap and the wait/signal set)\r\n"
                     : "ZZIMP marker zz_import_log.on absent, not watching "
                       "(said once)\r\n");
        b_flush(&b);
    }

    HANDLE h = CreateThread(NULL, 0, zz_worker, NULL, 0, NULL);
    if (h) CloseHandle(h);
    return 1;
}

static int g_zz_crash_installed = zz_crash_install();
