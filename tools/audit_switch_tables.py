#!/usr/bin/env python3
r"""Audit every [[switch]] block against the GUEST IMAGE. Launch-free.

Written for s4, after s3 died at sub_822D8180 on a table XenonAnalyse got wrong.
The notes recorded that failure as "switch.toml has 4 labels, the guest has 5" --
a truncation. It is worse than that, and the register is the tell:

    822D8188 rlwinm r0,r5,2,0,29     <- the guest indexes with r5
    switch.toml                       r = 9, default = 0x822D8160, 4 labels
    emitted C++                       switch (ADJ(ctx)->r9.u32)

0x822D8160 is the tail of the PREVIOUS function, which ends in `blr` at
0x822D8178, and that function contains its own absolute switch at 0x822D8098
which genuinely uses r9 and genuinely has 4 labels. ScanTable walks backwards up
to maxBack=128 instructions from the matched dispatch and takes the FIRST
bgt/ble + cmplwi pair it meets, with no stop at a basic-block boundary. So it
crossed that blr and stole the preceding switch's bounds check ENTIRE: register,
default label and length. The labels are not short, they belong to another
switch. TableLabelsPlausible cannot catch that -- the four stolen labels are
word-aligned addresses inside a code section, just the wrong four.

That makes it a CLASS of defect over all 876 blocks, not one bad entry, so this
finds them all at once instead of one crash per launch.

HOW IT ESTABLISHES TRUST IN ITSELF
----------------------------------
The decoder replicates ScanTable and ReadTable exactly, so reproducing an entry's
recorded r / default / labels proves the decode against XenonAnalyse's own
behaviour. An entry that does NOT reproduce is either a decoder bug or a
deliberate override from apply_s4_switch_fixes.py; it is not dismissed either
way, it falls through to the independent signals below and is reported as
OVERRIDDEN_OK only if they endorse it. (Bailing out on non-reproduction would
leave the audit unable to re-validate its own corrections, which is exactly the
gate this has to provide.) The two independent signals:

  1. REGISTER (primary). The index register is the one feeding the table load --
     ground truth by construction, since it is what the hardware indexes with.
     A compare stolen from another function is overwhelmingly likely to name a
     different register. Copies are handled by WIDENING the accepted set
     (alias_set propagates `mr` in reverse), never by moving the truth: an
     earlier version followed copy chains from the index and manufactured 51
     false mismatches against entries whose recorded register was correct.

  2. LENGTH (independent, inline tables only). When the table sits inline at
     bctr+4 -- the dominant shape here -- walking its words gives a length that
     owes NOTHING to the compare. Only targets AFTER the table bound its end; a
     case that branches backwards says nothing about the length, and treating it
     as a bound (which scan_jumptables_f2.py does) produced 32 more false alarms.
     Where the rule does not apply (out-of-line or non-absolute tables) the entry
     is reported REVIEW_NO_INDEP_LEN rather than auto-corrected.

Both signals disagreeing with the toml while agreeing with each other is a
certain defect. All three false-positive classes above were found by hand-reading
the guest at the flagged sites, which is the only reason the surviving findings
can be trusted.

    python audit_switch_tables.py [--toml] [--verbose]

--toml prints corrected [[switch]] blocks for every entry this can fully derive.
Nothing is written in place; apply_s4_switch_fixes.py patches switch.toml
deliberately, entry by entry.
"""
import re
import struct
import sys
from pathlib import Path

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kitgame
HERE = kitgame.game_dir()        # python -I tools\audit_switch_tables.py games\<name> [--toml] [--verbose]
IMAGE = HERE / "image.bin"
SWITCH = HERE / "switch.toml"

IMG_LO, IMG_HI = kitgame.image_range(HERE)

SWITCH_ABSOLUTE, SWITCH_COMPUTED, SWITCH_BYTEOFFSET, SWITCH_SHORTOFFSET = 0, 1, 2, 3
TYPE_NAME = {0: "ABSOLUTE", 1: "COMPUTED", 2: "BYTEOFFSET", 3: "SHORTOFFSET"}


