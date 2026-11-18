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

all: unittest benchmark pngdetail showpng premultiply16_test

%.o: %.cpp
	@mkdir -p `dirname $@`
	$(CXX) -I ./ $(CXXFLAGS) -c $< -o $@

premultiply16_test: premultiply16.o premultiply16_test.o
	$(CXX) premultiply16.o premultiply16_test.o $(CXXFLAGS) -o $@

premultiply16_test.o: premultiply16_test.cpp premultiply16.h
	$(CXX) -I ./ $(CXXFLAGS) -Wsign-conversion -Wshadow -c premultiply16_test.cpp -o $@

premultiply16.o: premultiply16.cpp premultiply16.h
	$(CXX) -I ./ $(CXXFLAGS) -Wsign-conversion -Wshadow -c premultiply16.cpp -o $@

unittest: lodepng.o lodepng_util.o lodepng_unittest.o
	$(CXX) $^ $(CXXFLAGS) -o $@

benchmark: lodepng.o lodepng_benchmark.o
	$(CXX) $^ $(CXXFLAGS) -lSDL2 -o $@

pngdetail: lodepng.o lodepng_util.o pngdetail.o
	$(CXX) $^ $(CXXFLAGS) -o $@

showpng: lodepng.o examples/example_sdl.o
	$(CXX) -I ./ $^ $(CXXFLAGS) -lSDL2 -o $@

clean:
	rm -f unittest benchmark pngdetail showpng premultiply16_test lodepng_unittest.o lodepng_benchmark.o lodepng.o lodepng_util.o pngdetail.o premultiply16.o premultiply16_test.o examples/example_sdl.o
