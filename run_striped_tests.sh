#!/bin/bash
#
# run_striped_tests.sh - one-shot runner for the striped PNG work.
#
# 1. Builds and runs the repository's existing unit test suite unchanged.
#    Any failure aborts the run (exit nonzero => task incomplete).
# 2. Builds the new striped-container module + its test/benchmark without
#    touching the Makefile or any existing source file, then runs it.
#
# No third-party libraries and no network access are required: only the
# system C++ compiler, the C++ standard library (incl. pthread) and the
# files already present in this repository. Build artifacts go to
# artifacts/ so the repository root stays clean.

set -euo pipefail

cd "$(dirname "$0")"
REPO_ROOT="$(pwd)"
BUILD_DIR="$REPO_ROOT/artifacts/striped_build"
LOG="$REPO_ROOT/artifacts/striped_test.log"
mkdir -p "$BUILD_DIR"

# Always remove the Makefile's build artifacts afterwards so only sources
# and the artifacts/ logs remain in the working tree.
trap 'make clean >/dev/null 2>&1 || true' EXIT

CXX="${CXX:-g++}"
# The new module is built with strict warnings. lodepng.cpp is upstream code
# that must not be modified, so it gets the same warning level the project's
# own Makefile uses (it does not pass -Wconversion and would warn under it).
STRICT_FLAGS="-std=c++11 -Wall -Wextra -pedantic -Wshadow -Wconversion -O2 -pthread -I$REPO_ROOT"
UPSTREAM_FLAGS="-std=c++11 -Wall -Wextra -pedantic -O2 -pthread -I$REPO_ROOT"

{
  echo "################################################################"
  echo "# Step 1/3: repository's existing unit tests (make clean+unittest)"
  echo "################################################################"
  make clean >/dev/null
  make -j"$(nproc)" unittest
  echo "--- running ./unittest ---"
  ./unittest
  echo

  echo "################################################################"
  echo "# Step 2/3: build striped PNG module + tests into artifacts/"
  echo "################################################################"
  # lodepng.cpp is compiled separately (unchanged source, C++11 mode).
  "$CXX" $UPSTREAM_FLAGS -c "$REPO_ROOT/lodepng.cpp"       -o "$BUILD_DIR/lodepng.o"
  "$CXX" $STRICT_FLAGS -c "$REPO_ROOT/striped_png.cpp"   -o "$BUILD_DIR/striped_png.o"
  "$CXX" $STRICT_FLAGS -c "$REPO_ROOT/striped_png_test.cpp" -o "$BUILD_DIR/striped_png_test.o"
  "$CXX" $STRICT_FLAGS \
      "$BUILD_DIR/striped_png_test.o" \
      "$BUILD_DIR/striped_png.o" \
      "$BUILD_DIR/lodepng.o" \
      -lpthread -o "$BUILD_DIR/striped_png_test"
  echo "build OK"
  echo

  echo "################################################################"
  echo "# Step 3/3: striped PNG correctness tests + benchmark"
  echo "################################################################"
  "$BUILD_DIR/striped_png_test"
} 2>&1 | tee "$LOG"

# Propagate failure even through the pipe.
status=${PIPESTATUS[0]}
if [ "$status" -ne 0 ]; then
  echo "FAILED (exit $status); see $LOG" >&2
  exit "$status"
fi
echo
echo "ALL TESTS PASSED (log saved to $LOG)"
