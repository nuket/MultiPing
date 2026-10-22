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

"""Makes screenshots/carousel.webp, the README's carousel, from the screenshots beside it.

Each screenshot in SHOTS becomes one frame of an animated WebP that loops forever, scaled to
WIDTH by HEIGHT and held for HOLD_MS, with a pill at the bottom naming the system.
Usage: python tools/make_carousel.py  (needs Pillow built with WebP)
"""
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS_DIR = os.path.join(HERE, "..", "screenshots")
OUT = os.path.join(SHOTS_DIR, "carousel.webp")

# Windows, then the Mac, then Linux; each system light, then dark where it has a dark mode.
SHOTS = [
    ("01-windows-xp.png", "Windows XP \u00b7 Light"),
    ("02-windows-7.png", "Windows 7 \u00b7 Light"),
    ("03-windows-10-light.png", "Windows 10 \u00b7 Light"),
    ("04-windows-10-dark.png", "Windows 10 \u00b7 Dark"),
    ("05-windows-11-light.png", "Windows 11 \u00b7 Light"),
    ("06-windows-11-dark.png", "Windows 11 \u00b7 Dark"),
    ("07-macos-light.png", "macOS 27 \u00b7 Light"),
    ("08-macos-dark.png", "macOS 27 \u00b7 Dark"),
    ("09-ubuntu-26.04-gtk-light.png", "Ubuntu 26.04 \u00b7 GTK 3 \u00b7 Light"),
    ("10-ubuntu-26.04-gtk-dark.png", "Ubuntu 26.04 \u00b7 GTK 3 \u00b7 Dark"),
    ("11-kubuntu-24.04-qt5-light.png", "Kubuntu 24.04 \u00b7 Qt 5 \u00b7 Light"),
    ("12-kubuntu-24.04-qt5-dark.png", "Kubuntu 24.04 \u00b7 Qt 5 \u00b7 Dark"),
    ("13-kubuntu-24.04-qt6-light.png", "Kubuntu 24.04 \u00b7 Qt 6 \u00b7 Light"),
    ("14-kubuntu-24.04-qt6-dark.png", "Kubuntu 24.04 \u00b7 Qt 6 \u00b7 Dark"),
    ("15-kubuntu-26.04-qt6-light.png", "Kubuntu 26.04 \u00b7 Qt 6 \u00b7 Light"),
    ("16-kubuntu-26.04-qt6-dark.png", "Kubuntu 26.04 \u00b7 Qt 6 \u00b7 Dark"),
]
WIDTH, HEIGHT = 1280, 720
HOLD_MS = 3000
FONTS = [
    "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    r"C:\Windows\Fonts\segoeuib.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
]


def font(size: int) -> ImageFont.FreeTypeFont:
    for path in FONTS:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default(size)


def frame(path: str, caption: str) -> Image.Image:
    """The screenshot, scaled, with the caption in a pill centred at the bottom."""
    shot = Image.open(path).convert("RGB").resize((WIDTH, HEIGHT), Image.LANCZOS).convert("RGBA")
    layer = Image.new("RGBA", shot.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    label = font(20)
    pad, height = 22, 44
    above = 56   # clear of the taskbars, the tallest (Windows 11's) 32 px at this scale
    width = int(pad + draw.textlength(caption, font=label) + pad)
    x0, y0 = (WIDTH - width) // 2, HEIGHT - height - above
    draw.rounded_rectangle((x0, y0, x0 + width, y0 + height), radius=height // 2, fill=(0, 0, 0, 160))
    draw.text((WIDTH / 2, y0 + height / 2), caption, font=label, fill=(255, 255, 255, 255), anchor="mm")
    return Image.alpha_composite(shot, layer).convert("RGB")


def main() -> None:
    frames = [frame(os.path.join(SHOTS_DIR, name), caption) for name, caption in SHOTS]
    frames[0].save(OUT, save_all=True, append_images=frames[1:], duration=HOLD_MS, loop=0, quality=85, method=6)
    print(f"wrote {os.path.normpath(OUT)} ({os.path.getsize(OUT):,} bytes), {len(frames)} frames of {WIDTH}x{HEIGHT}")


if __name__ == "__main__":
    main()
