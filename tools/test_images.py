#!/usr/bin/env python3
"""Browser pictures test: a page with a PNG, a GIF and a JPEG (sized differently, one inside a link),
an <img> with a missing file and one that is not a picture.  The browser logs each picture it
loads (size) or why it failed; the screenshot is kept for a visual check."""
import sys, os, re, time, shutil, subprocess, threading, http.server
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu
from PIL import Image, ImageDraw

S = "build/test"
IMG = f"{S}/images.img"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
os.makedirs(S, exist_ok=True)
shutil.copy("build/nimbus.img", IMG)

web = f"{S}/imgweb"
shutil.rmtree(web, ignore_errors=True)
os.makedirs(web)
def art(w, h, color):
    im = Image.new("RGB", (w, h), (240, 240, 245))
    d = ImageDraw.Draw(im)
    for i in range(0, w, 16): d.line([(i, 0), (w - 1 - i, h - 1)], fill=color, width=2)
    d.rectangle([0, 0, w - 1, h - 1], outline=(30, 30, 60), width=3)
    return im
art(120, 80, (200, 40, 40)).save(f"{web}/a.png")
art(100, 100, (40, 120, 40)).convert("P", palette=Image.ADAPTIVE, colors=32).save(f"{web}/b.gif")
art(160, 90, (40, 40, 200)).save(f"{web}/c.jpg", quality=90)
open(f"{web}/notimage.png", "w").write("this is not a picture")
open(f"{web}/page.html", "w").write("""<html><head><title>Pictures</title></head><body>
<h1>Pictures</h1>
<p>A PNG <img src="a.png" alt="red png"> and a GIF <img src="b.gif" alt="green gif" width="60"> inline with text.</p>
<p><a href="target.html"><img src="c.jpg" alt="blue jpeg"></a></p>
<p>Missing: <img src="missing.png" alt="not there" width="90" height="30"> and broken: <img src="notimage.png" alt="broken"></p>
</body></html>""")
open(f"{web}/target.html", "w").write("<html><head><title>Picture link target</title></head><body><p>You clicked the picture.</p></body></html>")

class Q(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=web, **k)
    def log_message(self, *a): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 8080), Q)
threading.Thread(target=server.serve_forever, daemon=True).start()
time.sleep(0.5)

try:
    with Qemu([IMG], name="images") as q:
        q.wait_serial(r"Desktop ready", timeout=40)
        q.wait_serial(r"DHCP: address 10\.0\.2\.15", 15)
        q.mouse_home()
        q.key("meta_l")
        for _ in range(11): q.key("down")
        q.key("ret")
        q.wait_serial(r'created: "Web Browser"', 5)
        q.key("ctrl-l")
        time.sleep(0.3)
        q.type("http://10.0.2.2:8080/page.html\n")
        q.wait_serial(r'loaded http://10\.0\.2\.2:8080/page\.html: HTTP 200, \d+ bytes, 1 links, title "Pictures"', 15)
        q.wait_serial(r"image 0 loaded 120x80", 20)
        q.wait_serial(r"image 1 loaded 100x100", 20)
        q.wait_serial(r"image 2 loaded 160x90", 20)
        q.wait_serial(r"image 3 failed", 20)
        q.wait_serial(r"image 4 cannot be shown", 20)
        time.sleep(1.5)
        q.screenshot(f"{S}/images-page.png")
        assert "PANIC" not in q.serial()
finally:
    server.shutdown()
print("images: PASS (page, 3 formats, a missing and a broken picture; see build/test/images-page.png)")
