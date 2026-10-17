#!/usr/bin/env python3
"""Browser CSS test: a page with a <style> block, classes, ids, inline styles,
text-align, underline and display:none is loaded in the Web Browser.  The page
title and link count come from the browser's own log line; the screenshot is
kept for a visual check (colours, bold face, centred heading, hidden paragraph)."""
import sys, os, re, subprocess, time
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu
import shutil

def ic(c, r): return (6 + c * 84 + 42, 6 + r * 86 + 22)
S = "build/test"
IMG = f"{S}/css.img"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
os.makedirs(S, exist_ok=True)
shutil.copy("build/nimbus.img", IMG)
web = subprocess.Popen([sys.executable, "-m", "http.server", "8080", "--bind", "127.0.0.1",
                        "--directory", os.path.join(os.path.dirname(__file__), "testweb")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.5)
try:
    with Qemu([IMG], name="css") as q:
        q.wait_serial(r"Desktop ready", timeout=40)
        q.wait_serial(r"DHCP: address 10\.0\.2\.15", 15)
        q.mouse_home()
        q.key("meta_l")                                 # Start menu: Web Browser is item 10
        for _ in range(11): q.key("down")
        q.key("ret")
        q.wait_serial(r'created: "Web Browser"', 5)
        q.key("ctrl-l")
        time.sleep(0.3)
        q.type("http://10.0.2.2:8080/css.html\n")
        q.wait_serial(r'loaded http://10\.0\.2\.2:8080/css\.html: HTTP 200, \d+ bytes, 0 links, title "CSS test page"', 15)
        time.sleep(1.0)
        q.screenshot(f"{S}/css-page.png")
        q.key("ctrl-l")                                 # structure: lists, indents, rules, italic, accents, table cells
        time.sleep(0.3)
        q.type("http://10.0.2.2:8080/layout.html\n")
        q.wait_serial(r'loaded http://10\.0\.2\.2:8080/layout\.html: HTTP 200, \d+ bytes, 0 links, title "Layout test"', 15)
        time.sleep(1.0)
        q.screenshot(f"{S}/layout-page.png")
        assert "PANIC" not in q.serial()
finally:
    web.terminate()
print("css: PASS (page loaded; check build/test/css-page.png)")
