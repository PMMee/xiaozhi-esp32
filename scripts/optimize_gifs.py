#!/usr/bin/env python3
"""
Rotate left/ GIFs 90 deg CCW for ESP32 display.

Default: rotate-only (preserves original quality)
Optimize: gifsicle -O3 --lossy=30 --colors=64

Output to left_rotated/ — original left/ never modified.

Usage:
    python scripts/optimize_gifs.py            # rotate only -> left_rotated/
    python scripts/optimize_gifs.py --optimize # rotate + compress
"""

import os, sys, subprocess, shutil

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
INPUT_DIR = os.path.join(PROJECT_DIR, "left")
OUTPUT_DIR = os.path.join(PROJECT_DIR, "left_rotated")


def find_gifsicle():
    """Search for gifsicle.exe in project dir and PATH."""
    for root, _, files in os.walk(os.path.join(PROJECT_DIR, "gifsicle")):
        if "gifsicle.exe" in files:
            return os.path.join(root, "gifsicle.exe")
    return shutil.which("gifsicle")


def rotate_gifsicle(src, dst, optimize=False):
    cmd = [find_gifsicle(), "--rotate-90"]
    if optimize:
        cmd += ["-O3", "--lossy=30", "--colors=64"]
    cmd += ["--no-warnings", src, "-o", dst]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    return r.returncode == 0


def rotate_pillow(src, dst):
    try:
        from PIL import Image, ImageSequence
    except ImportError:
        print("  ERROR: Pillow not installed. pip install Pillow")
        return False
    with Image.open(src) as im:
        if im.format != 'GIF':
            return False
        frames, durations = [], []
        for frame in ImageSequence.Iterator(im):
            frames.append(frame.rotate(90, expand=False))
            durations.append(frame.info.get('duration', 100) or 100)
        if not frames:
            return False
        frames[0].save(dst, save_all=True, append_images=frames[1:],
                       duration=durations, loop=0, disposal=2)
        return True


def main():
    optimize = "--optimize" in sys.argv
    gifsicle = find_gifsicle()
    engine = "gifsicle" + (" (optimize)" if optimize else " (rotate-only)")
    if not gifsicle:
        engine = "Pillow (rotate-only)"

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    src_files = sorted(f for f in os.listdir(INPUT_DIR)
                       if f.lower().endswith('.gif'))
    if not src_files:
        print(f"No GIFs in {INPUT_DIR}")
        return

    print(f"{len(src_files)} GIFs | engine: {engine}")
    total_in = total_out = 0

    for f in src_files:
        src = os.path.join(INPUT_DIR, f)
        dst = os.path.join(OUTPUT_DIR, f)
        sz_in = os.path.getsize(src)

        # Skip if output is newer than input
        if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            sz_out = os.path.getsize(dst)
            total_in += sz_in
            total_out += sz_out
            continue

        ok = False
        if gifsicle:
            ok = rotate_gifsicle(src, dst, optimize)
        else:
            ok = rotate_pillow(src, dst)

        sz_out = os.path.getsize(dst) if ok else sz_in
        total_in += sz_in
        total_out += sz_out
        pct = (1 - sz_out / sz_in) * 100 if ok else 0
        status = f"-> {sz_out//1024}KB ({pct:+.0f}%)" if ok else "FAILED"
        print(f"  {f}: {sz_in//1024}KB {status}")

    tp = (1 - total_out / total_in) * 100 if total_in else 0
    print(f"  Total: {total_in//1024}KB -> {total_out//1024}KB ({tp:+.0f}%)")
    print(f"  Output: {OUTPUT_DIR}")


if __name__ == '__main__':
    main()
