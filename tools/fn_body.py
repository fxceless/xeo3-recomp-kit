# Extract whole recompiled guest functions to files 
#   python -I tools\fn_body.py games\<name>\ppc <outdir> sub_82A8ED90 sub_82744CA8 ...
# Writes <outdir>\<sub_name>.c (the PPC_FUNC_IMPL body, with the original assembly in the
# comments). Read the asm lines only:  grep -E "^\s*// 8|^loc_" <outdir>\sub_82A8ED90.c
import sys, os, re, glob
want = set(sys.argv[3:]); outdir = sys.argv[2]
os.makedirs(outdir, exist_ok=True)
hdr = re.compile(r"PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)")
def flush(cur, buf):
    if cur in want:
        open(os.path.join(outdir, cur + ".c"), "w", encoding="utf-8").write("".join(buf))
for f in sorted(glob.glob(os.path.join(sys.argv[1], "ppc_recomp.*.cpp"))):
    cur = None; buf = []
    with open(f, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = hdr.search(line)
            if m:
                flush(cur, buf); cur = m.group(1); buf = [line]
            elif cur in want:
                buf.append(line)
    flush(cur, buf)
