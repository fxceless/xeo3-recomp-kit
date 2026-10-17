r"""Let the console's 360 kernel load the dashboard from the game folder.

    python -I tools\patch_kernel_dash.py status  "D:\XeO3 Dash"
    python -I tools\patch_kernel_dash.py apply   "D:\XeO3 Dash"
    python -I tools\patch_kernel_dash.py restore "D:\XeO3 Dash"

WHAT IT DOES. The console folder runs the 360 kernel as two precompiled DLLs
(xeo3_5d12c3b3_00175344.dll and its _no twin). Their image loader refuses dash.xex as a title,
for three reasons that only make sense on a real console, where the dashboard lives in flash:

  * allowed media   dash.xex allows "system flash" only; the game folder appears as a DVD.
  * bounding path   dash.xex names the one path it may load from, \Device\Flash\dash.xex;
                    from the game folder it is \Device\Cdrom0\default.xex.
  * device digest   a title carrying a device ID is expected on a hard drive, memory unit or
                    USB stick, never on a DVD.

None of them can be met by editing dash.xex: its header is integrity-checked in the guest
(changing one harmless byte of a header that boots made it stop booting). So the kernel's
answers change instead, six small edits in all (tools\kernel_dash_patch.json lists each):
the media check takes its "accepted" branch, the two bounding-path failures continue where a
matching path continues, and the device check returns success for DVD media.

What that changes for other titles: nothing that loads today. Modules and games that pass
these checks already took the same branches; the edits only turn a refusal into a load. The
first try, which made the bounding-path check skip itself for every module, broke the boot
(xam took the other path); that is why only the failure exits are redirected.

It only patches the exact DLLs it was made for (MD5 in the json), checks the result against
the MD5 of the copies the dashboard was tested with, and keeps each original as <dll>.orig;
`restore` puts them back. The json holds only the bytes this project wrote, plus a SHA-1 of
each byte run it replaces. Close the emulator first.
"""
import hashlib
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PATCH = json.load(open(os.path.join(HERE, "kernel_dash_patch.json"), encoding="utf-8"))


def md5(data):
    return hashlib.md5(data).hexdigest()


def state(spec, data):
    h = md5(data)
    return "ORIGINAL" if h == spec["source_md5"] else "PATCHED" if h == spec["result_md5"] else "UNKNOWN BUILD"


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("status", "apply", "restore"):
        sys.exit(__doc__)
    cmd, console = sys.argv[1], sys.argv[2]
    files = PATCH["files"]
    for name in files:
        if not os.path.exists(os.path.join(console, name)):
            sys.exit(f"no {name} in {console} -- not a console folder made from the package this was written for")

    # look at both before touching either: they are applied together or not at all
    plan = {}
    for name, spec in files.items():
        p = os.path.join(console, name)
        data = open(p, "rb").read()
        st = state(spec, data)
        print(f"{name}: {st}")
        plan[name] = (p, data, st)

    if cmd == "status":
        for name, spec in files.items():
            for e in spec["edits"]:
                print(f"  {name[:22]}  0x{e['offset']:06X}  {e['what']}")
        return

    if cmd == "restore":
        for name, spec in files.items():
            p = plan[name][0]
            bak = p + ".orig"
            if not os.path.exists(bak):
                print(f"  {name}: no backup, nothing to restore")
                continue
            if md5(open(bak, "rb").read()) != spec["source_md5"]:
                sys.exit(f"{bak} is not the original either -- not touching anything")
            shutil.copyfile(bak, p)
            print(f"  {name}: original restored")
        return

    if any(st == "UNKNOWN BUILD" for _, _, st in plan.values()):
        sys.exit("this console's kernel DLLs are not the build the patch was made for. Not patching. "
                 "(docs\\DASHBOARD.md, 'The kernel loader')")
    out = {}
    for name, spec in files.items():
        p, data, st = plan[name]
        if st == "PATCHED":
            continue
        b = bytearray(data)
        for e in spec["edits"]:
            new = bytes.fromhex(e["new"])
            old = bytes(b[e["offset"]:e["offset"] + len(new)])
            if hashlib.sha1(old).hexdigest() != e["old_sha1"]:
                sys.exit(f"{name}: bytes at 0x{e['offset']:X} are not what the patch expects -- not patching")
            b[e["offset"]:e["offset"] + len(new)] = new
        if md5(b) != spec["result_md5"] or len(b) != spec["size"]:
            sys.exit(f"{name}: the patched result does not match the tested build -- not writing it")
        out[name] = b
    for name, b in out.items():
        p = plan[name][0]
        if not os.path.exists(p + ".orig"):
            shutil.copyfile(p, p + ".orig")
        tmp = p + ".tmp"
        open(tmp, "wb").write(b)
        os.replace(tmp, p)       # a new file, so a hard link elsewhere keeps the original
        print(f"  {name}: patched, md5 {md5(open(p, 'rb').read())} (the build the dashboard was tested with)")
    if not out:
        print("already patched")


if __name__ == "__main__":
    main()
