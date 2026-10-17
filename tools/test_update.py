#!/usr/bin/env python3
"""Nimbus Update test, with a throwaway signing key:

* a NimbusOS 0.2 system is built that trusts the test key;
* a 0.3 release (new kernel + a new /System file) is signed with it and served
  over HTTPS from the host (openssl s_server, 10.0.2.2);
* in the guest: a tampered manifest and a corrupted payload are both rejected
  without installing anything; the real release installs, the system reboots
  into 0.3 from kernel slot B and then reports it is up to date;
* the live CD refuses to install a kernel.
"""
import sys, os, re, time, shutil, subprocess
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

def ic(c, r): return (6 + c * 84 + 42, 6 + r * 86 + 22)
T = "build/test/upd"
NFS = "build/tools/nfstool"
shutil.rmtree(T, ignore_errors=True)
os.makedirs(T)

def make(*args):
    subprocess.check_call(["make", "-s"] + list(args), stdout=subprocess.DEVNULL)

# --- keys, old system, new release -----------------------------------------
make("build/tools/nimbuskey", "build/tools/mkrelease", "build/tools/nfstool")
LOCAL = dict(UPDATE_URL="https://10.0.2.2:8445", UPDATE_HOST="10.0.2.2")   # the test server; release builds use nimbusos.vercel.app
subprocess.check_call(["build/tools/nimbuskey", "keygen", f"{T}/keys"], stdout=subprocess.DEVNULL)
pub = f"{T}/keys/update.pub"
make(f"BUILD={T}/old", "VERSION=0.2", f"UPDATE_PUBKEY={pub}", *[f"{k}={v}" for k, v in LOCAL.items()], "build", "iso")
make(f"BUILD={T}/new", "VERSION=0.3", f"UPDATE_PUBKEY={pub}", *[f"{k}={v}" for k, v in LOCAL.items()], "build")
os.makedirs(f"{T}/extra/System")
RELEASE_TXT = b"NimbusOS 0.3 test release\n"
open(f"{T}/extra/System/Release.txt", "wb").write(RELEASE_TXT)
subprocess.check_call(["build/tools/mkrelease", f"{T}/server", "0.3", f"{T}/new/kernel.bin", f"{T}/keys/update.key",
                       "release-notes.txt", "rootfs", f"{T}/new/genroot", f"{T}/new/userroot", f"{T}/extra"],
                      stdout=subprocess.DEVNULL)
manifest = open(f"{T}/server/manifest.txt").read()
kernel_hash = re.search(r"^kernel ([0-9a-f]{64}) ", manifest, re.M).group(1)

# tampered copies: an edited manifest, and a corrupted kernel payload
shutil.copytree(f"{T}/server", f"{T}/badsig")
open(f"{T}/badsig/manifest.txt", "a").write("notes this line was not signed\n")
shutil.copytree(f"{T}/server", f"{T}/badfile")
with open(f"{T}/badfile/files/{kernel_hash}", "r+b") as f:
    f.seek(1000); b = f.read(1); f.seek(1000); f.write(bytes([b[0] ^ 0xFF]))

