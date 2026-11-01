#!/usr/bin/env python3
"""
make_bootloader_art.py - the picker artwork for pico-bootLoader (themes 0 and 1).

    python3 tools/make_bootloader_art.py <c64chargen.rom> <out dir> <shot1.ppm> <shot2.ppm> <shot3.ppm>

Theme 0 (640x480, the flat style of the other computers' tiles): the C64's blues,
a keyboard, a small machine card and the name in the C64's own character set.
Theme 1 (320x240, the collage style): three slices of game screens - PPM screenshots
from hosttest/c64_host (800x600) - with the name over them.

Writes c64_0.png and c64_1.png; pico-bootLoader's tools/png2raw.py makes the .444 and
.555 caches. The character ROM is not part of the repository.
"""
import struct
import sys
import zlib

chargen = open(sys.argv[1], "rb").read()
out = sys.argv[2]
shots = sys.argv[3:6]

BLUE = (0x35, 0x28, 0x79)
LIGHT_BLUE = (0x6C, 0x5E, 0xB5)
KEY = (0x47, 0x3A, 0x95)
WHITE = (0xFF, 0xFF, 0xFF)
BLACK = (0x00, 0x00, 0x00)


def new(w, h, c):
    return [[c] * w for _ in range(h)]


def rect(img, x0, y0, x1, y1, c, r=0):
    """Filled rectangle [x0, x1) x [y0, y1), corners rounded by r"""
    for y in range(max(y0, 0), min(y1, len(img))):
        for x in range(max(x0, 0), min(x1, len(img[0]))):
            dx = max(x0 + r - x - 1, x - (x1 - r), 0)
            dy = max(y0 + r - y - 1, y - (y1 - r), 0)
            if r == 0 or dx * dx + dy * dy <= r * r:
                img[y][x] = c


def screen_code(ch):
    if "A" <= ch <= "Z":
        return ord(ch) - 64
    return ord(ch)                      # space, digits, / -


def text(img, x, y, s, c, scale):
    for i, ch in enumerate(s):
        code = screen_code(ch)
        for r in range(8):
            bits = chargen[code * 8 + r]
            for b in range(8):
                if bits & (0x80 >> b):
                    rect(img, x + (i * 8 + b) * scale, y + r * scale,
                         x + (i * 8 + b + 1) * scale, y + (r + 1) * scale, c)


def outlined(img, x, y, s, scale, width):
    for dx in range(-width, width + 1):
        for dy in range(-width, width + 1):
            text(img, x + dx, y + dy, s, BLACK, scale)
    text(img, x, y, s, WHITE, scale)


def write_png(path, img):
    h, w = len(img), len(img[0])
    raw = b"".join(b"\0" + b"".join(bytes(p) for p in row) for row in img)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    return [[tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) for x in range(w)] for y in range(h)]


# ---- Theme 0: 640x480 ----
img = new(640, 480, BLUE)
for y in range(0, 120):                 # a lighter band across the top, sloping down to the left
    for x in range(640):
        if y < 108 - x * 0.06:
            img[y][x] = LIGHT_BLUE
rect(img, 460, 92, 660, 156, LIGHT_BLUE, 26)
for row in range(4):                    # the keyboard
    y = 270 + row * 46
    for k in range(13):
        x = 16 + row * 10 + k * 46
        rect(img, x, y, x + 40, y + 40, KEY, 6)
rect(img, 160, 454, 470, 494, KEY, 6)   # the space bar
rect(img, 0, 188, 92, 292, WHITE)       # the machine card
text(img, 22, 196, "C64", BLUE, 2)
for row in range(3):
    for k in range(9):
        rect(img, 6 + k * 9, 222 + row * 9, 13 + k * 9, 228 + row * 9, BLUE)
rect(img, 4, 254, 88, 286, LIGHT_BLUE)
text(img, 14, 262, "64", WHITE, 2)
title = "COMMODORE 64"
tx = 118
text(img, tx, 214, title, WHITE, 4)
rect(img, 94, 229, 110, 232, WHITE)
rect(img, tx + len(title) * 32 + 8, 229, 638, 232, WHITE)
write_png(f"{out}/c64_0.png", img)

# ---- Theme 1: 320x240, three slices of game screens ----
img = new(320, 240, BLACK)
for i, path in enumerate(shots):
    shot = read_ppm(path)               # 800x600: the VIC's 400x300 window doubled
    for y in range(240):                # the part the board shows: from window x 40, y 23
        for x in range(106):
            img[y][i * 107 + x] = shot[(23 + y) * 2][(40 + 107 * i + x) * 2]
outlined(img, 160 - 3 * 24, 96, "C64", 6, 3)
write_png(f"{out}/c64_1.png", img)
print("written", f"{out}/c64_0.png", f"{out}/c64_1.png")
