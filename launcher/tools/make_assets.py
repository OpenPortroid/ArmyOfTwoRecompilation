# Builds the launcher's embedded images from user-provided pictures.
# Usage: python make_assets.py <background> [<logo.png>]
# Output goes to launcher\res (bg.jpg, bg_dim.jpg and, with a logo, logo.png);
# run once, the results are checked in. app.ico is kept as is.
import os
import sys
from PIL import Image, ImageEnhance, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "res")
os.makedirs(RES, exist_ok=True)

if len(sys.argv) not in (2, 3):
    sys.exit("usage: make_assets.py <background> [<logo>]")

# Window is 1200x660 logical. The main screen shows the art in a band between
# the title bar and the console (LauncherForm ArtTop..ArtBottom = 40..520), the
# settings screen behind everything.
BAND_W, BAND_H = 1200, 480
FULL_W, FULL_H = 1200, 660


def cover(img, aw, ah):
    """Centered crop of img to the aspect aw:ah."""
    sw, sh = img.size
    th = round(sw * ah / aw)
    if th <= sh:
        return img.crop((0, (sh - th) // 2, sw, (sh - th) // 2 + th))
    tw = round(sh * aw / ah)
    return img.crop(((sw - tw) // 2, 0, (sw - tw) // 2 + tw, sh))


src = Image.open(sys.argv[1]).convert("RGB")

# Main: the band, at most 2400 px wide (2x the window).
bg = cover(src, BAND_W, BAND_H)
if bg.size[0] > 2400:
    bg = bg.resize((2400, round(2400 * BAND_H / BAND_W)), Image.LANCZOS)
bg.save(os.path.join(RES, "bg.jpg"), quality=90)

# Settings: the window's shape, blurred and dimmed (light text on top).
BLUR, COLOR, BRIGHT = 14, 0.5, 0.55
dim = cover(src, FULL_W, FULL_H)
if dim.size[0] > 1600:
    dim = dim.resize((1600, round(1600 * FULL_H / FULL_W)), Image.LANCZOS)
dim = dim.filter(ImageFilter.GaussianBlur(BLUR))
dim = ImageEnhance.Color(dim).enhance(COLOR)
dim = ImageEnhance.Brightness(dim).enhance(BRIGHT)
dim.save(os.path.join(RES, "bg_dim.jpg"), quality=88)
print("bg", bg.size, "bg_dim", dim.size)

if len(sys.argv) == 3:
    # Logo: crop to the alpha bounding box (+ padding), 900 px wide.
    logo = Image.open(sys.argv[2]).convert("RGBA")
    bbox = logo.getchannel("A").getbbox()
    PAD = 4
    x0, y0, x1, y1 = bbox
    x0, y0 = max(0, x0 - PAD), max(0, y0 - PAD)
    x1, y1 = min(logo.size[0], x1 + PAD), min(logo.size[1], y1 + PAD)
    logo = logo.crop((x0, y0, x1, y1))
    lw = 900
    logo = logo.resize((lw, round(logo.size[1] * lw / logo.size[0])), Image.LANCZOS)
    logo.save(os.path.join(RES, "logo.png"))
    print("logo", logo.size)
