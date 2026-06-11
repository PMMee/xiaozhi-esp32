#!/usr/bin/env python3
"""
Rotate GIFs 90 degrees CCW and compress by dropping frames + reducing colors.

Usage:
    python scripts/rotate_compress_gifs.py -i left/ -o left_rotated/
    python scripts/rotate_compress_gifs.py -i left/ -o left/ --inplace
"""

import os
import sys
import argparse
from PIL import Image, ImageSequence


def rotate_and_compress(input_path, output_path, frame_skip=2, resize_pct=None):
    """Rotate 90 CCW, drop frames, optionally resize."""
    orig_size = os.path.getsize(input_path)

    with Image.open(input_path) as im:
        if im.format != 'GIF':
            print(f"  SKIP: {input_path}")
            return (orig_size, 0)

        frames = []
        durations = []
        idx = 0

        for frame in ImageSequence.Iterator(im):
            if idx % frame_skip != 0:
                idx += 1
                continue
            idx += 1

            # Rotate 90 CCW using original palette
            rotated = frame.rotate(90, expand=False)
            if resize_pct and resize_pct < 100:
                w, h = rotated.size
                nw = int(w * resize_pct / 100)
                nh = int(h * resize_pct / 100)
                rotated = rotated.resize((nw, nh), Image.BICUBIC)

            frames.append(rotated)
            try:
                dur = frame.info.get('duration', 100) or 100
            except (KeyError, AttributeError):
                dur = 100
            durations.append(dur * frame_skip)

        if not frames:
            return (orig_size, 0)

        # Save with original palette preserved per-frame
        disposal = 2  # restore to background
        frames[0].save(
            output_path,
            save_all=True,
            append_images=frames[1:],
            duration=durations,
            loop=0,
            disposal=disposal,
            optimize=False,
        )

    new_size = os.path.getsize(output_path)
    return (orig_size, new_size)


def main():
    p = argparse.ArgumentParser(description='Rotate GIFs 90 deg CCW + compress')
    p.add_argument('-i', '--input', required=True, help='Input dir with GIFs')
    p.add_argument('-o', '--output', required=True, help='Output dir')
    p.add_argument('-f', '--frame-skip', type=int, default=2,
                   help='Keep every Nth frame (default: 2)')
    p.add_argument('-r', '--resize', type=int, default=None,
                   help='Resize to N%% of original (e.g. 80)')
    p.add_argument('--inplace', action='store_true',
                   help='Overwrite input files')
    args = p.parse_args()

    if not os.path.isdir(args.input):
        print(f"ERROR: {args.input} not found")
        sys.exit(1)

    os.makedirs(args.output, exist_ok=True)

    gifs = sorted(f for f in os.listdir(args.input) if f.lower().endswith('.gif'))
    if not gifs:
        print(f"No GIFs in {args.input}")
        return

    print(f"{len(gifs)} GIFs | rotate=90CCW | frame_skip={args.frame_skip}x",
          f"| resize={args.resize}%" if args.resize else "")
    print(f"{'='*60}")

    t_orig = t_new = 0
    for g in gifs:
        inp = os.path.join(args.input, g)
        out = os.path.join(args.output, g)
        o, n = rotate_and_compress(inp, out, args.frame_skip, args.resize)
        t_orig += o
        t_new += n
        if n:
            pct = (1 - n / o) * 100
            print(f"  {g}: {o//1024}KB -> {n//1024}KB ({pct:+.0f}%)")

    print(f"{'='*60}")
    pct = (1 - t_new / t_orig) * 100 if t_orig else 0
    print(f"TOTAL: {t_orig//1024}KB -> {t_new//1024}KB ({pct:+.0f}%)")

    if not args.inplace:
        s = os.path.basename(sys.argv[0])
        print(f"\nApply: python scripts/{s} -i {args.input} "
              f"-o {args.input} --inplace")


if __name__ == '__main__':
    main()
