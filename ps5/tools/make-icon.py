#!/usr/bin/env python3
"""Draws the PS5 launcher icon (sce_sys/icon0.png, 512x512): a stitched teddy head over a
dark gradient, with "RECOMP" below. Original art, drawn here, so the title carries none of
the game's own artwork.

usage: make-icon.py OUT.png
"""
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SIZE = 512
SCALE = 2  # drawn at twice the size, then reduced, for smooth edges
S = SIZE * SCALE


def gradient():
    image = Image.new("RGB", (S, S))
    top, bottom = (58, 18, 30), (14, 8, 20)
    pixels = image.load()
    for y in range(S):
        t = y / (S - 1)
        colour = tuple(int(top[i] * (1 - t) + bottom[i] * t) for i in range(3))
        for x in range(S):
            pixels[x, y] = colour
    return image


def circle(draw, cx, cy, r, fill, outline=None, width=0):
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=fill, outline=outline, width=width)


def stitches(draw, points, colour, width):
    """A dashed seam with small cross stitches along a polyline."""
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        draw.line((x0, y0, x1, y1), fill=colour, width=width)
        steps = max(1, int(((x1 - x0) ** 2 + (y1 - y0) ** 2) ** 0.5 // (18 * SCALE)))
        for i in range(steps + 1):
            t = i / steps
            x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            dx, dy = y1 - y0, x0 - x1
            length = max(1.0, (dx * dx + dy * dy) ** 0.5)
            dx, dy = dx / length * 9 * SCALE, dy / length * 9 * SCALE
            draw.line((x - dx, y - dy, x + dx, y + dy), fill=colour, width=width)


def main(out):
    image = gradient()
    draw = ImageDraw.Draw(image)

    fur, inner, dark = (150, 92, 52), (214, 170, 120), (40, 22, 16)
    cx, cy = S // 2, int(S * 0.44)

    # Soft shadow under the head.
    shadow = Image.new("L", (S, S), 0)
    ImageDraw.Draw(shadow).ellipse((cx - 190 * SCALE, cy - 150 * SCALE, cx + 190 * SCALE, cy + 190 * SCALE), fill=150)
    shadow = shadow.filter(ImageFilter.GaussianBlur(24 * SCALE))
    image.paste((0, 0, 0), (0, 12 * SCALE), shadow)

    # Ears, head, muzzle.
    for side in (-1, 1):
        ex, ey = cx + side * 135 * SCALE, cy - 125 * SCALE
        circle(draw, ex, ey, 62 * SCALE, fur, dark, 6 * SCALE)
        circle(draw, ex, ey, 34 * SCALE, inner)
    circle(draw, cx, cy, 165 * SCALE, fur, dark, 6 * SCALE)
    draw.ellipse((cx - 78 * SCALE, cy + 20 * SCALE, cx + 78 * SCALE, cy + 120 * SCALE), fill=inner, outline=dark, width=5 * SCALE)

    # One button eye, one stitched-shut eye, a nose and a stitched grin.
    circle(draw, cx - 62 * SCALE, cy - 30 * SCALE, 24 * SCALE, (20, 14, 14))
    circle(draw, cx - 70 * SCALE, cy - 38 * SCALE, 7 * SCALE, (240, 240, 240))
    stitches(draw, [(cx + 38 * SCALE, cy - 52 * SCALE), (cx + 90 * SCALE, cy - 10 * SCALE)], dark, 5 * SCALE)
    stitches(draw, [(cx + 38 * SCALE, cy - 10 * SCALE), (cx + 90 * SCALE, cy - 52 * SCALE)], dark, 5 * SCALE)
    draw.ellipse((cx - 22 * SCALE, cy + 32 * SCALE, cx + 22 * SCALE, cy + 62 * SCALE), fill=dark)
    stitches(draw, [(cx - 48 * SCALE, cy + 88 * SCALE), (cx, cy + 102 * SCALE), (cx + 48 * SCALE, cy + 88 * SCALE)], dark, 5 * SCALE)
    # A seam across the forehead.
    stitches(draw, [(cx - 30 * SCALE, cy - 158 * SCALE), (cx - 10 * SCALE, cy - 110 * SCALE), (cx + 22 * SCALE, cy - 92 * SCALE)], (90, 50, 26), 5 * SCALE)

    # "RECOMP" under the head.
    text = "RECOMP"
    font = None
    for name in ("DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf"):
        try:
            font = ImageFont.truetype(name, 64 * SCALE)
            break
        except OSError:
            continue
    if font is None:
        font = ImageFont.load_default()
    box = draw.textbbox((0, 0), text, font=font)
    tx = (S - (box[2] - box[0])) // 2
    ty = int(S * 0.80)
    draw.text((tx + 4 * SCALE, ty + 4 * SCALE), text, font=font, fill=(0, 0, 0))
    draw.text((tx, ty), text, font=font, fill=(236, 214, 170))

    image.resize((SIZE, SIZE), Image.LANCZOS).convert("RGBA").save(out)


if __name__ == "__main__":
    main(sys.argv[1])
