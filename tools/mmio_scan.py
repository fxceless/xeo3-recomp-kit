r"""Find the instructions that touch an Xbox 360 register block (default: GPU, 0x7FC80000).

    python -I tools\mmio_scan.py games\<name>          # GPU block 0x7FC8
    python -I tools\mmio_scan.py games\<name> 0x7FEA   # XMA (audio) block 0x7FEA

Without an `mmio_instructions` entry the recompiler emits a plain guest-memory
access for these, so a register READ returns RAM garbage and a WRITE never reaches
Emu's MMIORead/MMIOWrite callbacks.

Method (same as the NFS list was built with): find every `lis rX,<block>`, then walk
FORWARD through that function's instruction stream tracking which registers still
hold the block base, and record every load/store that uses one as its base register.
A register stops counting when it is redefined by anything other than mr/addi/ori.

s3 additions (the XDK-4802 XAudio, ppc_recomp.181.cpp):
  * indexed forms `lwbrx/stwbrx/lwzx/stwx rX,0,rB` (and rA!=0 with one side tracked);
  * the PRE-SHIFTED form: `lis rX,<block>>>2` + `ori` -> `add` an index -> `rlwinm ..,2,0,29`
    yields the block address (the kick/lock/clear bit-array writes are built this way);
  * `lis rX,<block>+1` with negative d-form displacements (reaches back into the block);
  * a `bl` clobbers r0 and r3-r12.
Every hit prints the register offset in the block when it is a constant.
"""
import re, glob, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kitgame
os.chdir(kitgame.game_dir())

_rest = kitgame.rest_args()
BLOCK_HI = int(_rest[0], 16) if _rest else 0x7FC8
BLOCK_LO_ADDR = BLOCK_HI << 16
BLOCK_HI_ADDR = BLOCK_LO_ADDR + 0x10000
PRE_HI = BLOCK_HI >> 2                       # 0x7FEA -> 0x1FFA

RE_INSN = re.compile(r"^\s*// ([0-9A-F]{8}) (.+?)\s*$")
RE_FUNC = re.compile(r"^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)")
RE_LIS  = re.compile(r"^lis r(\d+),(-?\d+)$")
RE_MR   = re.compile(r"^mr r(\d+),r(\d+)$")
RE_ADDI = re.compile(r"^addi r(\d+),r(\d+),(-?\d+)$")
RE_ORI  = re.compile(r"^ori r(\d+),r(\d+),(-?\d+)$")
RE_ADD  = re.compile(r"^add r(\d+),r(\d+),r(\d+)$")
RE_RLW2 = re.compile(r"^rlwinm r(\d+),r(\d+),2,0,29$")
# lwz/lhz/lbz/lwa/stw/sth/stb and their update forms, base in parens
RE_MEM  = re.compile(r"^(l|st)(w|h|b|d)(z?)(u?)\s+r(\d+),(-?\d+)\(r(\d+)\)$")
# indexed: lwbrx/stwbrx/lwzx/stwx/lhzx/sthx/lbzx/stbx rD,rA|0,rB
RE_MEMX = re.compile(r"^(lwbrx|stwbrx|lwzx|stwx|lhzx|sthx|lbzx|stbx|lhbrx|sthbrx)\s+r(\d+),(0|r\d+),r(\d+)$")
# any instruction that writes rD as its first operand (stores excluded below)
RE_DEF  = re.compile(r"^([a-z][a-z0-9._]*)\s+r(\d+),")

def in_block(a):
    return BLOCK_LO_ADDR <= (a & 0xFFFFFFFF) < BLOCK_HI_ADDR

