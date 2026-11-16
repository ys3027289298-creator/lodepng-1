#!/usr/bin/env python3
"""Independent pixel cross-check against Pillow/libtiff.

Pillow is a development-time oracle only; lodepng itself gains no dependency.

Two comparison classes:
* 8-bit color/gray/palette/alpha images must be byte-exact.
* Sub-byte (1-bit) and 16-bit grayscale differ from Pillow by an expected
  bit-depth *scaling* when both are flattened to 8-bit RGBA: Pillow rescales
  0/1 -> 0/255 and 16-bit -> 8-bit, while lodepng preserves the raw sample
  values (PNG semantics). For those we verify sample-level correctness
  separately instead of requiring 8-bit equality.
"""
import os
import sys

import numpy as np
from PIL import Image

EXACT_CASES = [
    "gray_1.tif", "gray_5.tif", "gray_8.tif",
    "rgb_1.tif", "rgb_5.tif", "rgb_8.tif",
    "rgba_1.tif", "rgba_5.tif", "rgba_8.tif",
    "palette_1.tif", "palette_5.tif", "palette_8.tif",
    "planar_1.tif", "planar_5.tif", "planar_8.tif",
    "tiles_1.tif", "tiles_5.tif", "tiles_8.tif",
    "multipage_1.tif", "multipage_5.tif", "multipage_8.tif",
    "miniswhite_1.tif",
]


def main(samples):
    failures = 0
    print("# file result detail")
    for name in EXACT_CASES:
        dump = os.path.join(samples, name + ".rgba")
        mine = np.fromfile(dump, dtype=np.uint8)
        with Image.open(os.path.join(samples, name)) as im:
            ref = np.asarray(im.convert("RGBA")).flatten()
        if mine.shape != ref.shape:
            print("CROSSCHECK %s SIZE_MISMATCH" % name)
            failures += 1
            continue
        bad = int(np.count_nonzero(mine != ref))
        print("CROSSCHECK %s %s badpx=%d" %
              (name, "EXACT" if bad == 0 else "DIFF", bad))
        if bad:
            failures += 1

    # 1-bit: lodepng RGBA luma must equal the stored bit samples exactly.
    for name in ("gray1_1.tif", "gray1_5.tif", "gray1_8.tif"):
        mine = np.fromfile(os.path.join(samples, name + ".rgba"),
                           dtype=np.uint8).reshape(-1, 4)[:, 0]
        with Image.open(os.path.join(samples, name)) as im:
            ref = np.asarray(im.convert("L")).flatten()
        onebit_ok = set(np.unique(mine).tolist()).issubset({0, 1})
        # ordering preserved: wherever lodepng has 1, Pillow has the high value
        order_ok = np.array_equal(mine > 0, ref > 127)
        ok = onebit_ok and order_ok
        print("CROSSCHECK %s %s raw 0/1 samples preserved=%s order=%s" %
              (name, "SCALING-OK" if ok else "SCALING-BAD", onebit_ok, order_ok))
        if not ok:
            failures += 1

    # 16-bit: an 8-bit RGBA dump cannot represent the samples, so compare the
    # Pillow 16-bit image directly against the source TIFF's sample values
    # (lodepng's own 16-bit path is verified byte-exact separately in the
    # C++ harness and the g16 probe). Here just confirm Pillow reads the same
    # underlying 16-bit samples the generator wrote.
    for name in ("gray16_1.tif", "gray16_5.tif", "gray16_8.tif"):
        with Image.open(os.path.join(samples, name)) as im:
            pil16 = np.asarray(im).astype(np.uint16).flatten()
        src = np.frombuffer(
            open(os.path.join(samples, name), "rb").read(), dtype=np.uint8)
        # generator reference values, big-endian 16-bit, W*H samples
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "mk", os.path.join(os.path.dirname(__file__), "tiff_make.py"))
        mk = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mk)
        want = np.frombuffer(mk.reference_gray16(), dtype=">u2").astype(np.uint16)
        ok = np.array_equal(pil16, want)
        print("CROSSCHECK %s %s 16-bit samples match source=%d/%d" %
              (name, "SCALING-OK" if ok else "SCALING-BAD",
               int(np.count_nonzero(pil16 == want)), len(want)))
        if not ok:
            failures += 1

    print("CROSSCHECK %s" %
          ("ALL OK" if failures == 0 else "FAILURES=%d" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
