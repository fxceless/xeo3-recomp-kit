// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Crackdown s3 A2 (2026-10-14): force-included AHEAD of ppc_context.h (build_crackdown.ps1
// adds /FIzz_mmio_hook.h), so these two definitions win that header's #ifndef guards
// (ppc_context.h:179 / :213). ppc_config.h is not the place: the recompiler rewrites it on
// every run.
//
// Every recompiled MMIO access -- the six GPU (0x7FC8) sites and the 36 XMA (0x7FEA) sites
// listed in config.toml -- now calls zz_mm_load32 / zz_mm_store32 (zz_xmamm.cpp), which do
// EXACTLY what the originals did (load: bswap(PrecompiledPointers[8](addr)); store:
// PrecompiledPointers[9](addr, bswap(value))) and log the XMA block's first 400 accesses.
// Instrument only: no access is changed, consumed or reordered.
#ifndef ZZ_MMIO_HOOK_H
#define ZZ_MMIO_HOOK_H
#include <stdint.h>

extern "C" uint32_t zz_mm_load32(uint32_t addr);
extern "C" void     zz_mm_store32(uint32_t addr, uint32_t value);

#define PPC_MM_LOAD_U32(x)     zz_mm_load32((uint32_t)(x))
#define PPC_MM_STORE_U32(x, y) zz_mm_store32((uint32_t)(x), (uint32_t)(y))

#endif