hits = {}          # addr -> (text, func, offset-or-None)
for path in sorted(glob.glob("ppc/ppc_recomp.*.cpp")):
    cur = None
    insns = []
    for line in open(path, encoding="utf-8", errors="ignore"):
        m = RE_FUNC.match(line)
        if m:
            cur = int(m.group(1), 16)
        m = RE_INSN.match(line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2).strip(), cur))

    for i, (addr, text, fn) in enumerate(insns):
        m = RE_LIS.match(text)
        if not m:
            continue
        hi = int(m.group(2)) & 0xFFFF
        if hi in (BLOCK_HI, BLOCK_HI + 1):
            live = {int(m.group(1)): ("const", hi << 16)}
        elif hi == PRE_HI and PRE_HI:
            live = {int(m.group(1)): ("preconst", hi << 16)}
        else:
            continue
        for addr2, text2, fn2 in insns[i + 1:]:
            if fn2 != fn:                       # left the function
                break
            mm = RE_MEM.match(text2)
            if mm and int(mm.group(7)) in live:
                kind, v = live[int(mm.group(7))]
                if kind == "const":
                    ea = (v + int(mm.group(6))) & 0xFFFFFFFF
                    if in_block(ea):
                        hits.setdefault(addr2, (text2, fn, ea - BLOCK_LO_ADDR))
                elif kind == "full":
                    hits.setdefault(addr2, (text2, fn, None))
                if mm.group(1) == "l":
                    live.pop(int(mm.group(5)), None)
                continue
            mx = RE_MEMX.match(text2)
            if mx:
                ra = None if mx.group(3) == "0" else int(mx.group(3)[1:])
                rb = int(mx.group(4))
                srcs = [r for r in (ra, rb) if r is not None and r in live]
                if srcs:
                    kind, v = live[srcs[0]]
                    if kind == "const" and ra is None and in_block(v):
                        hits.setdefault(addr2, (text2, fn, v - BLOCK_LO_ADDR))
                    elif kind in ("full",) or (kind == "const" and ra is not None):
                        hits.setdefault(addr2, (text2, fn, None))
                if not mx.group(1).startswith("st"):
                    live.pop(int(mx.group(2)), None)
                continue
            mv = RE_MR.match(text2)
            if mv:
                d, s = int(mv.group(1)), int(mv.group(2))
                if s in live: live[d] = live[s]
                else: live.pop(d, None)
                continue
            ma = RE_ADDI.match(text2)
            if ma:
                d, s, imm = int(ma.group(1)), int(ma.group(2)), int(ma.group(3))
                if s in live and live[s][0] in ("const", "preconst"):
                    live[d] = (live[s][0], (live[s][1] + imm) & 0xFFFFFFFF)
                elif s in live:
                    live[d] = live[s]
                else:
                    live.pop(d, None)
                continue
            mo = RE_ORI.match(text2)
            if mo:
                d, s, imm = int(mo.group(1)), int(mo.group(2)), int(mo.group(3)) & 0xFFFF
                if s in live and live[s][0] in ("const", "preconst"):
                    live[d] = (live[s][0], live[s][1] | imm)
                elif s in live:
                    live[d] = live[s]
                else:
                    live.pop(d, None)
                continue
            mad = RE_ADD.match(text2)
            if mad:
                d, a, b = (int(x) for x in mad.groups())
                t = [live[r] for r in (a, b) if r in live]
                if len(t) == 1 and t[0][0] in ("preconst", "pre"):
                    live[d] = ("pre", t[0][1])        # pre-shifted base + index
                elif len(t) == 1 and t[0][0] in ("const", "full"):
                    live[d] = ("full", t[0][1])       # block base + index
                else:
                    live.pop(d, None)
                continue
            mr2 = RE_RLW2.match(text2)
            if mr2:
                d, s = int(mr2.group(1)), int(mr2.group(2))
                if s in live and live[s][0] == "pre":
                    live[d] = ("full", (live[s][1] << 2) & 0xFFFFFFFF)
                elif s in live and live[s][0] == "preconst":
                    live[d] = ("const", (live[s][1] << 2) & 0xFFFFFFFF)
                else:
                    live.pop(d, None)
                continue
            if text2.startswith("bl "):
                for r in [0] + list(range(3, 13)):
                    live.pop(r, None)
                continue
            md = RE_DEF.match(text2)
            if md and not md.group(1).startswith("st") and not md.group(1).startswith("cmp"):
                live.pop(int(md.group(2)), None)
            if not live:
                break

funcs = {fn for _, fn, _ in hits.values()}
print(f"# block 0x{BLOCK_HI:04X}: {len(hits)} MMIO instruction(s) across {len(funcs)} function(s)")
for addr in sorted(hits):
    text, fn, off = hits[addr]
    o = f"+0x{off:04X}" if off is not None else "+idx"
    print(f"    0x{addr:08X},   # {text:<24} {o:<8} in sub_{fn:08X}")
