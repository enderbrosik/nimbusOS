#!/usr/bin/env python3
"""Stage 3 test: keyboard, mouse, framebuffer graphics, ATA PIO."""
import sys, os, re
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

blank = "build/test/blank2.img"
os.makedirs("build/test", exist_ok=True)
with open(blank, "wb") as f:
    f.truncate(16 * 1024 * 1024)

with Qemu(["build/nimbus.img", blank], name="stage3") as q:
    q.wait_serial(r"Drivers OK", timeout=30)
    q.wait_serial(r"write/readback", timeout=10)
    q.type("Hello, World!")
    q.mouse_home()
    q.click(300, 200)
    q.mouse_to(512, 600)
    q.screenshot("build/test/stage3.png")
    log = q.serial()
    assert "write/readback OK" in log, "ATA"
    assert "Primary Slave" in log, "second drive not detected"
    keys = "".join(m for m in re.findall(r"ch '(.)'", log))
    assert "Hello, World!" in keys, keys
    m = re.search(r"mouse click at (\d+),(\d+)", log)
    assert m and abs(int(m.group(1)) - 300) <= 2 and abs(int(m.group(2)) - 200) <= 2, m and m.group(0)
print("stage3: PASS")
