#!/usr/bin/env python3
"""HTTPS test: the guest's TLS 1.3 client against OpenSSL's s_server on the
host (one server per cipher suite), HTTP/1.1 chunked transfer, an
http -> https redirect, wget saving an https download, and the Web
Browser opening an https page.  Host servers are reached at 10.0.2.2."""
import sys, os, re, time, shutil, subprocess, threading, http.server, socketserver
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

def ic(c, r): return (6 + c * 84 + 42, 6 + r * 86 + 22)
S = "build/test"
NFS = "build/tools/nfstool"
WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testweb")
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
IMG = f"{S}/https.img"
shutil.copy("build/nimbus.img", IMG)

# --- a self-signed certificate (not verified by NimbusOS anyway) ---
key, cert = f"{S}/tls-key.pem", f"{S}/tls-cert.pem"
subprocess.check_call(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                       "-nodes", "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=nimbus.test"],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def s_server(port, suite):
    # -WWW serves files from the current directory, one request per connection
    return subprocess.Popen(["openssl", "s_server", "-accept", str(port), "-cert", os.path.abspath(cert),
                             "-key", os.path.abspath(key), "-tls1_3", "-ciphersuites", suite, "-WWW", "-quiet"],
                            cwd=WEB, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

servers = [s_server(8443, "TLS_AES_128_GCM_SHA256"), s_server(8444, "TLS_CHACHA20_POLY1305_SHA256")]

# --- plain HTTP/1.1 server: a chunked response and a redirect to https ---
CHUNKS = [b"This body was sent ", b"in several HTTP/1.1 chunks ", b"and reassembled by NimbusOS.\n"]
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path == "/chunked":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            for c in CHUNKS:
                self.wfile.write(b"%x;ext=1\r\n%s\r\n" % (len(c), c))
            self.wfile.write(b"0\r\n\r\n")
        elif self.path == "/redirect":
            self.send_response(301)
            self.send_header("Location", "https://10.0.2.2:8444/index.html")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
        else:
            self.send_error(404)
socketserver.TCPServer.allow_reuse_address = True
plain = socketserver.TCPServer(("127.0.0.1", 8081), Handler)
threading.Thread(target=plain.serve_forever, daemon=True).start()
time.sleep(1.0)

index = open(os.path.join(WEB, "index.html"), "rb").read()
try:
    with Qemu([IMG], name="https") as q:
        q.wait_serial(r"Desktop ready", timeout=40)
        q.wait_serial(r"DHCP: address 10\.0\.2\.15", 15)
        q.mouse_home()
        q.double_click(*ic(0, 3))
        q.wait_serial(r'created: "Terminal"', 5)
        def run(cmd, pat, timeout=30):
            mark = len(q.serial())
            q.type(cmd + "\n", delay=0.015)
            t0 = time.time()
            while not re.search(pat, q.serial()[mark:]):
                assert time.time() - t0 < timeout, "%r: %r not seen; new output:\n%s" % (cmd, pat, q.serial()[mark:][-2000:])
                time.sleep(0.2)
            time.sleep(0.5)
        run("wget https://10.0.2.2:8443/index.html secure.html", r"\[term\] saved \d+ bytes|\[njob\] (wget|http): ")
        run("http https://10.0.2.2:8444/hello.txt", r"\[njob\] plain text file served|\[njob\] (wget|http): ")
        run("wget http://10.0.2.2:8081/chunked chunked.txt", r"\[term\] saved \d+ bytes|\[njob\] (wget|http): ")
        run("wget http://10.0.2.2:8081/redirect moved.html", r"\[term\] saved \d+ bytes|\[njob\] (wget|http): ")
        q.screenshot(f"{S}/https-terminal.png")
        q.key("alt-f4")
        q.wait_serial(r'destroyed: "Terminal"', 5)
        # Web Browser (Start menu, 11 items down)
        q.key("meta_l")
        for _ in range(11): q.key("down")
        q.key("ret")
        q.wait_serial(r'created: "Web Browser"', 5)
        q.key("ctrl-l")
        q.type("https://10.0.2.2:8444/index.html\n")
        q.wait_serial(r'loaded https://10\.0\.2\.2:8444/index\.html: HTTP 200', 30)
        time.sleep(1.0)
        q.screenshot(f"{S}/https-web.png")
        # UTF-8 and HTML entities become readable ASCII
        q.key("ctrl-l")
        q.type("https://10.0.2.2:8443/utf8.html\n")
        q.wait_serial(r'loaded https://10\.0\.2\.2:8443/utf8\.html: HTTP 200, \d+ bytes, 0 links, title "Cafe - \"Unicode\" test"', 30)
        time.sleep(1.0)
        q.screenshot(f"{S}/https-utf8.png")
        log = q.serial()
finally:
    for s in servers: s.terminate()
    plain.shutdown()

for pat in [r"\[tls \] connected to 10\.0\.2\.2: TLS 1\.3, AES-128-GCM, X25519",
            r"\[tls \] connected to 10\.0\.2\.2: TLS 1\.3, CHACHA20-POLY1305, X25519",
            r"\[njob\] Encrypted: TLS 1\.3",
            r"\[njob\] plain text file served over HTTP",
            r"\[http\] 301 redirect to https://10\.0\.2\.2:8444/index\.html"]:
    assert re.search(pat, log), "missing in log: " + pat
assert "PANIC" not in log

def cat(p): return subprocess.check_output([NFS, "cat", IMG, p])
assert cat("/secure.html") == index, "https download differs from the served file"
assert cat("/moved.html") == index, "redirected download differs"
assert cat("/chunked.txt") == b"".join(CHUNKS), cat("/chunked.txt")
print("https: PASS")
