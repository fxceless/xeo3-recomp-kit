r"""Apply the kit's graphics fixes to the emulator's VGPUDX12.dll in YOUR console folder.

    python -I tools\patch_vgpu.py status  "D:\XeO3 Console"
    python -I tools\patch_vgpu.py apply   "D:\XeO3 Console"
    python -I tools\patch_vgpu.py restore "D:\XeO3 Console"

Three bugs in the emulator's GPU layer that the runtime cannot fix from outside, because they
live in shader code VGPUDX12.dll compiles or ships (found and verified in game on Need for
Speed: Most Wanted; details in docs\RUNTIME.md):

  * blue world   the resolve write-back compute shader packed colours R/B swapped for the
                 360's byte order, so every scene that went through a resolve came back blue.
                 A rebuilt shader goes into a new .xfix section and its table entry points there.
  * shards       the vertex-shader template's FetchIndexBuffer read indices with a raw dword
                 load through a typed view; on PC GPUs every index at an odd offset came out one
                 element early (long black triangles). Rewritten in place, same length.
  * mirror       the Cube() helper sampled the side faces of environment cubes upside down.
                 Two characters, in place.

It only patches the exact file it was made from (MD5 below) and checks the result against
the MD5 of the copy that was tested in game. The original is kept as VGPUDX12.dll.orig;
`restore` puts it back. The patch file holds only the bytes this project wrote.

After apply/restore, clear the emulator's shader cache, or old shaders built from the old
templates keep being used. This script prints the folder.
"""
import base64
import glob
import hashlib
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PATCH = json.load(open(os.path.join(HERE, "vgpu_patch.json"), encoding="utf-8"))


def md5(data):
    return hashlib.md5(data).hexdigest()


def cache_dirs():
    base = os.environ.get("LOCALAPPDATA", "")
    return sorted(glob.glob(os.path.join(base, "Xbox360BackwardCompatibil.*", "ShaderCache")))


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("status", "apply", "restore"):
        sys.exit(__doc__)
    cmd, console = sys.argv[1], sys.argv[2]
    dll = os.path.join(console, PATCH["file"])
    bak = dll + ".orig"
    if not os.path.exists(dll):
        sys.exit(f"no {PATCH['file']} in {console}")
    data = open(dll, "rb").read()
    h = md5(data)
    state = ("ORIGINAL" if h == PATCH["source_md5"] else
             "PATCHED" if h == PATCH["result_md5"] else "UNKNOWN BUILD")
    print(f"{dll}: {state} (md5 {h})")

    if cmd == "status":
        for e in PATCH["edits"]:
            print(f"  0x{e['offset']:07X} {len(bytes.fromhex(e['new'])):4d} B  {e['what']}")
        print(f"  append  {len(base64.b64decode(PATCH['append']['base64'])):4d} B  {PATCH['append']['what']}")
        return

    if cmd == "restore":
        if not os.path.exists(bak):
            sys.exit(f"no backup {bak}; nothing to restore")
        if md5(open(bak, "rb").read()) != PATCH["source_md5"]:
            sys.exit(f"{bak} is not the original either -- not touching anything")
        shutil.copyfile(bak, dll)
        print("restored the original")
    else:
        if state == "PATCHED":
            print("already patched")
            return
        if state != "ORIGINAL":
            sys.exit("this is not the VGPUDX12.dll the patch was made for "
                     f"(expected md5 {PATCH['source_md5']}). Not patching. The runtime's in-memory "
                     "fixes check their own signatures and may still apply.")
        out = bytearray(data)
        for e in PATCH["edits"]:
            new = bytes.fromhex(e["new"])
            old = bytes(out[e["offset"]:e["offset"] + len(new)])
            if hashlib.sha1(old).hexdigest() != e["old_sha1"]:
                sys.exit(f"bytes at 0x{e['offset']:X} are not what the patch expects -- not patching")
            out[e["offset"]:e["offset"] + len(new)] = new
        out += base64.b64decode(PATCH["append"]["base64"])
        if md5(out) != PATCH["result_md5"] or len(out) != PATCH["result_size"]:
            sys.exit("the patched result does not match the tested build -- not writing it")
        if not os.path.exists(bak):
            shutil.copyfile(dll, bak)
            print(f"original kept as {bak}")
        tmp = dll + ".tmp"
        open(tmp, "wb").write(out)
        os.replace(tmp, dll)
        print(f"patched: md5 {md5(open(dll, 'rb').read())} (the build that was tested in game)")

    dirs = cache_dirs()
    print("\nNow clear the emulator's shader cache (move it aside, don't just trust it to rebuild):")
    for d in dirs or [r"%LOCALAPPDATA%\Xbox360BackwardCompatibil.<identity>\ShaderCache"]:
        print(f"  {d}")


if __name__ == "__main__":
    main()
