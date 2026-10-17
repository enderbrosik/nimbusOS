#!/usr/bin/env python3
"""Breakout test: launch from the Start menu, steer the paddle with the mouse,
launch the ball, let it fall past the paddle until a ball is lost, then close.

Breakout is start-menu only, and the Start menu lists visible apps in
registry order with Breakout last, so Up x3 from the opened menu reaches it
(past Shut Down, Restart and System Properties, separators skipped).
"""
import sys, os, re, time, shutil, subprocess
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
IMG = "build/test/breakout.img"
shutil.copy("build/nimbus.img", IMG)
S = "build/test"
with Qemu([IMG], name="breakout") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.mouse_home()

    # ---- open Breakout from the Start menu
    q.click(40, 768 - 18)
    q.wait_serial(r"start menu opened", 5)
    for _ in range(3):
        q.key("up")
    q.key("ret")
    q.wait_serial(r'created: "Breakout"', 5)
    time.sleep(0.3)
    q.screenshot(f"{S}/breakout-ready.png")

    # ---- the paddle follows the mouse; keep it on the far left so the ball is missed
    q.mouse_to(40, 300)
    q.mouse_to(10, 300)
    time.sleep(0.3)

    # ---- launch the ball and let it run until one is lost
    q.key("space")
    q.wait_serial(r"\[brk \] ball lost, 2 left", 60)
    assert "game over" not in q.serial()
    time.sleep(0.3)
    q.screenshot(f"{S}/breakout-playing.png")

    q.key("alt-f4")
    q.wait_serial(r'destroyed: "Breakout"', 5)
print("breakout: PASS")
