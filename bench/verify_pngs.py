#!/usr/bin/env python3
"""Independent PNG read-back verifier (standard library only).

For every PNG in the benchmark output directory it:
  * parses the PNG chunks and checks the signature/IHDR/CRC ordering rules,
  * concatenates IDAT, does zlib.decompress (the reference zlib decoder),
  * unfilters all scanlines per the PNG spec,
  * compares the reconstructed pixels against the original raw input image,
  * compares baseline vs multi-pass outputs pixel-by-pixel.

Run with: python3 -I verify_pngs.py <imagedir> <benchdir>
"""
import os
import struct
import sys
import zlib


PNG_SIG = b"\x89PNG\r\n\x1a\n"


def read_chunks(data):
    if data[:8] != PNG_SIG:
        raise ValueError("bad PNG signature")
    chunks = []
    p = 8
    while p + 12 <= len(data):
        length = struct.unpack(">I", data[p:p + 4])[0]
        ctype = data[p + 4:p + 8]
        body = data[p + 8:p + 8 + length]
        crc = struct.unpack(">I", data[p + 8 + length:p + 12 + length])[0]
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            raise ValueError("CRC mismatch in chunk %r" % ctype)
        chunks.append((ctype, body))
        p += 12 + length
        if ctype == b"IEND":
            break
    return chunks


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def unfilter(raw, width, height, bpp_bytes):
    stride = width * bpp_bytes
    out = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if ftype == 0:
            pass
        elif ftype == 1:  # Sub
            for i in range(bpp_bytes, stride):
                line[i] = (line[i] + line[i - bpp_bytes]) & 255
        elif ftype == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ftype == 3:  # Average
            for i in range(stride):
                a = line[i - bpp_bytes] if i >= bpp_bytes else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif ftype == 4:  # Paeth
            for i in range(stride):
                a = line[i - bpp_bytes] if i >= bpp_bytes else 0
                c = prev[i - bpp_bytes] if i >= bpp_bytes else 0
                line[i] = (line[i] + paeth(a, prev[i], c)) & 255
        else:
            raise ValueError("bad filter type %d" % ftype)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if pos != len(raw):
        raise ValueError("trailing compressed bytes")
    return bytes(out)


def decode_png(path):
    with open(path, "rb") as f:
        data = f.read()
    chunks = read_chunks(data)
    ihdr = chunks[0][0] == b"IHDR" and chunks[0][1]
    if not ihdr:
        raise ValueError("first chunk is not IHDR")
    width, height, bd, color, comp, filt, interlace = struct.unpack(">IIBBBBB", ihdr)
    if comp != 0 or filt != 0:
        raise ValueError("unsupported compression/filter method")
    if interlace != 0:
        raise ValueError("interlace not expected in benchmark outputs")
    idat = b"".join(body for ctype, body in chunks if ctype == b"IDAT")
    raw = zlib.decompress(idat)  # reference zlib decoder, also validates adler32
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    if bd == 16:
        bpp *= 2
    pixels = unfilter(raw, width, height, bpp)
    return width, height, bd, color, pixels, len(idat), len(raw)


def load_raw(path):
    with open(path, "rb") as f:
        data = f.read()
    # minimal PAM/PGM parser matching gen_images.py output
    if data[:2] == b"P7":
        end = data.index(b"ENDHDR")
        hdr = data[:end].decode("latin1").split()
        w = int(hdr[hdr.index("WIDTH") + 1])
        h = int(hdr[hdr.index("HEIGHT") + 1])
        depth = int(hdr[hdr.index("DEPTH") + 1])
        pos = end + len("ENDHDR") + 1
        return w, h, 6 if depth == 4 else 2, 8, data[pos:]
    if data[:2] == b"P5":
        nl = 0
        # header has exactly 4 whitespace-separated tokens
        parts = data[:64].split(None, 4)
        w, h, maxv = int(parts[1]), int(parts[2]), int(parts[3])
        pos = data.index(b"\n", data.index(str(maxv).encode())) + 1
        bd = 16 if maxv == 65535 else 8
        return w, h, 0, bd, data[pos:]
    raise ValueError("unknown raw format " + path)


def main(imagedir, benchdir):
    manifest = os.path.join(imagedir, "manifest.txt")
    failures = 0
    checked = 0
    with open(manifest) as f:
        entries = [line.split() for line in f if line.strip()]
    for name, kind, w_s, h_s, fname in entries:
        w, h = int(w_s), int(h_s)
        rw, rh, rcolor, rbd, raw = load_raw(os.path.join(imagedir, fname))
        variants = {}
        for suffix in ("base", "mpserial", "mppar"):
            png_path = os.path.join(benchdir, "%s.%s.png" % (name, suffix))
            pw, ph, pbd, pcolor, pixels, zsize, rawsize = decode_png(png_path)
            if (pw, ph) != (w, h) or (pw, ph) != (rw, rh):
                raise AssertionError("dim mismatch %s" % png_path)
            if pixels != raw:
                raise AssertionError("PIXEL MISMATCH vs original in %s" % png_path)
            variants[suffix] = (pixels, zsize, rawsize, pbd, pcolor)
            checked += 1
        if variants["base"][0] != variants["mpserial"][0] or \
           variants["mpserial"][0] != variants["mppar"][0]:
            raise AssertionError("pixels differ between variants of %s" % name)
        if variants["mpserial"][1] != variants["mppar"][1]:
            raise AssertionError("zlib payload size differs serial/parallel %s" % name)
        print("%-18s OK  %dx%d bd=%d color=%d  zlib=%6d raw=%8d  (all 3 variants identical)"
              % (name, w, h, variants["base"][3], variants["base"][4],
                 variants["base"][1], variants["base"][2]))
    print("\nverified %d PNG decodes with reference zlib; ALL PIXELS EXACT, 0 failures"
          % checked)
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: python3 -I verify_pngs.py <imagedir> <benchdir>")
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
