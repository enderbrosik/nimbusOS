#!/usr/bin/env python3
"""Stage 7 test: graphical installer, run ONLY against a blank .img file.

1. Boot the live image with a blank 64 MiB target image as the 2nd drive.
2. Walk the wizard: the live disk cannot be selected; the Erase button does
   nothing until the confirmation box is ticked (target still all zeros).
3. Install, restart, then boot the target image ALONE and check that it
   reaches the desktop as an installed system.
"""
import sys, os, re, subprocess, time, shutil, hashlib
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

def ic(col, row): return (6 + col * 84 + 42, 6 + row * 86 + 22)
S = "build/test"
LIVE = f"{S}/stage7-live.img"
TARGET = f"{S}/stage7-target.img"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
shutil.copy("build/nimbus.img", LIVE)
with open(TARGET, "wb") as f:          # blank sparse image file (no dd, no devices)
    f.truncate(64 * 1024 * 1024)

def head_bytes(path, n):
    with open(path, "rb") as f:
        return f.read(n)
live_boot_before = hashlib.sha256(head_bytes(LIVE, 4096 * 512)).hexdigest()

cx, cy = 212, 159                       # client origin of the centred setup window
NEXT, CANCEL = (cx + 400, cy + 414), (cx + 533, cy + 414)   # right-aligned buttons
row = lambda i: (cx + 300, cy + 130 + i * 44)
CHECKBOX = (cx + 40, cy + 316)

with Qemu([LIVE, TARGET], name="stage7-install") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.mouse_home()
    q.double_click(*ic(0, 5))                            # "Install NimbusOS"
    q.wait_serial(r"setup started \(live disk: Primary Master\)", 5)
    time.sleep(0.3)
    q.screenshot(f"{S}/stage7-1-welcome.png")
    q.click(*NEXT)
    q.wait_serial(r"page: choose disk", 5)
    q.click(*row(0))                                     # live disk: must not be selectable
    q.click(*NEXT)
    time.sleep(0.5)
    assert "selected Primary Master" not in q.serial() and "page: confirm" not in q.serial()
    q.click(*row(1))
    q.wait_serial(r"selected Primary Slave", 5)
    time.sleep(0.2)
    q.screenshot(f"{S}/stage7-2-disk.png")
    q.click(*NEXT)
    q.wait_serial(r"page: confirm erase of Primary Slave \(Empty", 5)
    q.click(*NEXT)                                       # "Erase and Install" while unticked
    time.sleep(1.0)
    assert "user confirmed" not in q.serial()
    assert head_bytes(TARGET, 4096 * 512) == bytes(4096 * 512), "target was written before confirmation!"
    q.click(*CHECKBOX)
    q.wait_serial(r"confirmation box ticked", 5)
    time.sleep(0.2)
    q.screenshot(f"{S}/stage7-3-confirm.png")
    q.click(*NEXT)
    q.wait_serial(r"user confirmed: installing to Primary Slave", 5)
    time.sleep(1.5)
    q.screenshot(f"{S}/stage7-4-progress.png")
    q.wait_serial(r"Installation finished successfully", 180)
    time.sleep(0.3)
    q.screenshot(f"{S}/stage7-5-done.png")
    q.click(*CANCEL)                                     # "Restart"
    q.wait_serial(r"(?s)restarting.*NimbusOS 0.1 - booting", 30)

assert hashlib.sha256(head_bytes(LIVE, 4096 * 512)).hexdigest() == live_boot_before, "live boot area modified!"
mbr = head_bytes(TARGET, 512)
assert mbr[510:512] == b"\x55\xaa" and mbr[0x1BE + 4] == 0x4E, "target MBR"
assert mbr[0x1B8:0x1BC] != head_bytes(LIVE, 512)[0x1B8:0x1BC], "disk signature must differ"
out = subprocess.check_output(["build/tools/nfstool", "check", TARGET]).decode()
assert "0 problem(s)" in out, out
inst = subprocess.check_output(["build/tools/nfstool", "cat", TARGET, "/System/installed.txt"]).decode()
assert "NimbusOS installed" in inst, inst
tree_live = subprocess.check_output(["build/tools/nfstool", "tree", LIVE]).decode()
tree_target = subprocess.check_output(["build/tools/nfstool", "tree", TARGET]).decode()
assert all(line in tree_target for line in tree_live.splitlines() if "icons.cfg" not in line and "desktop.cfg" not in line), "files missing"

# Boot the installed disk on its own.
with Qemu([TARGET], name="stage7-installed") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    log = q.serial()
    assert 'mounted "NIMBUS" from Primary Master' in log
    assert re.search(r"\[desk\] 9 icons", log), "installer icon should be gone on the installed system"
    time.sleep(1)
    q.screenshot(f"{S}/stage7-6-installed-desktop.png")
print("stage7: PASS")
