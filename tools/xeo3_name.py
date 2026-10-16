#!/usr/bin/env python3
r"""Compute the expected XeO3 precompiled-DLL name for a 360 module.

Naming rules (reverse-engineered from Xeo3\Emu.exe, validated 2026-08-11):

  Kernel (PE image, e.g. xboxkrnlce.bin / xboxkrnlcf.bin):
      xeo3_%08x_%08x  =  PE FILE_HEADER.TimeDateStamp, PE OPTIONAL_HEADER.CheckSum
      (xboxkrnlce.bin -> xeo3_544f6755_00186bb3, confirmed against Emu's probe)

  XEX2 module:
      xeo3_%08x_%08x_%08x_%08x_%08x
      = the 20-byte header digest stored at security_info + 0x164
        (security_info offset = big-endian u32 at file offset 0x10),
        taken 4 bytes at a time, each group read LITTLE-endian
        (i.e. byte-swapped relative to how the SHA-1 bytes lie in the file).
      Validated against all 19 system/game XEXs in the Castle Crashers dump
      vs. the 79 precompiled DLL names in cc_dlls_hold/ + Xeo3_CC/.

Emu also probes a "_no" variant of each name (…_no.dll).
"""
import struct, sys, os


def xeo3_name(path):
    with open(path, "rb") as f:
        d = f.read(0x4000)
    if d[:4] == b"XEX2":
        sec = struct.unpack_from(">I", d, 0x10)[0]
        digest = d[sec + 0x164 : sec + 0x178]
        if len(digest) != 20:
            raise ValueError("truncated XEX header")
        words = struct.unpack("<5I", digest)
        return "xeo3_" + "_".join(f"{w:08x}" for w in words)
    if d[:2] == b"MZ":
        e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
        if d[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
            raise ValueError("MZ without PE header")
        timestamp = struct.unpack_from("<I", d, e_lfanew + 8)[0]
        # CheckSum is at optional-header offset 0x40 for both PE32/PE32+
        checksum = struct.unpack_from("<I", d, e_lfanew + 0x18 + 0x40)[0]
        return f"xeo3_{timestamp:08x}_{checksum:08x}"
    raise ValueError(f"unrecognized module format: {d[:4]!r}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"usage: {os.path.basename(sys.argv[0])} <module.xex|kernel.bin> ...")
        sys.exit(1)
    for p in sys.argv[1:]:
        name = xeo3_name(p)
        print(f"{p}: {name}.dll  (and {name}_no.dll)")
