// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Crackdown s6 (2026-10-14): NFS's VGPU speed patches (work\ppc\zz_vgpu.cpp, s13-s24), ported per
// S6_GRAPHICS_PORT.md section 3 step 3: same code, same VGPUDX12.dll (md5 3f002570); markers <game>_zz_vgpu.on
// (+ _memo2 / _skip / _conv / _copy), log <console folder>\<game>_vgpu.log, no ZZ_VGPU env var, per-pid twin guard,
// the s13 substitute memo disabled. Everything below is NFS's text otherwise.
// s12 pt5: count what the VGPU thread is actually hashing.
//
// WHY THIS EXISTS
// ---------------
// 12 cdb stack samples put the VGPU thread inside a crc32 loop for ~half of
// them -- the single hottest thing in the process while the real GPU sits at
// 11% and the game runs at 7.7 fps. The loop is VGPUDX12 rva 0x44944, a leaf
// with no .pdata entry, signature (rcx = data, rdx = length, r8 = seed), and
// its caller hashes TWO guest ranges back to back (a draw's vertex and index
// buffers). That tells us hashing dominates, but not WHY: either VGPU re-walks
// the same command buffers many times per frame, or the buffers are enormous.
// Calls-per-frame and bytes-per-frame separates those two, and nothing short of
// counting them will.
//
// WHY AN INLINE HOOK AND NOT A BREAKPOINT
// ---------------------------------------
// An INT3 + vectored-handler hook costs a kernel exception round trip per call.
// On a function called (probably) thousands of times per frame that would cost
// far more than the thing being measured -- it would not be an instrument, it
// would be the new bottleneck. An inline jmp costs a few nanoseconds.
//
// SAFETY
// ------
// This patches a Microsoft DLL in-process, so it refuses to act unless it is
// certain: the 7 bytes at the target must match the exact expected opcodes, the
// trampoline must land within +/-2GB (rel32), and the whole thing is OPT-IN via
// ZZ_VGPU=1 so the ordinary play build is never patched. Any failure logs a
// reason and leaves the process untouched.
//
// The 7 patched bytes are three whole instructions -- never split one:
//     0x44944  4C 8B C9        mov r9, rcx
//     0x44947  48 83 FA 20     cmp rdx, 0x20
//     (0x4494b  72 33          jb ...   <- NOT touched; it consumes cmp's flags)
// so the trampoline re-executes mov+cmp and jumps back to 0x4494b with the
// flags freshly set, which is why the counter code saves and restores them.

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <nmmintrin.h>   // SSE4.2 _mm_crc32_* -- the same instruction VGPU uses
#include <windows.h>

#define ZZ_VGPU_RVA      0x44944u
static const unsigned char ZZ_VGPU_SIG[7] = { 0x4C,0x8B,0xC9, 0x48,0x83,0xFA,0x20 };

// s23 MEMO2 (marker zz_vgpu_memo2.on): same per-frame cache, but a MISS calls
// MICROSOFT'S ORIGINAL hash through a relocated-prologue stub, so every value
// handed back is bit-identical to what VGPU would have computed itself. That is
// what the s13 memo lacked: VGPU compares the hash with an EXPECTED value read
// from the resource descriptor (VGPUDX12+0x2C6A8), so a substitute algorithm
// took the mismatch branch and dereferenced a null. A hit additionally checks a
// 3x64-byte fingerprint (head/middle/tail) so a buffer rewritten between two
// draws of the same frame is re-hashed instead of served stale.
typedef unsigned (*zz_orig_hash_fn)(const void*, size_t, unsigned);
static zz_orig_hash_fn g_orig_hash;
static bool g_memo2;

// Read by the fps reporter in zz_profile.cpp so the counts land on the same
// line as the frame rate -- calls-per-frame is the number we are after, and
// correlating two log files after the fact is how you get it wrong.
// ZZ_STATS note (s24): every counter touched per hash/copy call is a PLAIN
// increment on its own 64-byte line, not an Interlocked op. Profile run s24-1
// put 30% of the VGPU thread on the gate's two `lock inc/add` and another 4% on
// the ring's `lock xadd` -- ~800k bus-locked RMWs/s at hundreds of ns each
// (the line was bouncing between cores). These are statistics read by a
// reporter thread; a torn or late read is fine, a locked bus is not.
#define ZZ_ALIGN64 __declspec(align(64))
extern "C" ZZ_ALIGN64 volatile LONG64 zz_hash_calls = 0;
extern "C" ZZ_ALIGN64 volatile LONG64 zz_hash_bytes = 0;

// The frame counter, so a hit can be attributed to the frame it happened in.
// Defined in zz_profile.cpp (bumped on VdSwap).
extern "C" volatile LONG zz_swaps;

// Defined further down; the installer needs its address in memo mode.
extern "C" unsigned long long zz_vgpu_hash(const void* p, size_t len, unsigned seed);

// ---------------------------------------------------------------------------
// The question this ring answers
// ---------------------------------------------------------------------------
// Measured: the collapsed state hashes 1,795 calls and 1,057 MB PER FRAME,
// ~600 KB per call, ~8 GB/s -- a single core's memory bandwidth, which is the
// whole 7.7 fps. Totals cannot say WHY. If those ~1,800 calls hash a handful of
// DISTINCT pointers, the same geometry is being re-hashed once per draw and a
// cache lookup is failing. If they are ~1,800 distinct 600 KB buffers, the
// guest really is submitting that much and the question moves elsewhere.
// So: record (pointer, length, frame) and count distinct pointers per frame.
#define ZZ_HIT_N 16384                  // ~9 frames at 1,800 calls/frame
struct ZzHit { uintptr_t ptr; unsigned len; unsigned frame; };
static ZzHit g_hits[ZZ_HIT_N];
static ZZ_ALIGN64 volatile LONG g_hit_pos;

// Called from the trampoline with the hashed range still in (rcx, rdx).
// Lock-free on purpose: this runs ~14,000 times a second and an instrument
// that contends is an instrument that changes the measurement.
static void zz_ring_record(const void* p, size_t len)
{
    LONG i = g_hit_pos++;   // s24: plain, not locked (see ZZ_STATS note)
    ZzHit& h = g_hits[i & (ZZ_HIT_N - 1)];
    h.ptr   = (uintptr_t)p;
    h.len   = (unsigned)len;
    h.frame = (unsigned)zz_swaps;
}

extern "C" void zz_vgpu_hit(const void* p, size_t len)
{
    zz_hash_calls++; zz_hash_bytes += (LONG64)len;
    zz_ring_record(p, len);
}

static FILE* zz_vgpu_log()
{
    static FILE* f;
    if (!f)
    {
        char self[MAX_PATH] = {0}, path[MAX_PATH * 2];
        HMODULE me = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&zz_vgpu_log, &me);
        GetModuleFileNameA(me, self, sizeof(self));
        const char* bn = strrchr(self, '\\');
        (void)bn;
        _snprintf_s(path, sizeof(path), _TRUNCATE,
                    ZZ_TITLE "_vgpu.log");
        f = fopen(path, "w");
    }
    return f;
}

static void zz_vgpu_say(const char* fmt, ...)
{
    FILE* f = zz_vgpu_log();
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);   // Emu exits without CRT teardown; a buffered tail is lost
}

