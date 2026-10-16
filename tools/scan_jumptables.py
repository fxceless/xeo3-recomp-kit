#!/usr/bin/env python3
r"""Find every jump table XenonAnalyse missed, from the recompiler's own output.

Written for s12 pt4 after the THIRD such table cost a session. The project notes said:
"If a third table fires, write the scanner instead of whack-a-mole." This is it.

The defect: a `bctr` dispatched through a table with no `cmplwi` bound in the
same function. XenonAnalyse sizes tables from that compare, so an unbounded one
is skipped silently, its words are disassembled as instructions (they always
decode as `lwz` -- any word 0x80000000..0x83FFFFFF has primary opcode 32), and
the recompiler emits a bare PPC_CALL_INDIRECT_FUNC(ctr). At runtime the entry
selected is a MID-FUNCTION address, Emu's per-address dispatch table has no slot
for it, and the guest dies with
    GENERAL FATAL ERROR - Unable to execute precompiled code for IAR <junk>
with the requested address left in rsi (s11 pt10: rsi=0x82285718 = entry [1];
s12 pt4: rsi=0x825B1FB0 = entry [0]).

Detection, purely from work/ppc/ppc_recomp.*.cpp:
  1. a `// XXXXXXXX bctr` comment whose EMITTED line is PPC_CALL_INDIRECT_FUNC
     (a table the recompiler DID recognise emits `switch (...)` instead),
  2. back-scan <= 16 instructions for `lis rT,H` + `addi rT,rT,L` (table base)
     and `rlwinm rD,rIDX,2,0,29` (the index register),
  3. rebuild the table words from the following disassembly comments -- a
     `lwz rD,IMM(rA)` line IS the word 0x80000000|D<<21|A<<16|(IMM&0xFFFF),
  4. stop at the lowest entry seen so far, which is the first instruction after
     the table. Self-terminating and exact: it reproduced 25 entries at
     0x825B1F4C and 4 at 0x825B1EEC by hand-check.

Output is ready to paste: [[switch]] blocks for work/switch.toml and the
companion `functions` rows for work/config.toml (without them the labels land
outside the analysed function and the recompiler rejects the switch).

    python -I tools\scan_jumptables.py games\<name> [--toml]
"""
import re
import sys
from pathlib import Path

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kitgame
GAME = kitgame.game_dir()
PPC = GAME / "ppc"

RE_INSN = re.compile(r"^\s*// ([0-9A-F]{8}) (.+?)\s*$")
RE_FUNC = re.compile(r"^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)")
RE_LIS = re.compile(r"^lis r(\d+),(-?\d+)$")
RE_ADDI = re.compile(r"^addi r(\d+),r(\d+),(-?\d+)$")
RE_RLWINM = re.compile(r"^rlwinm r(\d+),r(\d+),2,0,29$")
RE_LWZ = re.compile(r"^lwz r(\d+),(-?\d+)\(r(\d+)\)$")
RE_CMPLWI = re.compile(r"^cmplwi cr\d+,r(\d+),(\d+)$")

IMG_LO, IMG_HI = kitgame.image_range(GAME)


def word_of_lwz(m):
    """The instruction word a `lwz rD,IMM(rA)` comment was decoded FROM."""
    d, imm, a = int(m.group(1)), int(m.group(2)), int(m.group(3))
    return 0x80000000 | (d << 21) | (a << 16) | (imm & 0xFFFF)


