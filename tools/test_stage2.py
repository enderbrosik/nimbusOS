#!/usr/bin/env python3
"""Stage 2 test: PMM, paging, heap, preemptive scheduler, and the panic handler."""
import sys, os, subprocess
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu
import shutil
os.makedirs("build/test", exist_ok=True)
shutil.copy("build/nimbus.img", "build/test/stage2.img")   # never boot the pristine image

with Qemu(["build/test/stage2.img"], name="stage2") as q:
    q.wait_serial(r"Core kernel OK", timeout=30)
    log = q.serial()
    for needle in ["paging enabled", "[test] pmm", "[test] vmm", "[test] heap", "scheduler: preemptive"]:
        assert needle in log, needle

# Panic test: separate build with -DPANIC_TEST writes to an unmapped address.
subprocess.check_call(["make", "-s", "BUILD=build/panictest", "EXTRA_CFLAGS=-DPANIC_TEST", "build"],
                      stdout=subprocess.DEVNULL)
with Qemu(["build/panictest/nimbus.img"], name="panic") as q:
    q.wait_serial(r"System halted", timeout=30)
    q.screenshot("build/test/stage2-panic.png")
    log = q.serial()
    assert "Page Fault" in log and "CR2=badbad00" in log, log[-800:]
print("stage2: PASS")
