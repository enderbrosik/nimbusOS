#!/usr/bin/env python3
"""Boot the Raspberry Pi build in QEMU (raspi3b), wait, take a screenshot.
   usage: pi_shot.py seconds out.png serial.txt"""
import sys, os, socket, subprocess, time
secs, out, ser = float(sys.argv[1]), sys.argv[2], sys.argv[3]
sock = out + ".sock"
if os.path.exists(sock): os.remove(sock)
q = subprocess.Popen(["qemu-system-aarch64", "-M", "raspi3b", "-kernel", "build/pi/kernel8.elf",
                      "-serial", "file:" + ser, "-display", "none",
                      "-drive", "file=build/pi/qemu-sd.img,if=sd,format=raw",
                      "-monitor", "unix:%s,server,nowait" % sock])
try:
    time.sleep(secs)
    s = socket.socket(socket.AF_UNIX); s.connect(sock)
    s.sendall(("screendump %s.ppm\n" % out).encode()); time.sleep(1.5); s.close()
    from PIL import Image
    Image.open(out + ".ppm").save(out)
finally:
    q.terminate()
