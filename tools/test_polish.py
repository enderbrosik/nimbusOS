#!/usr/bin/env python3
"""Polish test: mouse wheel, drag-and-drop (Explorer <-> desktop, file onto
an app icon), Settings (live resolution change with keep/auto-revert,
accent colour) and persistence of settings across a reboot."""
import sys, os, re, time, shutil, subprocess
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

def ic(c, r): return (6 + c * 84 + 42, 6 + r * 86 + 22)
NFS = "build/tools/nfstool"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
IMG = "build/test/polish.img"
shutil.copy("build/nimbus.img", IMG)
S = "build/test"

with Qemu([IMG], name="polish") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    assert "PS/2 mouse ready (wheel)" in q.serial()
    q.mouse_home()

    # ---- wheel: scroll the Terminal back
    q.double_click(*ic(0, 3))
    q.wait_serial(r'created: "Terminal"', 5)
    q.type("tree /\n")
    q.mouse_to(300, 250)
    q.cmd("mouse_move 0 0 1")
    q.wait_serial(r'wheel -1 -> "Terminal"', 5)
    q.key("alt-f4")
    q.wait_serial(r'destroyed: "Terminal"', 5)

    # ---- drag a file from Explorer (/Documents) onto the desktop
    q.double_click(*ic(0, 0))
    q.wait_serial(r'created: "File Explorer"', 5)
    cx, cy = 81 + 28, 77 + 28                 # second window cascades one step
    row = lambda i: cy + 58 + i * 22 + 11
    q.double_click(cx + 200, row(1))           # Documents
    q.wait_serial(r"navigate /Documents", 5)
    q.drag(cx + 200, row(1), 600, 650, steps=10)   # notes.txt (Projects, notes.txt)
    q.wait_serial(r"dropped /Documents/notes.txt -> /Desktop/notes.txt", 5)
    time.sleep(0.6)
    q.screenshot(f"{S}/polish-dnd.png")
    # ---- and back: desktop icon into the Explorer window
    cfg = subprocess.check_output([NFS, "cat", IMG, "/System/icons.cfg"]).decode()
    m = re.search(r"file:notes.txt (\d+) (\d+)", cfg)
    nx, ny = int(m.group(1)) + 42, int(m.group(2)) + 22
    q.drag(nx, ny, cx + 300, row(4), steps=10)
    q.wait_serial(r'drop /Desktop/notes.txt on "File Explorer', 5)
    q.wait_serial(r"moved /Desktop/notes.txt -> /Documents/notes.txt", 5)
    # ---- a desktop file onto the Notepad icon opens it
    q.key("alt-f4")                            # close Explorer, it covers the icons
    q.wait_serial(r'destroyed: "File Explorer', 5)
    q.drag(*ic(1, 2), *ic(0, 1), steps=8)      # Welcome.txt -> Notepad
    q.wait_serial(r"open /Desktop/Welcome.txt with Notepad", 5)
    q.wait_serial(r"opened /Desktop/Welcome.txt", 5)
    q.key("alt-f4")

    # ---- Settings: 1280x800, keep it
    q.key("meta_l")
    for _ in range(12): q.key("down")
    q.key("ret")
    q.wait_serial(r'created: "Settings"', 5)
    sx, sy = 192, 159                          # centred 640x440 client at 1024x768
    q.click(sx + 180, sy + 16 + 34 + 7 * 26 + 8)    # 1280 x 800 (row 7 of the list)
    q.click(sx + 574, sy + 408)                # Apply
    q.wait_serial(r"resolution changed to 1280x800", 5)
    q.mouse_home()                             # pointer was re-centred by the mode switch
    time.sleep(0.5)
    q.screenshot(f"{S}/polish-1280x800.png")
    q.click(450 + 210, 335 + 94)               # "Keep changes" (dialog centred at 1280x800)
    q.wait_serial(r"kept resolution 1280x800", 5)
    # ---- 800x600 then let the countdown revert it
    q.click(sx + 180, sy + 16 + 34 + 4 * 26 + 8)    # 800 x 600 (row 4)
    q.click(sx + 574, sy + 408)
    q.wait_serial(r"resolution changed to 800x600", 5)
    q.wait_serial(r"reverted to 1280x800", 20)
    q.mouse_home()
    # ---- Appearance: green accent.  The 800x600 detour pulled the window
    # back on screen (642x467 frame -> 158,97), so its client is at 159,124.
    sx, sy = 159, 124
    q.click(sx + 60, sy + 12 + 34 + 15)        # nav: Appearance
    q.wait_serial(r"page Appearance", 5)
    q.click(sx + 170 + 44 + 18, sy + 16 + 36 + 18)
    q.wait_serial(r"accent=", 5)
    time.sleep(0.5)
    q.screenshot(f"{S}/polish-accent.png")

cfg = subprocess.check_output([NFS, "cat", IMG, "/System/settings.cfg"]).decode()
assert "resolution=1280x800" in cfg and "accent=" in cfg, cfg
assert "Documents/" in subprocess.check_output([NFS, "ls", IMG, "/"]).decode()
assert "notes.txt" in subprocess.check_output([NFS, "ls", IMG, "/Documents"]).decode()

# ---- settings survive a reboot
with Qemu([IMG], name="polish-reboot") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    log = q.serial()
    assert "mode 1280x800x32" in log, "resolution not restored"
    time.sleep(1)
    q.screenshot(f"{S}/polish-reboot.png")
assert "0 problem(s)" in subprocess.check_output([NFS, "check", IMG]).decode()
print("polish: PASS")
