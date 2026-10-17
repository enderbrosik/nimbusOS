#!/usr/bin/env python3
"""nimbusfuse + nfstool export test (Linux, needs libfuse 2 and /dev/fuse; skipped otherwise).
Mounts a copy of the system image, reads, writes, renames and deletes through the mount,
unmounts, then checks the image with nfstool (consistent, and the data is what was written)."""
import os, sys, shutil, subprocess, time, filecmp
os.chdir(os.path.join(os.path.dirname(__file__), ".."))
S = "build/test/fuse"
shutil.rmtree(S, ignore_errors=True); os.makedirs(S + "/mnt")
subprocess.check_call(["make", "-s", "build/nfstool"])
NFS = "build/nfstool"
if not os.path.exists("/dev/fuse") or subprocess.call("pkg-config --exists fuse", shell=True):
    print("fuse: SKIP (no libfuse 2 or /dev/fuse)"); sys.exit(0)
pc = subprocess.check_output("pkg-config --cflags --libs fuse", shell=True).decode().split()
subprocess.check_call(["gcc", "-std=gnu11", "-O2", "-I.", "tools/nimbusfuse.c", "fs/nbfs.c", *pc, "-o", f"{S}/nimbusfuse"])
img = f"{S}/disk.img"
subprocess.check_call([NFS, "mkfs", img, "16", "TEST"], stdout=subprocess.DEVNULL)
for d in ("Documents",): subprocess.check_call([NFS, "mkdir", img, "/" + d])
open(f"{S}/readme.txt", "w").write("a readme\n")
subprocess.check_call([NFS, "put", img, f"{S}/readme.txt", "/readme.txt"])
subprocess.check_call([f"{S}/nimbusfuse", img, f"{S}/mnt"], stderr=subprocess.DEVNULL)
m = f"{S}/mnt"
try:
    assert "readme.txt" in os.listdir(m) and "Documents" in os.listdir(m)
    big = os.urandom(300_000)
    open(f"{m}/Documents/big.bin", "wb").write(big)
    os.mkdir(f"{m}/Documents/Made Here")
    open(f"{m}/Documents/Made Here/note.txt", "w").write("hello from the mount\n")
    os.rename(f"{m}/Documents/Made Here/note.txt", f"{m}/Documents/Made Here/renamed.txt")
    assert open(f"{m}/Documents/big.bin", "rb").read() == big
    assert os.stat(f"{m}/Documents/big.bin").st_size == 300_000
    os.truncate(f"{m}/Documents/big.bin", 1000)
    os.remove(f"{m}/readme.txt")
finally:
    subprocess.call(["fusermount", "-u", m])
    time.sleep(0.5)
assert subprocess.run([NFS, "check", img], capture_output=True, text=True).returncode == 0, "image is not consistent"
assert subprocess.check_output([NFS, "cat", img, "/Documents/Made Here/renamed.txt"]) == b"hello from the mount\n"
assert len(subprocess.check_output([NFS, "cat", img, "/Documents/big.bin"])) == 1000
assert subprocess.run([NFS, "cat", img, "/readme.txt"], capture_output=True).returncode != 0
# export: the whole tree comes out and matches
subprocess.check_call([NFS, "export", img, "/Documents", f"{S}/out"], stderr=subprocess.DEVNULL)
assert open(f"{S}/out/Made Here/renamed.txt").read() == "hello from the mount\n"
print("fuse: PASS")
