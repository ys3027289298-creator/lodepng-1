#!/bin/sh
# Builds and runs the PNG filter strategy tests, then the repository's own unit test.
# Only the standard C++ toolchain and the sources in this repository are used.
#
# Usage: ./filter_strategy_test.sh
#
# Set CXX to choose the compiler (default g++). All temporary objects are put in a
# fresh temporary directory so the working tree is left clean.

set -u

CXX="${CXX:-g++}"
CXXFLAGS="-I. -W -Wall -Wextra -ansi -pedantic -O2"
TMPDIR_BUILD="${TMPDIR:-/tmp}/lodepng_filter_strategy_test.$$"

fail=0

cleanup() {
  rm -rf "$TMPDIR_BUILD"
}
trap cleanup EXIT INT TERM

mkdir -p "$TMPDIR_BUILD" || exit 1

echo "== building filter strategy test (with fault injection hook) =="
# The hook is guarded by LODEPNG_TEST_FILTER_INTERFACE and is not part of the
# lodepng public interface; it only exists so the test can simulate a miswired
# strategy interface.
$CXX $CXXFLAGS -DLODEPNG_TEST_FILTER_INTERFACE -c lodepng.cpp -o "$TMPDIR_BUILD/lodepng_test.o" || fail=1
$CXX $CXXFLAGS -c lodepng_util.cpp -o "$TMPDIR_BUILD/lodepng_util.o" || fail=1
$CXX $CXXFLAGS -DLODEPNG_TEST_FILTER_INTERFACE filter_strategy_test.cpp \
  "$TMPDIR_BUILD/lodepng_test.o" "$TMPDIR_BUILD/lodepng_util.o" \
  -o "$TMPDIR_BUILD/filter_strategy_test" || fail=1

if [ "$fail" -ne 0 ]; then
  echo "*** BUILD FAILED ***"
  exit 1
fi

echo
echo "== running filter strategy test =="
"$TMPDIR_BUILD/filter_strategy_test" || fail=1

echo
echo "== building and running the repository unit test =="
if command -v make >/dev/null 2>&1; then
  make unittest || fail=1
  if [ -x ./unittest ]; then
    ./unittest || fail=1
  else
    fail=1
  fi
else
  echo "make not available, skipping repository unit test"
  fail=1
fi

echo
if [ "$fail" -ne 0 ]; then
  echo "*** FILTER STRATEGY TEST RUN FAILED ***"
  exit 1
fi
echo "*** ALL FILTER STRATEGY TESTS PASSED ***"
exit 0
