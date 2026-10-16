r"""Shared helpers for the kit's Python tools: which game folder, and where its image lives.

Every tool takes the game folder (games\<name>) as its first non-flag argument and falls
back to the current directory, so both of these work:

    python -I tools\mmio_scan.py games\crackdown 0x7FEA
    cd games\crackdown && python -I ..\..\tools\mmio_scan.py . 0x7FEA

A game folder holds: xex\default.xex, config.toml, switch.toml, image.bin (XenonAnalyse's
flat image dump), ppc\ (the recompiler's output plus the runtime) and dll\.
"""
import re
import struct
import sys
from pathlib import Path


def game_dir(argv=None):
    """The first argument that is an existing directory, else the current directory."""
    for a in (sys.argv[1:] if argv is None else argv):
        if not a.startswith("-") and Path(a).is_dir():
            return Path(a).resolve()
    return Path.cwd()


def rest_args(argv=None):
    """The arguments after the game folder (flags included), for tools with extra inputs."""
    args = list(sys.argv[1:] if argv is None else argv)
    for i, a in enumerate(args):
        if not a.startswith("-") and Path(a).is_dir():
            return args[:i] + args[i + 1:]
    return args


def image_range(game):
    """(lo, hi) guest addresses of the game image.

    Read from ppc\\ppc_config.h (written by XenonRecomp) when it exists, else from the
    XEX header (load address + image size), so the tools also work before a recompile.
    """
    cfg = game / "ppc" / "ppc_config.h"
    if cfg.exists():
        t = cfg.read_text(encoding="utf-8", errors="ignore")
        b = re.search(r"PPC_IMAGE_BASE\s+(0x[0-9A-Fa-f]+)", t)
        s = re.search(r"PPC_IMAGE_SIZE\s+(0x[0-9A-Fa-f]+)", t)
        if b and s:
            lo = int(b.group(1), 16)
            return lo, lo + int(s.group(1), 16)
    xex = game / "xex" / "default.xex"
    d = xex.read_bytes()[:0x10000]
    sec = struct.unpack_from(">I", d, 0x10)[0]
    img_size = struct.unpack_from(">I", d, sec + 4)[0]
    load = struct.unpack_from(">I", d, sec + 0x110)[0]
    return load, load + img_size
