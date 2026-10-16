# Cuts the lettering of aot/docs/logo.png (the README logo) out of it: the
# letters with their bevel, without the rosette and the dark ground, plus a soft
# drop shadow so they read on the light parts of the cover.
# Usage: python make_logo.py [<logo.png>]  ->  launcher\res\logo.png (RGBA, 900 px wide)
import os
import sys

import numpy as np
from PIL import Image, ImageFilter
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "..", "aot", "docs", "logo.png")
OUT = os.path.join(ROOT, "res", "logo.png")

im = Image.open(SRC).convert("RGB")
rgb = np.asarray(im).astype(np.int32)
r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
lum = (r * 299 + g * 587 + b * 114) // 1000
# The letters are neutral grey; the rosette and the ground are tinted blue.
neutral = (b - r) < 9


def disk(radius):
    y, x = np.ogrid[-radius:radius + 1, -radius:radius + 1]
    return x * x + y * y <= radius * radius


# Faces: bright and neutral; close the dark specks of the stone texture, drop
# small bright bits of the engraving.
face = (lum > 120) & neutral
face = ndimage.binary_closing(face, structure=disk(3))
labels, n = ndimage.label(face)
sizes = ndimage.sum(face, labels, range(1, n + 1))
face = np.isin(labels, 1 + np.flatnonzero(sizes > 250))

# Bevel: the neutral dark rim right around the faces.
near = ndimage.binary_dilation(face, structure=disk(9))
bevel = near & ~face & neutral & (lum > 30)
mask = face | bevel
mask = ndimage.binary_closing(mask, structure=disk(2))
# Fill specks enclosed by the letters, but not the counters of O, R, A, P.
holes = ndimage.binary_fill_holes(mask) & ~mask
labels, n = ndimage.label(holes)
sizes = ndimage.sum(holes, labels, range(1, n + 1))
mask |= np.isin(labels, 1 + np.flatnonzero(sizes < 400))

alpha = Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8))
letters = im.copy()
letters.putalpha(alpha)

# Soft shadow under the letters.
pad = 24
w, h = im.size
canvas = Image.new("RGBA", (w + pad * 2, h + pad * 2), (0, 0, 0, 0))
shadow_a = Image.new("L", canvas.size, 0)
shadow_a.paste(alpha, (pad + 4, pad + 6))
shadow_a = shadow_a.filter(ImageFilter.GaussianBlur(7)).point(lambda v: v * 200 // 255)
shadow = Image.new("RGBA", canvas.size, (2, 10, 26, 255))
shadow.putalpha(shadow_a)
canvas.alpha_composite(shadow)
canvas.alpha_composite(letters, (pad, pad))

bbox = canvas.getchannel("A").point(lambda v: 255 if v > 6 else 0).getbbox()
out = canvas.crop(bbox)
lw = 900
out = out.resize((lw, round(out.size[1] * lw / out.size[0])), Image.LANCZOS)
out.save(OUT)
print("logo", out.size, "from", im.size, "bbox", bbox)
