# The pico-c64 screensaver image: a C64-style start screen, 160x124, in the C64's own
# character set (8x8, unscaled). Writes the .c arrays the menu uses (DefaultSS444.c,
# DefaultSS555.c), the raw .444/.555 images and PNGs.
#
#   python3 tools/make_screensaver.py <c64chargen.rom> <output dir>
#
# The character ROM is not part of the repository.
import struct, sys, zlib
chargen_path, outdir = sys.argv[1], sys.argv[2]
chargen = open(chargen_path, "rb").read()
W, H = 160, 124
PAL = {6: (0x35, 0x28, 0x79), 14: (0x6C, 0x5E, 0xB5)}     # Pepto: blue, light blue
BORDER, BG, FG = 14, 6, 14
LEFT, TOP = 8, 10                                         # border; the screen is 18 x 13 characters
lines = {1: "**** PICO-C64 ****", 3: "COMMODORE 64 FOR", 4: "RP2350", 6: "READY."}
center = {3, 4}

def screen_code(c):
    if "A" <= c <= "Z":
        return ord(c) - 64
    return ord(c)                                         # space, digits, * - .

img = [[BORDER] * W for _ in range(H)]
for y in range(TOP, H - TOP):
    for x in range(LEFT, W - LEFT):
        img[y][x] = BG

def put(col, row, code):
    for r in range(8):
        bits = chargen[code * 8 + r]
        for b in range(8):
            if bits & (0x80 >> b):
                img[TOP + row * 8 + r][LEFT + col * 8 + b] = FG

for row, text in lines.items():
    col = (18 - len(text)) // 2 if row in center else 0
    for i, c in enumerate(text):
        put(col + i, row, screen_code(c))
put(0, 7, 160)                                            # the cursor: reverse space

def pixels(fmt):
    out = bytearray(struct.pack("<HH", W, H))
    for row in img:
        for c in row:
            r, g, b = PAL[c]
            v = ((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)) if fmt == "555" else ((r >> 4) << 8 | (g >> 4) << 4 | (b >> 4))
            out += struct.pack("<H", v)
    return bytes(out)

for fmt in ("444", "555"):
    data = pixels(fmt)
    open(f"{outdir}/c64_screensaver.{fmt}", "wb").write(data)
    name = f"DefaultSS160_{fmt}"
    with open(f"{outdir}/DefaultSS{fmt}.c", "w") as f:
        f.write(f"const unsigned char {name}[] = {{\n")
        for i in range(0, len(data), 12):
            f.write("  " + ", ".join(f"0x{b:02x}" for b in data[i:i + 12]) + ",\n")
        f.write("};\n\n")
        f.write(f"const unsigned int {name}_len = {len(data)};\n")

def png(path, scale):
    raw = b"".join(b"\0" + b"".join(bytes(PAL[c]) * scale for c in row) for row in img for _ in range(scale))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W * scale, H * scale, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
png(f"{outdir}/c64_screensaver.png", 1)
png(f"{outdir}/c64_screensaver_x4.png", 4)
print("written", len(pixels("555")), "bytes per image")