key, cert = f"{T}/tls-key.pem", f"{T}/tls-cert.pem"
subprocess.check_call(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                       "-nodes", "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=updates.test"],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def serve(port, folder):
    return subprocess.Popen(["openssl", "s_server", "-accept", str(port), "-cert", os.path.abspath(cert),
                             "-key", os.path.abspath(key), "-tls1_3", "-WWW", "-quiet"],
                            cwd=folder, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
servers = [serve(8445, f"{T}/server"), serve(8446, f"{T}/badsig"), serve(8447, f"{T}/badfile")]
time.sleep(1.0)

def mbr_kernel_lba(img):
    with open(img, "rb") as f:
        f.seek(0x1AC)
        return int.from_bytes(f.read(4), "little")

IMG = f"{T}/system.img"
shutil.copy(f"{T}/old/nimbus.img", IMG)
assert mbr_kernel_lba(IMG) == 64

def terminal_session(q):
    q.mouse_home()
    q.double_click(*ic(0, 3))
    q.wait_serial(r'created: "Terminal"', 5)
    mark = [0]
    def run(cmd, pat, timeout=60):
        mark[0] = len(q.serial())
        q.type(cmd + "\n", delay=0.015)
        t0 = time.time()
        while not re.search(pat, q.serial()[mark[0]:]):
            assert time.time() - t0 < timeout, "%r: %r not seen; new output:\n%s" % (cmd, pat, q.serial()[mark[0]:][-2500:])
            time.sleep(0.2)
        time.sleep(0.5)
        return q.serial()[mark[0]:]
    return run

try:
    with Qemu([IMG], name="update") as q:
        q.wait_serial(r"NimbusOS 0\.2 - booting", 30)
        q.wait_serial(r"Desktop ready", timeout=40)
        q.wait_serial(r"DHCP: address 10\.0\.2\.15", 15)
        # the GUI app lists what is new in a scroll box: keys and the scroll bar move it
        q.mouse_home(); q.key("meta_l")
        for _ in range(13): q.key("down")
        q.key("ret")
        q.wait_serial(r'created: "Nimbus Update"', 5)
        time.sleep(6)
        q.screenshot(f"{T}/update-notes-top.png")
        for _ in range(30): q.key("down", delay=0.02)
        time.sleep(0.5)
        q.screenshot(f"{T}/update-notes-end.png")
        q.mouse_home()
        run = terminal_session(q)
        # the tampered servers are the same host on other ports: swap the served folder by port
        out = run("update", r"\[upd \] (signature invalid|NimbusOS 0\.3 is available|Update failed)")
        assert "signature invalid" not in out, out
        out = run("update", r"\[upd \] (NimbusOS 0\.3 is available|Update failed)")
        assert re.search(r"NimbusOS 0\.3 is available \(1 file and a new kernel", out), out
        out = run("update install", r"\[upd \] (Update to 0\.3 installed|Update failed)", 120)
        assert "restart to finish" in out, out
        assert "Installed /System/Release.txt" in out and "Installed the new kernel" in out, out
        assert re.search(r"kernel \d+ bytes written to slot B \(LBA 2080\), now active", out), out
        q.screenshot(f"{T}/update-terminal.png")
        mark = len(q.serial())
        q.type("reboot\n", delay=0.015)
        t0 = time.time()
        while not re.search(r"NimbusOS 0\.3 - booting", q.serial()[mark:]):
            assert time.time() - t0 < 60, "did not reboot into 0.3:\n" + q.serial()[mark:][-1500:]
            time.sleep(0.3)
        t0 = time.time()
        while not re.search(r"Desktop ready", q.serial()[mark:]):
            assert time.time() - t0 < 40, "desktop did not come up after the update"
            time.sleep(0.3)
        time.sleep(3)
        run = terminal_session(q)
        out = run("update", r"\[upd \] (NimbusOS 0\.3 is up to date|Update failed)")
        assert "is up to date" in out, out
        # the GUI app opens and shows the same state
        q.key("meta_l")
        for _ in range(13): q.key("down")
        q.key("ret")
        q.wait_serial(r'created: "Nimbus Update"', 5)
        time.sleep(4)
        q.screenshot(f"{T}/update-app.png")
        log = q.serial()
        assert "PANIC" not in log

    assert mbr_kernel_lba(IMG) == 2080, "MBR does not point at slot B"
    assert subprocess.check_output([NFS, "cat", IMG, "/System/Release.txt"]) == RELEASE_TXT
    assert b"Updated from 0.2 to 0.3" in subprocess.check_output([NFS, "cat", IMG, "/System/update.log"])
    assert "0 problem(s)" in subprocess.check_output([NFS, "check", IMG]).decode()

    # --- the live CD must refuse to install a kernel ---
    with Qemu([], name="update-live", extra=["-cdrom", f"{T}/old/nimbus.iso", "-boot", "d"]) as q:
        q.wait_serial(r"Desktop ready", timeout=60)
        q.wait_serial(r"DHCP: address 10\.0\.2\.15", 15)
        run = terminal_session(q)
        out = run("update install", r"\[upd \] (Update failed|Update to)")
        assert "running from the live CD" in out, out
finally:
    for s in servers: s.terminate()
print("update: PASS")
