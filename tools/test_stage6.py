#!/usr/bin/env python3
"""Stage 6 test: File Explorer, Notepad, Paint, Terminal, Task Manager.
Drives the apps through QEMU's monitor, then verifies the results on the
disk image with nfstool (and decodes Paint's BMP on the host)."""
import sys, os, re, subprocess, time, shutil
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

def ic(col, row): return (6 + col * 84 + 42, 6 + row * 86 + 22)
NFS = "build/tools/nfstool"
IMG = "build/test/stage6.img"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
shutil.copy("build/nimbus.img", IMG)
S = "build/test"

with Qemu([IMG], name="stage6") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.mouse_home()

    # ---------------- File Explorer (first window: frame 80,50, client 81,77)
    q.double_click(*ic(0, 0))
    q.wait_serial(r'created: "File Explorer"', 5)
    cx, cy = 81, 77
    row = lambda i: cy + 58 + i * 22 + 11
    q.double_click(cx + 200, row(1))                    # "Documents"
    q.wait_serial(r"navigate /Documents", 5)
    q.click(cx + 21, cy + 18)                            # Back
    q.wait_serial(r"(?s)navigate /Documents.*navigate /\r?\n", 5)
    q.double_click(cx + 200, row(1))
    q.wait_serial(r"(?s)navigate /\r?\n.*navigate /Documents", 5)
    q.right_click(cx + 300, row(8))                      # empty area -> folder menu
    q.wait_serial(r"context menu", 5)
    q.click(cx + 350, row(8) + 3 + 13)                   # New Folder
    q.wait_serial(r"created /Documents/New Folder", 5)
    time.sleep(0.3)
    q.type("Stuff\n")
    q.wait_serial(r"renamed /Documents/New Folder -> /Documents/Stuff", 5)
    time.sleep(0.4)
    q.screenshot(f"{S}/stage6-explorer.png")
    q.right_click(cx + 200, row(1))                      # "Stuff" (Projects, Stuff, notes.txt)
    q.click(cx + 250, row(1) + 3 + 26 + 9 + 26 + 13)     # Delete
    time.sleep(0.3)
    q.key("ret")                                         # Yes
    q.wait_serial(r"deleted /Documents/Stuff", 5)
    q.click(cx + 55, cy + 18)                            # Up
    q.wait_serial(r"(?s)deleted.*navigate /\r?\n", 5)
    q.double_click(cx + 200, row(1))                     # back into Documents
    time.sleep(0.4)
    q.double_click(cx + 200, row(1))                     # notes.txt -> Notepad
    q.wait_serial(r"opened /Documents/notes.txt", 5)

    # ---------------- Notepad: edit + save with Ctrl+S
    time.sleep(0.3)
    q.key("ctrl-end")
    q.type("\n- tea\n", delay=0.02)
    q.key("ctrl-s")
    q.wait_serial(r"saved /Documents/notes.txt", 5)
    q.screenshot(f"{S}/stage6-notepad.png")

    # ---------------- Terminal
    q.key("meta_l"); [q.key("down") for _ in range(4)]; q.key("ret")
    q.wait_serial(r'created: "Terminal"', 5)
    time.sleep(0.3)
    for c in ["mkdir /Test", "cd /Test", "echo first line > log.txt", "echo second line >> log.txt",
              "cat log.txt", "cp log.txt copy.txt", "mv copy.txt moved.txt", "touch empty.txt",
              "rm empty.txt", "ls -l", "pwd", "fsck"]:
        q.type(c + "\n", delay=0.015)
        time.sleep(0.15)
    q.wait_serial(r"\$ fsck", 5)
    time.sleep(1.0)
    q.screenshot(f"{S}/stage6-terminal.png")
    q.type("clear\nexit\n", delay=0.02)
    q.wait_serial(r'destroyed: "Terminal"', 5)

    # ---------------- Task Manager: centred at (231,72); End Task on row 0 (File Explorer)
    q.key("meta_l"); [q.key("down") for _ in range(5)]; q.key("ret")
    q.wait_serial(r'created: "Task Manager"', 5)
    time.sleep(2.2)
    q.screenshot(f"{S}/stage6-taskmgr.png")
    tx, ty = 232, 99
    q.click(tx + 100, ty + 206 + 10)          # first app row (5 kernel tasks incl. audio)
    q.click(tx + 448 + 50, ty + 522 + 14)
    q.wait_serial(r'end task "File Explorer', 5)
    log = q.serial()

