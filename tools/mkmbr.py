#!/usr/bin/env python3
"""Write an MBR with one partition (from sector 2048 to the end) into an image.
   usage: mkmbr.py image type_hex"""
import sys, os, struct
path, ptype = sys.argv[1], int(sys.argv[2], 16)
sectors = os.path.getsize(path) // 512
with open(path, "r+b") as f:
    f.seek(446)
    f.write(bytes([0, 0, 0, 0, ptype, 0, 0, 0]) + struct.pack("<II", 2048, sectors - 2048))
    f.seek(510)
    f.write(b"\x55\xaa")
