#!/usr/bin/env python3
"""FAT32 test.  A second drive (a FAT32 volume made by mkfs.fat and filled with mtools) is
attached to the guest; the Terminal's `fat` command lists, reads, writes, makes folders and
deletes on it.  Afterwards fsck.fat must find the volume clean and mtools must read back
exactly what the guest wrote.  Run once with a bare FAT32 disk and once with an MBR partition."""
import sys, os, re, time, shutil, subprocess, struct, random
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

S = "build/test/fat"
NFS = "build/tools/nfstool"
shutil.rmtree(S, ignore_errors=True); os.makedirs(S)
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
def ic(c, r): return (6 + c * 84 + 42, 6 + r * 86 + 22)
random.seed(7)
BIG = bytes(random.randrange(256) for _ in range(150_000))          # many clusters
open(f"{S}/big.bin", "wb").write(BIG)
open(f"{S}/hello.txt", "w").write("Hello from FAT32\n")
open(f"{S}/long.txt", "w").write("a file with a long name\n")
open(f"{S}/inner.txt", "w").write("inside the folder\n")

def make_disk(path, partitioned):
    mb = 64
    with open(path, "wb") as f: f.truncate(mb * 1024 * 1024)
    off = 2048 * 512 if partitioned else 0
    mk = ["mkfs.fat", "-F", "32", "-s", "1", "-n", "TESTDISK"]
    if partitioned:
        mk += ["--offset", "2048"]
        mbr = bytearray(512)
        mbr[446 + 4] = 0x0C; mbr[446 + 8:446 + 12] = struct.pack("<I", 2048)
        mbr[446 + 12:446 + 16] = struct.pack("<I", mb * 2048 - 2048); mbr[510:512] = b"\x55\xAA"
        with open(path, "r+b") as f: f.write(mbr)
    subprocess.check_call(mk + [path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t = f"{path}@@{off}"
    for src, dst in [("hello.txt", "::HELLO.TXT"), ("long.txt", "::A Long File Name.txt"), ("big.bin", "::BIG.BIN")]:
        subprocess.check_call(["mcopy", "-i", t, f"{S}/{src}", dst])
    subprocess.check_call(["mmd", "-i", t, "::DOCS"])
    subprocess.check_call(["mcopy", "-i", t, f"{S}/inner.txt", "::DOCS/inner.txt"])
    return t

def session(name, partitioned):
    disk = f"{S}/{name}.img"
    t = make_disk(disk, partitioned)
    sysimg = f"{S}/{name}-sys.img"
    shutil.copy("build/nimbus.img", sysimg)
    open(f"{S}/readme.txt", "w").write("written by the guest\n")
    subprocess.check_call([NFS, "put", sysimg, f"{S}/readme.txt", "/readme.txt"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with Qemu([sysimg, disk], name="fat-" + name) as q:
        q.wait_serial(r"Desktop ready", timeout=40)
        q.mouse_home()
        q.double_click(*ic(0, 3))
        q.wait_serial(r'created: "Terminal"', 5)
        def run(cmd, pat, timeout=30):
            mark = len(q.serial())
            q.type(cmd + "\n", delay=0.015)
            t0 = time.time()
            while not re.search(pat, q.serial()[mark:]):
                assert time.time() - t0 < timeout, "%r: %r not seen:\n%s" % (cmd, pat, q.serial()[mark:][-1500:])
                time.sleep(0.2)
            return q.serial()[mark:]
        out = run("fat info", r"\[fat \] FAT32 volume .* free")
        assert 'volume "TESTDISK"' in out, out
        out = run("fat ls", r"\[fat \] \d+ item")
        assert "A Long File Name.txt" in out and "HELLO.TXT" in out and "DOCS" in out and "BIG.BIN" in out and "4 item(s)" in out, out
        assert re.search(r"BIG\.BIN\s+150000", out), out
        out = run("fat cat /HELLO.TXT", r"\[fat \] Hello from FAT32")
        out = run("fat cat /docs/inner.txt", r"\[fat \] inside the folder")             # folders and names ignore case
        run("fat get /BIG.BIN /big-copy.bin", r"copied 150000 bytes")
        run("fat put /readme.txt /Written_By_Guest.txt", r"copied 21 bytes")
        q.type("fat mkdir /NewFolder\n", delay=0.015); time.sleep(0.8)
        run("fat put /readme.txt /NewFolder/Inside_Folder.txt", r"copied 21 bytes")
        run("fat put /big-copy.bin /BiggerCopy.bin", r"copied 150000 bytes", 90)       # a long chain of clusters
        q.type("fat rm /HELLO.TXT\n", delay=0.015); time.sleep(0.8)
        q.type("fat rm /DOCS\n", delay=0.015)                                           # not empty: must fail
        out = run("fat ls", r"\[fat \] \d+ item")
        assert "HELLO.TXT" not in out and "DOCS" in out and "NewFolder" in out and "BiggerCopy.bin" in out and "Written_By_Guest.txt" in out, out
        assert "Directory not empty" in q.serial() or "not empty" in q.serial().lower()
        run("fat ls /NewFolder", r"\[fat \] \d+ item")
        q.screenshot(f"{S}/{name}.png")
        assert "PANIC" not in q.serial()
    # the host must see a clean volume with exactly what the guest wrote
    if not partitioned:                                   # fsck.fat checks whole-disk volumes
        fs = subprocess.run(["fsck.fat", "-n", disk], capture_output=True, text=True)
        assert fs.returncode == 0, fs.stdout + fs.stderr
    ls = subprocess.check_output(["mdir", "-i", t, "-/", "::"]).decode()
    assert "BiggerCopy" in ls and "Written_By_Guest" in ls and "Inside_Folder" in ls and "HELLO" not in ls, ls
    got = subprocess.check_output(["mtype", "-i", t, "::BiggerCopy.bin"])
    assert got == BIG, "file written by the guest differs"
    assert subprocess.check_output(["mtype", "-i", t, "::NewFolder/Inside_Folder.txt"]) == b"written by the guest\n"
    assert subprocess.check_output(["mtype", "-i", t, "::DOCS/inner.txt"]) == b"inside the folder\n"
    print(f"fat32 {name}: PASS")

session("bare", False)
session("partitioned", True)
