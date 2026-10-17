"""Renders the waving-hand emoji (U+1F44B) from Segoe UI Emoji into MultiPing.ico.

Each icon size is rasterized directly at that size (not downscaled) so small sizes stay crisp.
Usage: python tools/make_icon.py  (writes MultiPing.ico and tools/icon_preview.png next to it)
"""
import os
from PIL import Image, ImageDraw, ImageFont

FONT = r"C:\Windows\Fonts\seguiemj.ttf"
EMOJI = "\U0001F44B"
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "MultiPing.ico")


def render(size: int) -> Image.Image:
    # Pick the largest font size whose glyph fits in the square with a small margin.
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    best = None
    for pt in range(size, 4, -1):
        font = ImageFont.truetype(FONT, pt)
        l, t, r, b = font.getbbox(EMOJI)
        if r - l <= size - 2 and b - t <= size - 2:
            best = (font, l, t, r, b)
            break
    font, l, t, r, b = best
    x = (size - (r - l)) // 2 - l
    y = (size - (b - t)) // 2 - t
    ImageDraw.Draw(img).text((x, y), EMOJI, font=font, embedded_color=True)
    return img


def main() -> None:
    frames = [render(s) for s in SIZES]
    frames[-1].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
    # Preview strip for a quick visual check.
    strip = Image.new("RGBA", (sum(SIZES) + 10 * len(SIZES), 256), (240, 240, 240, 255))
    x = 5
    for f in frames:
        strip.paste(f, (x, (256 - f.height) // 2), f)
        x += f.width + 10
    strip.save(os.path.join(HERE, "icon_preview.png"))
    print(f"wrote {os.path.normpath(OUT)} ({os.path.getsize(OUT)} bytes) with sizes {SIZES}")


if __name__ == "__main__":
    main()
