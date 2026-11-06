"""Contact sheet of dumped BMP frames (or PNG frames kept by drive_match.py), and how much each differs from the last.

    python -I tools/sheet.py <folder or prefix> <out.png> [--every N] [--cols C] [--scale S]

Prints each frame's mean absolute difference from the one before, so a flicker
shows as a spike without having to look at every frame.
"""
import argparse
import glob
import os

import numpy as np
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--every", type=int, default=1)
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--scale", type=float, default=0.5)
    ap.add_argument("--max", type=int, default=48)
    ap.add_argument("--last", type=int, default=0, help="only the last N frames")
    args = ap.parse_args()

    # Dumps are BMP; frames kept by tools/drive_match.py are PNG.
    pre = os.path.join(args.src, "") if os.path.isdir(args.src) else args.src
    files = sorted(glob.glob(pre + "*.bmp") + glob.glob(pre + "*.png"))
    if args.last:
        files = files[-args.last:]
    prev = None
    diffs = []
    for f in files:
        a = np.asarray(Image.open(f).convert("L").resize((160, 120))).astype(np.float32)
        diffs.append(0.0 if prev is None else float(np.abs(a - prev).mean()))
        prev = a
    print("frames", len(files))
    if diffs:
        print("diff mean %.2f max %.2f" % (np.mean(diffs), np.max(diffs)))
        print(" ".join("%.1f" % d for d in diffs))
    chosen = files[::args.every][:args.max]
    if not chosen:
        return
    w, h = Image.open(chosen[0]).size
    w, h = int(w * args.scale), int(h * args.scale)
    rows = (len(chosen) + args.cols - 1) // args.cols
    sheet = Image.new("RGB", (w * args.cols, h * rows))
    for i, f in enumerate(chosen):
        sheet.paste(Image.open(f).convert("RGB").resize((w, h)),
                    ((i % args.cols) * w, (i // args.cols) * h))
    sheet.save(args.out)


if __name__ == "__main__":
    main()
