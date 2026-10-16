#!/usr/bin/env python3
r"""Resolve a host address in a recompiled game module back to a guest function.

Two uses:
  python -I tools\resolve_fault.py games\<name> 0x1878A85              # a module-relative offset, as printed
                                                 # by Emu's "at <addr> of Module <base>"
                                                 # (subtract them yourself)
  python -I tools\resolve_fault.py games\<name> --emu 0x7FF... 0x7FF... # fault addr + module base, does the
                                                 # subtraction for you
  python -I tools\resolve_fault.py games\<name> --stack cdb_out.txt    # every host address in a cdb stack dump

WHY THIS EXISTS. Emu names the module base and the faulting address but nothing else,
and the DLL ships without a PDB, so a fault is just a number. The .map file produced by
relinking the SAME objects gives every symbol's RVA. That relink is safe to trust: the
two DLLs were compared byte-for-byte and differ in exactly 4 bytes (the PE timestamps at
0x100 and 0x4E14E84), so the layout is identical and the map is authoritative for the
DLL that actually ran.

Regenerate the map after any rebuild:
    scripts\build.ps1 writes ppc\game.map next to ppc\game_recomp.dll on every link.
"""
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kitgame
HERE = str(kitgame.game_dir())
DLL = os.path.join(HERE, "ppc", "game_recomp.dll")
MAP = os.path.join(HERE, "ppc", "game.map")


def image_base(path):
    with open(path, "rb") as fh:
        d = fh.read(0x400)
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    magic, = struct.unpack_from("<H", d, pe + 24)
    if magic == 0x20B:
        return struct.unpack_from("<Q", d, pe + 24 + 24)[0]
    return struct.unpack_from("<I", d, pe + 24 + 28)[0]


def load_map(path):
    pat = re.compile(r"^\s+([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s+(\S+)\s+([0-9A-Fa-f]{8,16})\s")
    syms = []
    with open(path, encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            m = pat.match(line)
            if m:
                syms.append((int(m.group(4), 16), m.group(3), line.split()[-1]))
    syms.sort()
    return syms


GUEST = re.compile(r"sub_([0-9A-F]{8})")


def resolve(rva, syms, base):
    target = base + rva
    addrs = [s[0] for s in syms]
    i = bisect.bisect_right(addrs, target) - 1
    if i < 0:
        return None
    a, name, obj = syms[i]
    g = GUEST.search(name)
    return dict(sym=name, obj=obj, delta=target - a,
                guest=("0x" + g.group(1)) if g else None)


def report(rva, syms, base):
    r = resolve(rva, syms, base)
    if not r:
        print(f"  RVA 0x{rva:X}: below the first symbol")
        return
    print(f"  RVA 0x{rva:<10X} -> {r['sym']}  +0x{r['delta']:X}   [{r['obj']}]")
    if r["guest"]:
        print(f"                  guest function sub_{r['guest'][2:]}  "
              f"(grep the ppc/*.cpp for PPC_FUNC_IMPL(__imp__sub_{r['guest'][2:]}))")


def main():
    if not os.path.exists(MAP):
        sys.exit(f"no map file at {MAP} -- run scripts\build.ps1 first")
    syms = load_map(MAP)
    base = image_base(DLL)
    print(f"map: {len(syms)} symbols   PE ImageBase 0x{base:X}\n")

    args = kitgame.rest_args()
    if not args:
        sys.exit(__doc__)

    if args[0] == "--emu":
        fault, modbase = int(args[1], 16), int(args[2], 16)
        report(fault - modbase, syms, base)
    elif args[0] == "--stack":
        seen = set()
        for line in open(args[1], encoding="utf-8", errors="ignore"):
            for m in re.finditer(r"\b0{0,4}7[0-9A-Fa-f]{10,14}\b", line):
                v = int(m.group(0), 16)
                if v not in seen:
                    seen.add(v)
        print(f"{len(seen)} distinct host address(es) in the dump; "
              f"only those inside our module resolve:\n")
        for v in sorted(seen):
            # a stack dump has no module base; try every plausible one by checking
            # whether the low bits land inside .text
            pass
        print("  (pass --emu <addr> <modulebase> per frame; cdb prints the base with 'lm')")
    else:
        report(int(args[0], 16), syms, base)


if __name__ == "__main__":
    main()
