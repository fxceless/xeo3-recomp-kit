// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// s9 pt7 -- force-included AHEAD of ppc_context.h (build_fable2.ps1 adds
// /FIzz_import_hook.h), so this definition wins that header's #ifndef guard.
//
// WHY. Six runs of sampling could not name what the frozen game waits for, and
// pt6 found the hard reason: a guest thread parked in an emulated kernel call
// unwinds through Emu's bridge, so its host stack carries NO guest frames at all.
// The guest caller is simply not on that stack.
//
// It is, however, in the guest LINK REGISTER at the moment of the call -- and every
// guest kernel call in this module passes through one of these 254 named thunks. So
// the caller, the arguments and the call itself are all available at the one place
// the stack cannot reach.
//
// This module's imports include VdSwap (the frame present), KeWaitForSingleObject,
// KeWaitForMultipleObjects, NtWaitForSingleObjectEx, NtWaitForMultipleObjectsEx and
// KeSetEvent -- exactly the set the question needs.
//
// TAIL CALL ONLY, deliberately. The dash track's profiler has a "wrap" path that
// calls the import and then reads its return value, and it works there, but it has
// to hand the next guest PC back itself and that handoff is the delicate part.
// Nothing here needs a return value, so nothing here takes that risk: the log
// happens BEFORE the call, and the original tail call is left exactly as the
// recompiler emitted it.
#ifndef ZZ_IMPORT_HOOK_H
#define ZZ_IMPORT_HOOK_H

extern "C" unsigned char zz_imp_watch[];

// The registers are read HERE, where ppc_context.h is unquestionably in scope, and
// passed as plain integers -- so zz_import.cpp needs no knowledge of the PPCContext
// layout at all. A second copy of those field offsets is exactly the kind of
// duplicated truth that goes stale silently.
// s9 pt12: `base` comes along now. It is the guest memory base, and the thunk already
// has it in a register -- so passing it costs nothing and buys the worker the ability
// to READ GUEST MEMORY at drain time. That is what turns NtCreateEvent's first
// argument from a useless guest pointer into the handle it returns: the call has long
// since completed by the time the worker looks, so the out-parameter is filled in.
// Without it there is no way to say WHICH handle a create produced, and the handle is
// the whole question (0xF8000028).
extern "C" void zz_imp_log(const char* name, unsigned int idx,
                           unsigned long long lr, unsigned long long r3,
                           unsigned long long r4, unsigned long long r5,
                           unsigned long long base);

// One byte load and a not-taken branch on every guest kernel call. The table is
// all-zero unless the marker armed it, so an unarmed build pays the load and
// nothing else.
// ---------------------------------------------------------------------------
// Crackdown s1 (2026-10-14): AUTO-LOGON -- the dashboard's own sign-in, made from here.
//
// Booted with -dvd there is no dashboard, so nobody signs a profile in, and Emu's
// sign-in blade cannot finish a local sign-in (selecting the profile does nothing --
// the same wall NFS met in s12). The dashboard gets past it with ONE real xam call at
// boot, measured in work\zz_dash_imports.log.pre_s43:
//     XamUserLogonEx(xuids=[<the profile's XUID>,0,0,0], flags=0x00120424, ...) -> 0
//     XamUserGetSigninState(0) -> 1   (signed in locally; saves then go to the profile)
// So the FIRST time the game asks XamUserGetSigninState, before forwarding it, this
// thunk makes that same call into real xam: XamUserLogonEx = xam export 0x224 =
// 0x816D9078 (resolved from xam's export table; the same decode reproduces the known
// XamShowGuideUI 0x816C46A0). Third argument NULL = xam's synchronous path (the dash
// passes an XOVERLAPPED; sub_819A0008 waits on a local completion when it is NULL).
// The XUID array (4 x 8 bytes, big-endian) lives in a scratch frame carved below the
// game's r1; every register the call may clobber is put back, so the game's own
// XamUserGetSigninState then runs exactly as if nothing had happened -- except that
// real xam now has a user. Armed by <console folder>\<game>_autologon.on (its content may
// hold a XUID in hex; default = xeo3_profile.txt); one attempt per process. Result:
// <game>_autologon.log.
extern "C" volatile unsigned char zz_autologon_pending;
extern "C" int  zz_autologon_claim(const char* import_name, unsigned long long* xuid);
extern "C" void zz_autologon_done(unsigned int hr, unsigned int sp);
extern "C" unsigned int zz_autologon_wait(unsigned int immediate, const unsigned char* xov);

