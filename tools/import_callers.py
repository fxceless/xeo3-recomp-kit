# Which guest functions call a given import thunk? 
#   python -I tools\import_callers.py games\<name>\ppc XamUserReadProfileSettings XNotifyGetNext ...
# Scans the recompiled ppc_recomp.*.cpp once (~20 s for 240 MB); prints, per import, the
# number of call sites and the sub_ functions they sit in. Static, no launch.
import sys, os, re, glob, collections
names = sys.argv[2:]
pat = re.compile(r"__import__(" + "|".join(names) + r")\(")
fn = re.compile(r"PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)")
out = collections.defaultdict(list)
for f in sorted(glob.glob(os.path.join(sys.argv[1], "ppc_recomp.*.cpp"))):
    cur = "?"
    with open(f, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = fn.search(line)
            if m: cur = m.group(1); continue
            m = pat.search(line)
            if m: out[m.group(1)].append(cur)
for n in names:
    c = collections.Counter(out.get(n, []))
    print(f"{n}: {sum(c.values())} sites / {len(c)} fns: " + " ".join(f"{k}x{v}" if v > 1 else k for k, v in sorted(c.items())))
