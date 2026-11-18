/*
Tests for the 16-bit premultiply / unpremultiply fixed point round trip.

Copyright (c) 2005-2026 Lode Vandevenne

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software in a
    product, an acknowledgment in the product documentation would be
    appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.

    3. This notice may not be removed or altered from any source
    distribution.
*/

// Build (default tests):
// g++ -I ./ premultiply16.cpp premultiply16_test.cpp -W -Wall -Wextra
//     -ansi -pedantic -O3 -o premultiply16_test && ./premultiply16_test
// Full exhaustive sweep over all 2^32 (color, alpha) pairs:
//   ./premultiply16_test --exhaustive

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "premultiply16.h"

static int g_failures = 0;
static int g_checks = 0;

static void check(bool condition, const char* message) {
  g_checks++;
  if (!condition) {
    g_failures++;
    std::printf("FAIL: %s\n", message);
  }
}

/* Deterministic xorshift32 so test runs are reproducible. */
static unsigned rng_next(unsigned& state) {
  unsigned x = state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state = x;
  return x;
}

static unsigned short sample16(unsigned x) {
  return (unsigned short)(x & PM16_MAX_VALUE);
}

/* The naive integer pipeline whose dark-value crushing motivates this work:
   premultiply and unpremultiply are plain truncating 16-bit divisions. */
static unsigned naive_premultiply(unsigned c, unsigned a) {
  return (unsigned)(((uint64_t)c * a) / PM16_MAX_VALUE);
}
static unsigned naive_unpremultiply(unsigned p, unsigned a) {
  if (a == 0) return p; /* convention: color copied through */
  unsigned r = (unsigned)(((uint64_t)p * PM16_MAX_VALUE) / a);
  return r > PM16_MAX_VALUE ? PM16_MAX_VALUE : r;
}

/* Naive truncation loses all fraction bits: show the shadow crushing. */
static void test_naive_dark_crushing() {
  unsigned max_naive_error = 0;
  unsigned worst_c = 0, worst_a = 0;
  unsigned c, a;
  for (a = 1; a <= PM16_MAX_VALUE; a += 7) {
    for (c = 0; c <= PM16_MAX_VALUE; c += 131) {
      unsigned p = naive_premultiply(c, a);
      unsigned back = naive_unpremultiply(p, a);
      unsigned error = (c >= back) ? (c - back) : (back - c);
      if (error > max_naive_error) {
        max_naive_error = error;
        worst_c = c;
        worst_a = a;
      }
    }
  }
  std::printf("[naive] max round trip error of truncating 16-bit pipeline: "
              "%u LSB (c=%u, a=%u)\n", max_naive_error, worst_c, worst_a);
  check(max_naive_error > 1,
        "naive pipeline should demonstrate multi-LSB shadow loss");
}

/* Channel-level edge cases and Q16 invariants. */
static void test_channel_edges() {
  unsigned c, a;
  for (c = 0; c <= PM16_MAX_VALUE; c++) {
    /* Opaque and fully transparent must be exact for every color. */
    check(pm16_unpremultiply_channel(
              pm16_premultiply_channel(c, PM16_MAX_VALUE),
              PM16_MAX_VALUE) == c,
          "opaque alpha must round trip exactly");
    check(pm16_unpremultiply_channel(
              pm16_premultiply_channel(c, 0), 0) == c,
          "transparent pixel color must be preserved");
    check(pm16_premultiply_channel(c, 0) == (uint32_t)c * PM16_ONE_Q16,
          "transparent premultiplied Q16 stores the color");
    check(pm16_premultiply_channel(c, PM16_MAX_VALUE)
              == (uint32_t)c * PM16_ONE_Q16,
          "opaque premultiplied Q16 equals the color");
  }
  for (a = 0; a <= PM16_MAX_VALUE; a++) {
    /* Black stays black; the intermediate value stays in range. */
    check(pm16_premultiply_channel(0, a) == 0, "black premultiplies to zero");
    check(pm16_premultiply_channel(PM16_MAX_VALUE, a) <= PM16_MAX_Q16,
          "Q16 premultiplied value out of range");
  }
}

