#!/usr/bin/env python3
"""Stage 5 test plan (desktop environment), driven through QEMU's monitor:
icons appear, a window opens on double-click, dragging/resizing works, the
taskbar restores a minimized window, right-click shows the context menu, the
Start menu opens, icons can be dragged, New Folder works, Alt+Tab cycles."""
import sys, os, re, subprocess, time
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

GRID_W, GRID_H, OX, OY = 84, 86, 6, 6
def icon_center(col, row): return (OX + col * GRID_W + GRID_W // 2, OY + row * GRID_H + 22)

import shutil
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
shots = "build/test"
IMG = "build/test/stage5.img"
shutil.copy("build/nimbus.img", IMG)        # always start from a pristine image
with Qemu([IMG], name="stage5") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    time.sleep(1)
    q.mouse_home()
    q.screenshot(f"{shots}/stage5-1-desktop.png")
    assert re.search(r"\[desk\] 11 icons", q.serial()), "icons"

    # 1. double-click the Notepad icon (column 0, row 1) -> window opens
    q.double_click(*icon_center(0, 1))
    q.wait_serial(r'window 1 created: "Untitled - Notepad" 620x420', 5)

    # 2. drag the window by its title bar (frame 622x448 at 80,50)
    q.drag(200, 62, 300, 150)
    q.wait_serial(r"window 1 moved to 180,138 622x448", 5)

    # 3. resize from the bottom-right corner
    q.drag(180 + 621, 138 + 447, 180 + 721, 138 + 517)
    q.wait_serial(r"window 1 resized to 180,138 722x518", 5)
    q.screenshot(f"{shots}/stage5-2-window.png")

    # 4. minimize via the title bar button, restore via the taskbar
    fx, fy, fw = 180, 138, 722
    q.click(fx + fw - 1 - 2 - 30 - 2 * 32 + 15, fy + 12)       # minimize button
    q.wait_serial(r"window 1 minimized", 5)
    q.click(150, 768 - 18)                                     # taskbar button
    q.wait_serial(r"taskbar: restore", 5)
    q.wait_serial(r"window 1 restored", 5)

    # 5. right-click on the desktop -> context menu
    q.right_click(700, 690)                     # below the window
    q.wait_serial(r"context menu opened", 5)
    q.screenshot(f"{shots}/stage5-3-context-menu.png")
    # the 154 px menu is pushed up to stay above the taskbar
    menu_y = min(690, 768 - 36 - 154)
    q.click(750, menu_y + 3 + 13)               # "New Folder", then name it
    q.wait_serial(r"created /Desktop/New Folder", 5)
    time.sleep(0.3)
    q.type("Projects")
    q.key("ret")
    time.sleep(1)

    # 6. Start menu via the Start button
    q.click(40, 768 - 18)
    q.wait_serial(r"start menu opened", 5)
    q.screenshot(f"{shots}/stage5-4-start-menu.png")
    q.key("esc")

    # 7. drag the Terminal icon (column 0, row 3) somewhere else
    x, y = icon_center(0, 3)
    q.drag(x, y, 420, 130)
    q.wait_serial(r'icon "Terminal" moved to', 5)

    # 8. open a second window and Alt+Tab between them
    q.double_click(*icon_center(0, 2))   # Paint
    q.wait_serial(r'window \d+ created: "Untitled - Paint"', 5)
    q.key("alt-tab")
    q.wait_serial(r'alt-tab -> "Untitled - Notepad"', 5)
    time.sleep(0.5)
    q.screenshot(f"{shots}/stage5-5-final.png")
    log = q.serial()

out = subprocess.check_output(["build/tools/nfstool", "ls", IMG, "/Desktop"]).decode()
assert "Projects/" in out, out
cfg = subprocess.check_output(["build/tools/nfstool", "cat", IMG, "/System/icons.cfg"]).decode()
assert "app:Terminal 342 92" in cfg, cfg
assert "0 problem(s)" in subprocess.check_output(["build/tools/nfstool", "check", IMG]).decode()
# Shut Down from the Start menu (Win key, Up = last item, Enter) -> ACPI S5.
shutil.copy("build/nimbus.img", IMG)
with Qemu([IMG], name="stage5-shutdown") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.key("meta_l"); q.key("up"); q.key("ret")
    q.wait_serial(r"shutting down", 10)
    for _ in range(50):
        if "shutdown" in q.cmd("info status"):
            break
        time.sleep(0.1)
    st = q.cmd("info status")
    assert "shutdown" in st, st

# Restart -> 8042 reset -> the machine boots again.
with Qemu([IMG], name="stage5-restart") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.key("meta_l"); q.key("up"); q.key("up"); q.key("ret")
    q.wait_serial(r"restarting", 10)
    q.wait_serial(r"(?s)restarting.*NimbusOS [0-9.]+ - booting", 30)
print("stage5: PASS")
