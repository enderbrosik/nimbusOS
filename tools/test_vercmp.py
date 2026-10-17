#!/usr/bin/env python3
"""Version comparison used by Nimbus Update: versions read like decimals (3.2 is newer than 3.123)."""
import os, subprocess, tempfile
os.chdir(os.path.join(os.path.dirname(__file__), ".."))
CASES = [("3.2", "3.123", 1), ("3.123", "3.122", 1), ("3.121", "3.12", 1), ("3.12", "3.121", -1), ("3.1", "3.11", -1),
         ("3.10", "3.1", 0), ("3.0", "3", 0), ("4.0", "3.999", 1), ("0.3", "0.2", 1), ("2.24", "3.0", -1),
         ("1.2.3", "1.23", 0), ("1.2.4", "1.2.3", 1), ("10.0", "9.9", 1), ("3.13", "3.123", 1), ("3.124", "3.2", -1)]
src = '#include <stdio.h>\n#include "kernel/vercmp.h"\nint main(int c, char **v) { int r = nimbus_version_cmp(v[1], v[2]); printf("%d", r < 0 ? -1 : r > 0); return 0; }\n'
with tempfile.TemporaryDirectory() as d:
    open(f"{d}/t.c", "w").write(src)
    subprocess.check_call(["cc", "-I.", f"{d}/t.c", "-o", f"{d}/t"])
    bad = [(a, b, e, subprocess.check_output([f"{d}/t", a, b]).decode()) for a, b, e in CASES]
    bad = [x for x in bad if str(x[2]) != x[3]]
assert not bad, bad
print("vercmp: PASS")
