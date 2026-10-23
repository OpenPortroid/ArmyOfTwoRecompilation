"""Convert the native renderer's frame capture (capture/index.txt + *.raw,
written with --ao2_native_gpu_capture_frame=N) to PNG files.

Usage: python capture_to_png.py [capture_dir]
Float formats are shown as-is clamped to [0, 1] and additionally tone-mapped
(x / (1 + x)) into *_tm.png; depth is normalized to its min..max range.
"""
import os
import sys

import numpy as np
from PIL import Image

# DXGI_FORMAT values -> (numpy dtype, channels, kind)
FORMATS = {
    2: (np.float32, 4, "float"),   # R32G32B32A32_FLOAT
    10: (np.float16, 4, "float"),  # R16G16B16A16_FLOAT
    11: (np.uint16, 4, "unorm"),   # R16G16B16A16_UNORM
    13: (np.int16, 4, "snorm"),    # R16G16B16A16_SNORM
    16: (np.float32, 2, "float"),  # R32G32_FLOAT
    24: (None, 4, "r10g10b10a2"),  # R10G10B10A2_UNORM
    26: (None, 3, "r11g11b10"),    # R11G11B10_FLOAT
    28: (np.uint8, 4, "unorm"),    # R8G8B8A8_UNORM
    29: (np.uint8, 4, "unorm"),    # R8G8B8A8_UNORM_SRGB
    34: (np.float16, 2, "float"),  # R16G16_FLOAT
    35: (np.uint16, 2, "unorm"),   # R16G16_UNORM
    41: (np.float32, 1, "depth"),  # R32_FLOAT
    49: (np.uint8, 2, "unorm"),    # R8G8_UNORM
    54: (np.float16, 1, "float"),  # R16_FLOAT
    56: (np.uint16, 1, "unorm"),   # R16_UNORM
    61: (np.uint8, 1, "unorm"),    # R8_UNORM
    62: (np.uint8, 1, "stencil"),  # R8_UINT: stencil plane (0 black, others 0.3..1)
}


def r11g11b10(v):
    """R11G11B10_FLOAT texels (uint32 array) as float32 RGB."""
    out = []
    for shift, mbits in ((0, 6), (11, 6), (22, 5)):
        bits = (v >> shift) & ((1 << (mbits + 5)) - 1)
        m = (bits & ((1 << mbits) - 1)).astype(np.float32) / (1 << mbits)
        e = (bits >> mbits).astype(np.int32)
        out.append(np.where(e == 0, m * 2.0**-14, (1 + m) * np.exp2((e - 15).astype(np.float32))))
    return np.stack(out, axis=2).astype(np.float32)


def to_rgb(img):
    if img.shape[2] == 1:
        img = np.repeat(img, 3, axis=2)
    elif img.shape[2] == 2:
        img = np.concatenate([img, np.zeros_like(img[:, :, :1])], axis=2)
    return img[:, :, :3]


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "capture"
    for line in open(os.path.join(root, "index.txt")):
        parts = line.split()
        name, w, h, pitch, fmt = parts[:5]
        w, h, pitch, fmt = int(w), int(h), int(pitch), int(fmt)
        # 6th column: the resolve swapped red and blue (guest ARGB layout).
        swapped = len(parts) > 5 and parts[5] == "1"
        raw = np.fromfile(os.path.join(root, name + ".raw"), dtype=np.uint8)
        if fmt not in FORMATS:
            print(f"{name}: unsupported format {fmt}")
            continue
        dtype, channels, kind = FORMATS[fmt]
        rows = raw[: pitch * h].reshape(h, pitch)
        if kind == "r10g10b10a2":
            v = rows[:, : w * 4].copy().view(np.uint32).reshape(h, w)
            img = np.stack([(v >> s) & 0x3FF for s in (0, 10, 20)], axis=2).astype(np.float32) / 1023.0
        elif kind == "r11g11b10":
            img = r11g11b10(rows[:, : w * 4].copy().view(np.uint32).reshape(h, w))
        else:
            bpp = np.dtype(dtype).itemsize * channels
            img = rows[:, : w * bpp].copy().view(dtype).reshape(h, w, channels).astype(np.float32)
            if kind == "unorm":
                img /= float(np.iinfo(dtype).max)
            elif kind == "stencil":
                img = np.where(img == 0, 0.0, 0.3 + 0.7 * img / 255.0)
            elif kind == "snorm":
                img /= 32767.0
        stats = f"min {img.min():.4g} max {img.max():.4g} mean {img.mean():.4g}"
        if kind == "depth":
            lo, hi = float(img.min()), float(img.max())
            img = (img - lo) / max(hi - lo, 1e-9)
        rgb = to_rgb(img)
        if swapped:
            rgb = rgb[:, :, ::-1]
        Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(os.path.join(root, name + ".png"))
        if kind in ("float", "snorm"):
            tm = np.clip(rgb, 0, None)
            tm = tm / (1 + tm)
            Image.fromarray((tm * 255).astype(np.uint8)).save(os.path.join(root, name + "_tm.png"))
        print(f"{name}: {w}x{h} fmt {fmt} {stats}")


if __name__ == "__main__":
    main()
