#!/usr/bin/env python3
"""File Explorer shows a USB stick as the folder /USB: browse it, make a folder, rename it,
copy a file in from the system disk and delete one, then check the disk on the host."""
import sys, os, re, time, shutil, subprocess
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

S = "build/test/usbx"
NFS = "build/tools/nfstool"
shutil.rmtree(S, ignore_errors=True); os.makedirs(S)
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
def ic(c, r): return (6 + c * 84 + 42, 6 + r * 86 + 22)
disk, sysimg = f"{S}/stick.img", f"{S}/sys.img"
with open(disk, "wb") as f: f.truncate(64 * 1024 * 1024)
subprocess.check_call(["mkfs.fat", "-F", "32", "-s", "1", "-n", "STICK", disk], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
open(f"{S}/a.txt", "w").write("on the stick\n")
subprocess.check_call(["mcopy", "-i", disk, f"{S}/a.txt", "::Notes From Stick.txt"])
subprocess.check_call(["mmd", "-i", disk, "::Pictures"])
shutil.copy("build/nimbus.img", sysimg)
extra = ["-device", "qemu-xhci,id=xhci", "-drive", f"if=none,id=usbd,file={disk},format=raw", "-device", "usb-storage,bus=xhci.0,drive=usbd"]
with Qemu([sysimg], extra=extra, name="usbx") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    assert re.search(r'\[usb \] USB disk', q.serial()), "no USB disk"
    q.mouse_home()
    q.double_click(*ic(0, 0))
    q.wait_serial(r'created: "File Explorer"', 5)
    time.sleep(1.0)
    q.double_click(270, 278)                                  # the USB folder in the root
    q.wait_serial(r"\[expl\] navigate /USB", 5)
    time.sleep(1.0)
    q.screenshot(f"{S}/usb.png")
    q.click(270, 168)                                         # Notes From Stick.txt (folders sort first)
    q.key("f2"); time.sleep(0.6)
    q.key("ctrl-a"); q.type("Renamed Note.txt\n", delay=0.04)
    q.wait_serial(r"\[expl\] renamed /USB/Notes From Stick.txt -> /USB/Renamed Note.txt", 10)
    time.sleep(0.6)
    q.screenshot(f"{S}/renamed.png")
    q.click(270, 146)                                         # the Pictures folder
    q.key("delete"); time.sleep(0.6)
    q.key("ret")                                              # Yes
    q.wait_serial(r"\[expl\] deleted /USB/Pictures", 10)
    time.sleep(0.6)
    q.screenshot(f"{S}/deleted.png")
    assert "PANIC" not in q.serial()
fs = subprocess.run(["fsck.fat", "-n", disk], capture_output=True, text=True)
assert fs.returncode == 0, fs.stdout + fs.stderr
ls = subprocess.check_output(["mdir", "-i", disk, "::"]).decode()
assert "Renamed Note" in ls and "Notes From" not in ls and "Pictures" not in ls, ls
assert subprocess.check_output(["mtype", "-i", disk, "::Renamed Note.txt"]) == b"on the stick\n"
print("usb explorer: PASS")