/* Structured sweep: edge alphas and colors plus dense alpha steps. */
static void test_structured_sweep(unsigned* max_error_out) {
  unsigned max_error = 0;
  unsigned c, a;
  const unsigned alpha_steps[] = {0, 1, 2, 3, 4, 7, 15, 16, 31, 255, 256,
                                  1024, 4096, 8192, 32767, 32768,
                                  60000, 65534, 65535};
  size_t k;
  for (k = 0; k < sizeof(alpha_steps) / sizeof(alpha_steps[0]); k++) {
    a = alpha_steps[k];
    for (c = 0; c <= PM16_MAX_VALUE; c++) {
      uint32_t q = pm16_premultiply_channel(c, a);
      unsigned back = pm16_unpremultiply_channel(q, a);
      unsigned error = (c >= back) ? (c - back) : (back - c);
      check(q <= PM16_MAX_Q16, "Q16 value exceeds 65535 * 65536");
      check(error <= 1, "round trip error exceeds 1 LSB");
      if (error > max_error) max_error = error;
    }
  }
  /* All colors at a few dense low-alpha points, where crushing is worst. */
  for (a = 1; a <= 64; a++) {
    for (c = 0; c <= PM16_MAX_VALUE; c += 3) {
      unsigned back = pm16_unpremultiply_channel(
          pm16_premultiply_channel(c, a), a);
      unsigned error = (c >= back) ? (c - back) : (back - c);
      check(error <= 1, "low-alpha round trip error exceeds 1 LSB");
      if (error > max_error) max_error = error;
    }
  }
  *max_error_out = max_error;
  std::printf("[sweep] structured sweep max round trip error: %u LSB\n",
              max_error);
}

/* Randomized fuzzing over the full 16-bit input domain. */
static void test_random_fuzz(unsigned pairs, unsigned seed,
                             unsigned* max_error_out) {
  unsigned state = seed;
  unsigned max_error = 0;
  unsigned i;
  for (i = 0; i < pairs; i++) {
    unsigned c = sample16(rng_next(state));
    unsigned a = sample16(rng_next(state));
    uint32_t q = pm16_premultiply_channel(c, a);
    unsigned back = pm16_unpremultiply_channel(q, a);
    unsigned error = (c >= back) ? (c - back) : (back - c);
    check(error <= 1, "random round trip error exceeds 1 LSB");
    if (error > max_error) max_error = error;
  }
  *max_error_out = max_error;
  std::printf("[fuzz]  %u random pairs, max round trip error: %u LSB\n",
              pairs, max_error);
}

/* Exhaustive sweep over every (color, alpha) pair: 2^32 channel round trips.
   Only run when requested; takes on the order of ten seconds at -O3. */
static void test_exhaustive() {
  unsigned max_error = 0;
  unsigned worst_c = 0, worst_a = 0;
  unsigned a;
  std::printf("[exhaustive] sweeping all 65536 * 65536 pairs...\n");
  for (a = 0; a <= PM16_MAX_VALUE; a++) {
    unsigned c;
    for (c = 0; c <= PM16_MAX_VALUE; c++) {
      unsigned back = pm16_unpremultiply_channel(
          pm16_premultiply_channel(c, a), a);
      unsigned error = (c >= back) ? (c - back) : (back - c);
      check(error <= 1, "exhaustive round trip error exceeds 1 LSB");
      if (error > max_error) {
        max_error = error;
        worst_c = c;
        worst_a = a;
      }
    }
    if ((a & 8191u) == 0) {
      std::printf("  alpha %u / 65535...\n", a);
      std::fflush(stdout);
    }
  }
  std::printf("[exhaustive] complete. max round trip error over all 2^32 "
              "pairs: %u LSB (worst c=%u a=%u)\n",
              max_error, worst_c, worst_a);
}

/* Four required 1M-pixel datasets. */
enum DatasetKind {
  DS_TRANSPARENT = 0, /* alpha 0 everywhere, varied colors */
  DS_HALF = 1,        /* alpha around 32768 */
  DS_LOW_ALPHA = 2,   /* alpha 1..32, the shadow-crushing regime */
  DS_HIGH_CONTRAST = 3/* adjacent pixels alternate 0 / 65535 on all channels */
};

