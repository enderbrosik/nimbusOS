#!/usr/bin/env python3
"""Stage 4 test: the kernel mounts NimbusFS from the boot disk and runs FS
operations against the real ATA disk; the host checker then validates the
image the kernel wrote to."""
import sys, os, subprocess
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

with Qemu(["build/nimbus.img"], name="stage4") as q:
    q.wait_serial(r"fsck clean", timeout=40)
    log = q.serial()
    assert "mounted \"NIMBUS\"" in log
out = subprocess.check_output(["build/tools/nfstool", "check", "build/nimbus.img"]).decode()
assert "0 problem(s)" in out, out
print("stage4: PASS")
