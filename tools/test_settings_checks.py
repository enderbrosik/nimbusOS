#!/usr/bin/env python3
"""Settings checks: the cursor image chooser and a UI scale change, driven in QEMU.

Cursor (at 100%): "Choose image..." opens the file dialog, /Pictures/Pointer.bmp, a 16x16 picture, is chosen
(logged as "pointer image ..."), and the choice is saved to /System/settings.cfg.
Appearance: 150% is chosen last, because the whole window re-lays out at that scale
(logged as "ui scale 150%"; the setting is saved as ui_scale=150)."""
import sys, os, re, subprocess, time, shutil
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

NFS = "build/tools/nfstool"
S = "build/test"
IMG = f"{S}/settings-checks.img"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
os.makedirs(S, exist_ok=True)
shutil.copy("build/nimbus.img", IMG)

# a 16x16 pointer picture: the chooser accepts BMPs of 32x32 or smaller
from PIL import Image
POINTER = f"{S}/pointer.bmp"
Image.new("RGB", (16, 16), (200, 30, 30)).save(POINTER)
subprocess.check_call([NFS, "put", IMG, POINTER, "/Pictures/Pointer.bmp"])

sx, sy = 192, 159                       # centred 640x440 client at 1024x768
CX, CY = 170, 16                        # content area origin inside the client
def nav(i): return (sx + 60, sy + 12 + i * 34 + 15)          # sidebar item i
def scale_btn(i): return (sx + CX + i * 80 + 36, sy + CY + 298 + 14)   # UI scale button i (0=75%)

with Qemu([IMG], name="settings-checks") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.mouse_home()
    q.key("meta_l")
    for _ in range(12): q.key("down")
    q.key("ret")
    q.wait_serial(r'created: "Settings"', 5)
    q.click(*nav(6))                    # Cursor
    q.wait_serial(r"page Cursor", 5)
    q.click(sx + CX + 200 + 75, sy + CY + 138 - 2 + 12)   # Choose image...
    q.wait_serial(r'created: "Choose Pointer Image"', 5)
    time.sleep(0.4)
    q.type("Pointer.bmp\n")
    q.wait_serial(r"pointer image /Pictures/Pointer\.bmp", 10)
    time.sleep(0.5)
    q.screenshot(f"{S}/settings-cursor-image.png")
    q.click(*nav(1))                    # Appearance
    q.wait_serial(r"page Appearance", 5)
    q.click(*scale_btn(3))              # 150%
    q.wait_serial(r"ui scale 150%", 5)
    time.sleep(0.5)
    q.screenshot(f"{S}/settings-scale-150.png")
    assert "PANIC" not in q.serial()

cfg = subprocess.check_output([NFS, "cat", IMG, "/System/settings.cfg"]).decode()
assert "ui_scale=150" in cfg, cfg
assert "cursor_file=/Pictures/Pointer.bmp" in cfg, cfg
assert "0 problem(s)" in subprocess.check_output([NFS, "check", IMG]).decode()
print("settings checks (cursor image, UI scale): PASS")