# --------------------------------------------------------------------------
# image access
# --------------------------------------------------------------------------
class Img:
    def __init__(self, path):
        self.d = path.read_bytes()
        self.base = IMG_LO

    def ok(self, va, n=4):
        return self.base <= va and (va - self.base + n) <= len(self.d)

    def u32(self, va):
        return struct.unpack_from(">I", self.d, va - self.base)[0]

    def u16(self, va):
        return struct.unpack_from(">H", self.d, va - self.base)[0]

    def u8(self, va):
        return self.d[va - self.base]


# --------------------------------------------------------------------------
# just enough PPC to replicate ScanTable/ReadTable
# --------------------------------------------------------------------------
def s16(v):
    return v - 0x10000 if v & 0x8000 else v


class Insn:
    __slots__ = ("w", "va", "id", "f")

    def __init__(self, w, va):
        self.w = w
        self.va = va
        self.id = None
        self.f = {}
        self.decode()

    def decode(self):
        w = self.w
        op = (w >> 26) & 0x3F
        rD = (w >> 21) & 0x1F
        rA = (w >> 16) & 0x1F
        rB = (w >> 11) & 0x1F

        if op == 15:
            self.id = "addis"
            self.f = dict(d=rD, a=rA, uimm=w & 0xFFFF, simm=s16(w & 0xFFFF))
        elif op == 14:
            self.id = "addi"
            self.f = dict(d=rD, a=rA, uimm=w & 0xFFFF, simm=s16(w & 0xFFFF))
        elif op == 24:
            self.id = "ori"
            self.f = dict(a=rA, s=rD, uimm=w & 0xFFFF)
        elif op == 21:
            # rlwinm rA,rS,SH,MB,ME -- destination is rA, SOURCE is rS.
            self.id = "rlwinm"
            self.f = dict(a=rA, s=rD, sh=rB, mb=(w >> 6) & 0x1F, me=(w >> 1) & 0x1F)
        elif op == 10:
            # cmpli; cmplwi is the L=0 form
            self.id = "cmplwi" if not ((w >> 21) & 1) else "cmpldi"
            self.f = dict(crf=(w >> 23) & 7, a=rA, uimm=w & 0xFFFF)
        elif op == 11:
            self.id = "cmpwi" if not ((w >> 21) & 1) else "cmpdi"
            self.f = dict(crf=(w >> 23) & 7, a=rA, simm=s16(w & 0xFFFF))
        elif op == 16:
            bo, bi = rD, rA
            bd = w & 0xFFFC
            if bd & 0x8000:
                bd -= 0x10000
            tgt = (bd if (w >> 1) & 1 else self.va + bd) & 0xFFFFFFFF
            self.id = "bc"
            self.f = dict(bo=bo, bi=bi, target=tgt, lk=w & 1)
        elif op == 18:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            self.id = "b"
            self.f = dict(target=(li if (w >> 1) & 1 else self.va + li) & 0xFFFFFFFF,
                          lk=w & 1)
        elif op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 16:
                self.id = "bclr"
                self.f = dict(bo=rD, bi=rA, lk=w & 1)
            elif xo == 528:
                self.id = "bcctr"
                self.f = dict(bo=rD, bi=rA, lk=w & 1)
        elif op == 31:
            xo = (w >> 1) & 0x3FF
            if xo == 23:
                self.id, self.f = "lwzx", dict(d=rD, a=rA, b=rB)
            elif xo == 87:
                self.id, self.f = "lbzx", dict(d=rD, a=rA, b=rB)
            elif xo == 279:
                self.id, self.f = "lhzx", dict(d=rD, a=rA, b=rB)
            elif xo == 266:
                self.id, self.f = "add", dict(d=rD, a=rA, b=rB)
            elif xo == 444:
                self.id = "mr" if rD == rB else "or"
                self.f = dict(a=rA, s=rD, b=rB)
            elif xo == 467:
                spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
                self.id = "mtctr" if spr == 9 else "mtspr"
                self.f = dict(s=rD, spr=spr)

    # --- helpers matching XenonAnalyse's opcode ids -----------------------
    def is_bgt(self):
        return self.id == "bc" and (self.f["bo"] & ~1) == 12 and (self.f["bi"] & 3) == 1

    def is_ble(self):
        return self.id == "bc" and (self.f["bo"] & ~1) == 4 and (self.f["bi"] & 3) == 1

    def is_bgtlr(self):
        return self.id == "bclr" and (self.f["bo"] & ~1) == 12 and (self.f["bi"] & 3) == 1

    def is_blelr(self):
        return self.id == "bclr" and (self.f["bo"] & ~1) == 4 and (self.f["bi"] & 3) == 1

    def is_bctr(self):
        return self.id == "bcctr" and self.f["bo"] == 20 and not self.f["lk"]

    def is_blr(self):
        return self.id == "bclr" and self.f["bo"] == 20 and not self.f["lk"]

    def is_uncond_b(self):
        return self.id == "b" and not self.f["lk"]

    def terminates_block(self):
        """Cannot fall through to the next instruction."""
        return self.is_blr() or self.is_bctr() or self.is_uncond_b()


