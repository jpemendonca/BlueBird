#!/usr/bin/env python3
"""Draws assets/launcher/icon0.png (512x512), the Media tab tile: a bird on blue, with sound waves. Needs Pillow. Usage: python tools/make_icon.py"""
from pathlib import Path

from PIL import Image, ImageDraw

SIZE, SS = 512, 4                       # drawn 4x larger, then scaled down for smooth edges
OUT = Path(__file__).resolve().parent.parent / "assets" / "launcher" / "icon0.png"


def bird(d, big):
    """A round bird facing right: body, head, beak, tail and a wing cut out in blue."""
    white, blue = (255, 255, 255), (36, 96, 214)
    u = big / 100
    d.ellipse([18 * u, 40 * u, 62 * u, 76 * u], fill=white)                    # body
    d.ellipse([44 * u, 30 * u, 66 * u, 52 * u], fill=white)                    # head
    d.polygon([(64 * u, 38 * u), (74 * u, 42 * u), (64 * u, 46 * u)], fill=(255, 196, 61))   # beak
    d.polygon([(22 * u, 56 * u), (6 * u, 46 * u), (10 * u, 62 * u)], fill=white)           # tail
    d.ellipse([56 * u, 37 * u, 60 * u, 41 * u], fill=(16, 22, 34))             # eye
    d.arc([24 * u, 42 * u, 50 * u, 66 * u], start=25, end=155, fill=blue, width=int(3 * u))   # wing


def main():
    big = SIZE * SS
    img = Image.new("RGB", (big, big), (16, 22, 34))
    d = ImageDraw.Draw(img)
    # background: a vertical blue gradient
    for yy in range(big):
        t = yy / big
        d.line([(0, yy), (big, yy)], fill=(int(31 + 10 * t), int(111 - 40 * t), int(235 - 60 * t)))
    bird(d, big)
    # sound waves on the right
    cx, cy = big * 0.72, big * 0.44
    for i, r in enumerate((0.10, 0.17, 0.24)):
        rr = big * r
        d.arc([cx - rr, cy - rr, cx + rr, cy + rr], start=-50, end=50, fill=(255, 255, 255), width=int(big * 0.028))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    img.save(OUT)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