#define ZZ_AUTOLOGON_RUN(n) do { \
    unsigned long long zz_xuid = 0; \
    if (zz_autologon_claim(&PPC_STRINGIFY(n)[10], &zz_xuid)) { \
        PPCContext* zz_c = ADJ(ctx); \
        const PPCRegister zz_r0 = zz_c->r0, zz_r1 = zz_c->r1, zz_r3 = zz_c->r3, zz_r4 = zz_c->r4, \
            zz_r5 = zz_c->r5, zz_r6 = zz_c->r6, zz_r7 = zz_c->r7, zz_r8 = zz_c->r8, zz_r9 = zz_c->r9, \
            zz_r10 = zz_c->r10, zz_r11 = zz_c->r11, zz_r12 = zz_c->r12, zz_ctr = zz_c->ctr; \
        const uint64_t zz_lr = zz_c->lr; \
        const PPCXERRegister zz_xer = zz_c->xer; \
        const PPCCRRegister zz_cr0 = zz_c->cr0, zz_cr1 = zz_c->cr1, zz_cr5 = zz_c->cr5, \
            zz_cr6 = zz_c->cr6, zz_cr7 = zz_c->cr7; \
        /* run 2 (s1): the synchronous form (r5 = NULL) failed 0x8007065B, which is   \
           xam's generic "task failed" (1627) -- the real hr is only kept in an        \
           XOVERLAPPED's dwExtendedError (+0x18). So: pass one, exactly as the dash    \
           does, and wait for it HERE while the scratch frame below r1 is still ours. */ \
        const uint32_t zz_sp = (zz_c->r1.u32 - 0x200u) & ~0xFu; \
        uint64_t* zz_x = (uint64_t*)(base + zz_sp + 0x80u); \
        zz_x[0] = __builtin_bswap64(zz_xuid); zz_x[1] = 0; zz_x[2] = 0; zz_x[3] = 0; \
        memset((void*)(base + zz_sp + 0xC0u), 0, 0x20); \
        zz_c->r1.u64 = zz_sp; \
        zz_c->r3.u64 = zz_sp + 0x80u; \
        zz_c->r4.u64 = 0x00120424u; \
        zz_c->r5.u64 = zz_sp + 0xC0u; \
        PPC_CALL_INDIRECT_FUNC(0x816D9078u); \
        const unsigned int zz_hr = zz_autologon_wait(zz_c->r3.u32, (const unsigned char*)(base + zz_sp + 0xC0u)); \
        zz_c->r0 = zz_r0; zz_c->r1 = zz_r1; zz_c->r3 = zz_r3; zz_c->r4 = zz_r4; zz_c->r5 = zz_r5; \
        zz_c->r6 = zz_r6; zz_c->r7 = zz_r7; zz_c->r8 = zz_r8; zz_c->r9 = zz_r9; zz_c->r10 = zz_r10; \
        zz_c->r11 = zz_r11; zz_c->r12 = zz_r12; zz_c->ctr = zz_ctr; zz_c->lr = zz_lr; zz_c->xer = zz_xer; \
        zz_c->cr0 = zz_cr0; zz_c->cr1 = zz_cr1; zz_c->cr5 = zz_cr5; zz_c->cr6 = zz_cr6; zz_c->cr7 = zz_cr7; \
        zz_autologon_done(zz_hr, zz_sp); \
    } \
} while (0)

// Crackdown s1: the LOCAL-SAVE layer (zz_save.cpp) answers its imports here and returns
// to the guest itself -- PPC_RETURN() hands ctx->lr back in ECX, exactly how a real
// import returns (NFS s12 pt3 mechanism). Claimed imports cost one byte load otherwise.
#include <stdint.h>
extern "C" unsigned char zz_save_claim[];
extern "C" int zz_save_handle(uintptr_t ctx, uintptr_t base, unsigned int idx);
// s2 build 11: a handler that completed an I/O carrying an event leaves the event with
// zz_save_signal_take(); the thunk signals it through the REAL NtSetEvent (an ordinary call,
// the mechanism s1's auto-logon proved mid-thunk), then restores r3 (our status), r1 and lr.
// r0/r4-r12/ctr/cr are volatile across an import call, so the guest expects them clobbered.
extern "C" unsigned zz_save_signal_take(void);
extern "C" void zz_save_signal_done(unsigned ev, unsigned status);
#define ZZ_SIGNAL_PENDING_EVENT() do { \
    const unsigned zz_ev = zz_save_signal_take(); \
    if (__builtin_expect(zz_ev != 0, 0) && ZZ_SI_NTSETEVENT >= 0) { \
        PPCContext* zz_c = ADJ(ctx); \
        const PPCRegister zz_r3 = zz_c->r3, zz_r1 = zz_c->r1; \
        const uint64_t zz_lr = zz_c->lr; \
        zz_c->r3.u64 = zz_ev; zz_c->r4.u64 = 0; \
        PPC_CALL_INDIRECT_FUNC(PrecompiledImportTable[ZZ_SI_NTSETEVENT >= 0 ? ZZ_SI_NTSETEVENT : 0].dest_addr); \
        zz_save_signal_done(zz_ev, zz_c->r3.u32); \
        zz_c->r3 = zz_r3; zz_c->r1 = zz_r1; zz_c->lr = zz_lr; \
    } \
} while (0)
extern "C" unsigned char zz_obs_claim[];
extern "C" int  zz_obs_match(unsigned idx, const char* name);
extern "C" void zz_obs_log(uintptr_t ctx, unsigned idx, const char* name, const unsigned* in);