def at(img, va):
    if not img.ok(va):
        return None
    return Insn(img.u32(va), va)


def defines(ins):
    """Registers this instruction writes. None means 'not modelled'."""
    if ins is None:
        return None
    op = (ins.w >> 26) & 0x3F
    rD = (ins.w >> 21) & 0x1F
    rA = (ins.w >> 16) & 0x1F
    if ins.id in ("addis", "addi", "lwzx", "lbzx", "lhzx", "add"):
        return {rD}
    if ins.id in ("ori", "mr", "or", "rlwinm"):
        return {rA}
    if ins.id in ("cmplwi", "cmpldi", "cmpwi", "cmpdi", "bc", "b", "bclr",
                  "bcctr", "mtctr", "mtspr"):
        return set()
    if op in (32, 33, 34, 35, 40, 41, 42, 43, 46, 48, 49, 50, 51, 58):
        return {rD}          # D-form loads
    if op in (36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55, 62):
        return set()         # D-form stores
    if op in (24, 25, 26, 27, 28, 29):
        return {rA}          # ori/oris/xori/xoris/andi./andis.
    if op in (7, 12, 13):
        return {rD}          # mulli, addic, addic.
    return None


def nearest_def(img, from_va, reg, span=24):
    """The closest instruction before from_va that writes `reg`.

    Stops at a block terminator. Instructions whose destination is not modelled
    are skipped rather than treated as writers -- being wrong in that direction
    only costs a missed unscale, never a manufactured finding.
    """
    va = from_va - 4
    for _ in range(span):
        if va < IMG_LO:
            return None
        ins = at(img, va)
        if ins is not None:
            d = defines(ins)
            if d and reg in d:
                return ins
            if ins.terminates_block():
                return None
        va -= 4
    return None


# --------------------------------------------------------------------------
# faithful replication of XenonAnalyse
# --------------------------------------------------------------------------
def scan_table(img, match_va, max_back=128):
    """Replicate ScanTable(): returns (r, default, nlabels, terminators_crossed).

    Walks backwards from the MATCH position (i = 0 examines the match itself),
    takes the first bgt/ble/bgtlr/blelr, then the first cmplwi on that CR field.
    No stop at a block boundary -- that absence is the bug being measured, so it
    is faithfully reproduced here and counted.
    """
    cr = None
    default = 0
    crossed = 0
    crossed_at_latch = None

    for i in range(max_back):
        va = match_va - 4 * i
        ins = at(img, va)
        if ins is None or ins.id is None:
            continue

        if i > 0 and ins.terminates_block():
            crossed += 1

        if cr is None and (ins.is_bgt() or ins.is_bgtlr() or ins.is_ble() or ins.is_blelr()):
            cr = ins.f["bi"] >> 2
            if ins.id == "bc":          # the *lr forms carry no target operand
                default = ins.f["target"]
        elif cr is not None:
            if ins.id == "cmplwi" and ins.f["crf"] == cr:
                crossed_at_latch = crossed
                return ins.f["a"], default, ins.f["uimm"] + 1, crossed_at_latch

    return None, 0, 0, crossed


