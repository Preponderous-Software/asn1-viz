#!/usr/bin/env python3
"""A flat 2D picture of an asn1-layout/1 file: each lane as rows of bits,
coloured by segment kind, inferred segments hatched. The RFC's 2D fallback
for a quick look or a document; the 3D viewer is app/main.cpp.

    python3 tools/layout_png.py LAYOUT.json OUT.png [--row-bits 64] [--cell 10]

Needs Pillow.
"""
import argparse
import json

from PIL import Image, ImageDraw

# One colour per kind, shared with the 3D viewer (src/scene/strip.cpp).
KIND_COLORS = {
    "value": (86, 156, 214),
    "presence-bit": (240, 200, 60),
    "presence-bitmap": (240, 200, 60),
    "undecoded": (150, 20, 20),
    "open-type-unknown": (180, 70, 90),
    "extension-bit": (240, 120, 40),
    "extension-bitmap": (240, 150, 60),
    "extension-bitmap-length": (200, 110, 50),
    "choice-index": (200, 120, 220),
    "length": (90, 200, 120),
    "count": (60, 170, 150),
    "normally-small-length": (120, 210, 90),
    "normally-small-number": (120, 210, 90),
    "alignment": (110, 110, 120),
    "open-type-length": (230, 90, 120),
    "open-type-padding": (150, 80, 100),
    "trailing-padding": (70, 70, 80),
    "structural": (200, 200, 200),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("layout")
    ap.add_argument("out")
    ap.add_argument("--row-bits", type=int, default=64)
    ap.add_argument("--cell", type=int, default=10)
    a = ap.parse_args()
    doc = json.load(open(a.layout, encoding="utf-8"))
    lanes = doc["lanes"]
    rows = max((lane["bits"] + a.row_bits - 1) // a.row_bits for lane in lanes)
    lane_w = a.row_bits * a.cell
    gap = 3 * a.cell
    img = Image.new("RGB", (len(lanes) * lane_w + (len(lanes) + 1) * gap, rows * a.cell + 2 * gap + 12), (10, 12, 18))
    d = ImageDraw.Draw(img)
    for li, lane in enumerate(lanes):
        x0 = gap + li * (lane_w + gap)
        d.text((x0, 4), f"{lane['codec'].upper()}  {lane['encodedBits']} bits", fill=(220, 225, 235))
        data = bytes.fromhex(lane["hex"])
        for s in lane["segments"]:
            color = KIND_COLORS.get(s["kind"], (255, 0, 255))
            for b in range(s["start"], s["start"] + s["len"]):
                r, c = divmod(b, a.row_bits)
                x, y = x0 + c * a.cell, gap + r * a.cell
                bit = (data[b // 8] >> (7 - b % 8)) & 1
                shade = 1.0 if bit else 0.55
                fill = tuple(int(v * shade) for v in color)
                d.rectangle([x, y, x + a.cell - 2, y + a.cell - 2], fill=fill)
                if s["source"] == "inferred":
                    d.line([x, y + a.cell - 2, x + a.cell - 2, y], fill=(20, 20, 20))
    img.save(a.out)


if __name__ == "__main__":
    main()
