r"""Compute function-boundary entries from XenonRecomp switch errors.

Difference from the older bisecting version: that one GUESSES which function contains
the failing switch by bisecting the detected-base list. Once config entries start
adding bases, the bisect can land on a config-added base (0x82BE9680) while the
recompiler actually emitted the switch inside a DIFFERENT, earlier function
(0x82BE9668) -- and the fix then lands on a function that was never failing, so the
error set stops shrinking.

Here the enclosing base is READ OUT of the generated source: the recompiler writes a
`// ERROR: 0x<label>` comment at the failing case, inside the PPC_FUNC_IMPL it is
actually compiling. That is ground truth, not inference.

End of range = the base of the function after the furthest label (same rule as the
original). Entries are emitted per enclosing base and never merged -- see
apply_boundaries.py for why.

    python -I tools\fix_boundaries.py games\<name>
Reads <game>\errors.txt (the recompiler's ERROR lines; scripts\recompile.ps1 writes it)
and <game>\ppc\*.cpp, writes <game>\functions_generated.toml.
"""
import re, glob, bisect, os, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kitgame
os.chdir(kitgame.game_dir())

IMPL = re.compile(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)")
ERRC = re.compile(r"// ERROR: 0x([0-9A-F]+)")

bases = set()
labels_by_fn = collections.defaultdict(set)      # enclosing base -> {label, ...}
for path in glob.glob("ppc/*.cpp"):
    cur = None
    with open(path, encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            m = IMPL.search(line)
            if m:
                cur = int(m.group(1), 16)
                bases.add(cur)
                continue
            e = ERRC.search(line)
            if e and cur is not None:
                labels_by_fn[cur].add(int(e.group(1), 16))
bases = sorted(bases)
print(f"detected functions: {len(bases)}   functions with switch errors: {len(labels_by_fn)}")

def next_base(addr):
    i = bisect.bisect_right(bases, addr)
    return bases[i] if i < len(bases) else addr + 4

sites = collections.defaultdict(set)
with open("errors.txt", encoding="utf-8-sig") as fh:
    for line in fh:
        m = re.search(r"Switch case at ([0-9A-F]+) is trying to jump outside function: ([0-9A-F]+)", line)
        if m:
            sites[int(m.group(1), 16)].add(int(m.group(2), 16))
print(f"error sites: {len(sites)}")

out = {}
for site, labels in sites.items():
    # the enclosing function is the one whose emitted ERROR comments match this
    # site's labels; ties broken by the base closest below the site
    cands = [(len(labels & s), b) for b, s in labels_by_fn.items() if b <= site and (labels & s)]
    if not cands:
        print(f"  !! site {site:X}: no enclosing PPC_FUNC_IMPL found, skipped")
        continue
    base = max(cands)[1]
    end = max(next_base(l) for l in labels)
    if end <= base:
        print(f"  !! site {site:X}: end {end:X} <= base {base:X}, skipped")
        continue
    out[base] = max(out.get(base, 0), end - base)
    print(f"  site {site:X} -> fn {base:X} size {end - base:X}")

with open("functions_generated.toml", "w") as fh:
    fh.write("functions = [\n")
    for a in sorted(out):
        fh.write(f"    {{ address = 0x{a:X}, size = 0x{out[a]:X} }},\n")
    fh.write("]\n")
print(f"wrote functions_generated.toml ({len(out)} entries)")