def scan_file(path):
    lines = path.read_text(errors="replace").splitlines()
    # (index in `lines`, address, mnemonic-text) for every disassembly comment
    insns, at = [], {}
    func_at = {}
    cur_func = None
    for i, ln in enumerate(lines):
        mf = RE_FUNC.match(ln)
        if mf:
            cur_func = int(mf.group(1), 16)
        m = RE_INSN.match(ln)
        if m:
            addr = int(m.group(1), 16)
            at[addr] = len(insns)
            func_at[addr] = cur_func
            insns.append((i, addr, m.group(2).strip()))

    found = []
    for n, (li, addr, text) in enumerate(insns):
        if text != "bctr":
            continue
        # Recognised tables emit `switch (`; only the bare indirect is a miss.
        emitted = lines[li + 1] if li + 1 < len(lines) else ""
        if "PPC_CALL_INDIRECT_FUNC" not in emitted:
            continue

        base = idx_reg = None
        bound = None
        lis = {}
        # s40: never scan back across an earlier bctr/blr. The table setup
        # (lis/addi/rlwinm) belongs to the FIRST bctr after it; a later bare
        # bctr within 16 instructions is a different jump (sub_824961C8: the
        # bounded dispatcher at 0x824961F8 was already a switch, so the scan
        # landed on the virtual tail call at 0x82496224 and bound the table to
        # it -> switch on a vtable pointer -> crash on msg 0 in the Bull race).
        lo = max(0, n - 16)
        for k in range(n - 1, lo - 1, -1):
            if insns[k][2] in ("bctr", "blr"):
                lo = k + 1
                break
        for k in range(lo, n):
            _, _, t = insns[k]
            m = RE_LIS.match(t)
            if m:
                lis[int(m.group(1))] = (int(m.group(2)) & 0xFFFF) << 16
                continue
            m = RE_ADDI.match(t)
            if m and int(m.group(2)) in lis:
                v = lis[int(m.group(2))] + int(m.group(3))
                lis[int(m.group(1))] = v & 0xFFFFFFFF
                base = v & 0xFFFFFFFF
                continue
            m = RE_RLWINM.match(t)
            if m:
                idx_reg = int(m.group(2))
                continue
            m = RE_CMPLWI.match(t)
            if m:
                bound = int(m.group(2))
        # No base + no scaled index = an ordinary indirect jump through ctr
        # (a virtual call or a tail call), not a table. There are ~800 of those
        # and they are fine; only the table shape is a miss.
        if base is None or idx_reg is None:
            continue

        # Walk the table words, stopping at the lowest target seen so far.
        labels, a, limit = [], base, None
        while a in at and (limit is None or a < limit):
            m = RE_LWZ.match(insns[at[a]][2])
            if not m:
                break
            w = word_of_lwz(m)
            if not (IMG_LO <= w < IMG_HI):
                break
            labels.append(w)
            limit = w if limit is None else min(limit, w)
            a += 4

        func = func_at.get(addr)
        found.append(dict(bctr=addr, base=base, r=idx_reg, labels=labels,
                          bound=bound, func=func, end=limit, file=path.name,
                          note="" if labels else "no table words decoded"))
    return found


def main():
    out = []
    for f in sorted(PPC.glob("ppc_recomp.*.cpp")):
        out.extend(scan_file(f))
    out.sort(key=lambda d: d["bctr"])

    print(f"# {len(out)} unrecognised bctr site(s)")
    for d in out:
        print(f"# {d['file']}  bctr 0x{d['bctr']:08X}  func sub_{(d.get('func') or 0):08X}  "
              f"base 0x{(d['base'] or 0):08X}  r{d['r']}  "
              f"{len(d['labels'])} labels  {d['note']}")

    if "--toml" not in sys.argv:
        return
    print("\n# ---- paste into work/switch.toml ----")
    for d in out:
        if not d["labels"]:
            continue
        print(f"\n# scan_jumptables.py: unbounded table, {len(d['labels'])} entries at "
              f"0x{d['base']:08X}, dispatched by bctr 0x{d['bctr']:08X} in "
              f"sub_{d['func']:08X}")
        print("[[switch]]")
        print(f"base = 0x{d['bctr']:08X}")
        print(f"r = {d['r']}")
        print(f"default = 0x{d['end']:08X}")
        print("labels = [")
        for l in d["labels"]:
            print(f"    0x{l:08X},")
        print("]")

    print("\n# ---- add to the `functions` list in work/config.toml ----")
    for d in out:
        if not d["labels"] or d.get("func") is None:
            continue
        # Must span from the function start past the last label, or the labels
        # fall outside the analysed function and the switch is rejected.
        size = (max(d["labels"]) + 0x20 - d["func"] + 0xF) & ~0xF
        print(f"    {{ address = 0x{d['func']:08X}, size = 0x{size:X} }},"
              f"   # scan_jumptables.py: table at 0x{d['base']:08X}")


if __name__ == "__main__":
    main()
