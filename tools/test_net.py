#!/usr/bin/env python3
"""Networking test over QEMU user networking (slirp):
DHCP lease, ping to the gateway, DNS against a tiny DNS server on the host,
wget/http from a host HTTP server (10.0.2.2 = host loopback), and the Web
browser loading a page and following a link."""
import sys, os, re, time, shutil, socket, struct, subprocess, threading
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

def ic(c, r): return (6 + c * 84 + 42, 6 + r * 86 + 22)
S = "build/test"
NFS = "build/tools/nfstool"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
IMG = f"{S}/net.img"
shutil.copy("build/nimbus.img", IMG)

# --- host HTTP server (port 8080) ---
web = subprocess.Popen([sys.executable, "-m", "http.server", "8080", "--bind", "127.0.0.1",
                        "--directory", os.path.join(os.path.dirname(__file__), "testweb")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# --- host DNS server on 127.0.0.1:53: answers A records for *.nimbus.test ---
dns_queries = []
def dns_server():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 53))
    s.settimeout(0.5)
    while not stop.is_set():
        try:
            q, addr = s.recvfrom(512)
        except socket.timeout:
            continue
        i, labels = 12, []
        while q[i]:
            labels.append(q[i + 1:i + 1 + q[i]].decode()); i += 1 + q[i]
        name = ".".join(labels); dns_queries.append(name)
        question = q[12:i + 5]
        if name.endswith("nimbus.test"):
            hdr = q[:2] + b"\x81\x80" + struct.pack(">HHHH", 1, 1, 0, 0)
            ans = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + bytes([10, 0, 2, 2])
            s.sendto(hdr + question + ans, addr)
        else:
            s.sendto(q[:2] + b"\x81\x83" + struct.pack(">HHHH", 1, 0, 0, 0) + question, addr)
stop = threading.Event()
threading.Thread(target=dns_server, daemon=True).start()
time.sleep(0.5)

try:
    with Qemu([IMG], name="net") as q:
        q.wait_serial(r"Desktop ready", timeout=40)
        q.wait_serial(r"DHCP: address 10\.0\.2\.15 mask 255\.255\.255\.0 gateway 10\.0\.2\.2 dns 10\.0\.2\.3", 15)
        q.mouse_home()
        q.double_click(*ic(0, 3))
        q.wait_serial(r'created: "Terminal"', 5)
        def run(cmd, wait):
            q.type(cmd + "\n", delay=0.015)
            time.sleep(wait)
        run("ping 10.0.2.2 3", 4)
        run("nslookup www.nimbus.test 10.0.2.2", 2)
        run("nslookup nosuch.example 10.0.2.2", 2)
        run("wget http://10.0.2.2:8080/index.html", 3)
        run("http http://10.0.2.2:8080/hello.txt", 3)
        run("wget http://www.nimbus.test:8080/hello.txt", 1)   # resolved via the DHCP DNS server: fails here
        time.sleep(3)
        q.screenshot(f"{S}/net-terminal.png")
        q.key("alt-f4")
        q.wait_serial(r'destroyed: "Terminal"', 5)
        # Web browser: Start menu item 10, then type the address
        q.key("meta_l")
        for _ in range(11): q.key("down")
        q.key("ret")
        q.wait_serial(r'created: "Web Browser"', 5)
        q.key("ctrl-l")
        q.type("http://10.0.2.2:8080/index.html\n")
        q.wait_serial(r'loaded http://10.0.2.2:8080/index.html: HTTP 200, \d+ bytes, 1 links, title "NimbusOS test page"', 15)
        time.sleep(0.5)
        q.screenshot(f"{S}/net-web.png")
        # the link is on layout line 2 (heading, paragraph break, paragraph, break, link paragraph)
        cx, cy = 132, 119
        for line in range(2, 8):
            q.click(cx + 14 + 30, cy + 36 + 14 + line * 18 + 8)
            if "follow link" in q.serial():
                break
        q.wait_serial(r'loaded http://10.0.2.2:8080/second.html: HTTP 200, \d+ bytes, 1 links, title "Second page"', 15)
        time.sleep(0.5)
        q.screenshot(f"{S}/net-web2.png")
        log = q.serial()
finally:
    stop.set()
    web.terminate()

for pat in [r"\[njob\] Reply from 10\.0\.2\.2: seq=3", r"3 sent, 3 received, 0% loss",
            r"\[njob\] Address: 10\.0\.2\.2", r"nosuch\.example: name not found",
            r"\[njob\] HTTP 200, \d+ bytes",
            r"\[njob\] plain text file served over HTTP"]:
    assert re.search(pat, log), "missing in log: " + pat
assert "www.nimbus.test" in dns_queries, dns_queries
# Terminal output is not in the serial log; check effects instead
saved = subprocess.check_output([NFS, "cat", IMG, "/index.html"]).decode()
assert "Hello from the host" in saved, saved[:200]
print("dns queries seen by the host server:", dns_queries)
print("net: PASS")
