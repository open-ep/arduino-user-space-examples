#!/usr/bin/env python3
# EXPERIMENT: Floyd-Steinberg dithering for the PixPaper 2.13" 4-color EPD.
#
# The panel has exactly 4 pixel states (black / white / yellow / red) and
# cannot mix colors per pixel. Error-diffusion dithering simulates the
# in-between colors spatially: red+yellow checker reads as orange, red+white
# as pink, black+white as gray, and so on.
#
# Output format is identical to the production png2packed_color.py
# (column-major with horizontal mirror, 31 bytes/column, 7750 bytes), so the
# generated img_packed.h drops straight into the existing pixpaper_213_color
# sketch - no firmware change.
#
# usage:
#   python3 png2packed_dither.py input.png -o img_packed.h
#   python3 png2packed_dither.py input.png --no-dither          # A/B baseline
#   python3 png2packed_dither.py input.png --preview out.png    # PC preview
#
# --preview writes a 2x-upscaled PNG of exactly what the panel will show,
# so you can judge the dithering on the PC before flashing anything.

import cv2
import numpy as np
import os
import argparse

IMG_WIDTH = 250
IMG_HEIGHT = 122
GROUPS = 31

PALETTE = [  # (r,g,b) -> 2-bit code
    ((0x00, 0x00, 0x00), 0b00),  # black
    ((0xFF, 0xFF, 0xFF), 0b01),  # white
    ((0xFF, 0xFF, 0x00), 0b10),  # yellow
    ((0xFF, 0x00, 0x00), 0b11),  # red
]
PAL_RGB = np.array([c for c, _ in PALETTE], dtype=np.int32)
PAL_CODE = [code for _, code in PALETTE]

parser = argparse.ArgumentParser(
    description="4-color EPD converter with Floyd-Steinberg dithering (experiment).")
parser.add_argument("input_png")
parser.add_argument("-o", "--output", default="img_packed.h")
parser.add_argument("--no-dither", action="store_true",
                    help="nearest-color quantization only (baseline for A/B)")
parser.add_argument("--preview", metavar="PNG",
                    help="also save a 2x preview PNG of the quantized result")
parser.add_argument("--boost", action="store_true",
                    help="boost saturation+contrast first (anime/illustration)")
parser.add_argument("--lines", action="store_true",
                    help="overlay Canny edges in black (rescues line art)")
args = parser.parse_args()

im = cv2.imread(args.input_png)
if im is None:
    raise SystemExit(f"cannot read {args.input_png}")
im = cv2.cvtColor(im, cv2.COLOR_BGR2RGB)
im = cv2.resize(im, (IMG_WIDTH, IMG_HEIGHT))

if args.boost:
    hsv = cv2.cvtColor(im, cv2.COLOR_RGB2HSV).astype(np.float32)
    # blue/cyan hues have no representable color on this panel: neutralize
    # them to equal-luminance gray so they dither into a black/white pattern
    # that keeps the shape visible (instead of vanishing into white or
    # turning into dark red noise)
    blue = (hsv[:, :, 0] > 80) & (hsv[:, :, 0] < 140) & (hsv[:, :, 1] > 40)
    hsv[:, :, 1] = np.where(blue, 0, hsv[:, :, 1])
    hsv[:, :, 1] = np.clip(hsv[:, :, 1] * 1.8, 0, 255)          # saturation
    im = cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2RGB)
    im = np.clip((im.astype(np.float32) - 128) * 1.15 + 128,
                 0, 255).astype(np.uint8)                        # contrast

edges = None
if args.lines:
    gray = cv2.cvtColor(im, cv2.COLOR_RGB2GRAY)
    edges = cv2.Canny(gray, 60, 160)
    edges = cv2.dilate(edges, np.ones((1, 1), np.uint8))

px = im.astype(np.int32)
codes = np.zeros((IMG_HEIGHT, IMG_WIDTH), dtype=np.uint8)

if args.no_dither:
    d = np.stack([((px - c) ** 2).sum(axis=2) for c in PAL_RGB])
    codes = np.array(PAL_CODE, dtype=np.uint8)[np.argmin(d, axis=0)]
else:
    # Floyd-Steinberg error diffusion, serpentine scan.
    buf = px.astype(np.float32)
    for y in range(IMG_HEIGHT):
        rng = range(IMG_WIDTH) if y % 2 == 0 else range(IMG_WIDTH - 1, -1, -1)
        sgn = 1 if y % 2 == 0 else -1
        for x in rng:
            old = buf[y, x]
            idx = int(((PAL_RGB - old) ** 2).sum(axis=1).argmin())
            codes[y, x] = PAL_CODE[idx]
            err = old - PAL_RGB[idx]
            if 0 <= x + sgn < IMG_WIDTH:
                buf[y, x + sgn] += err * (7 / 16)
            if y + 1 < IMG_HEIGHT:
                if 0 <= x - sgn < IMG_WIDTH:
                    buf[y + 1, x - sgn] += err * (3 / 16)
                buf[y + 1, x] += err * (5 / 16)
                if 0 <= x + sgn < IMG_WIDTH:
                    buf[y + 1, x + sgn] += err * (1 / 16)
    # note: buf drifts out of [0,255]; that's fine, distance still works

if edges is not None:
    codes[edges > 0] = 0b00      # force detected outlines to black

if args.preview:
    code2rgb = {code: rgb for rgb, code in PALETTE}
    out = np.zeros((IMG_HEIGHT, IMG_WIDTH, 3), np.uint8)
    for code, rgb in code2rgb.items():
        out[codes == code] = rgb
    out = cv2.resize(out, (IMG_WIDTH * 2, IMG_HEIGHT * 2),
                     interpolation=cv2.INTER_NEAREST)
    cv2.imwrite(args.preview, cv2.cvtColor(out, cv2.COLOR_RGB2BGR))
    print(f"preview: {args.preview}")

# pack: same stream order as png2packed_color.py (mirror, 31 bytes/column)
packed = []
for x in range(IMG_WIDTH):
    col = IMG_WIDTH - 1 - x
    for j in range(GROUPS):
        b = 0
        for k in range(4):
            y = j * 4 + k
            val = 0b11 if y >= IMG_HEIGHT else int(codes[y, col])
            b |= val << ((3 - k) * 2)
        packed.append(b)

name = os.path.basename(args.input_png)
mode = "nearest" if args.no_dither else "floyd-steinberg"
with open(args.output, "w", encoding="utf-8") as f:
    f.write("#ifndef IMG_PACKED_H\n#define IMG_PACKED_H\n\n#include <stdint.h>\n\n")
    f.write(f"// Packed 2bpp 4-color image data for: {name} ({mode})\n")
    f.write(f"// {IMG_WIDTH} x {IMG_HEIGHT}, column-major (mirrored), 31 bytes/column\n")
    f.write("// 00=black 01=white 10=yellow 11=red\n")
    f.write(f"const uint8_t img_packed[{len(packed)}] = {{\n")
    for i, byte in enumerate(packed):
        if i % 12 == 0:
            f.write("    ")
        f.write(f"0x{byte:02X}, ")
        if (i + 1) % 12 == 0:
            f.write("\n")
    f.write("\n};\n\n#endif // IMG_PACKED_H\n")

print(f"OK: {args.output} ({len(packed)} bytes, {mode})")
