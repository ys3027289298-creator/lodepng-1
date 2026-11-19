# This makefile only makes the unit test, benchmark and pngdetail and showpng
# utilities. It does not make the PNG codec itself as shared or static library.
# That is because:
# LodePNG itself has only 1 source file (lodepng.cpp, can be renamed to
# lodepng.c) and is intended to be included as source file in other projects and
# their build system directly.


CC ?= gcc
CXX ?= g++

override CFLAGS := -W -Wall -Wextra -ansi -pedantic -O3 -Wno-unused-function $(CFLAGS)
override CXXFLAGS := -W -Wall -Wextra -ansi -pedantic -O3 $(CXXFLAGS)

all: unittest benchmark pngdetail showpng

%.o: %.cpp
	@mkdir -p `dirname $@`
	$(CXX) -I ./ $(CXXFLAGS) -c $< -o $@

unittest: lodepng.o lodepng_util.o lodepng_unittest.o
	$(CXX) $^ $(CXXFLAGS) -o $@

benchmark: lodepng.o lodepng_benchmark.o
	$(CXX) $^ $(CXXFLAGS) -lSDL2 -o $@

pngdetail: lodepng.o lodepng_util.o pngdetail.o
	$(CXX) $^ $(CXXFLAGS) -o $@

showpng: lodepng.o examples/example_sdl.o
	$(CXX) -I ./ $^ $(CXXFLAGS) -lSDL2 -o $@

# Multi-pass iterative encoder benchmark. Enable the block-parallel path with
# ENABLE_PTHREAD=1 (needs -pthread). Run via bench/run_bench.sh, which also
# generates the 12 test images and does the independent read-back checks.
MP_CXXFLAGS := -W -Wall -Wextra -O3 -std=c++11
ifeq ($(ENABLE_PTHREAD),1)
MP_CXXFLAGS += -DLODEPNG_USE_PTHREAD -pthread
endif

bench/lodepng_mp.o: lodepng.cpp lodepng.h
	@mkdir -p bench
	$(CXX) -I ./ $(MP_CXXFLAGS) -c lodepng.cpp -o $@

multipass_bench: bench/lodepng_mp.o bench/bench_multipass.o
	$(CXX) -I ./ $^ $(MP_CXXFLAGS) -o $@

bench/%.o: bench/%.cpp
	@mkdir -p `dirname $@`
	$(CXX) -I ./ $(MP_CXXFLAGS) -c $< -o $@

clean:
	rm -f unittest benchmark pngdetail showpng multipass_bench lodepng_unittest.o lodepng_benchmark.o lodepng.o lodepng_util.o pngdetail.o examples/example_sdl.o bench/*.o
