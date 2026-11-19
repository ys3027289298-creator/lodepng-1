#!/usr/bin/env python3
"""Generate 12 diverse test images as raw RGBA/RGB/gray PPM/PGM files.

These are *raw* images, not PNGs, so the generator does not depend on any PNG
encoder. The C benchmark reads them and feeds the pixels straight into lodepng.
Run with: python3 -I gen_images.py <outdir>
"""
import math
import os
import sys
import random


def write_ppm(path, w, h, pixels, mode):
    # pixels: flat byte list; mode "rgb" (3 bpp) or "rgba" (4 bpp)
    maxv = 255
    if mode == "rgb":
        header = ("P6\n%d %d\n%d\n" % (w, h, maxv)).encode()
    else:
        header = ("P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL %d\nTUPLTYPE RGB_ALPHA\nENDHDR\n"
                  % (w, h, maxv)).encode()
    with open(path, "wb") as f:
        f.write(header)
        f.write(bytes(pixels))


def write_pgm(path, w, h, pixels, depth16=False):
    if depth16:
        header = ("P5\n%d %d\n65535\n" % (w, h)).encode()
        with open(path, "wb") as f:
            f.write(header)
            f.write(bytes(pixels))
    else:
        header = ("P5\n%d %d\n255\n" % (w, h)).encode()
        with open(path, "wb") as f:
            f.write(header)
            f.write(bytes(pixels))


def img_photo_noise(w, h, seed):
    rnd = random.Random(seed)
    px = bytearray()
    for _ in range(w * h):
        px += bytes((rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), 255))
    return px


def img_smooth_gradient(w, h):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            r = int(255 * x / max(1, w - 1))
            g = int(255 * y / max(1, h - 1))
            b = int(128 + 127 * math.sin((x + y) * 0.05))
            px += bytes((r, g, b, 255))
    return px


def img_radial(w, h):
    cx, cy = w / 2.0, h / 2.0
    px = bytearray()
    for y in range(h):
        for x in range(w):
            d = math.hypot(x - cx, y - cy) / math.hypot(cx, cy)
            v = int(255 * max(0.0, 1.0 - d))
            px += bytes((v, (v * 2) & 255, 255 - v, 255))
    return px


def img_text_like(w, h, seed):
    # flat background with sharp glyph-like rectangles: stresses filters hard
    rnd = random.Random(seed)
    px = bytearray([245, 245, 245, 255] * (w * h))
    for _ in range(90):
        gx = rnd.randrange(0, w - 20)
        gy = rnd.randrange(0, h - 12)
        c = (rnd.randrange(20, 80), rnd.randrange(20, 80), rnd.randrange(20, 80), 255)
        for yy in range(gy, min(h, gy + rnd.randrange(6, 11))):
            for xx in range(gx, min(w, gx + rnd.randrange(8, 18))):
                i = 4 * (yy * w + xx)
                px[i:i + 4] = bytes(c)
    return px


def img_large_flat(w, h):
    # one big near-uniform region plus a few thin bands -> huge match potential
    px = bytearray()
    for y in range(h):
        band = (y // 40) % 2
        for x in range(w):
            base = 200 if band == 0 else 205
            px += bytes((base, base, base + 5, 255))
    return px


def img_alpha_blend(w, h):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            r = int(255 * x / max(1, w - 1))
            g = int(255 * y / max(1, h - 1))
            a = int(255 * (x + y) / max(1, w + h - 2))
            px += bytes((r, g, 128, a))
    return px


def img_palette_like(w, h, seed):
    # only 16 distinct colors -> palette conversion path
    rnd = random.Random(seed)
    palette = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256))
               for _ in range(16)]
    px = bytearray()
    for _ in range(w * h):
        r, g, b = palette[rnd.randrange(16)]
        px += bytes((r, g, b, 255))
    return px


def img_gray_gradient(w, h):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            px.append(int(255 * (0.5 + 0.5 * math.sin(x * 0.08 + y * 0.03))))
    return px


def img_gray16(w, h):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            v = int(32767 + 32767 * math.sin(x * 0.04) * math.cos(y * 0.05)) & 0xFFFF
            px.append((v >> 8) & 255)
            px.append(v & 255)
    return px


