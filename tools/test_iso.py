#!/usr/bin/env python3
"""Live-CD test: boot build/nimbus.iso (El Torito HD emulation -> RAM disk),
install from it onto a BLANK 64 MiB .img file, then boot that disk alone."""
import sys, os, re, time, subprocess
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

S = "build/test"
TARGET = f"{S}/iso-target.img"
subprocess.check_call(["make", "-s", "iso"], stdout=subprocess.DEVNULL)
os.makedirs(S, exist_ok=True)
with open(TARGET, "wb") as f:
    f.truncate(64 * 1024 * 1024)
CD = ["-cdrom", "build/nimbus.iso", "-boot", "d"]
cx, cy = 212, 159
NEXT, CANCEL = (cx + 400, cy + 414), (cx + 533, cy + 414)
CHECKBOX = (cx + 40, cy + 316)

with Qemu([TARGET], name="iso-live", extra=CD) as q:
    q.wait_serial(r"Desktop ready", timeout=90)
    log = q.serial()
    assert "live CD: mounted" in log and "fsck clean" in log
    time.sleep(1)
    q.screenshot(f"{S}/iso-desktop.png")
    q.mouse_home()
    q.double_click(48, 6 + 5 * 86 + 22)                  # Install NimbusOS
    q.wait_serial(r"setup started \(live disk: Live CD", 5)
    q.click(*NEXT)
    q.wait_serial(r"page: choose disk", 5)
    q.click(cx + 300, cy + 130)                          # row 0: Primary Master (the blank disk)
    q.wait_serial(r"selected Primary Master", 5)
    q.click(*NEXT)
    q.wait_serial(r"page: confirm erase of Primary Master \(Empty", 5)
    q.click(*CHECKBOX)
    q.click(*NEXT)
    q.wait_serial(r"Installation finished successfully", 180)

out = subprocess.check_output(["build/tools/nfstool", "check", TARGET]).decode()
assert "0 problem(s)" in out, out
with Qemu([TARGET], name="iso-installed") as q:
    q.wait_serial(r"Desktop ready", timeout=60)
    log = q.serial()
    assert 'mounted "NIMBUS" from Primary Master' in log and "live CD" not in log
    assert re.search(r"\[desk\] 10 icons", log)
print("iso: PASS")