static void fill_dataset(std::vector<unsigned short>& pixels,
                         DatasetKind kind, unsigned seed) {
  size_t n = pixels.size() / 4;
  size_t i;
  unsigned state = seed;
  for (i = 0; i < n; i++) {
    unsigned r = sample16(rng_next(state));
    unsigned g = sample16(rng_next(state));
    unsigned b = sample16(rng_next(state));
    unsigned a;
    switch (kind) {
      case DS_TRANSPARENT:
        a = 0;
        break;
      case DS_HALF:
        a = 32768u + (rng_next(state) & 15u); /* 32768..32783 */
        break;
      case DS_LOW_ALPHA:
        a = 1u + (rng_next(state) % 32u); /* 1..32 */
        break;
      default: /* DS_HIGH_CONTRAST */
        a = (i & 1u) ? PM16_MAX_VALUE : 0u;
        r = g = b = (i & 1u) ? PM16_MAX_VALUE : 0u;
        break;
    }
    pixels[4 * i + 0] = (unsigned short)r;
    pixels[4 * i + 1] = (unsigned short)g;
    pixels[4 * i + 2] = (unsigned short)b;
    pixels[4 * i + 3] = (unsigned short)a;
  }
}

static void run_dataset(const char* name, DatasetKind kind,
                        size_t pixel_count, unsigned seed) {
  std::vector<unsigned short> straight(4 * pixel_count);
  std::vector<unsigned short> reconstructed(4 * pixel_count);
  std::vector<uint32_t> mid(4 * pixel_count);
  PM16Stats stats;
  clock_t start, elapsed;
  size_t i;
  double milliseconds;

  fill_dataset(straight, kind, seed);

  start = std::clock();
  pm16_premultiply_rgba16(&straight[0], &mid[0], pixel_count);
  pm16_unpremultiply_rgba16(&mid[0], &reconstructed[0], pixel_count);
  elapsed = std::clock() - start;
  milliseconds = (double)elapsed * 1000.0 / (double)CLOCKS_PER_SEC;

  pm16_roundtrip_rgba16(&straight[0], &mid[0], &reconstructed[0],
                        pixel_count, &stats);

  /* Transparent pixels must keep their color channels byte for byte. */
  for (i = 0; i < pixel_count; i++) {
    if (straight[4 * i + 3] == 0) {
      check(reconstructed[4 * i + 0] == straight[4 * i + 0],
            "transparent red channel not preserved");
      check(reconstructed[4 * i + 1] == straight[4 * i + 1],
            "transparent green channel not preserved");
      check(reconstructed[4 * i + 2] == straight[4 * i + 2],
            "transparent blue channel not preserved");
    }
    check(reconstructed[4 * i + 3] == straight[4 * i + 3],
          "alpha channel changed during round trip");
  }

  std::printf("%-14s pixels=%u  time=%7.2f ms  max_error=%u LSB  "
              "changed_channels=%u/%u  worst(c=%u,a=%u)\n",
              name, (unsigned)pixel_count, milliseconds,
              stats.max_abs_error,
              stats.error_channels,
              (unsigned)stats.total,
              stats.worst_color, stats.worst_alpha);

  check(stats.max_abs_error <= 1,
        "dataset round trip max error exceeds 1 LSB");
  check(milliseconds <= 800.0,
        "1M pixel round trip slower than 800 ms");
}

int main(int argc, char** argv) {
  const size_t pixel_count = 1000000;
  bool exhaustive = false;
  int i;

  for (i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--exhaustive") exhaustive = true;
  }

  std::printf("=== premultiply16 fixed point round trip tests ===\n");
  std::printf("Q16 intermediate: %u fractional bits, 32-bit container\n\n",
              PM16_FRAC_BITS);

  test_naive_dark_crushing();
  test_channel_edges();

  {
    unsigned sweep_error = 0;
    unsigned fuzz_error = 0;
    test_structured_sweep(&sweep_error);
    test_random_fuzz(5000000, 0x12345678u, &fuzz_error);
    check(sweep_error <= 1, "sweep error bound");
    check(fuzz_error <= 1, "fuzz error bound");
  }

  std::printf("\n--- four datasets, %u pixels each, budget 800 ms ---\n",
              (unsigned)pixel_count);
  run_dataset("transparent", DS_TRANSPARENT, pixel_count, 0xA11CEu);
  run_dataset("half", DS_HALF, pixel_count, 0xB0B55u);
  run_dataset("low_alpha", DS_LOW_ALPHA, pixel_count, 0x10A1u);
  run_dataset("high_contrast", DS_HIGH_CONTRAST, pixel_count, 0xC0FFEEu);

  if (exhaustive) {
    test_exhaustive();
  } else {
    std::printf("\n(run with --exhaustive for the full 2^32 pair sweep)\n");
  }

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
