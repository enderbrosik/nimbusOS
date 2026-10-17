#!/usr/bin/env python3
"""Clipboard test: Ctrl+C / Ctrl+V in a text field and the Ctrl+Shift+C history.

The Paint Canvas Size dialog has a text field.  Its text is copied ("640 x 400"),
then replaced and copied again ("32x32"), so the history holds two entries.
Esc closes the history popup and a second Ctrl+Shift+C reopens it.  Picking
entry 2 pastes "640 x 400" back into the field, and Enter applies it: the
serial log must report a 640x400 canvas, not 32x32."""
import sys, os, re, subprocess, time, shutil
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

def ic(col, row): return (6 + col * 84 + 42, 6 + row * 86 + 22)
NFS = "build/tools/nfstool"
IMG = "build/test/clipboard.img"
S = "build/test"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
os.makedirs(S, exist_ok=True)
shutil.copy("build/nimbus.img", IMG)

def count(q, pattern):
    return len(re.findall(pattern, q.serial()))

with Qemu([IMG], name="clipboard") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.mouse_home()
    q.double_click(*ic(0, 2))                          # Paint
    q.wait_serial(r'created: "Untitled - Paint"', 5)
    time.sleep(0.4)
    q.key("ctrl-w")                                    # Canvas Size dialog
    q.wait_serial(r'created: "Canvas Size"', 5)
    time.sleep(0.3)
    q.key("ctrl-a")
    q.key("ctrl-c")                                    # entry 1: "640 x 400"
    time.sleep(0.2)
    q.key("end")
    for _ in range(12): q.key("backspace")
    q.type("32x32")
    q.key("ctrl-a")
    q.key("ctrl-c")                                    # entry 2: "32x32"
    time.sleep(0.2)
    q.key("ctrl-shift-c")                              # history popup
    q.wait_serial(r'created: "Clipboard"', 5)
    time.sleep(0.3)
    q.screenshot(f"{S}/clipboard-history.png")
    q.key("esc")
    q.wait_serial(r'destroyed: "Clipboard"', 5)
    time.sleep(0.3)

    before = count(q, r'created: "Clipboard"')
    q.key("ctrl-shift-c")                              # reopen
    t = time.time()
    while count(q, r'created: "Clipboard"') == before and time.time() - t < 5: time.sleep(0.1)
    assert count(q, r'created: "Clipboard"') == before + 1, "history popup did not reopen"
    time.sleep(0.3)
    q.key("2")                                         # pick entry 2 = "640 x 400"
    time.sleep(0.4)
    q.key("ret")                                       # apply the field
    q.wait_serial(r"canvas size 640x400", 5)
    assert "PANIC" not in q.serial()

print("clipboard (text fields and history popup): PASS")