def img_checker(w, h, cell=16):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            on = ((x // cell) + (y // cell)) % 2
            v = 30 if on else 220
            px += bytes((v, v, v, 255))
    return px


def img_photographic(w, h, seed):
    # low-frequency correlated noise: more natural than white noise
    rnd = random.Random(seed)
    grid = 32
    low = [[rnd.uniform(-40, 40) for _ in range(grid)] for _ in range(grid)]
    px = bytearray()
    for y in range(h):
        for x in range(w):
            gx = x * grid / w
            gy = y * grid / h
            x0, y0 = int(gx) % grid, int(gy) % grid
            x1, y1 = (x0 + 1) % grid, (y0 + 1) % grid
            fx, fy = gx - int(gx), gy - int(gy)
            n = (low[y0][x0] * (1 - fx) * (1 - fy) + low[y0][x1] * fx * (1 - fy)
                 + low[y1][x0] * (1 - fx) * fy + low[y1][x1] * fx * fy)
            r = max(0, min(255, int(128 + 80 * math.sin(x * 0.02) + n)))
            g = max(0, min(255, int(110 + 70 * math.cos(y * 0.025) + n)))
            b = max(0, min(255, int(120 + 60 * math.sin((x + y) * 0.015) + n)))
            px += bytes((r, g, b, 255))
    return px


def img_sparse_edges(w, h, seed):
    rnd = random.Random(seed)
    px = bytearray([255, 255, 255, 255] * (w * h))
    for _ in range(25):
        x0, y0 = rnd.randrange(w), rnd.randrange(h)
        x1, y1 = rnd.randrange(w), rnd.randrange(h)
        steps = max(abs(x1 - x0), abs(y1 - y0), 1)
        for s in range(steps):
            xx = x0 + (x1 - x0) * s // steps
            yy = y0 + (y1 - y0) * s // steps
            i = 4 * (yy * w + xx)
            px[i:i + 4] = bytes((0, 0, 0, 255))
    return px


def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    specs = [
        ("01_white_noise",    "rgba", 512, 512, lambda w, h: img_photo_noise(w, h, 1)),
        ("02_smooth_grad",    "rgba", 640, 480, lambda w, h: img_smooth_gradient(w, h)),
        ("03_radial",         "rgba", 600, 600, lambda w, h: img_radial(w, h)),
        ("04_text_like",      "rgba", 640, 480, lambda w, h: img_text_like(w, h, 2)),
        ("05_large_flat",     "rgba", 800, 600, lambda w, h: img_large_flat(w, h)),
        ("06_alpha_blend",    "rgba", 512, 512, lambda w, h: img_alpha_blend(w, h)),
        ("07_palette16",      "rgba", 512, 512, lambda w, h: img_palette_like(w, h, 3)),
        ("08_gray_gradient",  "gray8", 640, 480, lambda w, h: img_gray_gradient(w, h)),
        ("09_gray16",         "gray16", 512, 512, lambda w, h: img_gray16(w, h)),
        ("10_checker",        "rgba", 512, 512, lambda w, h: img_checker(w, h)),
        ("11_photographic",   "rgba", 640, 480, lambda w, h: img_photographic(w, h, 4)),
        ("12_sparse_edges",   "rgba", 800, 600, lambda w, h: img_sparse_edges(w, h, 5)),
    ]
    manifest = []
    for name, kind, w, h, fn in specs:
        data = fn(w, h)
        if kind == "rgba":
            path = os.path.join(outdir, name + ".pam")
            write_ppm(path, w, h, data, "rgba")
        elif kind == "gray8":
            path = os.path.join(outdir, name + ".pgm")
            write_pgm(path, w, h, data)
        else:
            path = os.path.join(outdir, name + ".pgm16")
            write_pgm(path, w, h, data, depth16=True)
        manifest.append((name, kind, w, h, path))
        print("wrote", path, kind, w, "x", h, len(data), "bytes")
    with open(os.path.join(outdir, "manifest.txt"), "w") as f:
        for name, kind, w, h, path in manifest:
            f.write("%s %s %d %d %s\n" % (name, kind, w, h, os.path.basename(path)))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
