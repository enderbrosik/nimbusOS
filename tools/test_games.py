#!/usr/bin/env python3
"""Games test: Minesweeper, Snake and Solitaire launched from the desktop."""
import sys, os, re, time, shutil, subprocess
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

def ic(c, r): return (6 + c * 84 + 42, 6 + r * 86 + 22)
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
IMG = "build/test/games.img"
shutil.copy("build/nimbus.img", IMG)
S = "build/test"
with Qemu([IMG], name="games") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    q.mouse_home()

    # ---- Minesweeper (first window: client origin 81,77; board 9x9 centred in 330 px)
    q.double_click(*ic(0, 6))
    q.wait_serial(r'created: "Minesweeper - Beginner"', 5)
    cx, cy = 81, 77
    bx = cx + (330 - 9 * 22) // 2
    cell = lambda x, y: (bx + x * 22 + 11, cy + 72 + y * 22 + 11)
    q.click(*cell(4, 4))
    m = q.wait_serial(r"first click 4,4 revealed \d+ cells", 5)
    revealed = int(re.search(r"first click 4,4 revealed (\d+)", q.serial()).group(1))
    assert revealed >= 1                                   # first click is always safe
    assert "boom" not in q.serial()
    # flag a still-hidden corner cell (if the corner was revealed, try others)
    q.right_click(*cell(0, 8)); q.right_click(*cell(8, 0))
    q.wait_serial(r"\[mine\] flag", 5)
    q.click(cx + 10 + 2 * 104 + 50, cy + 15)              # Expert
    q.wait_serial(r"level Expert", 5)
    time.sleep(0.4)
    q.screenshot(f"{S}/games-mines.png")
    q.key("alt-f4")
    q.wait_serial(r'destroyed: "Minesweeper', 5)

    # ---- Snake: start moving up, never turn -> hits the top wall
    q.double_click(*ic(0, 7))
    q.wait_serial(r'created: "Snake"', 5)
    q.key("up")
    q.wait_serial(r"\[snak\] game over, score \d+", 10)
    time.sleep(0.3)
    q.screenshot(f"{S}/games-snake.png")
    q.key("ret")                                           # restart works
    q.key("alt-f4")
    q.wait_serial(r'destroyed: "Snake"', 5)

    # ---- Solitaire: deal from the stock, undo, then the win finale
    q.double_click(*ic(1, 0))
    q.wait_serial(r'created: "Solitaire"', 5)
    sx, sy = 81 + 28 * 2, 77 + 28 * 2                      # third window cascades by 2 steps
    q.click(sx + 12 + 35, sy + 12 + 48)                    # stock
    q.wait_serial(r"stock \(24 left\)", 5)
    q.click(sx + 12 + 35, sy + 12 + 48)
    q.wait_serial(r"stock \(23 left\)", 5)
    q.key("ctrl-z")
    q.click(sx + 12 + 35, sy + 12 + 48)
    q.wait_serial(r"(?s)stock \(23 left\).*stock \(23 left\)", 5)   # undo restored the card
    time.sleep(0.3)
    q.screenshot(f"{S}/games-solitaire.png")
    q.key("ctrl-shift-f")
    q.wait_serial(r"\[sol \] won", 5)
    time.sleep(3)
    q.screenshot(f"{S}/games-finale.png")
print("games: PASS")
