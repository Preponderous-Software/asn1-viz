#!/usr/bin/env python3
"""Assembles numbered PNG frames into an animated GIF (Pillow).

    python3 tools/make_gif.py FRAME_DIR OUT.gif [--width 960] [--ms 90] [--hold-last 1500]
"""
import argparse
import glob

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames")
    ap.add_argument("out")
    ap.add_argument("--width", type=int, default=960)
    ap.add_argument("--ms", type=int, default=90)
    ap.add_argument("--hold-last", type=int, default=1500)
    ap.add_argument("--colors", type=int, default=128)
    a = ap.parse_args()
    paths = sorted(glob.glob(a.frames + "/frame-*.png"))
    if not paths:
        raise SystemExit("no frame-*.png in " + a.frames)
    frames = []
    for p in paths:
        im = Image.open(p).convert("RGB")
        if im.width > a.width:
            im = im.resize((a.width, im.height * a.width // im.width), Image.LANCZOS)
        frames.append(im)
    # One shared palette (from a frame in the middle) keeps the GIF small and steady.
    pal = frames[len(frames) // 2].quantize(colors=a.colors, method=Image.MEDIANCUT)
    q = [f.quantize(palette=pal, dither=Image.NONE) for f in frames]
    durations = [a.ms] * len(q)
    durations[-1] = a.hold_last
    q[0].save(a.out, save_all=True, append_images=q[1:], duration=durations, loop=0, optimize=True)


if __name__ == "__main__":
    main()