def search_instr(img, match_va, start, want, limit=64):
    """Replicate SearchInstr(): first `want` at or after index `start`."""
    for k in range(start, limit):
        ins = at(img, match_va + 4 * k)
        if ins is not None and ins.id == want:
            return k, ins
    return None, None


def read_table(img, match_va, ttype, nlabels):
    """Replicate ReadTable(): returns (table_va, labels) or (None, None)."""
    head = at(img, match_va)
    if head is None or head.id != "addis":
        return None, None

    p = head.f["uimm"] << 16
    k, ins = search_instr(img, match_va, 1, "addi")
    if ins is None:
        return None, None
    p = (p + ins.f["simm"]) & 0xFFFFFFFF

    labels = []
    try:
        if ttype == SWITCH_ABSOLUTE:
            for i in range(nlabels):
                labels.append(img.u32(p + 4 * i))
        elif ttype == SWITCH_COMPUTED:
            k, ins = search_instr(img, match_va, 1, "rlwinm")
            if ins is None:
                return p, None
            shift = ins.f["sh"]
            k, ins = search_instr(img, match_va, k, "addis")
            if ins is None:
                return p, None
            b = ins.f["uimm"] << 16
            k, ins = search_instr(img, match_va, k, "addi")
            if ins is None:
                return p, None
            b = (b + ins.f["simm"]) & 0xFFFFFFFF
            for i in range(nlabels):
                labels.append((b + (img.u8(p + i) << shift)) & 0xFFFFFFFF)
        elif ttype == SWITCH_BYTEOFFSET:
            k, ins = search_instr(img, match_va, 2, "addis")
            if ins is None:
                return p, None
            b = ins.f["uimm"] << 16
            k, ins = search_instr(img, match_va, k, "addi")
            if ins is None:
                return p, None
            b = (b + ins.f["simm"]) & 0xFFFFFFFF
            for i in range(nlabels):
                labels.append((b + img.u8(p + i)) & 0xFFFFFFFF)
        elif ttype == SWITCH_SHORTOFFSET:
            k, ins = search_instr(img, match_va, 3, "addis")
            if ins is None:
                return p, None
            b = ins.f["uimm"] << 16
            k, ins = search_instr(img, match_va, k, "addi")
            if ins is None:
                return p, None
            b = (b + ins.f["simm"]) & 0xFFFFFFFF
            for i in range(nlabels):
                labels.append((b + img.u16(p + 2 * i)) & 0xFFFFFFFF)
    except (struct.error, IndexError):
        return p, None

    return p, labels


# --------------------------------------------------------------------------
# the independent signals
# --------------------------------------------------------------------------
def find_dispatch(img, base_va, span=40):
    """Locate the bctr for this entry and the load that feeds it.

    Handles BOTH base forms in the file: XenonAnalyse writes base = the matched
    `lis`, scan_jumptables_f2.py writes base = the `bctr` itself.
    """
    head = at(img, base_va)
    if head is not None and head.is_bctr():
        bctr_va = base_va
    else:
        bctr_va = None
        for k in range(span):
            ins = at(img, base_va + 4 * k)
            if ins is not None and ins.is_bctr():
                bctr_va = base_va + 4 * k
                break
        if bctr_va is None:
            return None, None

    # the table load is the last lwzx/lbzx/lhzx before the bctr
    load = None
    va = bctr_va - 4
    while va >= bctr_va - 4 * span and va >= IMG_LO:
        ins = at(img, va)
        if ins is not None and ins.id in ("lwzx", "lbzx", "lhzx"):
            load = ins
            break
        if ins is not None and ins.terminates_block():
            break
        va -= 4
    return bctr_va, load


