// xeo3-recomp-kit dashboard runtime -- hand-written support code compiled into the
// recompiled NXE dashboard (dash.xex). <console folder> = the folder Emu.exe runs from.
// The notes keep the history of WHY each piece exists; docs\DASHBOARD.md is the overview.
//
// The import hook. Force-included ahead of ppc_context.h by scripts\build.ps1 (the dash's
// game.json lists it), so this PPC_IMPORT_FUNC_IMPL wins that header's #ifndef guard.
// Every import thunk the recompiler generated for the dash expands it with its own
// literal index, and zz_dash_claimed()/zz_dash_wrapped() are constexpr, so an import
// that is not ours compiles to exactly the plain tail call it would have been.
//
// The indices come from THIS recompile's ppc_import_mapping.cpp, written into
// zz_dash_idx.h by tools\gen_dash_idx.py (scripts\recompile-dashboard.ps1 runs it). They
// are never typed by hand. An import the dash does not have is -1 and is never matched.
//
// THREE SHAPES:
//   claimed  -- zz_dash_import() sees the call on the way IN. It returns
//                 0  let the real import run (the original tail call),
//                 1  "r3 is set, do not call the import" -- PPC_RETURN() hands ctx->lr
//                    back in ECX, which is exactly how a real import returns,
//                 2  redirect: the call becomes XamLoaderLaunchTitleEx with the
//                    registers the hook loaded (My Games, see zz_dash_imports.cpp).
//   wrapped  -- the real import runs as an ORDINARY call (not a tail call) so its result
//               can be read out of r3 afterwards; then we hand back the next guest PC
//               ourselves. Safe because the callee restores ctx->lr before returning,
//               so the ECX produced is the one it would have produced.
//   neither  -- untouched.
#pragma once
#include <stdint.h>
#include "zz_dash_idx.h"

extern "C" int  zz_dash_import(uintptr_t ctx, uintptr_t base, unsigned int idx);
extern "C" void zz_dash_import_ret(uintptr_t ctx, uintptr_t base, unsigned int idx);
extern "C" int  zz_dash_guide_take(void);

// Guest address of XamShowGuideUI in the xam that ships with the console package the kit
// was built against (xam.xex = xeo3_83fb8ce4_..., see dashboard\dash.json). The dash does
// not import it, so it is called by address -- and only when <console folder>\dash_guide.on
// exists, which scripts\install-dashboard.ps1 creates only for that exact xam build.
#ifndef ZZ_DASH_SHOWGUIDEUI
#define ZZ_DASH_SHOWGUIDEUI 0x816C46A0u
#endif

static constexpr bool zz_dash_claimed(int i)
{
    return i >= 0 && (
        // the Xbox Live Vision camera: the emulator has no implementation and the guest
        // bug-checks right after calling one; answered "no camera" (zz_dash_imports.cpp)
           i == ZZ_DI_XUSBCAMGETSTATE || i == ZZ_DI_XUSBCAMCREATE || i == ZZ_DI_XUSBCAMDESTROY
        || i == ZZ_DI_XUSBCAMGETCONFIG || i == ZZ_DI_XUSBCAMSETCONFIG || i == ZZ_DI_XUSBCAMSETVIEW
        || i == ZZ_DI_XUSBCAMSETCAPTUREMODE || i == ZZ_DI_XUSBCAMREADFRAME
        // a module whose precompiled DLL is missing: decline instead of the emulator's FATAL
        || i == ZZ_DI_XEXLOADIMAGE
        // title launch: remembered for the switcher (zz_dash_launch.cpp), real import still runs
        || i == ZZ_DI_XAMLOADERLAUNCHTITLE || i == ZZ_DI_XAMLOADERLAUNCHTITLEEX
        || i == ZZ_DI_XAMLOADERLAUNCHTITLEFORREASON
        // My Games "A": resolved on the host and redirected (return 2)
        || i == ZZ_DI_XAMCONTENTLAUNCHIMAGEINTERNALEX
        // launch data, carried across the dash's own relaunch
        || i == ZZ_DI_XAMLOADERSETLAUNCHDATA || i == ZZ_DI_XAMLOADERGETLAUNCHDATA
        || i == ZZ_DI_XAMLOADERGETLAUNCHDATASIZE);
}

static constexpr bool zz_dash_wrapped(int i)
{
    // the UI thread's keystroke poll, once per frame: where the Guide button is raised
    return i >= 0 && i == ZZ_DI_XAMINPUTGETKEYSTROKEEX;
}

#define ZZ_DASH_REDIRECT_IDX (ZZ_DI_XAMLOADERLAUNCHTITLEEX >= 0 ? ZZ_DI_XAMLOADERLAUNCHTITLEEX : 0)

#define PPC_IMPORT_FUNC_IMPL(n,i) __attribute__((preserve_none)) void n(uintptr_t ctx, uintptr_t base, uintptr_t frame) { \
    if (zz_dash_claimed(i)) { \
        int zz_rc = zz_dash_import(ctx, base, (i)); \
        if (zz_rc == 1) PPC_RETURN(); \
        if (zz_rc == 2) { PPC_CALL_INDIRECT_FUNC(PrecompiledImportTable[ZZ_DASH_REDIRECT_IDX].dest_addr); PPC_RETURN(); } \
    } \
    if (zz_dash_wrapped(i)) { \
        (PPC_LOOKUP_FUNC(base, PrecompiledImportTable[i].dest_addr))(ctx, base, frame); \
        zz_dash_import_ret(ctx, base, (i)); \
        /* Xbox button -> XamShowGuideUI(0) on the UI thread; the poll's own r3 is kept */ \
        if (zz_dash_guide_take()) { \
            uint64_t zz_r3 = ADJ(ctx)->r3.u64; ADJ(ctx)->r3.u64 = 0; \
            PPC_CALL_INDIRECT_FUNC(ZZ_DASH_SHOWGUIDEUI); \
            ADJ(ctx)->r3.u64 = zz_r3; \
        } \
        PPC_RETURN(); \
    } \
    PPC_CALL_INDIRECT_FUNC(PrecompiledImportTable[i].dest_addr); \
}
