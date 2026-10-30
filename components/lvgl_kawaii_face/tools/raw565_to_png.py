#!/usr/bin/env python3
"""Convert raw little-endian RGB565 dumps (from tools/host_render) to PNG.

Usage: python3 raw565_to_png.py out/*.raw
Each .raw needs its width/height; they are read from the sibling facehost stdout
when available, otherwise pass WxH explicitly:
    python3 raw565_to_png.py out/mouth.raw 144x121
"""
import sys
import struct

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: python3 -m pip install pillow")

# canvas geometry produced by tools/host_render/main.c (S=320 panel)
GEOMETRY = {
    "eye_left": (144, 144),
    "eye_right": (144, 144),
    "mouth": (144, 121),
}


def rgb565_to_png(path, w, h, scale=3):
    raw = open(path, "rb").read()
    need = w * h * 2
    if len(raw) < need:
        sys.exit(f"{path}: {len(raw)} bytes, need {need} for {w}x{h} RGB565")
    px = bytearray()
    for i in range(w * h):
        v = struct.unpack_from("<H", raw, i * 2)[0]
        px.append(((v >> 11) & 0x1F) * 255 // 31)  # R5 -> R8
        px.append(((v >> 5) & 0x3F) * 255 // 63)   # G6 -> G8
        px.append((v & 0x1F) * 255 // 31)          # B5 -> B8
    im = Image.frombytes("RGB", (w, h), bytes(px))
    out = path.rsplit(".", 1)[0] + ".png"
    im.resize((w * scale, h * scale), Image.NEAREST).save(out)
    # quick sanity report: dominant colours
    from collections import Counter
    top = Counter(im.getdata()).most_common(5)
    print(f"{out}: {w}x{h}  dominant={top}")
    return out


def main(argv):
    if not argv:
        sys.exit(__doc__)
    for path in argv:
        name = path.rsplit("/", 1)[-1].rsplit(".", 1)[0]
        if len(argv) == 2 and "x" in argv[1]:
            w, h = (int(x) for x in argv[1].split("x"))
        elif name in GEOMETRY:
            w, h = GEOMETRY[name]
        else:
            sys.exit(f"unknown geometry for {path}; pass WxH as the second argument")
        rgb565_to_png(path, w, h)


if __name__ == "__main__":
    main(sys.argv[1:])