def true_index_register(img, bctr_va, load, table_va, span=40):
    """The register the guest actually indexes with.

    EA = rA + rB for the table load; whichever operand is NOT the register
    holding the table base is the (possibly scaled) index. If that register was
    produced by an rlwinm, the real index is the rlwinm's SOURCE.

    Returns (reg, scaled_via_rlwinm, copy_chain) where copy_chain lists any
    `mr` hops followed backwards from the index register.
    """
    if load is None:
        return None, False, []

    ra, rb = load.f["a"], load.f["b"]

    # which operand carries the table base? the one an addis/addi built.
    base_reg = None
    va = load.va - 4
    while va >= load.va - 4 * span and va >= IMG_LO:
        ins = at(img, va)
        if ins is None:
            va -= 4
            continue
        # `addi rD,rA,lo` where some earlier `addis rA,0,hi` built rA. rA need
        # not equal rD: 0x82BF9278 does `addis r10,0,hi` / `addi r8,r10,lo` and
        # indexes with `lwzx r7,r9,r8`, so the table base lands in r8 via r10.
        if ins.id == "addi" and ins.f["d"] in (ra, rb):
            prev = nearest_def(img, va, ins.f["a"])
            if prev is not None and prev.id == "addis":
                base_reg = ins.f["d"]
                break
        if ins.id == "addis" and ins.f["d"] in (ra, rb):
            base_reg = ins.f["d"]
            break
        if ins.terminates_block():
            break
        va -= 4

    if base_reg == ra:
        idx = rb
    elif base_reg == rb:
        idx = ra
    else:
        # fall back: the scaled index is the one an rlwinm produced
        idx = None
        for cand in (rb, ra):
            v = load.va - 4
            while v >= load.va - 4 * span and v >= IMG_LO:
                ins = at(img, v)
                if ins is not None and ins.id == "rlwinm" and ins.f["a"] == cand:
                    idx = cand
                    break
                v -= 4
            if idx is not None:
                break
        if idx is None:
            idx = rb

    # Unscale, but ONLY through the NEAREST definition of the index register.
    #
    # Scanning back for "any rlwinm writing idx" is wrong when the register is
    # rebuilt afterwards. At 0x831141E8 the guest forms
    #     rlwinm r11,r6,1,0,30 ; rlwinm r9,r5,0,24,31 ; or r11,r11,r9
    #     cmplwi r11,15 ; bgt ; ... ; lbzx r0,r12,r11
    # so the index really is r11 -- exactly what switch.toml records -- yet the
    # loose scan walked past the `or` to the rlwinm and reported r6, condemning
    # a correct entry.
    scaled = False
    d = nearest_def(img, load.va, idx)
    if d is not None and d.id == "rlwinm" and d.f["sh"] in (1, 2, 3):
        idx = d.f["s"]
        scaled = True

    # Do NOT follow `mr` copies backwards here. An earlier version did, and it
    # manufactured 51 false mismatches: at 0x823936F8 the guest indexes with r11
    # and switch.toml correctly says r=11, but an `mr r11,r15` two instructions
    # before the dispatch moved the "true" register to r15 and the entry was
    # condemned. Copies make MORE registers acceptable, they do not move the
    # truth -- the register the dispatch indexes with is the one the rlwinm
    # reads, full stop. Widening lives in alias_set(), which propagates copies in
    # the correct direction (destination in set => add the source).
    chain = []
    return idx, scaled, chain


