# MIT License
#
# MultiPing
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

The glyph is rendered once at high resolution, cropped to its visible pixels so it fills the icon
square, and scaled down to each size.
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


MASTER = 1024   # render size; every icon size is scaled down from this one image
MARGIN = 0.04   # fraction of the icon left clear around the glyph (Edge/Chrome-style tight fit)


def render_master() -> Image.Image:
    """The glyph at high resolution, cropped to its visible pixels and padded to a square.

    The font's own bounding box includes generous padding, which made the hand sit small in the
    icon. Cropping to the alpha channel and scaling that to the square fills it the way other
    app icons do.
    """
    font = ImageFont.truetype(FONT, MASTER // 2)
    canvas = Image.new("RGBA", (MASTER * 2, MASTER * 2), (0, 0, 0, 0))
    ImageDraw.Draw(canvas).text((MASTER // 2, MASTER // 2), EMOJI, font=font, embedded_color=True)
    box = canvas.getchannel("A").getbbox()
    glyph = canvas.crop(box)
    side = max(glyph.width, glyph.height)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(glyph, ((side - glyph.width) // 2, (side - glyph.height) // 2), glyph)
    return square


def render(master: Image.Image, size: int) -> Image.Image:
    inner = max(1, round(size * (1 - 2 * MARGIN)))
    scaled = master.resize((inner, inner), Image.LANCZOS)
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    img.paste(scaled, ((size - inner) // 2, (size - inner) // 2), scaled)
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
    master = render_master()
    frames = [render(master, s) for s in SIZES]
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