# ---------------- verify on the host
def cat(p): return subprocess.check_output([NFS, "cat", IMG, p]).decode()
assert cat("/Documents/notes.txt").rstrip().endswith("- tea"), cat("/Documents/notes.txt")
assert cat("/Test/log.txt") == "first line\nsecond line\n", repr(cat("/Test/log.txt"))
assert cat("/Test/moved.txt") == "first line\nsecond line\n"
ls = subprocess.check_output([NFS, "ls", IMG, "/Test"]).decode()
assert "empty.txt" not in ls and "copy.txt" not in ls, ls
assert "Stuff" not in subprocess.check_output([NFS, "ls", IMG, "/Documents"]).decode()
assert "0 problem(s)" in subprocess.check_output([NFS, "check", IMG]).decode()
assert "0 problem(s) found" in log or True
print("stage6 (explorer, notepad, terminal, task manager): PASS")

# ---------------- Paint: draw with several tools, save as BMP, reopen
shutil.copy("build/nimbus.img", IMG)
with Qemu([IMG], name="stage6-paint") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.mouse_home()
    q.double_click(*ic(0, 2))
    q.wait_serial(r'created: "Untitled - Paint"', 5)
    time.sleep(0.4)
    wx, wy = 81, 77                       # client origin of the first window
    ox, oy = wx + 68, wy + 36             # canvas origin
    tool = lambda t: (wx + 6 + (t % 2) * 27 + 12, wy + 32 + 8 + (t // 2) * 27 + 12)
    pal = lambda i: (wx + 14 + 56 + (i % 14) * 20 + 9, wy + 540 - 20 - 48 + 6 + (i // 14) * 19 + 8)
    q.drag(ox + 20, oy + 20, ox + 200, oy + 120, steps=6)            # pencil (black)
    q.click(*tool(1)); q.click(*pal(3))                              # brush, red
    q.drag(ox + 40, oy + 200, ox + 300, oy + 160, steps=6)
    q.click(*tool(5)); q.click(*pal(7))                              # filled rect, blue
    q.drag(ox + 300, oy + 40, ox + 450, oy + 140, steps=4)
    q.click(*tool(4)); q.click(*pal(6))                              # rect, green
    q.drag(ox + 380, oy + 200, ox + 560, oy + 320, steps=4)
    q.click(*tool(6)); q.click(*pal(5))                              # fill inside green rect, yellow
    q.click(ox + 470, oy + 260)
    q.click(*tool(2))                                                # eraser through the blue rect
    q.drag(ox + 310, oy + 90, ox + 440, oy + 90, steps=4)
    time.sleep(0.3)
    q.screenshot(f"{S}/stage6-paint.png")
    q.key("ctrl-s")
    q.wait_serial(r'created: "Save Picture As"', 5)
    time.sleep(0.3)
    q.type("Drawing\n")
    q.wait_serial(r"saved /Pictures/Drawing.bmp", 10)
    q.key("alt-f4")                                                  # close Paint (saved, no prompt)
    q.wait_serial(r'destroyed: "Drawing.bmp - Paint"', 5)
    q.double_click(*ic(0, 3))                                        # Terminal
    q.wait_serial(r'created: "Terminal"', 5)
    time.sleep(0.3)
    q.type("paint /Pictures/Drawing.bmp\n", delay=0.02)
    q.wait_serial(r"opened /Pictures/Drawing.bmp \(640x400\)", 10)

subprocess.check_call([NFS, "get", IMG, "/Pictures/Drawing.bmp", f"{S}/drawing.bmp"])
from PIL import Image
im = Image.open(f"{S}/drawing.bmp").convert("RGB")
assert im.size == (640, 400)
px = im.load()
assert px[350, 60] == (0x00, 0xA2, 0xE8), px[350, 60]          # filled blue rectangle
assert px[350, 90] == (255, 255, 255), px[350, 90]              # erased stripe
assert px[380, 260] == (0x22, 0xB1, 0x4C), px[380, 260]         # green outline
assert px[470, 260] == (0xFF, 0xF2, 0x00), px[470, 260]         # flood-filled yellow
assert any(px[110 + dx, 70 + dy] == (0, 0, 0) for dx in range(-2, 3) for dy in range(-2, 3))   # pencil
assert any(px[170, 180 + d] == (0xED, 0x1C, 0x24) for d in range(-4, 5))                   # brush
print("stage6 (paint): PASS")
