import cv2
import numpy as np
import os
import argparse

# Converts a PNG into a packed 2bpp C array for the PixPaper 2.13" 4-color EPD,
# pre-packed in the controller write order so a small MCU (CH32V003, 2KB SRAM)
# can stream it from flash with no framebuffer.
#
# Byte stream matches epd_write_img() in
# nxp/user-space-examples/2.13/color/spi/pixpaper-213-c-test-frdm-imx93.c:
#   for x in 0..249:            # stream column
#     for j in 0..31:           # 4 pixels per byte, rows y = j*4 .. j*4+3
#       byte = p(y0)<<6 | p(y1)<<4 | p(y2)<<2 | p(y3)
# with the horizontal mirror from png2epd.py (stream col x = image col 249-x)
# and out-of-range pixels (y >= 122) = 0b11, same as the reference.
#
# Colors (from png2epd.py): 00=black, 01=white, 10=yellow, 11=red.
# Pixels are quantized to the nearest of the four (png2epd.py required exact
# matches; nearest-color is friendlier to real images).

parser = argparse.ArgumentParser(
    description="Convert a PNG into a packed 2bpp C const array for the 2.13\" 4-color EPD.")
parser.add_argument("input_png", type=str, help="The PNG file to convert.")
parser.add_argument("-o", "--output", type=str, default="img_packed.h",
                    help="Output header file (default: img_packed.h)")
args = parser.parse_args()

IMG_WIDTH = 250
IMG_HEIGHT = 122
GROUPS = 31  # 31 bytes/column = rows 0-123 (122 used, 122-123 padded 0b11);
             # rows 124-127 are a constant 0xFF byte the sketch writes itself

PALETTE = [  # (r,g,b) -> 2-bit code
    ((0x00, 0x00, 0x00), 0b00),  # black
    ((0xFF, 0xFF, 0xFF), 0b01),  # white
    ((0xFF, 0xFF, 0x00), 0b10),  # yellow
    ((0xFF, 0x00, 0x00), 0b11),  # red
]

im = cv2.imread(args.input_png)
if im is None:
    print(f"Error: cannot read '{args.input_png}'")
    exit(1)

im = cv2.cvtColor(im, cv2.COLOR_BGR2RGB)
im = cv2.resize(im, (IMG_WIDTH, IMG_HEIGHT))

# quantize every pixel to the nearest palette color
px = im.astype(np.int32)
dists = np.stack([((px - np.array(c)) ** 2).sum(axis=2) for c, _ in PALETTE])
codes = np.array([code for _, code in PALETTE], dtype=np.uint8)[np.argmin(dists, axis=0)]

packed = []
for x in range(IMG_WIDTH):
    col = IMG_WIDTH - 1 - x  # horizontal mirror, as in png2epd.py
    for j in range(GROUPS):
        b = 0
        for k in range(4):
            y = j * 4 + k
            val = 0b11 if y >= IMG_HEIGHT else int(codes[y, col])
            b |= val << ((3 - k) * 2)
        packed.append(b)

name = os.path.basename(args.input_png)
with open(args.output, "w", encoding="utf-8") as f:
    f.write("#ifndef IMG_PACKED_H\n")
    f.write("#define IMG_PACKED_H\n\n")
    f.write("#include <stdint.h>\n\n")
    f.write(f"// Packed 2bpp 4-color image data for: {name}\n")
    f.write(f"// {IMG_WIDTH} x {IMG_HEIGHT}, column-major (mirrored), 32 bytes/column\n")
    f.write("// 00=black 01=white 10=yellow 11=red\n")
    f.write(f"const uint8_t img_packed[{len(packed)}] = {{\n")
    for i, byte in enumerate(packed):
        if i % 12 == 0:
            f.write("    ")
        f.write(f"0x{byte:02X}, ")
        if (i + 1) % 12 == 0:
            f.write("\n")
    f.write("\n};\n\n")
    f.write("#endif // IMG_PACKED_H\n")

print(f"OK: {args.output} ({len(packed)} bytes)")
