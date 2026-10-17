#!/usr/bin/env python3
"""Calculator test: open it from the Start menu, then work a sum by keyboard and by mouse."""
import sys, os, shutil, subprocess, time
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
IMG = "build/test/calc.img"
shutil.copy("build/nimbus.img", IMG)
S = "build/test"

# Calculator window: client origin 81,77 (as in test_games). Keys are 56x40 with a 62 x 46 pitch,
# starting 12 px in and 88 px down, in the order of apps/calculator.c.
def key_xy(i):
    return (81 + 12 + (i % 4) * 62 + 28, 77 + 88 + (i // 4) * 46 + 20)
SEVEN, SIX, TIMES, EQUALS = 4, 10, 7, 19       # indices into the key table in apps/calculator.c

with Qemu([IMG], name="calc") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.mouse_home()

    # Start menu: Calculator is the last application, just above the separator
    q.key("meta_l")
    q.wait_serial(r"start menu opened", 5)
    for _ in range(4):                      # Up from the top: Shut Down, Restart, System Properties, Calculator
        q.key("up")
    q.key("ret")
    q.wait_serial(r'created: "Calculator"', 10)
    time.sleep(0.4)
    q.screenshot(f"{S}/calc-open.png")

    # mouse: 7 * 6 =
    for i in (SEVEN, TIMES, SIX, EQUALS):
        q.click(*key_xy(i))
    q.wait_serial(r"\[calc\] = 42\b", 5)

    # keyboard: 1 + 2 =   (the result 42 is replaced by the next number)
    q.type("1+2=")
    q.wait_serial(r"\[calc\] = 3\b", 5)
    time.sleep(0.3)
    q.screenshot(f"{S}/calc.png")

    # division by zero shows a message and Esc clears it
    q.type("5/0=")
    time.sleep(0.5)
    q.screenshot(f"{S}/calc-error.png")
    q.key("esc")
    q.screenshot(f"{S}/calc-clear.png")
    q.key("alt-f4")
    q.wait_serial(r'destroyed: "Calculator"', 5)
print("calc: PASS")