// OBSERVE (zz_save.cpp's watch list): call the real import as an ordinary call -- the
// callee restores ctx->lr, so the ECX handed back afterwards is the one it would have
// produced (NFS's wrap path) -- then log inputs r3-r8 + LR and the answer r3.
// zz_obs_claim: 0 unseen, 1 observe, 2 not wanted (decided once, by the thunk's name).
#define ZZ_OBSERVE(n,i) do { \
    unsigned char zz_o = zz_obs_claim[i]; \
    if (__builtin_expect(zz_o == 0, 0)) zz_o = zz_obs_match((i), &PPC_STRINGIFY(n)[10]) ? 1 : 2; \
    if (__builtin_expect(zz_o == 1, 0)) { \
        const unsigned zz_in[7] = { ADJ(ctx)->r3.u32, ADJ(ctx)->r4.u32, ADJ(ctx)->r5.u32, \
            ADJ(ctx)->r6.u32, ADJ(ctx)->r7.u32, ADJ(ctx)->r8.u32, (unsigned)ADJ(ctx)->lr }; \
        (PPC_LOOKUP_FUNC(base, PrecompiledImportTable[i].dest_addr))(ctx, base, frame); \
        zz_obs_log(ctx, (i), &PPC_STRINGIFY(n)[10], zz_in); \
        PPC_RETURN(); \
    } \
} while (0)

// s2: the frame counter (zz_fps.cpp). `i` is a literal per thunk, so only VdSwap's thunk
// keeps the add -- one interlocked add per presented frame is the meter's whole cost.
// zz_fps_base / zz_fps_dev: the guest memory base and the D3D device, so the reporter can
// read the device's present interval each second (C0: the cap is read live, not assumed).
// The device is heap-allocated; D3D hands it to the kernel as the graphics interrupt
// callback's context (sub_826C9620 @826C96D0: mr r4,r31 -> VdSetGraphicsInterruptCallback).
#include "zz_save_idx.h"
extern "C" volatile long zz_swaps;
// The guest memory base, captured from the first import call (every thunk is handed it), for
// host-side code that reads guest memory later: zz_crash.cpp, zz_xmamm.cpp, zz_xma.cpp.
extern "C" uintptr_t zz_g_base;
extern "C" uintptr_t zz_fps_base;
extern "C" volatile unsigned zz_fps_dev;

#define PPC_IMPORT_FUNC_IMPL(n,i) __attribute__((preserve_none)) void n(uintptr_t ctx, uintptr_t base, uintptr_t frame) { \
    if (__builtin_expect(zz_g_base == 0, 0)) zz_g_base = (base); \
    if ((i) == ZZ_SI_VDSWAP) { zz_fps_base = (base); __atomic_add_fetch(&zz_swaps, 1, __ATOMIC_RELAXED); } \
    if ((i) == ZZ_SI_GFXCALLBACK && ADJ(ctx)->r4.u32) zz_fps_dev = ADJ(ctx)->r4.u32; \
    if (__builtin_expect(zz_save_claim[i] != 0, 0) && zz_save_handle(ctx, base, (i))) { ZZ_SIGNAL_PENDING_EVENT(); PPC_RETURN(); } \
    if (__builtin_expect(zz_autologon_pending != 0, 0)) ZZ_AUTOLOGON_RUN(n); \
    ZZ_OBSERVE(n, i); \
    if (__builtin_expect(zz_imp_watch[i] != 0, 0)) \
        zz_imp_log(&PPC_STRINGIFY(n)[10], (i), (unsigned long long)ADJ(ctx)->lr, \
                   (unsigned long long)ADJ(ctx)->r3.u64, \
                   (unsigned long long)ADJ(ctx)->r4.u64, \
                   (unsigned long long)ADJ(ctx)->r5.u64, \
                   (unsigned long long)(base)); \
    PPC_CALL_INDIRECT_FUNC(PrecompiledImportTable[i].dest_addr); \
}

#endif