def alias_set(img, bctr_va, idx_reg, span=128):
    """Registers that have held the index value, walking back from the dispatch.

    Reverse dataflow WITH REDEFINITION TRACKING. Once a register in the live set
    is written by anything that is not a register-to-register copy, no earlier
    instruction mentioning it refers to the index any more, so it leaves the live
    set; when the live set empties the walk is over. The verdict is taken against
    the UNION of everything that was ever live, so a compare written against a
    pre-copy register still validates.
    //
    THIS WAS THE s4 LAUNCH BUG. The first version only ever ADDED registers and
    never removed them, so it happily reported r5 as an alias of r10 at
    0x82B8E638 -- where the guest does `lwz r10,308(r1)` in the instruction
    immediately before the dispatch, meaning NOTHING earlier can alias it. The
    loose set returned {5, 8, 10, 27}, the audit blessed a table recorded as r=5,
    and the launch died at that dispatch with index=65535. Worse, the C++
    ScanFix had ALREADY rejected that entry correctly, and this function is what
    convinced me the rejection was a false loss and had to be "fixed".
    An instrument that only widens can only ever excuse.
    """
    live = {idx_reg}
    seen = {idx_reg}
    va = bctr_va - 4
    for _ in range(span):
        if va < IMG_LO or not live:
            break
        ins = at(img, va)
        if ins is not None:
            d = defines(ins) or set()
            hit = live & d
            if hit:
                # Remove BEFORE adding the copy source. The other order makes a
                # SELF-copy annihilate itself: `rlwinm r11,r11,0,0,31` (a plain
                # zero-extend, and a real idiom -- 0x8223FB90 and 0x82CAE6AC both
                # sit right behind one) would add r11 and then immediately drop
                # it, emptying the set and discarding a valid table.
                live -= hit
                if ins.id == "mr" and ins.f["a"] in hit:
                    live.add(ins.f["s"])
                    seen.add(ins.f["s"])
                elif ins.id == "rlwinm" and ins.f["a"] in hit and ins.f["sh"] == 0:
                    live.add(ins.f["s"])
                    seen.add(ins.f["s"])
        va -= 4
    return seen


def inline_walk(img, table_va, bctr_va):
    """Self-terminating length for an inline absolute table.

    Only valid when the table sits immediately after the dispatch: walk words
    while each is a word-aligned in-image address, stopping at the lowest target
    seen so far (which is the first instruction after the table).
    """
    if table_va != bctr_va + 4:
        return None

    # Only targets that lie AFTER the table can bound its end. A case that
    # branches BACKWARDS is a perfectly ordinary target and says nothing about
    # the table's length.
    #
    # scan_jumptables_f2.py takes the min over ALL targets, which is right for
    # the unbounded tables it was written for but wrong in general: at
    # 0x82C8AC2C the guest's table[0] is 0x82C8AB9C, 0xA8 bytes BEFORE the table
    # itself, so an all-targets minimum stops the walk after one word and
    # reports a 9-entry table as 1. Thirty-two entries failed that way before
    # this was corrected -- every one of them a false alarm against a table
    # whose recorded length was right.
    labels, va, limit = [], table_va, None
    while limit is None or va < limit:
        if not img.ok(va):
            break
        w = img.u32(va)
        if not (IMG_LO <= w < IMG_HI) or (w & 3):
            break
        labels.append(w)
        if w > table_va:
            limit = w if limit is None else min(limit, w)
        va += 4
        if len(labels) > 4096:
            break
    return labels


# --------------------------------------------------------------------------
# switch.toml
# --------------------------------------------------------------------------
def parse_switch_toml(path):
    text = path.read_text(errors="replace")
    ttype = SWITCH_ABSOLUTE
    entries = []
    cur = None
    for line in text.splitlines():
        if "---- ABSOLUTE JUMPTABLE" in line:
            ttype = SWITCH_ABSOLUTE
        elif "---- COMPUTED JUMPTABLE" in line:
            ttype = SWITCH_COMPUTED
        elif "---- OFFSETED JUMPTABLE" in line:
            ttype = SWITCH_BYTEOFFSET      # refined per entry from the dispatch
        if line.strip() == "[[switch]]":
            cur = dict(type=ttype, labels=[])
            entries.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r"^base = (0x[0-9A-Fa-f]+)", line)
        if m:
            cur["base"] = int(m.group(1), 16)
            continue
        m = re.match(r"^r = (\d+)", line)
        if m:
            cur["r"] = int(m.group(1))
            continue
        m = re.match(r"^default = (0x[0-9A-Fa-f]+)", line)
        if m:
            cur["default"] = int(m.group(1), 16)
            continue
        m = re.match(r"^\s+(0x[0-9A-Fa-f]+),", line)
        if m:
            cur["labels"].append(int(m.group(1), 16))
    return entries


