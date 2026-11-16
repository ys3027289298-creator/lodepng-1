#!/bin/bash
# One-shot build + verification for the TIFF extension.
# Paths are resolved relative to this script; run from anywhere.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SAMPLES="$HERE/samples"
BIN="$HERE/bin"

mkdir -p "$BIN"

# 1. (re)generate the small samples with the stdlib-only generator
python3 -I "$HERE/tiff_make.py" "$SAMPLES"

# 2. build lodepng with the harness's instrumented allocators
CXX="${CXX:-g++}"
"$CXX" -I "$ROOT" -DLODEPNG_NO_COMPILE_ALLOCATORS -W -Wall -Wextra -ansi -pedantic -O3 \
    -c "$ROOT/lodepng.cpp" -o "$BIN/lodepng_instr.o"
"$CXX" -I "$ROOT" -W -Wall -Wextra -ansi -pedantic -O3 \
    "$BIN/lodepng_instr.o" "$HERE/tiff_verify.cpp" -lrt -o "$BIN/tiff_verify"

# 3. build a normal (default allocator) variant of the library too
"$CXX" -I "$ROOT" -W -Wall -Wextra -ansi -pedantic -O3 \
    -c "$ROOT/lodepng.cpp" -o "$BIN/lodepng.o"

# 4. run verification, machine-readable log
"$BIN/tiff_verify" "$SAMPLES" "$SAMPLES" | tee "$HERE/verify.log"

# 5. independent pixel cross-check with Pillow/libtiff (dev-time oracle only;
#    lodepng itself gains no dependency). PIL is not importable under -I here,
#    so this step runs a normal interpreter and is allowed to be absent.
if python3 -c "import PIL" 2>/dev/null; then
  python3 "$HERE/tiff_check.py" "$SAMPLES" | tee "$HERE/crosscheck.log"
else
  echo "Pillow not available; skipping third-party cross-check" | tee "$HERE/crosscheck.log"
fi
