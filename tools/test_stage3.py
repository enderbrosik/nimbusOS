#!/usr/bin/env python3
"""Stage 3 test (drivers), checked through the final system:
ATA identify of two drives, the VBE framebuffer mode, keyboard input
(typed into the Terminal, incl. shifted characters) and mouse position
accuracy (a double-click at exact icon coordinates opens the app)."""
import sys, os, re, time, shutil
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

os.makedirs("build/test", exist_ok=True)
blank = "build/test/blank2.img"
with open(blank, "wb") as f:
    f.truncate(16 * 1024 * 1024)
img = "build/test/stage3.img"
shutil.copy("build/nimbus.img", img)

with Qemu([img, blank], name="stage3") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    log = q.serial()
    assert re.search(r'Primary Master: "QEMU HARDDISK" 131072 sectors', log), "ATA master"
    assert re.search(r'Primary Slave: "QEMU HARDDISK" 32768 sectors', log), "ATA slave"
    assert "1024x768x32 linear framebuffer" in log, "framebuffer"
    assert "fsck clean" in log, "ATA read/write via the FS self test"
    q.mouse_home()
    q.double_click(6 + 42, 6 + 3 * 86 + 22)              # Terminal icon, exact centre
    q.wait_serial(r'created: "Terminal"', 5)
    time.sleep(0.3)
    q.type("echo Hello, World! (Caps & Symbols: ~!@#$%^&*_+{}|:<>?)\n")
    q.wait_serial(re.escape("[term] $ echo Hello, World! (Caps & Symbols: ~!@#$%^&*_+{}|:<>?)"), 5)
    q.screenshot("build/test/stage3.png")
print("stage3: PASS")
