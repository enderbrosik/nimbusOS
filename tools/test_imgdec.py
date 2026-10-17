#!/usr/bin/env python3
"""Picture decoder test (host side): gui/imgdec.c is compiled with small shim headers and run on
files made by Pillow.  PNG and GIF must match Pillow's pixels exactly (transparency over white);
JPEG is allowed a small error, because decoders round differently."""
import sys, os, subprocess, io, random
from PIL import Image

T = "build/test/img"
os.makedirs(T, exist_ok=True)
subprocess.check_call(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Itools/hostshim", "-I.",
                       "gui/imgdec.c", "tools/imgtest.c", "-o", f"{T}/imgtest"])

random.seed(7)
def picture(w, h):
    """a test picture with gradients, hard edges and some noise"""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            r, g, b = x * 255 // max(1, w - 1), y * 255 // max(1, h - 1), (x * y) % 256
            if (x // 8 + y // 8) % 2: r, g, b = 255 - r, 255 - g, 255 - b
            if 10 < x < 30 and 5 < y < 20: r, g, b = 200, 30, 30
            px[x, y] = (r, g, b)
    return im

def smooth(w, h):
    """photo-like: smooth colour changes (hard chroma edges make decoders differ legitimately)"""
    import math
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (int(127 + 120 * math.sin(x / 23.0)), int(127 + 120 * math.cos(y / 17.0)), (x + y) * 255 // (w + h))
    return im

def decode(path):
    out = f"{path}.rgb"
    subprocess.check_call([f"{T}/imgtest", path, out])
    data = open(out, "rb").read()
    head, rest = data.split(b"\n", 1)
    w, h = map(int, head.split())
    return w, h, rest

def over_white(im):
    im = im.convert("RGBA")
    bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
    return Image.alpha_composite(bg, im).convert("RGB")

def check_exact(name, im_file, expect, tol=0):
    """tol 1 for partly transparent pictures: blending over white rounds differently by one level"""
    w, h, rgb = decode(im_file)
    assert (w, h) == expect.size, (name, (w, h), expect.size)
    e = expect.tobytes()
    worst = max(abs(a - b) for a, b in zip(rgb, e))
    if worst > tol:
        bad = sum(1 for i in range(0, len(rgb), 3) if rgb[i:i+3] != e[i:i+3])
        raise AssertionError(f"{name}: {bad} of {w*h} pixels differ (worst {worst})")
    print(f"  {name}: " + ("exact" if worst == 0 else f"within {worst}"))

def check_close(name, im_file, expect, mean_max, max_max):
    w, h, rgb = decode(im_file)
    assert (w, h) == expect.size, (name, (w, h), expect.size)
    e = expect.tobytes()
    diffs = [abs(a - b) for a, b in zip(rgb, e)]
    mean = sum(diffs) / len(diffs)
    assert mean <= mean_max and max(diffs) <= max_max, f"{name}: mean error {mean:.2f}, max {max(diffs)}"
    print(f"  {name}: mean error {mean:.2f}, max {max(diffs)}")

base = picture(97, 61)        # odd sizes on purpose
print("PNG")
for name, im in [("rgb", base), ("gray", base.convert("L")), ("palette", base.convert("P", palette=Image.ADAPTIVE, colors=32)),
                 ("1-bit", base.convert("1")), ("rgba", None)]:
    if im is None:
        im = base.convert("RGBA")
        a = Image.linear_gradient("L").resize(base.size)
        im.putalpha(a)
    f = f"{T}/{name}.png"
    im.save(f)
    check_exact("png " + name, f, over_white(im), tol=1)
la = base.convert("LA"); la.save(f"{T}/la.png"); check_exact("png gray+alpha", f"{T}/la.png", over_white(la), tol=1)
pt = base.convert("P", palette=Image.ADAPTIVE, colors=16); pt.info["transparency"] = 3; pt.save(f"{T}/ptrans.png", transparency=3)
check_exact("png palette+transparency", f"{T}/ptrans.png", over_white(Image.open(f"{T}/ptrans.png")), tol=1)
big = picture(300, 200); big.save(f"{T}/big.png", compress_level=9); check_exact("png large (dynamic Huffman)", f"{T}/big.png", big)
big.save(f"{T}/stored.png", compress_level=0); check_exact("png stored blocks", f"{T}/stored.png", big)

print("GIF")
g = base.convert("P", palette=Image.ADAPTIVE, colors=64)
g.save(f"{T}/a.gif"); check_exact("gif", f"{T}/a.gif", over_white(Image.open(f"{T}/a.gif")))
g.save(f"{T}/t.gif", transparency=5); check_exact("gif transparency", f"{T}/t.gif", over_white(Image.open(f"{T}/t.gif")))
g.save(f"{T}/i.gif", interlace=True); check_exact("gif interlaced", f"{T}/i.gif", over_white(Image.open(f"{T}/i.gif")))
big.convert("P", palette=Image.ADAPTIVE, colors=256).save(f"{T}/big.gif")
check_exact("gif 256 colours", f"{T}/big.gif", over_white(Image.open(f"{T}/big.gif")))

print("JPEG")
photo = smooth(300, 200)
for sub, label, mean_max, max_max in [(0, "4:4:4", 1.0, 5), (1, "4:2:2", 1.5, 8), (2, "4:2:0", 2.2, 10)]:
    f = f"{T}/s{sub}.jpg"
    photo.save(f, quality=90, subsampling=sub)
    check_close(f"jpeg {label}", f, Image.open(f).convert("RGB"), mean_max, max_max)
photo.convert("L").save(f"{T}/gray.jpg", quality=85)
check_close("jpeg gray", f"{T}/gray.jpg", Image.open(f"{T}/gray.jpg").convert("RGB"), 1.0, 6)
smooth(97, 61).save(f"{T}/odd.jpg", quality=75, subsampling=2)
check_close("jpeg odd size 97x61", f"{T}/odd.jpg", Image.open(f"{T}/odd.jpg").convert("RGB"), 2.5, 12)

print("unsupported and damaged data")
prog = f"{T}/prog.jpg"; photo.save(prog, quality=80, progressive=True)
assert subprocess.call([f"{T}/imgtest", prog, prog + ".rgb"], stderr=subprocess.DEVNULL) == 1; print("  progressive jpeg: refused, no crash")
for name in ["rgb.png", "a.gif", "s2.jpg"]:
    data = open(f"{T}/{name}", "rb").read()
    for cut in (len(data) // 2, 20):
        f = f"{T}/cut-{cut}-{name}"; open(f, "wb").write(data[:cut])
        rc = subprocess.call([f"{T}/imgtest", f, f + ".rgb"], stderr=subprocess.DEVNULL)
        assert rc in (0, 1), (name, cut, rc)              # a clean failure or a partial picture, never a crash
print("  truncated files: no crash")
open(f"{T}/junk.png", "wb").write(b"\x89PNG\r\n\x1a\n" + bytes(random.randrange(256) for _ in range(500)))
assert subprocess.call([f"{T}/imgtest", f"{T}/junk.png", f"{T}/junk.rgb"], stderr=subprocess.DEVNULL) in (0, 1)
print("imgdec: PASS")
