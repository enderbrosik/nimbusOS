#!/usr/bin/env python3
"""Web search test.  A local HTTPS server (10.0.2.2:8445) imitates the HTML endpoint of
DuckDuckGo: GET /html/?q=... answers with a results page whose links are wrapped
as //duckduckgo.com/l/?uddg=<encoded target>&amp;rut=... .  The guest's search_url
setting points at it, so the test checks, end to end:

* words typed in the address bar become a search URL (spaces -> '+');
* the request carries the honest default User-Agent (NimbusOS/<version>);
* &amp; in links is decoded and the uddg wrapper is removed, so clicking a result
  goes straight to the target with its query string intact.
The real duckduckgo.com is not used (the test environment has no internet)."""
import sys, os, re, time, shutil, ssl, subprocess, threading, http.server, urllib.parse
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

S = "build/test"
NFS = "build/tools/nfstool"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
os.makedirs(S, exist_ok=True)
IMG = f"{S}/search.img"
shutil.copy("build/nimbus.img", IMG)

# the guest searches this engine instead of the real one
try:
    cfg = subprocess.check_output([NFS, "cat", IMG, "/System/settings.cfg"], stderr=subprocess.DEVNULL).decode()
except subprocess.CalledProcessError:
    cfg = ""                                          # a fresh image has no settings file yet
cfg = "".join(l + "\n" for l in cfg.splitlines() if not l.startswith("search_url="))
cfg += "search_url=https://10.0.2.2:8445/html/?q=\n"
open(f"{S}/search-settings.cfg", "w").write(cfg)
subprocess.check_call([NFS, "put", IMG, f"{S}/search-settings.cfg", "/System/settings.cfg"])

key, cert = f"{S}/tls-key.pem", f"{S}/tls-cert.pem"
subprocess.check_call(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                       "-nodes", "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=search.test"],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

TARGET_QUERY = "x=1&y=2"
def wrapped(target):
    return "//duckduckgo.com/l/?uddg=" + urllib.parse.quote(target, safe="") + "&amp;rut=0123456789abcdef"

seen = {"requests": [], "agents": [], "posts": []}
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):                      # the results page's own search form (method="post")
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode()
        seen["posts"].append((self.path, body, self.headers.get("Content-Type", "")))
        q = urllib.parse.parse_qs(body).get("q", [""])[0]
        self.answer(self.results(q))
    def results(self, q):
        options = "".join(f'<option value="r{i}">Region number {i}</option>' for i in range(200))
        if True:
            t1 = wrapped("https://10.0.2.2:8445/target.html?" + TARGET_QUERY)
            t2 = wrapped("https://10.0.2.2:8445/other.html")
            body = f"""<html><head><title>{q} at DuckDuckGo</title></head><body>
<div id="header"><a class="header__logo-wrap" href="/html/"><img class="header__logo" src="//duckduckgo.com/assets/logo_header.v108.svg" alt="DuckDuckGo"></a>
<form action="/html/" method="post"><input type="text" name="q" value="{q}"><input type="hidden" name="kl" value="us-en"><select name="region"><option value="">All Regions</option>{options}</select><button type="submit" name="go" value="1">Search <b>now</b></button></form></div>
<div class="result"><h2 class="result__title"><a class="result__a" href="{t1}">NimbusOS hobby OS</a></h2>
<span class="result__icon"><a href="{t1}"><img class="result__icon__img" width="16" height="16" alt="" src="//external-content.duckduckgo.com/ip3/10.0.2.2.ico" name="i"></a></span>
<a class="result__url" href="{t1}">10.0.2.2:8445/target.html</a>
<a class="result__snippet" href="{t1}">A hobby <b>operating system</b> written from scratch.</a></div>
<div class="result"><h2 class="result__title"><a class="result__a" href="{t2}">Another result</a></h2></div>
<a href="/html/?q={urllib.parse.quote_plus(q)}&amp;s=30">Next</a>
</body></html>"""
        return body
    def do_GET(self):
        seen["requests"].append(self.path)
        seen["agents"].append(self.headers.get("User-Agent", ""))
        url = urllib.parse.urlparse(self.path)
        if url.path == "/html/":
            body = self.results(urllib.parse.parse_qs(url.query).get("q", [""])[0])
        elif url.path == "/target.html":
            body = "<html><head><title>Search target</title></head><body><h1>You reached the target</h1></body></html>"
        else:
            self.send_error(404)
            return
        self.answer(body)
    def answer(self, body):
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(cert, key)
server = http.server.ThreadingHTTPServer(("127.0.0.1", 8445), Handler)
server.socket = ctx.wrap_socket(server.socket, server_side=True)
threading.Thread(target=server.serve_forever, daemon=True).start()
time.sleep(0.5)

LINK1 = (170, 253)          # the first result title link, below the logo and the results page's own search form
try:
    with Qemu([IMG], name="search") as q:
        q.wait_serial(r"Desktop ready", timeout=40)
        q.wait_serial(r"DHCP: address 10\.0\.2\.15", 15)
        q.mouse_home()
        q.key("meta_l")
        for _ in range(11): q.key("down")
        q.key("ret")
        q.wait_serial(r'created: "Web Browser"', 5)
        q.key("ctrl-l")
        time.sleep(0.3)
        q.type("nimbus os search\n")
        q.wait_serial(r'loaded https://10\.0\.2\.2:8445/html/\?q=nimbus\+os\+search: HTTP 200, \d+ bytes, \d+ links, title "nimbus os search at DuckDuckGo"', 30)
        time.sleep(1.0)
        q.screenshot(f"{S}/search-results.png")
        # the results page has its own search form: click the box, replace the words, press Enter (POST)
        q.click(200, 213)
        time.sleep(0.3)
        q.key("ctrl-a")
        q.type("second query\n", delay=0.04)
        q.wait_serial(r'loaded https://10\.0\.2\.2:8445/html/: HTTP 200, \d+ bytes, \d+ links, title "second query at DuckDuckGo"', 30)
        time.sleep(1.0)
        q.screenshot(f"{S}/search-form.png")
        q.click(*LINK1)
        q.wait_serial(r'loaded https://10\.0\.2\.2:8445/target\.html\?x=1&y=2: HTTP 200, \d+ bytes, 0 links, title "Search target"', 30)
        time.sleep(1.0)
        q.screenshot(f"{S}/search-target.png")
        assert "PANIC" not in q.serial()
finally:
    server.shutdown()

assert "/html/?q=nimbus+os+search" in seen["requests"], seen["requests"]
assert "/target.html?" + TARGET_QUERY in seen["requests"], seen["requests"]
assert not [r for r in seen["requests"] if "logo" in r or ".ico" in r or ".svg" in r], seen["requests"]     # no pictures fetched for icons and svg
assert len(seen["posts"]) == 1, seen["posts"]
path, body, ctype = seen["posts"][0]
assert path == "/html/" and ctype == "application/x-www-form-urlencoded", (path, ctype)
assert urllib.parse.parse_qs(body) == {"q": ["second query"], "kl": ["us-en"]}, body        # typed text + hidden field
assert all(a.startswith("NimbusOS/") for a in seen["agents"]), seen["agents"]
print("search: PASS")
