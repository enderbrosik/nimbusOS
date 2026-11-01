#!/usr/bin/env python3
"""Raspberry Pi build test (QEMU raspi3b): the kernel boots to the desktop,
the on-screen log is drawn, and the log reaches NIMBUS.LOG on the SD card."""
import os, subprocess, sys, time
S = "build/test"
os.makedirs(S, exist_ok=True)
subprocess.check_call(["make", "-s", "pi", "build/pi/qemu-sd.img"], stdout=subprocess.DEVNULL)
subprocess.check_call([sys.executable, "tools/pi_shot.py", "14", f"{S}/pi-desktop.png", f"{S}/pi-serial.txt"],
                      stderr=subprocess.DEVNULL)
ser = open(f"{S}/pi-serial.txt", errors="replace").read()
for want in ["booting on Raspberry Pi", "timer interrupts working", "SD card ready",
             "writing this log to NIMBUS.LOG", "Core kernel OK.", "Desktop ready"]:
    assert want in ser, "missing in serial log: " + want
assert "PANIC" not in ser
subprocess.check_call(["mcopy", "-o", "-i", "build/pi/qemu-sd.img@@1M", "::NIMBUS.LOG", f"{S}/pi-NIMBUS.LOG"])
log = open(f"{S}/pi-NIMBUS.LOG", errors="replace").read()
assert "booting on Raspberry Pi" in log and "Desktop ready" in log, "NIMBUS.LOG incomplete"
print("pi: PASS (desktop in build/test/pi-desktop.png, log in build/test/pi-NIMBUS.LOG)")
