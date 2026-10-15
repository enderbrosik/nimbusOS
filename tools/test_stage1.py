#!/usr/bin/env python3
"""Stage 1 test: boots, reaches protected mode, IDT works, serial logging works."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

with Qemu(["build/nimbus.img"], name="stage1") as q:
    q.wait_serial(r"Stage 1 OK", timeout=20)
    q.screenshot("build/test/stage1.png")
    log = q.serial()
    for needle in ["GDT loaded", "IDT loaded", "breakpoint exception handled"]:
        assert needle in log, needle
print("stage1: PASS")
