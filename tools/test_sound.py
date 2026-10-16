#!/usr/bin/env python3
"""Sound test: QEMU's AC'97 output is captured to a WAV file and analysed on
the host (Goertzel filter, pure Python): the startup chime must contain its
C-E-G-C notes, the Media Player must play the sample tune, mute must silence."""
import sys, os, time, shutil, struct, math, subprocess
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

def ic(c, r): return (6 + c * 84 + 42, 6 + r * 86 + 22)
S = "build/test"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
IMG = f"{S}/sound.img"
WAV = f"{S}/sound-capture.wav"
shutil.copy("build/nimbus.img", IMG)

def read_wav(path):
    d = open(path, "rb").read()
    rate = struct.unpack_from("<I", d, 24)[0]
    ch = struct.unpack_from("<H", d, 22)[0]
    pos = d.find(b"data") + 8
    n = (len(d) - pos) // 2
    s = struct.unpack_from("<%dh" % n, d, pos)
    mono = [(s[i] + s[i + 1]) / 2 for i in range(0, n - 1, ch)] if ch == 2 else list(s)
    return rate, mono

def goertzel(x, rate, f):
    k = 2 * math.cos(2 * math.pi * f / rate)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + k * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2)) / len(x)

def loud_region(x, rate, thresh=300):
    win = rate // 50
    for i in range(0, len(x) - win, win):
        if max(abs(v) for v in x[i:i + win]) > thresh:
            return i
    return -1

with Qemu([IMG], name="sound", audio_wav=WAV) as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    assert "AC'97 at I/O" in q.serial(), "AC97 not detected"
    q.wait_serial(r"\[snd \] event 0", 5)            # startup chime
    time.sleep(2.0)
    q.mouse_home()
    q.double_click(*ic(0, 3))
    q.wait_serial(r'created: "Terminal"', 5)
    q.type("play \"/Music/Ode to Joy.wav\"\n")
    q.wait_serial(r"loaded /Music/Ode to Joy.wav: \d+ frames, 22050 Hz, 1 ch", 5)
    q.wait_serial(r"\[play\] playing", 5)
    time.sleep(3.0)
    q.screenshot(f"{S}/sound-player.png")
    q.cmd("stop")                                     # pause the VM: closes the capture cleanly
q_rate, x = read_wav(WAV)
start = loud_region(x, q_rate)
assert start >= 0, "capture is silent"
chime = x[start:start + int(q_rate * 1.2)]
on = {f: goertzel(chime, q_rate, f) for f in (523.25, 659.25, 783.99, 1046.5)}
off = {f: goertzel(chime, q_rate, f) for f in (450.0, 600.0, 720.0, 900.0)}
print("chime notes:", {k: round(v) for k, v in on.items()}, "off-key:", {k: round(v) for k, v in off.items()})
assert min(on.values()) > 4 * max(off.values()), "startup chime notes not found"
# the tune starts later: look for E4 / G4 energy after the chime
tune = x[start + int(q_rate * 2.5):]
e4, g4, offk = goertzel(tune, q_rate, 329.63), goertzel(tune, q_rate, 392.0), goertzel(tune, q_rate, 370.0)
print("tune: E4 %.0f G4 %.0f off %.0f" % (e4, g4, offk))
assert e4 > 3 * offk and g4 > 3 * offk, "melody not found in the capture"

# mute via the tray speaker: no audio after that
WAV2 = f"{S}/sound-muted.wav"
with Qemu([IMG], name="sound-mute", audio_wav=WAV2) as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    time.sleep(1.8)
    q.mouse_home()
    q.click(1024 - 92 - 76 + 58, 768 - 18)            # speaker icon in the tray
    q.wait_serial(r"sound muted", 5)
    time.sleep(0.5)
    q.type("")
    q.double_click(*ic(1, 1))                         # Media Player (column 1, row 1)
    q.wait_serial(r'created: "Media Player"', 5)
    q.key("meta_l"); q.key("esc")
    time.sleep(0.3)
    q.cmd("stop")
r2, y = read_wav(WAV2)
tail = y[-int(r2 * 0.5):]
assert max(abs(v) for v in tail) < 50, "audio not muted"
print("sound: PASS")
