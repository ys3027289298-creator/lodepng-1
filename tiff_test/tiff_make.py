#!/usr/bin/env python3
"""Generate small TIFF test samples using only the Python standard library.

Run with -I (isolated mode): python3 -I tiff_make.py <outdir>

For each of the three required compressions (1=none, 5=LZW, 8=Adobe
deflate/zlib) it emits gray, RGB, RGBA, 16-bit gray, 1-bit, palette, planar
and tiled variants, plus a two-page multi-page document whose first IFD is
the gray reference image.

The writer uses a single build pass: every variable blob (arrays, color map,
compressed pixel chunks) is registered first and given a key; TiffWriter.build
then lays out the IFD and all blobs in one step and resolves every offset
from the final entry count, so offsets can never be stale.
"""
import os
import struct
import sys
import zlib

W, H = 32, 24

T_BYTE, T_ASCII, T_SHORT, T_LONG, T_RATIONAL = 1, 2, 3, 4, 5
UNIT = {T_BYTE: 1, T_ASCII: 1, T_SHORT: 2, T_LONG: 4, T_RATIONAL: 8}


# --------------------------- reference pixels ---------------------------

def reference_rgb():
    px = bytearray()
    for y in range(H):
        for x in range(W):
            px += bytes(((x * 7 + y * 3) & 255,
                         (x * 5 + y * 11) & 255,
                         (x * 13 + y * 17 + 9) & 255))
    return bytes(px)


def reference_gray():
    return bytes(((x * 7 + y * 3) & 255) for y in range(H) for x in range(W))


def reference_gray16():
    return b"".join(struct.pack(">H", (x * 997 + y * 131) & 65535)
                    for y in range(H) for x in range(W))


def reference_rgba():
    rgb = reference_rgb()
    out = bytearray()
    for i in range(W * H):
        out += rgb[i * 3:i * 3 + 3]
        out.append(128 + (i & 127))
    return bytes(out)


