import struct, zlib, os, sys, ctypes
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctypes_setup
W = H = 8
def read_chunks(d):
    p, res = 8, []
    while p < len(d):
        n = struct.unpack('>I', d[p:p + 4])[0]
        res.append((d[p + 4:p + 8], d[p + 8:p + 8 + n]))
        p += 12 + n
    return res
def unfilter(raw, bpp=4):
    stride, prev, out = W * bpp + 1, bytearray(W * bpp), bytearray()
    for y in range(len(raw) // stride):
        ft, line = raw[y * stride], bytearray(raw[y * stride + 1:(y + 1) * stride])
        for i in range(len(line)):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], (prev[i - bpp] if i >= bpp else 0)
            if ft == 1: line[i] = (line[i] + a) & 255
            elif ft == 2: line[i] = (line[i] + b) & 255
            elif ft == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                q = a + b - c
                pa, pb, pc = abs(q - a), abs(q - b), abs(q - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out, prev = out + line, line
    return bytes(out)
def inspect(path):
    d = open(path, 'rb').read()
    acts, fcs, pixels = (1, 1), [], []
    for t, body in read_chunks(d):
        if t == b'acTL': acts = struct.unpack('>II', body)
        elif t == b'fcTL': fcs.append(struct.unpack('>IIIIIHHBB', body))
        elif t in (b'IDAT', b'fdAT'):
            pixels.append(unfilter(zlib.decompress(body[4:] if t == b'fdAT' else body)))
    print('  frames=%d plays=%d (0=infinite)' % acts)
    for i, px in enumerate(pixels):
        fc = fcs[i] if i < len(fcs) else None
        delay = ('%d/%d s = %.3fs' % (fc[5], fc[6], fc[5] / float(fc[6]))) if fc else 'N/A (plain PNG)'
        cx = ((H // 2) * W + (W // 2)) * 4
        print('  frame %d: delay=%s dispose=%d blend=%d center RGBA=%s'
              % (i, delay, fc[7] if fc else 0, fc[8] if fc else 0, list(px[cx:cx + 4])))
    return acts, fcs, pixels
def single_byte_check(lib):
    from make_frames import frame
    buf = frame(123, 200, 47, 128)
    ref, rsz = ctypes.POINTER(ctypes.c_ubyte)(), ctypes.c_size_t()
    lib.lodepng_encode32(ctypes.byref(ref), ctypes.byref(rsz), buf, W, H)
    ref_bytes = ctypes.string_at(ref, rsz.value)
    got = open('output/apng/single.png', 'rb').read()
    print('  single-frame byte-identical to lodepng_encode32: %s' % (ref_bytes == got))
    assert ref_bytes == got
if __name__ == '__main__':
    for name in sorted(os.listdir('output/apng')):
        if name.endswith('.png'):
            print(name + ':')
            inspect('output/apng/' + name)
    single_byte_check(ctypes.api)
