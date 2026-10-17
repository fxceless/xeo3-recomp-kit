// xeo3-recomp-kit dashboard runtime -- THE PRESENT FIX: without it the dashboard draws its
// first screen and the window never changes again (the guest keeps running at 60 fps).
// Marker <console folder>\dash_swapfix.on (scripts\install-dashboard.ps1 creates it).
//
// WHY THE PICTURE NEVER MOVES (measured: RenderDoc said "Not presenting", a breakpoint on
// IDXGISwapChain::Present got almost no hits while the guest's VdSwap ran 60/s). The dash's
// D3D flips the display by a CPU write to the primary-surface-address register (0x6110).
// The emulator's GPU layer (VGPUDX12.dll) handles that register in TrySwap (+0x5E70): on
// the VGPU thread it presents; on ANY OTHER thread it only sets a request flag and wakes
// the VGPU thread, whose consumer (+0x50098) refreshes the surface address -- and never
// swaps. A game flips through the GPU command stream, which lands on the VGPU thread and
// presents; the dash flips from the CPU, which never does. The frames the user ever saw
// came from the mode-set path.
//
// THE FIX: detour the consumer's tail (VGPUDX12+0x500CC, 20 bytes, byte-checked) to a
// trampoline that repeats those four instructions and then calls TrySwap(swap state) ON
// THE VGPU THREAD. rbx = the swap state there, rsp is call-aligned (the function calls
// WaitForSingleObject at this depth), and +0x500E0 (the epilogue) needs nothing else.
//
// It was written against VGPUDX12.dll md5 410a4dea... (Microsoft's) / 3f002570... (after
// tools\patch_vgpu.py, which never touches .text). Any other build: the 20 bytes do not
// match, nothing is patched, and dash_runtime.log says so.
#include "zz_dash_common.h"
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <initializer_list>

namespace
{
    void SwapfixInstall(uintptr_t vg)
    {
        //   0x500CC  mov rax,[rip+0x55bc05]  ; g = [VGPUDX12+0x5abcd8]
        //   0x500D3  mov rcx,[rax+8]
        //   0x500D7  mov eax,[rcx+0x8170]    ; register 0x6110
        //   0x500DD  mov [rbx+0x28],eax      ; swap state +0x28 = surface address
        //   0x500E0  (epilogue)
        uint8_t* site = (uint8_t*)(vg + 0x500CC);
        static const uint8_t expect[20] = { 0x48,0x8b,0x05,0x05,0xbc,0x55,0x00, 0x48,0x8b,0x48,0x08,
                                            0x8b,0x81,0x70,0x81,0x00,0x00, 0x89,0x43,0x28 };
        if (memcmp(site, expect, sizeof(expect)) != 0)
        {
            zz_dash_log("ZZSWAPFIX VGPUDX12 is not the build this was written for (bytes at +0x500CC differ) -- not installed\n");
            return;
        }
        // the trampoline must be within +/-2 GB of the site (rel32 jumps)
        uint8_t* cave = nullptr;
        for (uintptr_t a = (vg & ~0xFFFFull) - 0x10000; a > vg - 0x40000000ull && !cave; a -= 0x10000)
            cave = (uint8_t*)VirtualAlloc((void*)a, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!cave) { zz_dash_log("ZZSWAPFIX no memory near VGPUDX12 for the trampoline -- not installed\n"); return; }
        uintptr_t gptr = vg + 0x5abcd8, tryswap = vg + 0x5E70;
        uint8_t* c = cave;
        auto emit = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *c++ = x; };
        emit({0x48,0xb8}); memcpy(c, &gptr, 8); c += 8;      // mov rax, &g
        emit({0x48,0x8b,0x00});                              // mov rax,[rax]
        emit({0x48,0x8b,0x48,0x08});                         // mov rcx,[rax+8]
        emit({0x8b,0x81,0x70,0x81,0x00,0x00});               // mov eax,[rcx+0x8170]
        emit({0x89,0x43,0x28});                              // mov [rbx+0x28],eax
        emit({0x48,0x8b,0xcb});                              // mov rcx,rbx
        emit({0x48,0xb8}); memcpy(c, &tryswap, 8); c += 8;   // mov rax, TrySwap
        emit({0xff,0xd0});                                   // call rax
        emit({0xe9}); { int32_t rel = (int32_t)((vg + 0x500E0) - (uintptr_t)(c + 4)); memcpy(c, &rel, 4); c += 4; }
        DWORD old;
        if (!VirtualProtect(site, 20, PAGE_EXECUTE_READWRITE, &old)) return;
        uint8_t patch[20]; memset(patch, 0x90, sizeof(patch));
        patch[0] = 0xe9; int32_t rel = (int32_t)((uintptr_t)cave - (uintptr_t)(site + 5)); memcpy(patch + 1, &rel, 4);
        memcpy(site, patch, 20);
        VirtualProtect(site, 20, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 20);
        zz_dash_log("ZZSWAPFIX installed at VGPUDX12+0x500CC\n");
    }
}

// From InitPrecompiledDll. The trampoline lives in its own allocation and calls only VGPU
// code, so it survives this DLL's unload when the dash launches a title.
extern "C" void zz_dash_vgpu_init()
{
    if (!zz_dash_marker("dash_swapfix.on"))
    {
        zz_dash_log("ZZSWAPFIX dash_swapfix.on absent -- the dashboard picture will not update\n");
        return;
    }
    HMODULE vg = GetModuleHandleA("VGPUDX12.dll");
    if (!vg)
    {
        zz_dash_log("ZZSWAPFIX VGPUDX12.dll is not loaded -- not installed\n");
        return;
    }
    SwapfixInstall((uintptr_t)vg);
}