def reference_gray1():
    rows = []
    for y in range(H):
        row = bytearray((W + 7) // 8)
        for x in range(W):
            if (x + y) & 1:
                row[x >> 3] |= 1 << (7 - (x & 7))
        rows.append(bytes(row))
    return b"".join(rows)


def reference_palette():
    rows = []
    for y in range(H):
        row = bytearray((W + 1) // 2)
        for x in range(W):
            idx = (x + y * 2) & 15
            if x & 1:
                row[x >> 1] |= idx
            else:
                row[x >> 1] |= idx << 4
        rows.append(bytes(row))
    return b"".join(rows)


# ------------------------------- LZW -----------------------------------

def lzw_encode(data):
    """TIFF LZW. The writer widens at 512/1024/2048; the matching decoder
    widens one code earlier (511/1023/2047) — the standard TIFF early-change
    convention seen from the two ends of the bitstream."""
    bitbuf = 0
    nbits = 0
    out = bytearray()
    state = {}

    def emit(code, width):
        nonlocal bitbuf, nbits
        bitbuf = (bitbuf << width) | code
        nbits += width
        while nbits >= 8:
            nbits -= 8
            out.append((bitbuf >> nbits) & 255)
            bitbuf &= (1 << nbits) - 1

    def reset():
        state["table"] = {bytes([i]): i for i in range(256)}
        state["code_size"] = 9
        state["next_code"] = 258

    emit(256, 9)
    reset()
    w = b""
    for byte in data:
        c = bytes([byte])
        wc = w + c
        if wc in state["table"]:
            w = wc
        else:
            emit(state["table"][w], state["code_size"])
            if state["next_code"] < 4096:
                state["table"][wc] = state["next_code"]
                state["next_code"] += 1
                if state["next_code"] in (512, 1024, 2048):
                    state["code_size"] += 1
            # once the table is full (4096 entries), TIFF LZW keeps emitting
            # 12-bit codes without resetting; no new phrases are added
            w = c
    if w:
        emit(state["table"][w], state["code_size"])
    emit(257, state["code_size"])
    if nbits:
        out.append((bitbuf << (8 - nbits)) & 255)
    return bytes(out)


def compress(data, scheme):
    if scheme == 1:
        return data
    if scheme == 5:
        return lzw_encode(data)
    if scheme == 8:
        return zlib.compress(data, 9)
    raise ValueError(scheme)


# ----------------------------- TIFF writer ------------------------------

class TiffWriter:
    """Single-IFD builder with one resolution pass.

    Entries are (tag, type, count, spec) where spec is one of:
      ("int", value)             scalar known up front
      ("inline", raw_bytes)      <=4 bytes stored in the value field
      ("ref", key)               one LONG whose value is blob key's offset
      ("refs", [key, ...])       LONG array blob of blob-key offsets
    Blobs are registered with add_blob()/add_payload() and get a key; their
    file offsets are all computed in build().
    """

    def __init__(self, little_endian=True):
        self.le = little_endian
        self.endian = "<" if little_endian else ">"
        self.entries = []
        self.blobs = {}      # key -> bytes
        self._seq = 0

    def _key(self):
        k = ("blob", self._seq)
        self._seq += 1
        return k

    def add_scalar(self, tag, type_, value):
        self.entries.append((tag, type_, 1, ("int", int(value))))

    def add_short_array(self, tag, values):
        raw = b"".join(struct.pack(self.endian + "H", v) for v in values)
        if len(raw) <= 4:
            self.entries.append((tag, T_SHORT, len(values), ("inline", raw)))
        else:
            key = self._key()
            self.blobs[key] = raw
            self.entries.append((tag, T_SHORT, len(values), ("ref", key)))

    def add_blob(self, tag, type_, value_bytes):
        key = self._key()
        self.blobs[key] = value_bytes
        count = len(value_bytes) // UNIT[type_]
        self.entries.append((tag, type_, count, ("ref", key)))
        return key

    def add_payload(self, offset_tag, count_tag, chunks):
        """Register compressed chunk(s) and the offset/count IFD entries."""
        keys = []
        for chunk in chunks:
            key = self._key()
            self.blobs[key] = chunk
            keys.append(key)
        if len(keys) == 1:
            self.entries.append((offset_tag, T_LONG, 1, ("ref", keys[0])))
            self.entries.append((count_tag, T_LONG, 1, ("int", len(chunks[0]))))
        else:
            self.entries.append((offset_tag, T_LONG, len(keys), ("refs", keys)))
            self.entries.append((count_tag, T_LONG, len(keys),
                                 ("int_array", [len(c) for c in chunks])))

    def build(self, next_ifd=0):
        # "int_array" specs need their own blob; synthesize them first.
        for idx, (tag, type_, count, spec) in enumerate(self.entries):
            if spec[0] == "int_array":
                key = self._key()
                self.blobs[key] = b"".join(
                    struct.pack(self.endian + "L", v) for v in spec[1])
                self.entries[idx] = (tag, type_, count, ("ref", key))

        n = len(self.entries)
        ifd_start = 8
        cursor = ifd_start + 2 + 12 * n + 4

        # stable blob ordering: by key sequence
        ordered_keys = sorted(self.blobs.keys(), key=lambda k: k[1])
        offsets = {}
        for key in ordered_keys:
            offsets[key] = cursor
            cursor += len(self.blobs[key])

        rendered = []
        for tag, type_, count, spec in sorted(self.entries, key=lambda e: e[0]):
            total = UNIT[type_] * count
            kind = spec[0]
            if kind == "int":
                # pack at the field's own width; the ljust below left-aligns
                # sub-4-byte scalars (e.g. an inline SHORT) per the TIFF spec
                if type_ == T_SHORT:
                    field = struct.pack(self.endian + "H", spec[1] & 0xFFFF)
                else:
                    field = struct.pack(self.endian + "L", spec[1])
            elif kind == "inline":
                field = spec[1]
            elif kind == "ref":
                field = struct.pack(self.endian + "L", offsets[spec[1]])
            elif kind == "refs":
                # array of offsets: build its blob now (already positioned via
                # a registered key only if pre-created); handle inline arrays
                raw = b"".join(struct.pack(self.endian + "L", offsets[k])
                               for k in spec[1])
                if total <= 4:
                    field = raw
                else:
                    # need a blob; register and resolve via a fresh key placed
                    # at the current end
                    key = self._key()
                    self.blobs[key] = raw
                    new_cursor = cursor
                    offsets[key] = new_cursor
                    cursor += len(raw)
                    field = struct.pack(self.endian + "L", new_cursor)
            else:
                raise ValueError("unknown spec %r" % (spec,))
            if total <= 4:
                field = field.ljust(4, b"\x00")
            rendered.append(struct.pack(self.endian + "HHI", tag, type_, count)
                            + field)

        if self.le:
            header = b"II" + struct.pack("<H", 42) + struct.pack("<L", ifd_start)
        else:
            header = b"MM" + struct.pack(">H", 42) + struct.pack(">L", ifd_start)
        out = header + struct.pack(self.endian + "H", n) + b"".join(rendered)
        out += struct.pack(self.endian + "L", next_ifd)
        # re-derive ordered keys after any late blob registration
        ordered_keys = sorted(self.blobs.keys(), key=lambda k: k[1])
        for key in ordered_keys:
            out += self.blobs[key]
        return out


# ------------------------------ layouts ---------------------------------

def common_tags(tw, w, h, bps, samples, photometric, compression, planar=1,
                fill=1):
    tw.add_scalar(256, T_LONG, w)
    tw.add_scalar(257, T_LONG, h)
    tw.add_short_array(258, [bps] * samples)   # BitsPerSample
    tw.add_scalar(259, T_SHORT, compression)
    tw.add_scalar(262, T_SHORT, photometric)
    tw.add_scalar(277, T_SHORT, samples)       # SamplesPerPixel
    tw.add_scalar(284, T_SHORT, planar)        # PlanarConfiguration
    tw.add_scalar(266, T_SHORT, fill)          # FillOrder
    tw.add_scalar(339, T_SHORT, 1)             # SampleFormat = uint


def write_simple(path, pixel_bytes, w, h, bps, samples, photometric,
                 compression, planar=1, little_endian=True, fill=1):
    data = compress(pixel_bytes, compression)
    tw = TiffWriter(little_endian=little_endian)
    common_tags(tw, w, h, bps, samples, photometric, compression, planar, fill)
    tw.add_scalar(278, T_LONG, h)             # RowsPerStrip
    tw.add_payload(273, 279, [data])          # StripOffsets/ByteCounts
    with open(path, "wb") as f:
        f.write(tw.build())


def write_palette(path, indices, w, h, compression, little_endian=True):
    data = compress(indices, compression)
    ncolors = 16
    colormap = ([c * 4000 for c in range(ncolors)]
                + [(15 - c) * 4000 for c in range(ncolors)]
                + [20000 + c * 1000 for c in range(ncolors)])
    cmap_raw = b"".join(struct.pack(("<" if little_endian else ">") + "H", v)
                        for v in colormap)
    tw = TiffWriter(little_endian=little_endian)
    common_tags(tw, w, h, 4, 1, 3, compression)
    tw.add_scalar(278, T_LONG, h)
    tw.add_blob(320, T_SHORT, cmap_raw)       # ColorMap
    tw.add_payload(273, 279, [data])
    with open(path, "wb") as f:
        f.write(tw.build())


def write_tiled(path, rgb, w, h, compression):
    tile_w = tile_h = 16
    tiles_x = (w + tile_w - 1) // tile_w
    tiles_y = (h + tile_h - 1) // tile_h
    raw_tiles = []
    for ty in range(tiles_y):
        for tx in range(tiles_x):
            buf = bytearray()
            for yy in range(tile_h):
                for xx in range(tile_w):
                    ax, ay = tx * tile_w + xx, ty * tile_h + yy
                    if ax < w and ay < h:
                        buf += rgb[(ay * w + ax) * 3:(ay * w + ax) * 3 + 3]
                    else:
                        buf += b"\x00\x00\x00"
            raw_tiles.append(bytes(buf))
    comp_tiles = [compress(t, compression) for t in raw_tiles]
    tw = TiffWriter(little_endian=True)
    common_tags(tw, w, h, 8, 3, 2, compression)
    tw.add_scalar(322, T_LONG, tile_w)        # TileWidth
    tw.add_scalar(323, T_LONG, tile_h)        # TileLength
    tw.add_payload(324, 325, comp_tiles)      # TileOffsets/ByteCounts
    with open(path, "wb") as f:
        f.write(tw.build())


def write_planar(path, rgb, w, h, compression):
    planes = [bytes(rgb[c::3]) for c in range(3)]
    comp_parts = [compress(p, compression) for p in planes]
    tw = TiffWriter(little_endian=True)
    common_tags(tw, w, h, 8, 3, 2, compression, planar=2)
    tw.add_scalar(278, T_LONG, h)
    tw.add_payload(273, 279, comp_parts)      # one strip per component plane
    with open(path, "wb") as f:
        f.write(tw.build())


def write_multipage(path, gray, w, h, compression):
    """Two IFDs assembled with a self-contained per-IFD builder subclass path.
    Page 1 is the gray reference; page 2 is solid gray. Offsets are resolved
    from absolute positions computed up front."""
    data1 = compress(gray, compression)
    data2 = compress(bytes([128]) * (w * h), compression)
    endian = "<"

    def build_ifd(entries, next_ifd, strip_off, strip_len):
        all_entries = sorted(entries + [
            (273, T_LONG, 1, ("int", strip_off)),
            (279, T_LONG, 1, ("int", strip_len)),
        ])
        n = len(all_entries)
        out = struct.pack(endian + "H", n)
        for tag, type_, count, spec in all_entries:
            field = struct.pack(endian + "L", spec[1])
            out += struct.pack(endian + "HHI", tag, type_, count) + field
        out += struct.pack(endian + "L", next_ifd)
        return out

    page1 = [
        (256, T_LONG, 1, ("int", w)), (257, T_LONG, 1, ("int", h)),
        (258, T_SHORT, 1, ("int", 8)), (259, T_SHORT, 1, ("int", compression)),
        (262, T_SHORT, 1, ("int", 1)), (277, T_SHORT, 1, ("int", 1)),
        (278, T_LONG, 1, ("int", h)), (339, T_SHORT, 1, ("int", 1)),
    ]
    page2 = [
        (256, T_LONG, 1, ("int", w)), (257, T_LONG, 1, ("int", h)),
        (258, T_SHORT, 1, ("int", 8)), (259, T_SHORT, 1, ("int", compression)),
        (262, T_SHORT, 1, ("int", 1)), (277, T_SHORT, 1, ("int", 1)),
        (278, T_LONG, 1, ("int", h)),
    ]
    n1, n2 = len(page1) + 2, len(page2) + 2
    ifd1_size = 2 + 12 * n1 + 4
    ifd2_size = 2 + 12 * n2 + 4
    page1_ifd = 8
    data1_abs = page1_ifd + ifd1_size
    page2_ifd = data1_abs + len(data1)
    data2_abs = page2_ifd + ifd2_size
    header = b"II" + struct.pack("<H", 42) + struct.pack("<L", page1_ifd)
    with open(path, "wb") as f:
        f.write(header)
        f.write(build_ifd(page1, page2_ifd, data1_abs, len(data1)))
        f.write(data1)
        f.write(build_ifd(page2, 0, data2_abs, len(data2)))
        f.write(data2)


def main(outdir):
    rgb = reference_rgb()
    gray = reference_gray()
    g16 = reference_gray16()
    rgba = reference_rgba()
    g1 = reference_gray1()
    pal = reference_palette()

    for c in (1, 5, 8):
        write_simple(os.path.join(outdir, "gray_%d.tif" % c),
                     gray, W, H, 8, 1, 1, c)
        write_simple(os.path.join(outdir, "rgb_%d.tif" % c),
                     rgb, W, H, 8, 3, 2, c)
        write_simple(os.path.join(outdir, "rgba_%d.tif" % c),
                     rgba, W, H, 8, 4, 2, c)
        write_simple(os.path.join(outdir, "gray16_%d.tif" % c),
                     g16, W, H, 16, 1, 1, c, little_endian=False)
        write_simple(os.path.join(outdir, "gray1_%d.tif" % c),
                     g1, W, H, 1, 1, 1, c)
        write_palette(os.path.join(outdir, "palette_%d.tif" % c),
                      pal, W, H, c)
        write_planar(os.path.join(outdir, "planar_%d.tif" % c),
                     rgb, W, H, c)
        write_tiled(os.path.join(outdir, "tiles_%d.tif" % c),
                    rgb, W, H, c)
        write_multipage(os.path.join(outdir, "multipage_%d.tif" % c),
                        gray, W, H, c)

    write_simple(os.path.join(outdir, "miniswhite_1.tif"),
                 gray, W, H, 8, 1, 0, 1)

    # failure: a structurally valid TIFF whose LZW strip contains an invalid
    # code. The IFD is left intact so parsing succeeds and the error (132) is
    # raised by the LZW decoder itself.
    good = bytearray(open(os.path.join(outdir, "gray_5.tif"), "rb").read())
    little = good[0] == 0x49
    en = "<" if little else ">"
    ifd = struct.unpack(en + "L", bytes(good[4:8]))[0]
    n = struct.unpack(en + "H", bytes(good[ifd:ifd + 2]))[0]
    strip_off = strip_len = 0
    for k in range(n):
        e = ifd + 2 + k * 12
        tag = struct.unpack(en + "H", bytes(good[e:e + 2]))[0]
        typ = struct.unpack(en + "H", bytes(good[e + 2:e + 4]))[0]
        cnt = struct.unpack(en + "L", bytes(good[e + 4:e + 8]))[0]
        if tag in (273, 279):
            if typ == 3 and cnt == 1:
                val = struct.unpack(en + "H", bytes(good[e + 8:e + 10]))[0]
            else:
                val = struct.unpack(en + "L", bytes(good[e + 8:e + 12]))[0]
            if tag == 273:
                strip_off = val
            else:
                strip_len = val
    # flip bits well inside the strip; this produces an undefined LZW code
    mid = strip_off + strip_len // 2
    good[mid] ^= 0xFF
    good[mid + 1] ^= 0x0F
    with open(os.path.join(outdir, "corrupt_lzw5.tif"), "wb") as f:
        f.write(bytes(good))

    # failure: unsupported compression (7 = JPEG)
    payload = b"\x00\x00\x00\x00"
    tw = TiffWriter(little_endian=True)
    common_tags(tw, W, H, 8, 1, 1, 7)
    tw.add_scalar(278, T_LONG, H)
    tw.add_payload(273, 279, [payload])
    with open(os.path.join(outdir, "badcompress_1.tif"), "wb") as f:
        f.write(tw.build())

    # failure: corrupted deflate stream (valid zlib header, broken body)
    def8 = bytearray(open(os.path.join(outdir, "gray_8.tif"), "rb").read())
    so = sl = 0
    ifd = struct.unpack("<L", bytes(def8[4:8]))[0]
    n8 = struct.unpack("<H", bytes(def8[ifd:ifd + 2]))[0]
    for k in range(n8):
        e = ifd + 2 + k * 12
        tag = struct.unpack("<H", bytes(def8[e:e + 2]))[0]
        val = struct.unpack("<L", bytes(def8[e + 8:e + 12]))[0]
        if tag == 273:
            so = val
        elif tag == 279:
            sl = val
    def8[so + sl // 2] ^= 0xFF
    with open(os.path.join(outdir, "corrupt_deflate8.tif"), "wb") as f:
        f.write(bytes(def8))

    # failure: unsupported photometric (4 = transparency mask)
    tw = TiffWriter(little_endian=True)
    common_tags(tw, W, H, 8, 1, 4, 1)
    tw.add_scalar(278, T_LONG, H)
    tw.add_payload(273, 279, [bytes(W * H)])
    with open(os.path.join(outdir, "badphotometric_1.tif"), "wb") as f:
        f.write(tw.build())

    # failure: strip offset points outside the file
    tw = TiffWriter(little_endian=True)
    common_tags(tw, W, H, 8, 1, 1, 1)
    tw.add_scalar(278, T_LONG, H)
    tw.add_scalar(273, T_LONG, 0x7FFFFFFF)   # impossible StripOffsets
    tw.add_scalar(279, T_LONG, W * H)
    with open(os.path.join(outdir, "badoffset_1.tif"), "wb") as f:
        f.write(tw.build())

    # failure: truncated file (header + partial IFD only)
    full = open(os.path.join(outdir, "gray_1.tif"), "rb").read()
    with open(os.path.join(outdir, "truncated_1.tif"), "wb") as f:
        f.write(full[:14])

    # failure: not a TIFF at all
    with open(os.path.join(outdir, "nottiff.bin"), "wb") as f:
        f.write(b"this is definitely not a tiff file" + bytes(40))

    with open(os.path.join(outdir, "reference.bin"), "wb") as f:
        f.write(struct.pack("<II", W, H))
        f.write(gray + rgb + rgba + g16 + g1 + pal)


if __name__ == "__main__":
    main(sys.argv[1])
