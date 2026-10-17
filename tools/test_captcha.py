#!/usr/bin/env python3
"""Picture-challenge page test (a human-solved flow, nothing is solved automatically).
A local HTTPS server (10.0.2.2:8446) shows a 3-picture challenge with checkboxes and a
Verify button.  The test plays the human: it ticks the boxes by clicking, submits, and checks
that the picture requests, the POST body, and the cookie round trip all work:
the server's Set-Cookie on the 302 must come back as a Cookie header on the next request."""
import sys, os, time, shutil, ssl, io, subprocess, threading, http.server, urllib.parse
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu
from PIL import Image

S = "build/test"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
os.makedirs(S, exist_ok=True)
IMG = f"{S}/captcha.img"
shutil.copy("build/nimbus.img", IMG)
key, cert = f"{S}/tls-key.pem", f"{S}/tls-cert.pem"
subprocess.check_call(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                       "-nodes", "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=captcha.test"],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
COLORS = [(220, 40, 40), (40, 160, 60), (50, 80, 220)]
def png(i):
    b = io.BytesIO(); Image.new("RGB", (64, 64), COLORS[i]).save(b, "PNG"); return b.getvalue()

seen = {"gets": [], "posts": [], "cookies": [], "agents": []}
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype="text/html", extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for k, v in extra: self.send_header(k, v)
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        seen["gets"].append(self.path); seen["agents"].append(self.headers.get("User-Agent", "")); seen["cookies"].append((self.path, self.headers.get("Cookie", "")))
        if self.path.startswith("/pic"):
            return self.send(200, png(int(self.path[4])), "image/png")
        if self.path == "/ok":
            ok = "challenge=passed" in self.headers.get("Cookie", "")
            return self.send(200, (f"<html><head><title>{'Verified' if ok else 'Denied'}</title></head><body><h1>{'Verified' if ok else 'Denied'}</h1></body></html>").encode())
        page = """<html><head><title>Challenge</title></head><body><h2>Select the pictures you like</h2>
<form action="/verify" method="post"><input type="hidden" name="token" value="t123">
<img src="/pic0.png" width="64" height="64"> <img src="/pic1.png" width="64" height="64"> <img src="/pic2.png" width="64" height="64"><br>
<input type="checkbox" name="pick" value="0"> A <input type="checkbox" name="pick" value="1"> B <input type="checkbox" name="pick" value="2"> C<br>
<input type="submit" value="Verify"></form></body></html>"""
        self.send(200, page.encode(), extra=[("Set-Cookie", "session=abc; Path=/; HttpOnly")])
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); body = self.rfile.read(n).decode()
        seen["posts"].append((self.path, body, self.headers.get("Cookie", "")))
        self.send(302, b"", extra=[("Location", "/ok"), ("Set-Cookie", "challenge=passed; Path=/")])

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(cert, key)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 8446), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
threading.Thread(target=srv.serve_forever, daemon=True).start(); time.sleep(0.5)

BOX = [tuple(map(int, a.split(","))) for a in os.environ.get("BOXES", "152,286;188,286;224,286").replace(";", " ").split()]
VERIFY = tuple(map(int, os.environ.get("VERIFY", "179,303").split(",")))
try:
    with Qemu([IMG], name="captcha") as q:
        q.wait_serial(r"Desktop ready", timeout=40)
        q.wait_serial(r"DHCP: address 10\.0\.2\.15", 15)
        q.mouse_home(); q.key("meta_l")
        for _ in range(11): q.key("down")
        q.key("ret"); q.wait_serial(r'created: "Web Browser"', 5)
        q.key("ctrl-l"); time.sleep(0.3); q.type("https://10.0.2.2:8446/\n")
        q.wait_serial(r'loaded https://10\.0\.2\.2:8446/: HTTP 200', 30)
        time.sleep(3.0); q.screenshot(f"{S}/captcha-page.png")
        q.click(817, 137); time.sleep(0.4); q.screenshot(f"{S}/captcha-ualist.png"); q.click(740, 188); time.sleep(0.3)
        q.key("ctrl-l"); time.sleep(0.3); q.type("https://10.0.2.2:8446/\n")
        q.wait_serial(r'loaded https://10\.0\.2\.2:8446/: HTTP 200', 30)
        time.sleep(3.0)
        if True:
            for b in (BOX[0], BOX[2]): q.click(*b); time.sleep(0.3)
            q.screenshot(f"{S}/captcha-ticked.png")
            q.click(*VERIFY)
            q.wait_serial(r'loaded https://10\.0\.2\.2:8446/ok: HTTP 200.*title "Verified"', 30)
            time.sleep(1.0); q.screenshot(f"{S}/captcha-done.png")
        assert "PANIC" not in q.serial()
finally:
    srv.shutdown()
if True:
    assert all(p in seen["gets"] for p in ("/pic0.png", "/pic1.png", "/pic2.png")), seen["gets"]
    path, body, ck = seen["posts"][0]
    assert urllib.parse.parse_qs(body) == {"token": ["t123"], "pick": ["0", "2"]}, body
    assert "session=abc" in ck, ck
    assert [c for p, c in seen["cookies"] if p == "/ok"][0].count("challenge=passed") == 1
    assert seen["agents"][0].startswith("NimbusOS/") and "Chrome/" in seen["agents"][-1], seen["agents"]
    print("captcha: PASS")
else:
    print("calibration run", seen["gets"])
