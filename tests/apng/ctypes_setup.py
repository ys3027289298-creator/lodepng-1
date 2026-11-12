import ctypes, os, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
api = ctypes.CDLL(os.path.join(ROOT, "output", "apng", "liblodepng.so"))
api.lodepng_encode_apng.argtypes = [ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),
    ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p, ctypes.c_uint,
    ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
api.lodepng_encode32.argtypes = [ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),
    ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
class LodepngFrame(ctypes.Structure):
    _fields_ = [("buffer", ctypes.c_void_p), ("delay_num", ctypes.c_uint),
                ("delay_den", ctypes.c_uint), ("dispose_op", ctypes.c_uint),
                ("blend_op", ctypes.c_uint)]
ctypes.api, ctypes.LodepngFrame, ctypes.LCT_RGBA = api, LodepngFrame, 6
