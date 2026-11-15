/*
LodePNG Examples

Copyright (c) 2005-2026 Lode Vandevenne

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software
    in a product, an acknowledgment in the product documentation would be
    appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.

    3. This notice may not be removed or altered from any source
    distribution.
*/

/*
Pixel-art color reduction example.

Build (no extra dependencies):
  g++ lodepng.cpp examples/example_quantize.cpp -ansi -pedantic -Wall -Wextra -O3

It generates a small self-made gradient image (gradients show color banding
clearly after reducing the number of color levels), then runs the three
quantization modes of lodepng_quantize on it with the same per-channel level
count and prints, for each mode:
  - number of distinct colors (color levels effectively used),
  - raw output size and PNG-encoded size (volume),
  - mean absolute and root mean square error against the original image.
It also demonstrates that identical parameters reproduce identical pixels and
that illegal levels / strength / dimensions return an error without touching
the output.
*/

#include <iostream>
#include <vector>

#include "lodepng.h"

static const unsigned W = 12;
static const unsigned H = 9;
static const unsigned CHANNELS = 4;

static void makeSample(std::vector<unsigned char>& image) {
  image.resize((size_t)W * H * CHANNELS);
  for(unsigned y = 0; y < H; y++) {
    for(unsigned x = 0; x < W; x++) {
      size_t i = ((size_t)y * W + x) * CHANNELS;
      image[i + 0] = (unsigned char)(x * 255 / (W - 1));
      image[i + 1] = (unsigned char)(y * 255 / (H - 1));
      image[i + 2] = (unsigned char)((x + y) * 255 / (W + H - 2));
      image[i + 3] = (unsigned char)(x * 255 / (W - 1));
    }
  }
}

static void printRow(const char* name, const std::vector<unsigned char>& image,
                     const std::vector<unsigned char>& out,
                     const LodePNGQuantizeStats& stats) {
  std::vector<unsigned char> png;
  unsigned png_error = lodepng::encode(png, out, W, H, LCT_RGBA, 8);
  if(png_error) {
    std::cout << name << ": png encode error " << png_error << std::endl;
    return;
  }
  std::cout << name
            << "\n  colors=" << stats.distinct_colors
            << " raw_bytes=" << stats.output_size
            << " png_bytes=" << png.size()
            << "\n  mae/1000 (r,g,b)=" << stats.mae_milli[0] << ","
            << stats.mae_milli[1] << "," << stats.mae_milli[2]
            << "  rmse/1000 (r,g,b)=" << stats.rmse_milli[0] << ","
            << stats.rmse_milli[1] << "," << stats.rmse_milli[2]
            << "\n  input_bytes=" << image.size() << std::endl;
}

int main() {
  std::vector<unsigned char> image;
  makeSample(image);

  const unsigned levels = 4;
  const unsigned strength = 100;
  std::vector<unsigned char> none_out, ordered_out, fs_out;
  LodePNGQuantizeStats none_stats, ordered_stats, fs_stats;

  unsigned e0 = lodepng::quantize(none_out, image, W, H, CHANNELS, levels,
                                  LQ_NONE, strength, &none_stats);
  unsigned e1 = lodepng::quantize(ordered_out, image, W, H, CHANNELS, levels,
                                  LQ_ORDERED, strength, &ordered_stats);
  unsigned e2 = lodepng::quantize(fs_out, image, W, H, CHANNELS, levels,
                                  LQ_FS, strength, &fs_stats);
  if(e0 || e1 || e2) {
    std::cout << "quantize failed: " << e0 << " " << e1 << " " << e2
              << std::endl;
    return 1;
  }

  std::cout << "sample " << W << "x" << H << " RGBA, levels=" << levels
            << " strength=" << strength << "%" << std::endl;
  printRow("LUT only        ", image, none_out, none_stats);
  printRow("LUT+ordered     ", image, ordered_out, ordered_stats);
  printRow("LUT+error-diff  ", image, fs_out, fs_stats);

  /*Reproducibility: run ordered dithering again with the same arguments.*/
  std::vector<unsigned char> repeat;
  if(lodepng::quantize(repeat, image, W, H, CHANNELS, levels, LQ_ORDERED,
                       strength) != 0 || repeat != ordered_out) {
    std::cout << "reproducibility check FAILED" << std::endl;
    return 1;
  }
  std::cout << "reproducibility: identical pixels on repeated run" << std::endl;

  /*Failure paths.*/
  std::vector<unsigned char> bad;
  unsigned bad_levels = lodepng::quantize(bad, image, W, H, CHANNELS, 1,
                                          LQ_NONE, strength);
  unsigned bad_strength = lodepng::quantize(bad, image, W, H, CHANNELS, levels,
                                            LQ_ORDERED, 101);
  unsigned bad_dims = lodepng::quantize(bad, image, 0, H, CHANNELS, levels,
                                        LQ_NONE, strength);
  std::cout << "failure paths -> levels=1:" << bad_levels
            << " strength=101:" << bad_strength
            << " width=0:" << bad_dims << std::endl;
  if(bad_levels != 124 || bad_strength != 125 || bad_dims != 126) return 1;

  return 0;
}
