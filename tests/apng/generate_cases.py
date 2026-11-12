import ctypes, os, sys
sys.path.insert(0, os.path.dirname(__file__))
from make_frames import case
# two frames, opaque, fixed loop count
case("two_frames", [(255, 0, 0, 255), (0, 0, 255, 255)],
     [(100, 1000), (250, 1000)], 3, [1, 0], [0, 1])
# four frames with transparency, background dispose + over blend
case("four_alpha", [(255, 0, 0, 64), (0, 255, 0, 128),
                    (0, 0, 255, 192), (255, 255, 0, 255)],
     [(1, 10), (2, 10), (3, 10), (4, 10)], 1, 1, 1)
# infinite loop (num_plays = 0), dispose none / blend source
case("infinite", [(10, 20, 30, 255), (40, 50, 60, 255)],
     [(500, 1000), (500, 1000)], 0, 0, 0)
# single-frame control: must stay byte-identical to the plain encoder
case("single", [(123, 200, 47, 128)], [(100, 1000)], 1, 0, 0)
print("generated:", sorted(f for f in os.listdir("output/apng") if f.endswith(".png")))