def refine_type(img, base_va, declared):
    """OFFSETED holds two subtypes; trust the dispatch, not the section comment."""
    bctr_va, load = find_dispatch(img, base_va)
    if load is None:
        return declared, bctr_va, load
    if load.id == "lwzx":
        return SWITCH_ABSOLUTE, bctr_va, load
    if load.id == "lhzx":
        return SWITCH_SHORTOFFSET, bctr_va, load
    if load.id == "lbzx":
        # computed shifts the byte; byteoffset does not
        return (SWITCH_COMPUTED if declared == SWITCH_COMPUTED
                else SWITCH_BYTEOFFSET), bctr_va, load
    return declared, bctr_va, load


# --------------------------------------------------------------------------
def main():
    verbose = "--verbose" in sys.argv
    want_toml = "--toml" in sys.argv

    if not IMAGE.exists():
        print(f"missing {IMAGE}\n"
              f"generate it with:\n"
              f"  XenonAnalyse.exe ./xex/default.xex <scratch>/regen.toml ./image.bin")
        return 1

    img = Img(IMAGE)
    entries = parse_switch_toml(SWITCH)
    print(f"# image {IMAGE.name} 0x{len(img.d):X} bytes @ 0x{img.base:08X}")
    print(f"# switch.toml: {len(entries)} [[switch]] blocks")
    print()

    verdicts = {}
    findings = []
    stats = dict(scanner_form=0, copy_chain=0, crossed_terminator=0,
                 inline=0, outofline=0)

    for e in entries:
        base = e["base"]
        declared = e["type"]
        ttype, bctr_va, load = refine_type(img, base, declared)
        head = at(img, base)
        scanner_form = head is not None and head.is_bctr()
        if scanner_form:
            stats["scanner_form"] += 1

        rec_r = e.get("r")
        rec_labels = e["labels"]
        rec_default = e.get("default", 0)

        row = dict(base=base, type=ttype, rec_r=rec_r, rec_n=len(rec_labels),
                   bctr=bctr_va, scanner=scanner_form)

        if bctr_va is None or load is None:
            verdicts.setdefault("NO_DISPATCH", []).append(row)
            continue

        # ---- replicate XenonAnalyse, but only for entries it produced -----
        repro_ok = None
        crossed = 0
        if not scanner_form:
            sr, sdef, sn, crossed = scan_table(img, base)
            table_va, slabels = read_table(img, base, ttype, sn)
            repro_ok = (sr == rec_r and sn == len(rec_labels)
                        and (slabels or []) == rec_labels)
            row.update(scan_r=sr, scan_n=sn, scan_default=sdef, table=table_va,
                       crossed=crossed)
            if crossed:
                stats["crossed_terminator"] += 1
            # An entry that does NOT match ScanTable's output is either a
            # decoder bug or a deliberate override applied by
            # apply_s4_switch_fixes.py. Do not bail here -- fall through to the
            # independent signals and let them decide. If they endorse the toml
            # the entry is OVERRIDDEN_OK; if they do not it surfaces as a normal
            # mismatch. Bailing would have made the audit unable to re-validate
            # its own corrections, which is precisely the gate we need.
            row["overridden"] = not repro_ok
            if not repro_ok:
                row["why"] = (f"differs from ScanTable (scan r={sr} n={sn}) -- "
                              f"expected for an audited override")
        else:
            table_va = bctr_va + 4
            row["table"] = table_va

        # ---- signal 1: the register the guest really indexes with ---------
        tr, scaled, chain = true_index_register(img, bctr_va, load, table_va)
        row.update(true_r=tr, chain=chain)
        if chain:
            stats["copy_chain"] += 1

        aliases = alias_set(img, bctr_va, tr) if tr is not None else set()
        reg_ok = rec_r in aliases if tr is not None else True
        if tr is not None and rec_r == tr:
            reg_ok = True

        # ---- signal 2: independent length for inline absolute tables ------
        inline = None
        if ttype == SWITCH_ABSOLUTE and table_va == bctr_va + 4:
            stats["inline"] += 1
            inline = inline_walk(img, table_va, bctr_va)
        else:
            stats["outofline"] += 1
        row["inline_n"] = len(inline) if inline is not None else None

        len_ok = True if inline is None else (len(inline) == len(rec_labels))
        labels_ok = True if inline is None else (inline == rec_labels)

        if not reg_ok and not len_ok:
            row["why"] = (f"toml r={rec_r} but guest indexes r{tr}; "
                          f"toml {len(rec_labels)} labels but inline table has {len(inline)}")
            verdicts.setdefault("MISLATCH", []).append(row)
            findings.append((row, inline, tr))
        elif not reg_ok:
            row["why"] = f"toml r={rec_r} not in alias set of guest index r{tr} {sorted(aliases)}"
            verdicts.setdefault("MISMATCH_REG", []).append(row)
            findings.append((row, inline, tr))
        elif not len_ok:
            row["why"] = f"toml {len(rec_labels)} labels, inline table has {len(inline)}"
            verdicts.setdefault("MISMATCH_LEN", []).append(row)
            findings.append((row, inline, tr))
        elif not labels_ok:
            row["why"] = "label values differ from inline table"
            verdicts.setdefault("MISMATCH_LABELS", []).append(row)
            findings.append((row, inline, tr))
        elif inline is None:
            verdicts.setdefault("REVIEW_NO_INDEP_LEN", []).append(row)
        elif row.get("overridden"):
            verdicts.setdefault("OVERRIDDEN_OK", []).append(row)
        else:
            verdicts.setdefault("OK", []).append(row)

    # ---------------- report ----------------
    order = ["OK", "OVERRIDDEN_OK", "REVIEW_NO_INDEP_LEN", "MISLATCH", "MISMATCH_REG",
             "MISMATCH_LEN", "MISMATCH_LABELS", "REPRO_FAIL", "NO_DISPATCH"]
    print("# ---- verdict summary ----")
    for k in order:
        if k in verdicts:
            print(f"#   {k:22s} {len(verdicts[k]):5d}")
    for k in verdicts:
        if k not in order:
            print(f"#   {k:22s} {len(verdicts[k]):5d}")
    print()
    print("# ---- measurements ----")
    print(f"#   scanner-form entries (base = bctr) : {stats['scanner_form']}")
    print(f"#   inline absolute tables            : {stats['inline']}")
    print(f"#   out-of-line / non-absolute        : {stats['outofline']}")
    print(f"#   index reached via mr copy chain   : {stats['copy_chain']}")
    print(f"#   ScanTable crossed a terminator    : {stats['crossed_terminator']}")
    print()

    for k in order:
        if k in ("OK", "OVERRIDDEN_OK") or k not in verdicts:
            continue
        print(f"# ---- {k} ({len(verdicts[k])}) ----")
        for row in verdicts[k]:
            print(f"#   base 0x{row['base']:08X} bctr 0x{(row.get('bctr') or 0):08X} "
                  f"{TYPE_NAME.get(row['type'],'?'):11s} "
                  f"r={row.get('rec_r')} n={row.get('rec_n')} "
                  f"true_r={row.get('true_r')} inline_n={row.get('inline_n')} "
                  f"crossed={row.get('crossed')}  {row.get('why','')}")
        print()

    if verbose:
        print("# ---- single-label tables (legal but worth eyes) ----")
        for row in verdicts.get("OK", []) + verdicts.get("REVIEW_NO_INDEP_LEN", []):
            if row["rec_n"] == 1:
                print(f"#   base 0x{row['base']:08X} r={row['rec_r']} true_r={row.get('true_r')}")
        print()

    if want_toml and findings:
        print("# ---- corrected [[switch]] blocks (scanner form: base = bctr) ----")
        for row, inline, tr in findings:
            if inline is None or tr is None:
                print(f"# base 0x{row['base']:08X}: cannot derive automatically -- {row.get('why')}")
                continue
            print()
            print(f"# s4 audit: was r={row['rec_r']} with {row['rec_n']} labels at "
                  f"base 0x{row['base']:08X}; {row.get('why')}")
            print("[[switch]]")
            print(f"base = 0x{row['bctr']:08X}")
            print(f"r = {tr}")
            print(f"default = 0x{min(inline):08X}")
            print("labels = [")
            for l in inline:
                print(f"    0x{l:08X},")
            print("]")

    return 0


if __name__ == "__main__":
    sys.exit(main())
