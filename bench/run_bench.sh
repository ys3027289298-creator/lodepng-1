#!/usr/bin/env bash
# Build and run the full multi-pass encoder benchmark + independent verification.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMGDIR="$ROOT/artifacts/images"
BENCHDIR="$ROOT/artifacts/bench"
JOBS="${1:-8}"
REPEATS="${2:-3}"

mkdir -p "$IMGDIR" "$BENCHDIR"

echo "== generating 12 raw test images =="
python3 -I "$ROOT/bench/gen_images.py" "$IMGDIR" >/dev/null

echo "== building benchmark (pthread enabled) =="
g++ -W -Wall -Wextra -O3 -std=c++11 -DLODEPNG_USE_PTHREAD -pthread -I "$ROOT" \
    "$ROOT/lodepng.cpp" "$ROOT/bench/bench_multipass.cpp" \
    -o "$BENCHDIR/bench_multipass"

echo "== running benchmark: baseline vs mp-serial vs mp-parallel($JOBS), $REPEATS repeats =="
"$BENCHDIR/bench_multipass" "$IMGDIR/manifest.txt" "$BENCHDIR" "$JOBS" "$REPEATS"

echo
echo "== independent standard-decoder read-back (python zlib) =="
python3 -I "$ROOT/bench/verify_pngs.py" "$IMGDIR" "$BENCHDIR"
