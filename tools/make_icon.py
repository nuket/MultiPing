# MIT License
#
# Copyright (c) 2026 Max Vilimpoc
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""Renders the waving-hand emoji (U+1F44B) from Segoe UI Emoji into MultiPing.ico.

Each icon size is rasterized directly at that size (not downscaled) so small sizes stay crisp.
Usage: python tools/make_icon.py  (writes MultiPing.ico and tools/icon_preview.png next to it)
"""
import io
import os
import struct
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


def bmp_entry(img: Image.Image) -> bytes:
    """32-bpp BGRA DIB with an AND mask, as Windows XP's icon loader expects (no PNG support)."""
    w, h = img.size
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    px = img.load()
    xor = bytearray()
    for y in range(h - 1, -1, -1):          # bottom-up rows
        for x in range(w):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
    stride = ((w + 31) // 32) * 4           # 1-bpp mask rows padded to 4 bytes
    mask = bytearray()
    for y in range(h - 1, -1, -1):
        row = 0
        for x in range(w):
            if px[x, y][3] < 128:
                row |= 1 << (stride * 8 - 1 - x)
        mask += row.to_bytes(stride, "big")
    return header + bytes(xor) + bytes(mask)


def png_entry(img: Image.Image) -> bytes:
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def write_ico(path: str, frames: list) -> None:
    # BMP entries for sizes up to 64 px so XP can use them; PNG for the big ones (Vista and later).
    entries = [(f, bmp_entry(f) if f.width <= 64 else png_entry(f)) for f in frames]
    offset = 6 + 16 * len(entries)
    out = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    for f, data in entries:
        w = f.width if f.width < 256 else 0
        out += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in entries:
        out += data
    with open(path, "wb") as fh:
        fh.write(out)


def main() -> None:
    frames = [render(s) for s in SIZES]
    write_ico(OUT, frames)
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
