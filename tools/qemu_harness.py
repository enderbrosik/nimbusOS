#!/usr/bin/env python3
"""
NimbusOS - headless QEMU test harness.

Boots a disk image in QEMU with no display, captures COM1 to a log file and
drives the guest through the QEMU human monitor (HMP): key presses, PS/2
mouse movement/clicks and screenshots.  Used by tools/test_stage*.py and
usable interactively:

    from qemu_harness import Qemu
    with Qemu(["build/nimbus.img"]) as q:
        q.wait_serial("Desktop ready")
        q.screenshot("shot.png")
"""
import os, socket, subprocess, time, tempfile, re

class Qemu:
    def __init__(self, drives, mem="256M", extra=None, outdir="build/test", name="qemu"):
        os.makedirs(outdir, exist_ok=True)
        self.outdir = outdir
        self.serial_path = os.path.join(outdir, name + "-serial.log")
        self.mon_path = os.path.join(tempfile.mkdtemp(), "mon.sock")
        args = ["qemu-system-i386", "-m", mem, "-vga", "std", "-display", "none",
                "-serial", "file:" + self.serial_path,
                "-monitor", "unix:%s,server,nowait" % self.mon_path, "-no-shutdown"]
        for i, d in enumerate(drives):
            args += ["-drive", "file=%s,format=raw,index=%d,media=disk" % (d, i)]
        args += extra or []
        if os.path.exists(self.serial_path):
            os.remove(self.serial_path)
        self.proc = subprocess.Popen(args)
        self.sock = None
        for _ in range(100):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.mon_path)
                self.sock = s
                break
            except OSError:
                time.sleep(0.05)
        if not self.sock:
            raise RuntimeError("could not connect to QEMU monitor")
        self.sock.settimeout(5)
        self._read_prompt()
        self.mx = self.my = None

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.quit()

    def _read_prompt(self):
        buf = b""
        while not buf.endswith(b"(qemu) "):
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf.decode(errors="replace")

    def cmd(self, c):
        self.sock.sendall((c + "\n").encode())
        return self._read_prompt()

    def serial(self):
        try:
            with open(self.serial_path, "rb") as f:
                return f.read().decode(errors="replace")
        except FileNotFoundError:
            return ""

    def wait_serial(self, pattern, timeout=30):
        t = time.time()
        while time.time() - t < timeout:
            if re.search(pattern, self.serial()):
                return True
            if self.proc.poll() is not None:
                break
            time.sleep(0.1)
        raise TimeoutError("serial pattern %r not seen; log tail:\n%s" % (pattern, self.serial()[-3000:]))

    def screenshot(self, png_path):
        ppm = png_path[:-4] + ".ppm"
        self.cmd("screendump " + os.path.abspath(ppm))
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                break
            time.sleep(0.05)
        time.sleep(0.2)
        try:
            from PIL import Image
            Image.open(ppm).save(png_path)
            os.remove(ppm)
            return png_path
        except ImportError:
            return ppm

    # ---- keyboard ------------------------------------------------------
    def key(self, k, delay=0.08):
        """k uses QEMU sendkey names, e.g. 'a', 'ret', 'shift-a', 'alt-tab', 'meta_l'."""
        self.cmd("sendkey " + k)
        time.sleep(delay)

    def type(self, text, delay=0.04):
        names = {' ': 'spc', '\n': 'ret', '.': 'dot', ',': 'comma', '/': 'slash', '-': 'minus',
                 '=': 'equal', ';': 'semicolon', "'": 'apostrophe', '\\': 'backslash',
                 '[': 'bracket_left', ']': 'bracket_right', '`': 'grave_accent', '\t': 'tab'}
        shifted = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
                   '(': '9', ')': '0', '_': 'minus', '+': 'equal', ':': 'semicolon', '"': 'apostrophe',
                   '<': 'comma', '>': 'dot', '?': 'slash', '|': 'backslash', '{': 'bracket_left',
                   '}': 'bracket_right', '~': 'grave_accent'}
        for ch in text:
            if ch in names:
                self.key(names[ch], delay)
            elif ch in shifted:
                self.key("shift-" + shifted[ch], delay)
            elif ch.isupper():
                self.key("shift-" + ch.lower(), delay)
            else:
                self.key(ch, delay)

    # ---- mouse (PS/2, relative) ---------------------------------------
    def mouse_home(self):
        for _ in range(8):
            self.cmd("mouse_move -200 -200")
            time.sleep(0.03)
        # QEMU's PS/2 model drains accumulated motion at the guest's packet
        # rate, so give it time to fully arrive before moving elsewhere.
        time.sleep(0.5)
        self.mx = self.my = 0

    def mouse_to(self, x, y):
        if self.mx is None:
            self.mouse_home()
        dx, dy = x - self.mx, y - self.my
        while dx or dy:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            self.cmd("mouse_move %d %d" % (sx, sy))
            time.sleep(0.03)
            dx -= sx; dy -= sy
        self.mx, self.my = x, y
        time.sleep(0.1)

    def button(self, state):
        self.cmd("mouse_button %d" % state)
        time.sleep(0.08)

    def click(self, x, y, button=1):
        self.mouse_to(x, y)
        self.button(button)
        self.button(0)

    def right_click(self, x, y):
        self.click(x, y, 2)

    def double_click(self, x, y):
        self.mouse_to(x, y)
        self.cmd("mouse_button 1"); self.cmd("mouse_button 0")
        self.cmd("mouse_button 1"); self.cmd("mouse_button 0")
        time.sleep(0.15)

    def drag(self, x0, y0, x1, y1, steps=8):
        self.mouse_to(x0, y0)
        self.button(1)
        for i in range(1, steps + 1):
            self.mouse_to(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        self.button(0)

    def quit(self):
        try:
            self.cmd("quit")
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
