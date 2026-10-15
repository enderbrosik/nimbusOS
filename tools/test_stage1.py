#!/usr/bin/env python3
"""Stage 1 test: MBR + stage 2 load the kernel, protected mode, GDT/IDT/PIC,
exception handling and serial logging all work (checked on the final kernel's
early boot log; the screenshot shows the VGA text console before the
desktop takes over)."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

with Qemu(["build/nimbus.img"], name="stage1") as q:
    q.wait_serial(r"breakpoint exception handled", timeout=20)
    q.screenshot("build/test/stage1.png")
    log = q.serial()
    assert "NimbusOS 0.1 - booting" in log
    assert "[boot] drive 80" in log, "boot info from stage 2"
    for needle in ["GDT loaded", "IDT loaded, PIC remapped", "breakpoint at eip="]:
        assert needle in log, needle
print("stage1: PASS")
