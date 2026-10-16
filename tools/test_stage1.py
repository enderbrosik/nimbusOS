#!/usr/bin/env python3
"""Stage 1 test: MBR + stage 2 load the kernel, protected mode -> 64-bit long mode, GDT/IDT/PIC,
exception handling and serial logging all work (checked on the final kernel's
early boot log; the screenshot shows the VGA text console before the
desktop takes over)."""
import sys, os, re
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu
import shutil
os.makedirs("build/test", exist_ok=True)
shutil.copy("build/nimbus.img", "build/test/stage1.img")   # never boot the pristine image

with Qemu(["build/test/stage1.img"], name="stage1") as q:
    q.wait_serial(r"breakpoint exception handled", timeout=20)
    q.screenshot("build/test/stage1.png")
    log = q.serial()
    assert re.search(r"NimbusOS \d+\.\d+(\.\d+)? - booting", log), "boot banner"
    assert "[boot] drive 80" in log, "boot info from stage 2"
    for needle in ["long mode active, GDT + TSS loaded", "IDT loaded, PIC remapped", "breakpoint at rip="]:
        assert needle in log, needle
print("stage1: PASS")
