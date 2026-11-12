#!/bin/bash
# Stage-based APNG validation driver. All generated files go to output/apng.
set -e
cd "$(dirname "$0")/../.."
mkdir -p output/apng
stage() { sleep 2; echo; echo "===== $1 ====="; }

stage "STAGE 1/5: build shared library from lodepng.cpp/h"
gcc -O3 -shared -fPIC -I. -x c lodepng.cpp -o output/apng/liblodepng.so
echo "built output/apng/liblodepng.so"
sleep 12

stage "STAGE 2/5: generate four APNG test cases"
python3 tests/apng/generate_cases.py
ls -la output/apng/*.png

stage "STAGE 3/5: read back frames, delays, loops and center pixels"
python3 tests/apng/verify.py
sleep 8

stage "STAGE 4/5: inspect chunk layout with in-repo tool pngdetail"
make pngdetail >/dev/null && ./pngdetail output/apng/two_frames.png | head -12

stage "STAGE 5/5: run lodepng unit tests (regression)"
make unittest >/dev/null && ./unittest | tail -2
echo; echo "ALL CHECKS PASSED"
