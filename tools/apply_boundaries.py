r"""Splice functions_generated.toml's entries into config.toml's `functions = [...]` block.

fix_boundaries.py reports only the errors that survive the boundaries the last run was
given, so the entries already in config.toml must be carried forward and unioned.

DO NOT merge overlapping ranges. A config entry overrides the ANALYSER's function at
that exact base; the analyser's spurious mid-function splits are precisely what has to
be overridden, so a range must keep an entry at every base that needs widening. Merging
{0x831FBAC8,0x64} into the wider {0x831FBA60,0xCC} left the analyser's short
sub_831FBAC8 in place and the switch at 0x831FBAF8 kept failing. NFS's own config shows
the correct shape -- four entries at 0x822F3540/44/4C/8C all ending at 0x822F3954.

Duplicate addresses collapse to the largest size.
"""
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kitgame
os.chdir(kitgame.game_dir())   # python -I tools\apply_boundaries.py games\<name>

ENTRY = re.compile(r"\{\s*address\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*size\s*=\s*(0x[0-9A-Fa-f]+)\s*\}")

def entries(text):
    return [(int(a, 16), int(s, 16)) for a, s in ENTRY.findall(text)]

cfg = open("config.toml", encoding="utf-8").read()
m = re.search(r"(functions\s*=\s*\[)(.*?)(^\])", cfg, re.S | re.M)
if not m:
    sys.exit("config.toml: no functions = [ ... ] block")

old = entries(m.group(2))
new = entries(open("functions_generated.toml", encoding="utf-8").read())

best = {}
for a, s in old + new:
    if s > best.get(a, 0):
        best[a] = s

body = "\n" + "".join(f"    {{ address = 0x{a:X}, size = 0x{best[a]:X} }},\n" for a in sorted(best))
open("config.toml", "w", encoding="utf-8").write(cfg[:m.start(2)] + body + cfg[m.end(2):])
print(f"config.toml functions: {len(old)} old + {len(new)} new -> {len(best)} entries")