// Allocate executable memory within rel32 reach of `near`, so the 5-byte jmp
// can address it. A plain VirtualAlloc can land anywhere in the 64-bit space.
static void* zz_alloc_near(uintptr_t near_addr, size_t n)
{
    const uintptr_t GRAN = 0x10000;
    uintptr_t base = near_addr & ~(GRAN - 1);
    for (uintptr_t delta = GRAN; delta < 0x4000000; delta += GRAN)
    {
        for (int dir = 0; dir < 2; dir++)
        {
            uintptr_t try_at = dir ? base + delta : base - delta;
            void* p = VirtualAlloc((void*)try_at, n, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

static void zz_vgpu_install()
{
    HMODULE vgpu = GetModuleHandleA("VGPUDX12.dll");
    if (!vgpu) { zz_vgpu_say("# VGPUDX12.dll not loaded -- no hook"); return; }

    unsigned char* target = (unsigned char*)vgpu + ZZ_VGPU_RVA;
    if (memcmp(target, ZZ_VGPU_SIG, sizeof(ZZ_VGPU_SIG)) != 0)
    {
        zz_vgpu_say("# SIGNATURE MISMATCH at VGPUDX12+%X: %02X %02X %02X %02X %02X %02X %02X"
                    " -- refusing to patch (different VGPUDX12 build?)",
                    ZZ_VGPU_RVA, target[0], target[1], target[2], target[3],
                    target[4], target[5], target[6]);
        return;
    }

    unsigned char* tramp = (unsigned char*)zz_alloc_near((uintptr_t)target, 128);
    if (!tramp) { zz_vgpu_say("# no trampoline within rel32 range -- no hook"); return; }

    // s23 MEMO2 MODE: build a callable copy of the original entry (the two
    // relocated instructions + absolute jump to target+7), then replace the
    // function with zz_vgpu_hash exactly as memo mode does. tramp[0..63] is the
    // replacement stub, tramp[64..] the original-entry stub.
    if (GetFileAttributesA(ZZ_TITLE "_zz_vgpu_memo2.on") != INVALID_FILE_ATTRIBUTES)
    {
        unsigned char* orig = tramp + 64;
        size_t k = 0;
        orig[k++] = 0x4C; orig[k++] = 0x8B; orig[k++] = 0xC9;                 // mov r9, rcx
        orig[k++] = 0x48; orig[k++] = 0x83; orig[k++] = 0xFA; orig[k++] = 0x20; // cmp rdx, 0x20
        orig[k++] = 0xFF; orig[k++] = 0x25; orig[k++] = 0; orig[k++] = 0; orig[k++] = 0; orig[k++] = 0; // jmp [rip+0]
        *(void**)(orig + k) = (void*)(target + 7); k += 8;
        g_orig_hash = (zz_orig_hash_fn)orig;
        g_memo2 = true;

        size_t j = 0;
        tramp[j++] = 0x48; tramp[j++] = 0xB8;           // mov rax, &zz_vgpu_hash
        *(void**)(tramp + j) = (void*)&zz_vgpu_hash; j += 8;
        tramp[j++] = 0xFF; tramp[j++] = 0xE0;           // jmp rax

        intptr_t r = (intptr_t)tramp - ((intptr_t)target + 5);
        if (r > 0x7FFFFFFF || r < -0x7FFFFFFF)
        { zz_vgpu_say("# trampoline out of rel32 range -- no hook"); return; }
        DWORD o = 0;
        if (!VirtualProtect(target, 7, PAGE_EXECUTE_READWRITE, &o))
        { zz_vgpu_say("# VirtualProtect failed (%lu) -- no hook", GetLastError()); return; }
        target[0] = 0xE9;
        *(int*)(target + 1) = (int)r;
        target[5] = 0x90; target[6] = 0x90;
        VirtualProtect(target, 7, o, &o);
        FlushInstructionCache(GetCurrentProcess(), target, 7);
        zz_vgpu_say("# MEMO2 MODE: VGPUDX12+%X -> per-frame memo of MS's ORIGINAL hash "
                    "(orig stub %p, fingerprint-checked hits)", ZZ_VGPU_RVA, (void*)orig);
        return;
    }

    // MEMO MODE: replace the function outright. Our zz_vgpu_hash already takes
    // (rcx, rdx, r8) and returns in rax, so the stub is just an absolute jump
    // and the original code is never executed.
    if (false)   /* Crackdown: the s13 substitute-hash memo is not ported (it took VGPU's mismatch branch) */
    {
        size_t j = 0;
        tramp[j++] = 0x48; tramp[j++] = 0xB8;           // mov rax, &zz_vgpu_hash
        *(void**)(tramp + j) = (void*)&zz_vgpu_hash; j += 8;
        tramp[j++] = 0xFF; tramp[j++] = 0xE0;           // jmp rax

        intptr_t r = (intptr_t)tramp - ((intptr_t)target + 5);
        if (r > 0x7FFFFFFF || r < -0x7FFFFFFF)
        { zz_vgpu_say("# trampoline out of rel32 range -- no hook"); return; }
        DWORD o = 0;
        if (!VirtualProtect(target, 7, PAGE_EXECUTE_READWRITE, &o))
        { zz_vgpu_say("# VirtualProtect failed (%lu) -- no hook", GetLastError()); return; }
        target[0] = 0xE9;
        *(int*)(target + 1) = (int)r;
        target[5] = 0x90; target[6] = 0x90;
        VirtualProtect(target, 7, o, &o);
        FlushInstructionCache(GetCurrentProcess(), target, 7);
        zz_vgpu_say("# MEMO MODE: VGPUDX12+%X replaced by zz_vgpu_hash "
                    "(per-frame cache; watch for stale geometry)", ZZ_VGPU_RVA);
        return;
    }

    // Calls zz_vgpu_hit(rcx = data, rdx = length) -- the hashed range is
    // already in the right registers for the x64 calling convention, so no
    // argument shuffling is needed.
    //
    // Everything the callee is allowed to clobber must be preserved, because
    // the real function still needs rcx/rdx/r8 afterwards: that is rax, rcx,
    // rdx, r8-r11, plus the flags. Stack maths: entry rsp % 16 == 8 (the call
    // pushed a return address); 8 pushes keep it at 8; `sub rsp, 0x28` makes it
    // 0, so the callee sees the ABI-required 8 after its own call pushes. The
    // 0x28 is 32 bytes of mandatory shadow space plus 8 to realign.
    size_t i = 0;
    tramp[i++] = 0x9C;                                  // pushfq
    tramp[i++] = 0x50;                                  // push rax
    tramp[i++] = 0x51;                                  // push rcx
    tramp[i++] = 0x52;                                  // push rdx
    tramp[i++] = 0x41; tramp[i++] = 0x50;               // push r8
    tramp[i++] = 0x41; tramp[i++] = 0x51;               // push r9
    tramp[i++] = 0x41; tramp[i++] = 0x52;               // push r10
    tramp[i++] = 0x41; tramp[i++] = 0x53;               // push r11
    tramp[i++] = 0x48; tramp[i++] = 0x83;
    tramp[i++] = 0xEC; tramp[i++] = 0x28;               // sub rsp, 0x28
    tramp[i++] = 0x48; tramp[i++] = 0xB8;               // mov rax, &zz_vgpu_hit
    *(void**)(tramp + i) = (void*)&zz_vgpu_hit; i += 8;
    tramp[i++] = 0xFF; tramp[i++] = 0xD0;               // call rax
    tramp[i++] = 0x48; tramp[i++] = 0x83;
    tramp[i++] = 0xC4; tramp[i++] = 0x28;               // add rsp, 0x28
    tramp[i++] = 0x41; tramp[i++] = 0x5B;               // pop r11
    tramp[i++] = 0x41; tramp[i++] = 0x5A;               // pop r10
    tramp[i++] = 0x41; tramp[i++] = 0x59;               // pop r9
    tramp[i++] = 0x41; tramp[i++] = 0x58;               // pop r8
    tramp[i++] = 0x5A;                                  // pop rdx
    tramp[i++] = 0x59;                                  // pop rcx
    tramp[i++] = 0x58;                                  // pop rax
    tramp[i++] = 0x9D;                                  // popfq
    // the two relocated instructions, verbatim
    tramp[i++] = 0x4C; tramp[i++] = 0x8B; tramp[i++] = 0xC9;               // mov r9, rcx
    tramp[i++] = 0x48; tramp[i++] = 0x83; tramp[i++] = 0xFA; tramp[i++] = 0x20; // cmp rdx, 0x20
    // jmp [rip+0] -> absolute back to the instruction after the patch
    tramp[i++] = 0xFF; tramp[i++] = 0x25;
    tramp[i++] = 0x00; tramp[i++] = 0x00; tramp[i++] = 0x00; tramp[i++] = 0x00;
    *(void**)(tramp + i) = (void*)(target + 7); i += 8;

    intptr_t rel = (intptr_t)tramp - ((intptr_t)target + 5);
    if (rel > 0x7FFFFFFF || rel < -0x7FFFFFFF)
    { zz_vgpu_say("# trampoline out of rel32 range -- no hook"); return; }

    DWORD old = 0;
    if (!VirtualProtect(target, 7, PAGE_EXECUTE_READWRITE, &old))
    { zz_vgpu_say("# VirtualProtect failed (%lu) -- no hook", GetLastError()); return; }
    target[0] = 0xE9;
    *(int*)(target + 1) = (int)rel;
    target[5] = 0x90; target[6] = 0x90;
    VirtualProtect(target, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 7);

    zz_vgpu_say("# hooked VGPUDX12+%X (module %p, trampoline %p)",
                ZZ_VGPU_RVA, (void*)vgpu, (void*)tramp);
}


// ---------------------------------------------------------------------------
// s23 COPY PROFILE (marker zz_vgpu_copy.on): who calls the endian-swap copy?
// VGPUDX12+0x269E8 is the hottest VGPU code in the race profile (~1/3 of the
// VGPU thread): (rcx=?, edx=guest addr, r8=host dst, r9=len, [rsp+0x28]=mode).
// Its first two instructions are 10 relocatable bytes
//     48 89 5C 24 08   mov [rsp+8], rbx
//     48 89 74 24 10   mov [rsp+0x10], rsi
// so: jmp trampoline (5 B) + 5 nops; the trampoline calls the gate below and
// either re-executes the two movs and jumps to +0x269F2, or (gate said skip)
// simply `ret`s to the caller with the copy not done. Per second the reporter
// prints the top return addresses by bytes and calls.
//
// s24 COPY SKIP (marker zz_vgpu_skip.on) -- the 60 fps patch. READ FROM THE
// DLL, not guessed: the two hot callers (+0x77817 / +0x778DB inside the
// "UploadBuffer" method 0x776DC, and the same pair again in its sibling
// 0x77D8C at +0x77EE6) copy the draw's vertex and index buffers into TWO
// std::vector<uint8_t> scratch buffers ({begin,end,cap} triples at
// VGPU+0x5F48F8 and +0x5F4910; 0x675b0 is vector::_Reallocate, 0x52694 =
// memset, 0x56FD0.. = the atexit dtor thunks). There is NO ring: .text holds
// no store to the begin pointer at all, only to `end` (resize). So every draw
// bswap-copies its whole VB (256 KB) and IB (128 KB) to the SAME two host
// addresses -- 960x per frame for the same two guest buffers = 360 MB/frame,
// 14.5 GB/s, the entire VGPU thread. The copy is redundant exactly when the
// last copy into that vector was the same (guest addr, len, mode) and the
// guest bytes have not changed since. That is what the gate checks:
//   - dst must be vec1.data() or vec2.data() (read live from the globals);
//     any other caller/destination passes through untouched -- an advancing
//     upload ring could be overlapped by other writes and must NOT be skipped;
//   - same (guest, len, mode) as the previous copy into that vector,
//   - same VdSwap frame (zz_swaps) -- so a stale skip can never outlive a
//     frame; the win is intra-frame anyway (960 -> ~1 copy per buffer per frame),
//   - the guest fingerprint (16 x 64 B, head/tail included) matches.
// The 0x44944 hash that follows the copy then runs on unchanged bytes and hits
// the MEMO2 cache; MS's own "hash == last hash -> skip processing" branch does
// the rest. Failure mode if the fingerprint ever misses a rewrite: one draw
// with the previous contents of that buffer (stale geometry, no pointers
// involved) -- the SAME exposure MEMO2 already had, since it too keys on this
// fixed data() address and relies on its fingerprint (which s24 widened from
// 3 to 16 sample points for both).
// ---------------------------------------------------------------------------
#define ZZ_COPY_RVA 0x269E8u
#define ZZ_VEC1_RVA 0x5F48F8u     // std::vector<uint8_t> for the vertex buffer copy (begin ptr)
#define ZZ_VEC2_RVA 0x5F4910u     // ... for the index buffer copy
#define ZZ_GBASE_RVA 0x5F48C0u    // -> pointer to the guest base (0x269F7: r11 = (edx&0x1FFFFFFF) + [[rip+..]])
static const unsigned char ZZ_COPY_SIG[10] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x74,0x24,0x10 };
struct ZzCopySite { uintptr_t ret; volatile LONG64 calls, bytes; };
static ZzCopySite g_copy[64];
static volatile LONG g_copy_n;
static bool g_copy_prof, g_copy_skip;
static unsigned char* g_vgpu_base;
extern "C" ZZ_ALIGN64 volatile LONG64 zz_copy_calls = 0;
extern "C" ZZ_ALIGN64 volatile LONG64 zz_copy_bytes = 0;
extern "C" ZZ_ALIGN64 volatile LONG64 zz_copy_skips = 0;
extern "C" ZZ_ALIGN64 volatile LONG64 zz_copy_skip_bytes = 0;
// s24: VGPUDX12+0x7C61C is a WAIT_REG_MEM-style poll loop (read a guest dword
// through the copy fn at +0x7C67B, test it via 0x7C4CC, yield, retry with a
// QPC timeout). Its poll count per second is "how much VGPU sat waiting" --
// counted here in profile mode (all copies reach the gate) and printed on the
// fps line by zz_profile.cpp so it lines up with the frame rate.
extern "C" ZZ_ALIGN64 volatile LONG64 zz_wait_polls = 0;
#define ZZ_WAIT_RET_RVA 0x7C680u

// One entry per scratch vector: what the vector holds right now.
struct ZzCopyMemo { unsigned guest, len, mode, fp, gen; unsigned valid; unsigned guarded, unguarded; };
static ZzCopyMemo g_copy_memo[2];

// s24 texture census: which (guest addr, len) go through the CPU texture
// translator's scratch vectors, how often, and how many of those calls the
// skip could drop. Small linear table, VGPU thread only, printed per second.
#define ZZ_TEX_N 96
struct ZzTex { unsigned guest, len, mode; unsigned calls, skips; };
static ZzTex g_tex[ZZ_TEX_N]; static int g_tex_n; static unsigned g_tex_overflow;
static void zz_tex_note(unsigned guest, unsigned len, unsigned mode, int skipped)
{
    for (int i = 0; i < g_tex_n; i++)
        if (g_tex[i].guest == guest && g_tex[i].len == len) { g_tex[i].calls++; g_tex[i].skips += skipped; return; }
    if (g_tex_n < ZZ_TEX_N) { ZzTex& t = g_tex[g_tex_n++]; t.guest = guest; t.len = len; t.mode = mode; t.calls = 1; t.skips = skipped; }
    else g_tex_overflow++;
}

static inline unsigned zz_fp(const unsigned char* s, size_t len);

// The MEMO2 hash cache (defined with the hash code below, declared here because
// the gate consults it): VGPU hashes the scratch vector right after the copy
// and BEFORE the per-stream processing loop, and MEMO2 fingerprints the vector
// at that moment. So a MEMO2 entry for (dst, len) from this frame is a snapshot
// of "what the vector held straight after the last copy" -- comparing the
// vector's fingerprint NOW against it proves the processing loop did not
// modify the vector in place (read from the DLL it does not, but this makes
// the skip refuse rather than corrupt if that reading is wrong). Same thread
// (VGPU) writes both, so the TLS table is the right one to look in.
struct ZzMemo { uintptr_t ptr; unsigned len; unsigned seed; unsigned gen; unsigned val; unsigned valid; unsigned fp; };
#define ZZ_MEMO_N 64
static __declspec(thread) ZzMemo t_memo[ZZ_MEMO_N];
static bool zz_vec_unchanged_since_hash(const unsigned char* dst, size_t len, unsigned gen, bool* found)
{
    *found = false;
    for (int k = 0; k < ZZ_MEMO_N; k++)
    {
        const ZzMemo& c = t_memo[k];
        if (c.valid && c.gen == gen && c.ptr == (uintptr_t)dst && c.len == (unsigned)len)
        { *found = true; return zz_fp(dst, len) == c.fp; }
    }
    return true;   // no snapshot to compare against (MEMO2 off / evicted): caller decides
}

// Returns nonzero when the copy may be skipped (dst already holds these bytes).
extern "C" int zz_copy_gate(uintptr_t ret, unsigned guest, unsigned char* dst, size_t len, unsigned mode)
{
    zz_copy_calls++; zz_copy_bytes += (LONG64)len;
    if (g_copy_prof)
    {
        if (ret == (uintptr_t)(g_vgpu_base + ZZ_WAIT_RET_RVA)) zz_wait_polls++;
        LONG n = g_copy_n; bool found = false;
        for (LONG i = 0; i < n; i++)
            if (g_copy[i].ret == ret) { g_copy[i].calls++; g_copy[i].bytes += (LONG64)len; found = true; break; }
        if (!found && n < 64)
        {
            LONG k = InterlockedIncrement(&g_copy_n) - 1;
            if (k < 64) { g_copy[k].ret = ret; g_copy[k].calls = 1; g_copy[k].bytes = (LONG64)len; }
        }
    }
    if (!g_copy_skip || len == 0) return 0;

    int slot;
    if      (dst == *(unsigned char**)(g_vgpu_base + ZZ_VEC1_RVA)) slot = 0;
    else if (dst == *(unsigned char**)(g_vgpu_base + ZZ_VEC2_RVA)) slot = 1;
    else return 0;

    unsigned char* gbase = *(unsigned char**)*(void**)(g_vgpu_base + ZZ_GBASE_RVA);
    const unsigned char* src = gbase + (guest & 0x1FFFFFFFu);
    unsigned fp = zz_fp(src, len);
    unsigned gen = (unsigned)zz_swaps;
    ZzCopyMemo& m = g_copy_memo[slot];
    if (m.valid && m.guest == guest && m.len == (unsigned)len && m.mode == mode && m.gen == gen && m.fp == fp)
    {
        bool found;
        if (zz_vec_unchanged_since_hash(dst, len, gen, &found))
        {
            if (found) m.guarded++; else m.unguarded++;
            zz_copy_skips++; zz_copy_skip_bytes += (LONG64)len;
            zz_tex_note(guest, (unsigned)len, mode, 1);
            return 1;
        }
        // The vector no longer holds what the hash saw -> something wrote it
        // after the copy. Re-copy, and say so once (this would be new info).
        static LONG said; if (InterlockedIncrement(&said) <= 3)
            zz_vgpu_say("  copy skip: vector %d modified after its hash (guest %08X len %u) -- re-copying, NOT skipping", slot + 1, guest, (unsigned)len);
    }
    zz_tex_note(guest, (unsigned)len, mode, 0);
    // Record BEFORE the copy runs (we return 0 and the original code copies):
    // the next call sees a vector that holds exactly these bytes.
    m.guest = guest; m.len = (unsigned)len; m.mode = mode; m.gen = gen; m.fp = fp; m.valid = 1;
    return 0;
}
// s24 PROCESSING CENSUS: 0x776DC only does its per-level conversion/upload
// loop when the two hashes differ from [this+0x24]/[+0x28] (the jne/je at
// 0x7796C/0x77972); 0x77978 is the first instruction of that path. Hook it to
// count how often the expensive path runs, for which guest ranges, and how
// many distinct translator objects (`this` = r15) are involved -- if two
// ranges alternate through ONE object, MS's single-entry cache never hits.
#define ZZ_PROC_RVA 0x77978u
static const unsigned char ZZ_PROC_SIG[7] = { 0x4D,0x8B,0x4F,0x08, 0x48,0x8B,0xCB };
struct ZzProc { uintptr_t self; unsigned guest, len; unsigned n; };
static ZzProc g_proc[32]; static int g_proc_n; static unsigned g_proc_total, g_proc_over;
extern "C" void zz_proc_hit(uintptr_t self, const unsigned char* desc)
{
    unsigned guest = *(const unsigned*)(desc + 4), len = *(const unsigned*)(desc + 8);
    g_proc_total++;
    for (int i = 0; i < g_proc_n; i++)
        if (g_proc[i].self == self && g_proc[i].guest == guest) { g_proc[i].n++; return; }
    if (g_proc_n < 32) { ZzProc& q = g_proc[g_proc_n++]; q.self = self; q.guest = guest; q.len = len; q.n = 1; }
    else g_proc_over++;
}
static void zz_proc_install()
{
    unsigned char* target = g_vgpu_base + ZZ_PROC_RVA;
    if (memcmp(target, ZZ_PROC_SIG, 7) != 0) { zz_vgpu_say("# proc census: signature mismatch, not patched"); return; }
    unsigned char* t = (unsigned char*)zz_alloc_near((uintptr_t)target, 128);
    if (!t) return;
    size_t i = 0;
    t[i++]=0x9C; t[i++]=0x50; t[i++]=0x51; t[i++]=0x52;                       // pushfq; push rax, rcx, rdx
    t[i++]=0x41;t[i++]=0x50; t[i++]=0x41;t[i++]=0x51; t[i++]=0x41;t[i++]=0x52; t[i++]=0x41;t[i++]=0x53; // push r8-r11
    t[i++]=0x4C;t[i++]=0x89;t[i++]=0xF9;                                       // mov rcx, r15 (this)
    t[i++]=0x48;t[i++]=0x89;t[i++]=0xDA;                                       // mov rdx, rbx (desc)
    t[i++]=0x48;t[i++]=0x83;t[i++]=0xEC;t[i++]=0x28;                           // sub rsp,0x28
    t[i++]=0x48;t[i++]=0xB8; *(void**)(t+i)=(void*)&zz_proc_hit; i+=8;         // mov rax,&hit
    t[i++]=0xFF;t[i++]=0xD0;                                                   // call rax
    t[i++]=0x48;t[i++]=0x83;t[i++]=0xC4;t[i++]=0x28;                           // add rsp,0x28
    t[i++]=0x41;t[i++]=0x5B; t[i++]=0x41;t[i++]=0x5A; t[i++]=0x41;t[i++]=0x59; t[i++]=0x41;t[i++]=0x58; // pop r11-r8
    t[i++]=0x5A; t[i++]=0x59; t[i++]=0x58; t[i++]=0x9D;                        // pop rdx, rcx, rax; popfq
    memcpy(t+i, ZZ_PROC_SIG, 7); i+=7;
    t[i++]=0xFF;t[i++]=0x25;t[i++]=0;t[i++]=0;t[i++]=0;t[i++]=0; *(void**)(t+i)=(void*)(target+7); i+=8;
    intptr_t rel=(intptr_t)t-((intptr_t)target+5);
    if (rel>0x7FFFFFFF||rel<-0x7FFFFFFF) return;
    DWORD o=0; if(!VirtualProtect(target,7,PAGE_EXECUTE_READWRITE,&o)) return;
    target[0]=0xE9; *(int*)(target+1)=(int)rel; target[5]=0x90; target[6]=0x90;
    VirtualProtect(target,7,o,&o); FlushInstructionCache(GetCurrentProcess(),target,7);
    zz_vgpu_say("# PROC CENSUS: hooked VGPUDX12+%X (translator processing path)", ZZ_PROC_RVA);
}
// s24 CONVERTER REPLACEMENT (marker zz_vgpu_conv.on). VGPUDX12+0x10599C is
// 17% of the VGPU thread (profile runs s24-2/3): the texture-format converter
// the CPU translator applies to the two render-to-texture surfaces that change
// ~6x per frame (0x06748000/0x067A8000, 256 KB each). Read in full: three
// nested loops (slices, rows, elements) whose element body zeroes the 4 dst
// bytes and ORs in src[0], src[1], src[2]<<16, src[3]<<24 -- dst dword = src
// dword. A per-component template instantiated for an identity 4x8-bit format,
// ~12 memory ops per pixel. Signature (x64 ABI, verified from the prologue's
// home-slot use): (rcx=w, rdx=h, r8=depth, r9=srcPitch, [rsp+28]=srcSlicePitch,
// [30]=src, [38]=dstPitch, [40]=dstSlicePitch, [48]=dst); returns nothing the
// callers use (rax is left holding a loop temp). Replacement = a row memcpy
// with the same pitches; the entry is overwritten with an absolute jump.
#define ZZ_CONV_RVA 0x10599Cu
static const unsigned char ZZ_CONV_SIG[16] = { 0x4D,0x85,0xC0, 0x0F,0x84,0xF6,0x00,0x00,0x00, 0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08 };
extern "C" ZZ_ALIGN64 volatile LONG64 zz_conv_calls = 0, zz_conv_bytes = 0;
extern "C" void zz_conv_copy4(size_t w, size_t h, size_t d, size_t srcPitch, size_t srcSlice,
                              const unsigned char* src, size_t dstPitch, size_t dstSlice, unsigned char* dst)
{
    zz_conv_calls++; zz_conv_bytes += (LONG64)(w * 4 * h * d);
    const size_t row = w * 4;
    for (size_t z = 0; z < d; z++)
    {
        const unsigned char* sp = src + z * srcSlice; unsigned char* dp = dst + z * dstSlice;
        if (srcPitch == row && dstPitch == row) { memcpy(dp, sp, row * h); continue; }
        for (size_t y = 0; y < h; y++, sp += srcPitch, dp += dstPitch) memcpy(dp, sp, row);
    }
}
static void zz_conv_install()
{
    unsigned char* target = g_vgpu_base + ZZ_CONV_RVA;
    if (memcmp(target, ZZ_CONV_SIG, 16) != 0) { zz_vgpu_say("# conv: signature mismatch at VGPUDX12+%X, not patched", ZZ_CONV_RVA); return; }
    unsigned char code[12]; size_t i = 0;
    code[i++]=0x48; code[i++]=0xB8; *(void**)(code+i)=(void*)&zz_conv_copy4; i+=8;   // mov rax, imm64
    code[i++]=0xFF; code[i++]=0xE0;                                                 // jmp rax
    DWORD o=0; if(!VirtualProtect(target,12,PAGE_EXECUTE_READWRITE,&o)) { zz_vgpu_say("# conv: VirtualProtect failed"); return; }
    memcpy(target, code, 12);
    VirtualProtect(target,12,o,&o); FlushInstructionCache(GetCurrentProcess(),target,12);
    zz_vgpu_say("# CONV: VGPUDX12+%X (identity 4x8-bit texture converter) replaced by row memcpy", ZZ_CONV_RVA);
}
static void zz_copy_install()
{
    HMODULE vgpu = GetModuleHandleA("VGPUDX12.dll");
    g_vgpu_base = (unsigned char*)vgpu;
    unsigned char* target = (unsigned char*)vgpu + ZZ_COPY_RVA;
    if (memcmp(target, ZZ_COPY_SIG, sizeof(ZZ_COPY_SIG)) != 0) { zz_vgpu_say("# copy hook: signature mismatch, not patched"); return; }
    // The vector globals are only trusted if the code that uses them is the
    // build we read: check the two `mov r8,[rip+..]` loads of vec1/vec2.begin
    // in 0x776DC (0x777F7 and 0x778BB) and the guest-base load in the copy fn.
    static const unsigned char SIG_777F7[7] = { 0x4C,0x8B,0x05,0xFA,0xD0,0x57,0x00 };
    static const unsigned char SIG_778BB[7] = { 0x4C,0x8B,0x05,0x4E,0xD0,0x57,0x00 };
    static const unsigned char SIG_269F7[7] = { 0x48,0x8B,0x05,0xC2,0xDE,0x5C,0x00 };
    bool vec_ok = memcmp((unsigned char*)vgpu + 0x777F7, SIG_777F7, 7) == 0 &&
                  memcmp((unsigned char*)vgpu + 0x778BB, SIG_778BB, 7) == 0 &&
                  memcmp((unsigned char*)vgpu + 0x269F7, SIG_269F7, 7) == 0;
    if (g_copy_skip && !vec_ok) { zz_vgpu_say("# copy skip: vector/guest-base signatures mismatch -- skip DISABLED, profile only"); g_copy_skip = false; }
    unsigned char* t = (unsigned char*)zz_alloc_near((uintptr_t)target, 256);
    if (!t) { zz_vgpu_say("# copy hook: no near memory"); return; }
    size_t i = 0;
    // s24 FAST PATH (skip mode without profiling): the copy fn is called up to
    // 460k times/s, ~95% of them tiny copies to other destinations; routing all
    // of them through the C gate cost ~15% of the VGPU thread (run s24-5). So
    // compare dst (r8) with the two vector begin pointers right here and only
    // enter the gate on a match; everything else pays 2 loads + 2 compares.
    // rax and flags are dead at the original entry (verified in the disasm).
    size_t je1 = 0, je2 = 0;
    if (!g_copy_prof)
    {
        t[i++]=0x48;t[i++]=0xB8; *(void**)(t+i)=(void*)(g_vgpu_base + ZZ_VEC1_RVA); i+=8;  // mov rax, &vec1.begin
        t[i++]=0x4C;t[i++]=0x3B;t[i++]=0x00;                                            // cmp r8,[rax]
        t[i++]=0x74; je1 = i; t[i++]=0x00;                                              // je slow
        t[i++]=0x48;t[i++]=0xB8; *(void**)(t+i)=(void*)(g_vgpu_base + ZZ_VEC2_RVA); i+=8;  // mov rax, &vec2.begin
        t[i++]=0x4C;t[i++]=0x3B;t[i++]=0x00;                                            // cmp r8,[rax]
        t[i++]=0x74; je2 = i; t[i++]=0x00;                                              // je slow
        memcpy(t+i, ZZ_COPY_SIG, 10); i+=10;                                            // relocated movs
        t[i++]=0xFF;t[i++]=0x25;t[i++]=0;t[i++]=0;t[i++]=0;t[i++]=0; *(void**)(t+i)=(void*)(target+10); i+=8; // jmp back
        t[je1] = (unsigned char)(i - (je1 + 1));
        t[je2] = (unsigned char)(i - (je2 + 1));
    }
    // slow: the C gate.
    // The original function writes rax/rcx/r10/r11 before reading them and
    // does not depend on incoming flags, so only the argument registers need
    // preserving. Entry: [rsp]=return address, [rsp+0x28]=mode (5th arg).
    // After 4 pushes: [rsp+0x20]=return address, [rsp+0x48]=mode.
    t[i++]=0x51; t[i++]=0x52;                                       // push rcx, rdx
    t[i++]=0x41;t[i++]=0x50; t[i++]=0x41;t[i++]=0x51;               // push r8, r9
    t[i++]=0x4C;t[i++]=0x8B;t[i++]=0x54;t[i++]=0x24;t[i++]=0x48;    // mov r10,[rsp+0x48]  (mode)
    t[i++]=0x48;t[i++]=0x8B;t[i++]=0x4C;t[i++]=0x24;t[i++]=0x20;    // mov rcx,[rsp+0x20]  (return address)
    // rdx = guest, r8 = dst, r9 = len: already in place
    t[i++]=0x48;t[i++]=0x83;t[i++]=0xEC;t[i++]=0x28;                // sub rsp,0x28
    t[i++]=0x4C;t[i++]=0x89;t[i++]=0x54;t[i++]=0x24;t[i++]=0x20;    // mov [rsp+0x20],r10  (5th arg)
    t[i++]=0x48;t[i++]=0xB8; *(void**)(t+i)=(void*)&zz_copy_gate; i+=8;  // mov rax,&gate
    t[i++]=0xFF;t[i++]=0xD0;                                        // call rax
    t[i++]=0x48;t[i++]=0x83;t[i++]=0xC4;t[i++]=0x28;                // add rsp,0x28
    t[i++]=0x41;t[i++]=0x59; t[i++]=0x41;t[i++]=0x58;               // pop r9, r8
    t[i++]=0x5A; t[i++]=0x59;                                       // pop rdx, rcx
    t[i++]=0x85;t[i++]=0xC0;                                        // test eax,eax
    t[i++]=0x75; size_t jz = i; t[i++]=0x00;                        // jnz skip (patched below)
    memcpy(t+i, ZZ_COPY_SIG, 10); i+=10;           // the two relocated movs
    t[i++]=0xFF;t[i++]=0x25;t[i++]=0;t[i++]=0;t[i++]=0;t[i++]=0; *(void**)(t+i)=(void*)(target+10); i+=8; // jmp [rip+0]
    t[jz] = (unsigned char)(i - (jz + 1));         // skip: -> here
    t[i++]=0xC3;                                                    // ret  (rsp -> caller's return address)
    intptr_t rel=(intptr_t)t-((intptr_t)target+5);
    if (rel>0x7FFFFFFF||rel<-0x7FFFFFFF){ zz_vgpu_say("# copy hook: out of range"); return; }
    DWORD o=0; if(!VirtualProtect(target,10,PAGE_EXECUTE_READWRITE,&o)){ zz_vgpu_say("# copy hook: VirtualProtect failed"); return; }
    target[0]=0xE9; *(int*)(target+1)=(int)rel; for(int k=5;k<10;k++) target[k]=0x90;
    VirtualProtect(target,10,o,&o); FlushInstructionCache(GetCurrentProcess(),target,10);
    zz_vgpu_say("# COPY HOOK at VGPUDX12+%X (bswap copy): profile=%d skip=%d (skip = redundant re-copies of the same guest texture into the two scratch vectors are dropped; without profile mode only vector-destination copies reach the gate and its counters)", ZZ_COPY_RVA, (int)g_copy_prof, (int)g_copy_skip);
    if (g_copy_skip) zz_proc_install();
}
static DWORD WINAPI zz_copy_reporter(LPVOID)
{
    HMODULE vgpu = GetModuleHandleA("VGPUDX12.dll");
    LONG64 pc=0,pb=0,ps=0,psb=0;
    for(;;){
        Sleep(1000);
        LONG64 c=zz_copy_calls,b=zz_copy_bytes,s=zz_copy_skips,sb=zz_copy_skip_bytes;
        LONG64 dc=c-pc,db=b-pb,ds=s-ps,dsb=sb-psb; pc=c;pb=b;ps=s;psb=sb;
        if(!dc) continue;
        zz_vgpu_say("  copy/s: %lld calls, %.1f MB asked | %lld skipped (%.1f%%), %.1f MB NOT copied | %.1f MB actually copied"
                    " | skips guarded by a MEMO2 snapshot: VB %u/%u IB %u/%u (cum)",
                    (long long)dc, (double)db/(1024.0*1024.0), (long long)ds, dc?100.0*(double)ds/(double)dc:0.0,
                    (double)dsb/(1024.0*1024.0), (double)(db-dsb)/(1024.0*1024.0),
                    g_copy_memo[0].guarded, g_copy_memo[0].guarded + g_copy_memo[0].unguarded,
                    g_copy_memo[1].guarded, g_copy_memo[1].guarded + g_copy_memo[1].unguarded);
        if (g_copy_skip)
        {
            // texture census: top 10 by calls in the last second, then reset
            int n = g_tex_n; if (n > ZZ_TEX_N) n = ZZ_TEX_N;
            zz_vgpu_say("  textures via CPU translator this second: %d distinct (+%u overflow); top by calls (guest len mode calls skipped):", n, g_tex_overflow);
            for (int rank = 0; rank < 10 && rank < n; rank++)
            {
                int bi = -1; unsigned best = 0;
                for (int i = 0; i < n; i++) if (g_tex[i].calls > best) { best = g_tex[i].calls; bi = i; }
                if (bi < 0) break;
                zz_vgpu_say("    %08X %7u m%u  x%-6u skipped %u", g_tex[bi].guest, g_tex[bi].len, g_tex[bi].mode, g_tex[bi].calls, g_tex[bi].skips);
                g_tex[bi].calls = 0;
            }
            g_tex_n = 0; g_tex_overflow = 0;   // racy vs the VGPU thread; a lost count is fine
            static unsigned pt; unsigned tot = g_proc_total; 
            zz_vgpu_say("  translator PROCESSING path ran %u times this second; %d distinct (this,guest) (+%u overflow):", tot - pt, g_proc_n, g_proc_over); pt = tot;
            for (int i = 0; i < g_proc_n && i < 32; i++)
            { zz_vgpu_say("    this %p guest %08X len %7u  x%u", (void*)g_proc[i].self, g_proc[i].guest, g_proc[i].len, g_proc[i].n); g_proc[i].n = 0; }
            g_proc_n = 0; g_proc_over = 0;
            static LONG64 pcc, pcb; LONG64 cc = zz_conv_calls, cb = zz_conv_bytes;
            if (cc != pcc) zz_vgpu_say("  conv replacement: %lld calls, %.1f MB this second", (long long)(cc - pcc), (double)(cb - pcb) / (1024.0 * 1024.0));
            pcc = cc; pcb = cb;
        }
        if(!g_copy_prof) continue;
        LONG n=g_copy_n; if(n>64)n=64;
        // top 6 by bytes (cumulative)
        for(int rank=0;rank<6;rank++){ LONG64 best=-1; int bi=-1; for(LONG i=0;i<n;i++){ if(g_copy[i].bytes>best){ bool used=false; /* skip already printed by marking negative? simple: allow repeats via threshold */ best=g_copy[i].bytes; bi=i; } }
            if(bi<0) break; ZzCopySite& s=g_copy[bi];
            zz_vgpu_say("    ret VGPUDX12+%llX  calls %lld  bytes %.1f MB (cum)", (unsigned long long)(s.ret-(uintptr_t)vgpu), (long long)s.calls, (double)s.bytes/(1024.0*1024.0));
            s.bytes = -s.bytes; }  // mark printed
        for(LONG i=0;i<n;i++) if(g_copy[i].bytes<0) g_copy[i].bytes=-g_copy[i].bytes; // restore
    }
    return 0;
}

// VGPUDX12 is loaded by Emu after our module's static init, so poll for it
// rather than patching at load time and finding nothing.
static DWORD WINAPI zz_vgpu_thread(LPVOID)
{
    for (int i = 0; i < 600; i++)   // up to ~60 s
    {
        if (GetModuleHandleA("VGPUDX12.dll"))
        {
            zz_vgpu_install();
            g_copy_prof = GetFileAttributesA(ZZ_TITLE "_zz_vgpu_copy.on") != INVALID_FILE_ATTRIBUTES;
            g_copy_skip = GetFileAttributesA(ZZ_TITLE "_zz_vgpu_skip.on") != INVALID_FILE_ATTRIBUTES;
            g_vgpu_base = (unsigned char*)GetModuleHandleA("VGPUDX12.dll");
            if (g_copy_prof || g_copy_skip)
            { zz_copy_install(); CloseHandle(CreateThread(nullptr, 0, zz_copy_reporter, nullptr, 0, nullptr)); }
            if (GetFileAttributesA(ZZ_TITLE "_zz_vgpu_conv.on") != INVALID_FILE_ATTRIBUTES) zz_conv_install();
            return 0;
        }
        Sleep(100);
    }
    zz_vgpu_say("# VGPUDX12.dll never appeared -- no hook");
    return 0;
}

// Sort by (ptr, len) so both questions fall out of one pass: how many distinct
// BUFFERS there are, and how many distinct (buffer, length) RANGES.
//
// That second number is the one that decides the next move. 46 distinct buffers
// absorbing ~1,790 calls proves the same memory is hashed over and over, but if
// each of those repeats covers a DIFFERENT sub-range then a cache keyed on
// (ptr,len) would never hit and memoising is pointless. If ranges == buffers,
// the calls are bit-for-bit identical work and memoising is exactly right.
// ---------------------------------------------------------------------------
// THE FIX EXPERIMENT: memoise the hash (marker file zz_vgpu_memo.on)
// ---------------------------------------------------------------------------
// Measured: 1,790 calls/frame cover 48 distinct (ptr,len) ranges; the four hot
// ones are each recomputed ~433 times per frame, bit for bit. Hashing each
// range once per frame instead turns ~1.05 GB/frame into ~2.5 MB.
//
// This REPLACES the function rather than wrapping it, which is what makes it
// simple: the result is only ever used as a content key for VGPU's own lookup
// tables, so it does not have to equal Microsoft's CRC -- it only has to be
// deterministic and collision-resistant. Same instruction (CRC32C), same
// seeding, so in practice it is the same value anyway.
//
// THE RISK, stated plainly: the cache is invalidated per frame (keyed on the
// VdSwap counter). If the guest rewrites one of these buffers BETWEEN draws in
// the same frame, VGPU would re-hash to notice and we would hand back a stale
// key -- which shows up immediately as wrong geometry on screen. That is the
// thing to watch for when this is enabled.
// (struct ZzMemo / t_memo are declared above the copy gate, which reads them.)

// Fingerprint: 16 x 64-byte samples spread evenly from head to tail (s24;
// was 3 -- head/middle/tail). Both the MEMO2 hash cache and the s24 copy skip
// stand on this: each keys on a FIXED host address (the scratch vector's
// data()) whose contents change from draw to draw, so the fingerprint is the
// only thing that tells "same buffer again" from "different buffer at the same
// address". 1 KB read per call vs the 128-256 KB copy/hash it avoids.
#define ZZ_FP_N 16
static inline unsigned zz_fp(const unsigned char* s, size_t len)
{
    unsigned long long c = 0xFFFFFFFFu;
    if (len >= ZZ_FP_N * 64)
    {
        const size_t span = len - 64;              // last sample ends exactly at the tail
        for (int k = 0; k < ZZ_FP_N; k++)
        {
            const unsigned char* q = s + ((span * (size_t)k / (ZZ_FP_N - 1)) & ~(size_t)7);
            for (int i = 0; i < 64; i += 8)
                c = _mm_crc32_u64(c, *(const unsigned long long*)(q + i));
        }
    }
    else
    {
        size_t n = len;
        while (n >= 8) { c = _mm_crc32_u64(c, *(const unsigned long long*)s); s += 8; n -= 8; }
        while (n--)    { c = _mm_crc32_u8((unsigned)c, *s++); }
    }
    return (unsigned)c;
}
// 64 entries, LINEARLY SCANNED -- deliberately not a hash table.
//
// The first attempt was direct-mapped on ((ptr>>4) ^ len) & 255 and measured a
// 0.8% hit rate: every hot length is a multiple of 64 KB (0x40000, 0x180000,
// 0x20000, 0x90000), so `len` contributed NOTHING to the index and the two
// ranges of each buffer collided and evicted each other on every alternating
// call. Only ~48 distinct ranges are live per frame, so a 64-entry linear scan
// holds all of them, cannot collide by construction, and costs far less than
// the megabytes of crc32 it avoids.
// Per-thread (t_memo, declared above), so there is no lock and no torn read:
// a shared table would need either a lock on a path called 14,000 times a
// second, or a 128-bit CAS.

extern "C" ZZ_ALIGN64 volatile LONG64 zz_hash_hits = 0;   // calls served from the cache
// Bytes actually run through crc32. zz_hash_bytes counts what was ASKED for
// (hits included), so in memo mode the two diverge -- and that gap IS the win.
extern "C" ZZ_ALIGN64 volatile LONG64 zz_hash_real_bytes = 0;

extern "C" unsigned long long zz_vgpu_hash(const void* p, size_t len, unsigned seed)
{
    zz_hash_calls++; zz_hash_bytes += (LONG64)len;
    // s24: the distinct-buffer ring (256 KB of writes per 16K calls) cost 5%
    // of the VGPU thread in profile run s24-2; the answer it gave (48 ranges,
    // hashed 400x each) is banked. Only fed in the plain memo mode now.
    if (!g_memo2) zz_ring_record(p, len);

    const unsigned gen = (unsigned)zz_swaps;
    const uintptr_t key = (uintptr_t)p;
    int free_slot = -1;
    for (int k = 0; k < ZZ_MEMO_N; k++)
    {
        ZzMemo& c = t_memo[k];
        if (!c.valid || c.gen != gen) { if (free_slot < 0) free_slot = k; continue; }
        if (c.ptr == key && c.len == (unsigned)len && c.seed == seed)
        {
            if (g_memo2 && zz_fp((const unsigned char*)p, len) != c.fp) { c.valid = 0; free_slot = k; break; }
            zz_hash_hits++;
            return c.val;
        }
    }
    // Table full of this frame's entries (more than 64 live ranges): fall back
    // to slot 0 rather than skipping the cache, so behaviour stays defined.
    ZzMemo& e = t_memo[free_slot >= 0 ? free_slot : 0];

    zz_hash_real_bytes += (LONG64)len;
    if (g_memo2)
    {
        unsigned v = g_orig_hash(p, len, seed);
        e.ptr = key; e.len = (unsigned)len; e.seed = seed; e.gen = gen; e.val = v; e.valid = 1;
        e.fp = zz_fp((const unsigned char*)p, len);
        return v;
    }
    unsigned long long c = seed;
    const unsigned char* s = (const unsigned char*)p;
    size_t n = len;
    while (n >= 8) { c = _mm_crc32_u64(c, *(const unsigned long long*)s); s += 8; n -= 8; }
    unsigned c32 = (unsigned)c;
    while (n >= 4) { c32 = _mm_crc32_u32(c32, *(const unsigned*)s); s += 4; n -= 4; }
    while (n--)    { c32 = _mm_crc32_u8(c32, *s++); }

    e.ptr = key; e.len = (unsigned)len; e.seed = seed; e.gen = gen; e.val = c32; e.valid = 1;
    return c32;
}

static int zz_cmp_ptr(const void* a, const void* b)
{
    const ZzHit* x = (const ZzHit*)a;
    const ZzHit* y = (const ZzHit*)b;
    if (x->ptr != y->ptr) return x->ptr < y->ptr ? -1 : 1;
    if (x->len != y->len) return x->len < y->len ? -1 : 1;
    return 0;
}

// Once a second, describe ONE complete frame: how many hash calls it made, how
// much it hashed, and -- the point of the exercise -- how many DISTINCT
// buffers those calls covered, with the busiest ones named.
static DWORD WINAPI zz_vgpu_reporter(LPVOID)
{
    static ZzHit snap[ZZ_HIT_N];
    for (;;)
    {
        Sleep(1000);

        // Print the global counters FIRST and unconditionally. The previous
        // version bailed out when the ring was empty, which in memo mode meant
        // the log showed nothing at all and the hit rate -- the one number the
        // experiment turns on -- was unobservable.
        {
            static LONG64 pHits, pReal, pCalls;
            LONG64 nHits = zz_hash_hits, nReal = zz_hash_real_bytes, nCalls = zz_hash_calls;
            LONG64 dH = nHits - pHits, dR = nReal - pReal, dC = nCalls - pCalls;
            pHits = nHits; pReal = nReal; pCalls = nCalls;
            if (dC) zz_vgpu_say("  per second: %lld calls, %lld memo hits (%.1f%%), %.2f MB actually hashed",
                                (long long)dC, (long long)dH,
                                dC ? 100.0 * (double)dH / (double)dC : 0.0,
                                (double)dR / (1024.0 * 1024.0));
        }

        LONG pos = g_hit_pos;
        if (pos <= 0) continue;

        LONG n = pos < ZZ_HIT_N ? pos : ZZ_HIT_N;
        LONG base = pos - n;
        unsigned maxf = 0;
        LONG m = 0;
        for (LONG k = 0; k < n; k++)
        {
            ZzHit h = g_hits[(base + k) & (ZZ_HIT_N - 1)];
            if (h.frame > maxf) maxf = h.frame;
            snap[m++] = h;
        }
        if (maxf == 0) continue;
        unsigned want = maxf - 1;          // the newest COMPLETE frame

        // keep only that frame
        LONG f = 0;
        LONG64 bytes = 0;
        for (LONG k = 0; k < m; k++)
            if (snap[k].frame == want) { bytes += snap[k].len; snap[f++] = snap[k]; }
        if (f == 0) continue;

        qsort(snap, (size_t)f, sizeof(ZzHit), zz_cmp_ptr);

        // distinct buffers, distinct (buffer,length) ranges, and the four
        // most-repeated ranges
        LONG buffers = 0, ranges = 0;
        uintptr_t topP[4] = {0}; unsigned topC[4] = {0}, topL[4] = {0};
        for (LONG k = 0; k < f; )
        {
            LONG j = k;
            while (j < f && snap[j].ptr == snap[k].ptr && snap[j].len == snap[k].len) j++;
            unsigned cnt = (unsigned)(j - k);
            ranges++;
            if (k == 0 || snap[k].ptr != snap[k - 1].ptr) buffers++;
            for (int s = 0; s < 4; s++)
                if (cnt > topC[s])
                {
                    for (int t = 3; t > s; t--) { topC[t]=topC[t-1]; topP[t]=topP[t-1]; topL[t]=topL[t-1]; }
                    topC[s] = cnt; topP[s] = snap[k].ptr; topL[s] = snap[k].len;
                    break;
                }
            k = j;
        }

        zz_vgpu_say("frame %u: %ld calls, %.1f MB asked | %ld buffers, %ld ranges"
                    " | top: %p x%u (%u B), %p x%u (%u B), %p x%u (%u B), %p x%u (%u B)",
                    want, (long)f, (double)bytes / (1024.0 * 1024.0),
                    (long)buffers, (long)ranges,
                    (void*)topP[0], topC[0], topL[0], (void*)topP[1], topC[1], topL[1],
                    (void*)topP[2], topC[2], topL[2], (void*)topP[3], topC[3], topL[3]);
    }
    return 0;
}

// Crackdown: enabled ONLY by the marker file <game>_zz_vgpu.on next to
// Emu.exe. The marker matters: the user launches the game themselves, and an
// env var set in my shell would not survive into their double-click. Emu's
// working directory is the build folder, so a relative path is right.
static bool zz_vgpu_enabled()
{
    return GetFileAttributesA(ZZ_TITLE "_zz_vgpu.on") != INVALID_FILE_ATTRIBUTES;
}

struct ZzVgpuInit
{
    ZzVgpuInit()
    {
        if (!zz_vgpu_enabled()) return;
        // Crackdown: both twins load; one owner per process (one log, one patch per site)
        char nm[64];
        _snprintf_s(nm, sizeof(nm), _TRUNCATE, "Local\\" ZZ_TITLE "_zz_vgpu_owner_%08lX", GetCurrentProcessId());
        HANDLE own = CreateMutexA(nullptr, FALSE, nm);
        if (own && GetLastError() == ERROR_ALREADY_EXISTS) return;
        CloseHandle(CreateThread(nullptr, 0, zz_vgpu_thread,   nullptr, 0, nullptr));
        CloseHandle(CreateThread(nullptr, 0, zz_vgpu_reporter, nullptr, 0, nullptr));
    }
};
static ZzVgpuInit g_zz_vgpu_init;
