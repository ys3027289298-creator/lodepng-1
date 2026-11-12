import ctypes, os, sys
sys.path.insert(0, os.path.dirname(__file__))
import ctypes_setup
W = H = 8
def frame(r, g, b, a=255):
    px = b"".join(bytes([(r + k) & 255, g, b, a]) for k in range(W * H))
    return (ctypes.c_ubyte * len(px))(*px)
def case(name, colors, delays, plays, disp, blend):
    frames, bufs = [], []
    for k, ((r, g, b, a), (n, d)) in enumerate(zip(colors, delays)):
        buf = frame(r, g, b, a); bufs.append(buf)
        dp = disp[k] if isinstance(disp, (list, tuple)) else disp
        bo = blend[k] if isinstance(blend, (list, tuple)) else blend
        frames.append(ctypes.LodepngFrame(ctypes.cast(buf, ctypes.c_void_p), n, d, dp, bo))
    arr = (ctypes.LodepngFrame * len(frames))(*frames)
    out, size = ctypes.POINTER(ctypes.c_ubyte)(), ctypes.c_size_t()
    err = ctypes.api.lodepng_encode_apng(ctypes.byref(out), ctypes.byref(size),
        arr, len(frames), W, H, ctypes.LCT_RGBA, 8, plays)
    assert err == 0, err
    open("output/apng/" + name + ".png", "wb").write(ctypes.string_at(out, size.value))
