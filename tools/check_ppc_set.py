r"""Build gate: the generated source set in ppc\ is exactly ONE recompiler run.

Why (s1, 2026-10-14): XenonRecomp rewrites only the ppc_recomp.N.cpp files whose content
changed and never deletes surplus ones. The boundary loop's final pass produced 244 files
where pass 1 produced 248, so ppc_recomp.244-247 survived as stale copies of functions
already in 0-243 -- 525 duplicate definitions -- and the link died on LNK2005/LNK1227
after a full 20-minute compile. A link failure is the GOOD outcome; a stale file holding
a function the final run no longer emits would have linked fine and run old code.

Checks: every function in ppc_recomp_shared.h (the final run's list) is defined exactly
once, and no ppc_recomp.*.cpp defines anything outside that list. Exit 1 on any finding.
"""
import collections, glob, os, re, sys

ppc = sys.argv[1] if len(sys.argv) > 1 else "ppc"
if os.path.isdir(os.path.join(ppc, "ppc")):   # given the game folder
    ppc = os.path.join(ppc, "ppc")
IMPL = re.compile(r"^PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)", re.M)
want = set(re.findall(r"PPC_EXTERN_FUNC\((sub_[0-9A-F]+)\)",
                      open(os.path.join(ppc, "ppc_recomp_shared.h"), encoding="utf-8", errors="ignore").read()))
where = collections.defaultdict(list)
for f in glob.glob(os.path.join(ppc, "ppc_recomp.*.cpp")):
    for name in IMPL.findall(open(f, encoding="utf-8", errors="ignore").read()):
        where[name].append(os.path.basename(f))

dupes = {n: fs for n, fs in where.items() if len(fs) > 1}
extra = sorted(set(where) - want)
missing = sorted(want - set(where))
print(f"ppc set: {len(want)} functions wanted, {len(where)} defined, "
      f"{len(dupes)} duplicated, {len(extra)} not in the final run, {len(missing)} missing")
for n, fs in list(dupes.items())[:5]:
    print(f"  DUPLICATE {n}: {', '.join(sorted(fs))}")
for n in extra[:5]:
    print(f"  STALE {n}: {', '.join(where[n])}")
for n in missing[:5]:
    print(f"  MISSING {n}")
if dupes or extra or missing:
    print("  -> stale files from an earlier recompile pass? Move every ppc_recomp.*.cpp aside and re-run XenonRecomp.")
    sys.exit(1)
