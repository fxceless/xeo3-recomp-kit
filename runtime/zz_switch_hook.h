// xeo3-recomp-kit runtime -- hand-written support code compiled into every recompiled game
// module. It grew out of the Need for Speed, Fable II and Crackdown recompiles; the notes
// below keep the history of WHY each piece exists. <game> = the game's short name (ZZ_TITLE,
// zz_title.h), <console folder> = the folder Emu.exe runs from (the working directory).

// Fable II: opt this build into jump-table default-arm logging.
//
// Force-included ahead of every generated source (build_fable2.ps1 adds
// /FIzz_switch_hook.h to each chunk rsp), so this definition wins over the
// no-op default in ppc_context.h. Nothing else in the tree defines
// PPC_SWITCH_MISS, so NFS, the dash, xam and the kernel keep the no-op and link
// with no new symbol -- their safety net is the indirect fallback alone.
#pragma once

extern "C" void zz_switch_miss(unsigned int addr, unsigned int index,
                               unsigned int target);

#define PPC_SWITCH_MISS(addr, index, target) zz_switch_miss((addr), (index), (target))
