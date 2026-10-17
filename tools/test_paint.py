#!/usr/bin/env python3
"""Paint test: the transparent pen and the canvas Size dialog.

A 32x32 canvas is made with Ctrl+W, the transparent pen (first palette
swatch) draws two strokes, and the picture is saved.  The host checks the
saved 32-bit BMP: drawn pixels have alpha 0, untouched pixels are opaque
white.  Reopening it in Paint must report the 32x32 size."""
import sys, os, re, subprocess, time, shutil
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

def ic(col, row): return (6 + col * 84 + 42, 6 + row * 86 + 22)
NFS = "build/tools/nfstool"
IMG = "build/test/paint.img"
S = "build/test"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
os.makedirs(S, exist_ok=True)
shutil.copy("build/nimbus.img", IMG)

wx, wy = 81, 77                       # client origin of the first Paint window
ox, oy = wx + 68, wy + 36             # canvas origin (view offset plus 4px margin)
pal = lambda i: (wx + 14 + 56 + (i % 15) * 20 + 9, wy + 540 - 20 - 48 + 6 + (i // 15) * 19 + 8)

with Qemu([IMG], name="paint") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.mouse_home()
    q.double_click(*ic(0, 2))                          # Paint
    q.wait_serial(r'created: "Untitled - Paint"', 5)
    time.sleep(0.4)
    q.key("ctrl-w")                                    # Canvas Size dialog
    q.wait_serial(r'created: "Canvas Size"', 5)
    time.sleep(0.3)
    q.key("end")
    for _ in range(12): q.key("backspace")             # clear "640 x 400"
    q.type("32x32\n", delay=0.04)
    q.wait_serial(r"canvas size 32x32", 5)
    time.sleep(0.4)
    q.click(*pal(0))                                   # transparent pen
    q.drag(ox + 4, oy + 4, ox + 20, oy + 4, steps=4)
    q.drag(ox + 4, oy + 10, ox + 20, oy + 10, steps=4)
    time.sleep(0.4)
    q.screenshot(f"{S}/paint-alpha.png")
    q.key("ctrl-s")
    q.wait_serial(r'created: "Save Picture As"', 5)
    time.sleep(0.3)
    q.type("Alpha\n")
    q.wait_serial(r"saved /Pictures/Alpha\.bmp", 10)
    q.key("alt-f4")
    q.wait_serial(r'destroyed: "Alpha\.bmp - Paint"', 5)
    q.double_click(*ic(0, 3))                          # Terminal
    q.wait_serial(r'created: "Terminal"', 5)
    time.sleep(0.3)
    q.type("paint /Pictures/Alpha.bmp\n", delay=0.02)
    q.wait_serial(r"opened /Pictures/Alpha\.bmp \(32x32\)", 10)
    time.sleep(0.5)
    q.screenshot(f"{S}/paint-alpha-reopened.png")
    assert "PANIC" not in q.serial()

subprocess.check_call([NFS, "get", IMG, "/Pictures/Alpha.bmp", f"{S}/alpha.bmp"])
from PIL import Image
im = Image.open(f"{S}/alpha.bmp").convert("RGBA")
assert im.size == (32, 32), im.size
px = im.load()
assert px[10, 4][3] == 0 and px[4, 4][3] == 0, px[10, 4]          # first stroke is transparent
assert px[10, 10][3] == 0, px[10, 10]                              # second stroke too
assert px[10, 7] == (255, 255, 255, 255), px[10, 7]                # between strokes: opaque white
assert px[0, 0] == (255, 255, 255, 255), px[0, 0]
print("paint (transparent pen, canvas size): PASS")

# ---------------- the largest preset: 3840x2160 needs about 33 MB for the canvas.
# Success is logged as "canvas size"; a refusal shows as "Not enough memory" in the dialog
# (not logged), so this only checks that nothing crashes and records which one happened.
with Qemu([IMG], name="paint-4k") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.mouse_home()
    q.double_click(*ic(0, 2))
    q.wait_serial(r'created: "Untitled - Paint"', 5)
    time.sleep(0.4)
    q.key("ctrl-w")
    q.wait_serial(r'created: "Canvas Size"', 5)
    time.sleep(0.3)
    q.key("end")
    for _ in range(12): q.key("backspace")
    q.type("3840x2160\n", delay=0.04)
    t0 = time.time()
    while time.time() - t0 < 10 and "canvas size 3840x2160" not in q.serial():
        time.sleep(0.2)
    time.sleep(0.5)
    q.screenshot(f"{S}/paint-4k.png")
    assert "PANIC" not in q.serial()
    result = "canvas size 3840x2160" in q.serial()
    print("paint 3840x2160 preset: %s" % ("applied" if result else "refused (not enough memory) or not applied"))
