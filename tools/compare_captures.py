"""Compare two frame captures (capture/ dirs of the native renderer, e.g. the
FREEZE_CAPTURE_EACH states of work/iraq_fight.sh): each resolve destination by
address and occurrence, over the area both have. Prints the largest and mean
absolute difference per texture (values as the capture holds them: a texture
that took a render target's resource holds them before the exponent bias).

Usage: python compare_captures.py <dir a> <dir b>
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_to_png import FORMATS  # noqa: E402


def load(root):
    out = {}
    seen = {}
    for line in open(os.path.join(root, 'index.txt')):
        parts = line.split()
        name, w, h, pitch, fmt = parts[0], int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
        key = name.split('_', 1)[1]  # address_kind
        seen[key] = seen.get(key, 0) + 1
        key = f'{key}#{seen[key]}'
        if fmt not in FORMATS:
            continue
        dtype, channels, kind = FORMATS[fmt]
        raw = np.fromfile(os.path.join(root, name + '.raw'), dtype=np.uint8)
        rows = raw[: pitch * h].reshape(h, pitch)
        if kind == 'r10g10b10a2':
            v = rows[:, : w * 4].copy().view(np.uint32).reshape(h, w)
            img = np.stack([(v >> s) & 0x3FF for s in (0, 10, 20, 30)], axis=2).astype(np.float32)
            img /= np.array([1023.0, 1023.0, 1023.0, 3.0], np.float32)
        else:
            bpp = np.dtype(dtype).itemsize * channels
            img = rows[:, : w * bpp].copy().view(dtype).reshape(h, w, channels).astype(np.float32)
            if kind == 'unorm':
                img /= float(np.iinfo(dtype).max)
        out[key] = img
    return out


a, b = load(sys.argv[1]), load(sys.argv[2])
for key in a:
    if key not in b:
        print(f'{key:28} only in {sys.argv[1]}')
        continue
    x, y = a[key], b[key]
    h, w = min(x.shape[0], y.shape[0]), min(x.shape[1], y.shape[1])
    c = min(x.shape[2], y.shape[2])
    d = np.abs(x[:h, :w, :c] - y[:h, :w, :c])
    d = np.nan_to_num(d, nan=1e9)
    print(f'{key:28} {x.shape[1]}x{x.shape[0]} vs {y.shape[1]}x{y.shape[0]}: max {d.max():.4g} mean {d.mean():.3g} '
          f'pixels > 1/255: {(d.max(axis=2) > 1 / 255).mean() * 100:.3f}%')
for key in b:
    if key not in a:
        print(f'{key:28} only in {sys.argv[2]}')
