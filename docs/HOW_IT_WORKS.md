# How it works

## XEO3 doesn't emulate PowerPC

XEO3 is Microsoft's Xbox 360 back-compat emulator. It simulates the hypervisor and the
important hardware (GPU, audio); everything from the kernel up runs as the real 360 software.
But it never interprets PowerPC: for **every module** the hypervisor loads (the kernel, xam,
the dashboard, a game) it loads a **DLL holding that module's code, translated ahead of time
to x86-64**, and looks functions up by guest address. The XEX file is still mapped, so the
module's data sections exist in guest memory.

Microsoft shipped such DLLs only for the titles on its back-compat list. This kit produces
the game's DLL from its XEX; Microsoft's own DLLs from a back-compat package provide the
kernel and system modules ([CONSOLE_FOLDER.md](CONSOLE_FOLDER.md)).

*(This section follows [WitherOrNot's write-up](https://github.com/WitherOrNot/XenonRecomp-XEO3),
which found all of it first.)*

## The DLL contract

Every module DLL exports at least:

| export | purpose |
|---|---|
| `InitPrecompiledDll` | called on load: map guest code addresses to native functions, receive the emulator's callbacks, return flags and version (46 for the current emulator) |
| `CleanupPrecompiledDll` | called before unload |
| `PrecompiledPointers` | a table the emulator fills with its callbacks (below) |
| `PrecompiledImportTable` | one entry per import thunk; the emulator writes each import's target |

`PrecompiledPointers`:

```
0  void     CallGuestFunc(uint32_t ppc_addr)          call any guest address
1  void     CallHostFunction(void*, uint16_t ordinal) emulator-provided functions
2  void     Syscall()                                 syscall number in r3
3  uint32_t GetCR(PPCContext*)                        the full condition register
4  uint64_t GetFrequency()
5  void     RaiseException(uint64_t address)
8  uint32_t MMIORead(uint32_t address)                hardware register read (byte-swapped)
9  void     MMIOWrite(uint32_t address, uint32_t value)
```

`recompiler\XenonUtils\ppc_precomp_init.cpp` implements the exports; `recompile.ps1` copies
it into every game.

## The calling convention

The emulator calls recompiled code with a convention of its own, close to but incompatible
with MSVC's `__preserve_none`:

```
rbx  address of the PowerPC register context + 0x80 (= &r3)
r15  base address of guest memory
r14  frame
all other registers clobbered; the callee cleans the stack
```

WitherOrNot's LLVM build swaps clang's `preserve_none` for exactly this, which is why the
kit needs that compiler. Hand-written code that wants a normal Win64 function should be a
`preserve_none` thunk wrapping a Win64 body. A `preserve_none` function that calls Win64
code directly loses `rbx`/`r15` (the context and memory base) and crashes later.

## DLL names

The emulator finds a module's DLL by name, computed from the module itself
(`tools\xeo3_name.py`):

- **XEX module**: `xeo3_%08x_%08x_%08x_%08x_%08x`, the 20-byte header digest at
  `security_info + 0x164`, as five 32-bit words, each read **little-endian**.
- **PE image** (the kernel): `xeo3_%08x_%08x`, the PE timestamp and checksum.

It loads `<name>.dll` **and** `<name>_no.dll`. Install both; a single one is a half-install
that runs the old code some of the time. Original-Xbox packages also carry `xefu_...` DLLs, named the same two ways
([ORIGINAL_XBOX.md](ORIGINAL_XBOX.md)).

Since the name is a hash of the header, **editing the XEX changes the name and the emulator's
own integrity checks reject it.** Every fix goes into the DLL or the runtime, never the XEX.

## The pipeline

```
disc ISO --xdvdfs--> disc folder ---------------------------------------------+
                         |                                                    |
                    default.xex                                               |
                         |                                                    |
   XenonAnalyse ---------+--> switch.toml, image.bin, save/restore addresses  |
                         |                                                    |
   XenonRecomp  <--- config.toml (functions, mmio, setjmp)                    |
        |                                                                     |
   ppc\ppc_recomp.*.cpp  (one C++ function per guest function, 62k for Crackdown)
   ppc\ppc_func_mapping.cpp, ppc_import_mapping.cpp, ppc_config.h, ...        |
        +  runtime\zz_*.cpp + ppc_precomp_init.cpp                            |
        |                                                                     |
   clang-cl (WitherOrNot) --> xeo3_<hash>.dll + xeo3_<hash>_no.dll            |
                                    |                                         |
                            console folder\  <-- Emu.exe -dvd <disc folder> --+
```

Inside a recompiled function every PowerPC instruction becomes a line of C++ on the register
context (`ctx.r3.u32 = ...`), with the original instruction in a comment above it. Branches
inside a function are `goto`s; calls to other guest functions are direct C++ calls; indirect
calls and jump tables the analyser couldn't resolve go through `PPC_CALL_INDIRECT_FUNC`, which
asks the emulator, and that's where a missed table becomes a fatal error.

## Where the hooks are

The runtime gets control at a few fixed points, each a macro in `ppc_context.h` that a
force-included header overrides (`build.ps1` passes `/FI` for each):

| hook | header | used for |
|---|---|---|
| `PPC_IMPORT_FUNC_IMPL` | `zz_import_hook.h` | every kernel/xam call: local saves, sign-in, the frame counter, call logging |
| `PPC_MM_LOAD_U32` / `PPC_MM_STORE_U32` | `zz_mmio_hook.h` | hardware register access (XMA audio) |
| `PPC_SWITCH_MISS` | `zz_switch_hook.h` | a jump table took an index it doesn't list |
| `PPC_SETJMP` / `PPC_LONGJMP` | `ppc_context.h` + `zz_setjmp.cpp` | the C runtime's non-local jumps |
| `zz_mftb()` | `zz_time.cpp` | the `mftb` timebase |
| `PPC_FUNC(sub_X)` | any `.cpp` in `ppc\` | replace or wrap one guest function (the recompiler's own definitions are weak) |

An import thunk "returns" by handing the next guest PC back in `ECX` (`PPC_RETURN()`), the way
a real import does. That's how the save layer answers a call without ever calling into xam.

## What the fork changed in the recompiler

See [recompiler/README.md](../recompiler/README.md#what-this-fork-adds).
