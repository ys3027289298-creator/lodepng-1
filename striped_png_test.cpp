/*
 * striped_png_test.cpp - correctness tests and benchmark for the SPNG1
 * striped PNG container (striped_png.h / striped_png.cpp).
 *
 * Part 1: correctness tests. Any failure prints a message and makes the
 *         process exit nonzero.
 * Part 2: benchmark on four small, self-contained synthetic image classes
 *         (photo, screenshot, gradient, noise): striped vs single-stream
 *         file sizes with bloat reporting, parallel decode speedup and the
 *         cost of decoding a single strip.
 *
 * Build (see run_striped_tests.sh):
 *   g++ -std=c++11 -O2 -pthread -I. striped_png_test.cpp \
 *       striped_png.cpp lodepng.cpp -o striped_png_test
 * No third-party libraries and no network access are used.
 */

#include "striped_png.h"
#include "lodepng.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <string>
#include <iostream>
#include <thread>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
  ++g_checks; \
  if(!(cond)) { \
    ++g_failures; \
    std::cout << "CHECK FAILED (" << __FILE__ << ":" << __LINE__ \
              << "): " #cond << std::endl; \
  } \
} while(0)

static std::vector<unsigned char> make_rgba(unsigned w, unsigned h) {
  return std::vector<unsigned char>((size_t)w * h * 4u, 0);
}

/* Deterministic LCG so every run produces identical images and numbers. */
static unsigned lcg_next(unsigned& state) {
  state = state * 1103515245u + 12345u;
  return (state >> 16) & 0x7fff;
}

static unsigned char clamp_byte(int v) {
  if(v < 0) v = 0;
  if(v > 255) v = 255;
  return (unsigned char)v;
}

/* --- four image classes, all 8-bit RGBA, tightly packed rows --- */

static void gen_photo(std::vector<unsigned char>& img,
                      unsigned w, unsigned h) {
  double cx = w / 2.0, cy = h / 2.0;
  for(unsigned y = 0; y < h; ++y) {
    for(unsigned x = 0; x < w; ++x) {
      double dx = x - cx, dy = y - cy;
      double v = std::sin(x * 0.05) + std::sin(y * 0.07)
               + std::sin((x + y) * 0.03)
               + std::sin(std::sqrt(dx * dx + dy * dy) * 0.11);
      unsigned seed = (x * 73856093u) ^ (y * 19349663u);
      double fine = ((int)(lcg_next(seed) & 31) - 15) / 255.0;
      v = v / 4.0 + fine;
      size_t p = ((size_t)y * w + x) * 4u;
      img[p]     = clamp_byte((int)(128 + 100 * v));
      img[p + 1] = clamp_byte((int)(110 + 90 * std::sin(v * 2.0)));
      img[p + 2] = clamp_byte((int)(130 + 80 * std::cos(v * 3.0)));
      img[p + 3] = 255;
    }
  }
}

static void gen_screenshot(std::vector<unsigned char>& img,
                           unsigned w, unsigned h) {
  static const unsigned char palette[][4] = {
    {240, 240, 244, 255}, {45, 62, 92, 255}, {255, 255, 255, 255},
    {70, 140, 220, 255}, {230, 90, 70, 255}, {60, 180, 120, 255},
    {20, 22, 28, 255},   {210, 210, 215, 255}
  };
  unsigned seed = 1234;
  for(unsigned y = 0; y < h; ++y) {
    for(unsigned x = 0; x < w; ++x) {
      /* flat rectangular regions with 1-pixel borders, like UI blocks */
      unsigned bx = x / 40, by = y / 24;
      unsigned c = (bx * 7 + by * 13 + (lcg_next(seed) % 3)) % 8;
      if(x % 40 == 0 || y % 24 == 0) c = 7;
      if(y < 18) c = 1; /* title bar */
      size_t p = ((size_t)y * w + x) * 4u;
      img[p] = palette[c][0]; img[p + 1] = palette[c][1];
      img[p + 2] = palette[c][2]; img[p + 3] = palette[c][3];
    }
  }
}

static void gen_gradient(std::vector<unsigned char>& img,
                         unsigned w, unsigned h) {
  for(unsigned y = 0; y < h; ++y) {
    for(unsigned x = 0; x < w; ++x) {
      size_t p = ((size_t)y * w + x) * 4u;
      img[p]     = (unsigned char)((unsigned long long)x * 255 / w);
      img[p + 1] = (unsigned char)((unsigned long long)y * 255 / h);
      img[p + 2] = (unsigned char)(((unsigned long long)x + y) * 255
                                   / (w + h));
      img[p + 3] = 255;
    }
  }
}

static void gen_noise(std::vector<unsigned char>& img,
                      unsigned /*w*/, unsigned /*h*/) {
  unsigned seed = 987654321u;
  for(size_t i = 0; i < img.size(); ++i) {
    img[i] = (unsigned char)(lcg_next(seed) & 0xff);
    if((i & 3u) == 3u) img[i] = 255; /* alpha */
  }
}

/* ---------- Part 1: correctness ---------- */

static void test_roundtrip() {
  const unsigned dims[][2] = {
    {1, 1}, {64, 64}, {65, 33}, {100, 100}, {200, 7}, {37, 129}
  };
  const unsigned heights[] = {1, 16, 32, 64, 1000};

  for(size_t d = 0; d < sizeof(dims) / sizeof(dims[0]); ++d) {
    unsigned w = dims[d][0], h = dims[d][1];
    std::vector<unsigned char> ref = make_rgba(w, h);
    gen_photo(ref, w, h);

    for(size_t k = 0; k < sizeof(heights) / sizeof(heights[0]); ++k) {
      unsigned sh = heights[k];
      std::vector<unsigned char> spng;
      unsigned err = stripedpng::encode(spng, ref, w, h,
                                        stripedpng::EncodeOptions(sh));
      CHECK(err == 0);

      stripedpng::Info info;
      CHECK(stripedpng::inspect(spng, info) == 0);
      CHECK(info.width == w && info.height == h);
      CHECK(info.strip_height == sh);
      CHECK(info.offsets.size() == info.num_strips + 1);
      CHECK(info.offsets.front() ==
            5u + 24u + (info.num_strips + 1u) * 8u);
      CHECK(info.offsets.back() == spng.size());

      /* every strip starts with a standalone PNG signature */
      for(unsigned s = 0; s < info.num_strips; ++s) {
        size_t o = (size_t)info.offsets[s];
        CHECK(spng[o] == 0x89 && spng[o + 1] == 'P' &&
              spng[o + 2] == 'N' && spng[o + 3] == 'G');
      }

      /* sequential and parallel full decodes must reproduce the input */
      std::vector<unsigned char> out_seq, out_par;
      unsigned ow, oh;
      CHECK(stripedpng::decode(out_seq, ow, oh, spng, 1) == 0);
      CHECK(ow == w && oh == h && out_seq == ref);
      CHECK(stripedpng::decode(out_par, ow, oh, spng, 0) == 0);
      CHECK(ow == w && oh == h && out_par == ref);

      /* random access: each strip must match its rows of the input */
      for(unsigned s = 0; s < info.num_strips; ++s) {
        std::vector<unsigned char> strip;
        unsigned sw, shh, row0;
        CHECK(stripedpng::decode_strip(strip, sw, shh, row0,
                                       spng, s) == 0);
        CHECK(sw == w);
        CHECK(row0 == s * info.strip_height);
        size_t expected_rows =
            std::min((unsigned)info.strip_height, h - row0);
        CHECK(shh == expected_rows);
        CHECK(strip.size() == expected_rows * (size_t)w * 4u);
        for(size_t i = 0; i < strip.size(); ++i) {
          CHECK(strip[i] == ref[(size_t)row0 * w * 4u + i]);
        }
      }

      /* out-of-range strip index is rejected */
      std::vector<unsigned char> dummy;
      unsigned a, b, c;
      CHECK(stripedpng::decode_strip(dummy, a, b, c, spng,
                                     info.num_strips)
            == stripedpng::ERR_STRIP_INDEX);
    }
  }
}

/* The embedded per-strip bytes must be byte-for-byte the untouched output
 * of lodepng's single-stream encoder for exactly those pixels. */
static void test_single_stream_byte_identical() {
  const unsigned w = 48, h = 40, sh = 16;
  std::vector<unsigned char> ref = make_rgba(w, h);
  gen_screenshot(ref, w, h);

  std::vector<unsigned char> full_png, full_png2;
  CHECK(lodepng::encode(full_png, ref, w, h) == 0);
  CHECK(lodepng::encode(full_png2, ref, w, h) == 0);
  CHECK(full_png == full_png2); /* encoder is deterministic */

  std::vector<unsigned char> spng;
  CHECK(stripedpng::encode(spng, ref, w, h,
                           stripedpng::EncodeOptions(sh)) == 0);
  stripedpng::Info info;
  CHECK(stripedpng::inspect(spng, info) == 0);
  CHECK(info.num_strips == 3); /* 16 + 16 + 8 rows */

  for(unsigned s = 0; s < info.num_strips; ++s) {
    unsigned row0 = s * sh;
    unsigned rows = std::min(sh, h - row0);
    std::vector<unsigned char> expect;
    CHECK(lodepng::encode(expect,
                          ref.data() + (size_t)row0 * w * 4u,
                          w, rows) == 0);
    size_t begin = (size_t)info.offsets[s];
    size_t end = (size_t)info.offsets[s + 1];
    CHECK(end - begin == expect.size());
    CHECK(std::equal(expect.begin(), expect.end(),
                     spng.begin() + begin));
  }

  /* the whole-image single stream must itself remain valid and decodable
   * to the identical pixels (its bytes are never altered by this module) */
  std::vector<unsigned char> dec;
  unsigned dw, dh;
  CHECK(lodepng::decode(dec, dw, dh, full_png) == 0);
  CHECK(dw == w && dh == h && dec == ref);
}

static void test_error_handling() {
  unsigned w = 32, h = 32;
  std::vector<unsigned char> ref = make_rgba(w, h);
  gen_gradient(ref, w, h);

  std::vector<unsigned char> spng;
  CHECK(stripedpng::encode(spng, ref, w, h,
                           stripedpng::EncodeOptions(8)) == 0);

  /* bad magic */
  std::vector<unsigned char> bad = spng;
  bad[0] = 'X';
  stripedpng::Info info;
  CHECK(stripedpng::inspect(bad, info) == stripedpng::ERR_BAD_MAGIC);

  /* truncated header */
  std::vector<unsigned char> short_hdr(spng.begin(),
                                       spng.begin() + 10);
  CHECK(stripedpng::inspect(short_hdr, info) ==
        stripedpng::ERR_TRUNCATED);

  /* truncated strip payload: offset table promises bytes that are gone */
  std::vector<unsigned char> cut = spng;
  cut.resize(cut.size() - 5);
  CHECK(stripedpng::inspect(cut, info) == stripedpng::ERR_BAD_HEADER);

  /* tampered offset table (duplicate zero-length strip) */
  std::vector<unsigned char> tampered = spng;
  size_t table = 5u + 24u;
  tampered[table + 8] = tampered[table];
  CHECK(stripedpng::inspect(tampered, info) ==
        stripedpng::ERR_BAD_HEADER);

  /* bad arguments */
  std::vector<unsigned char> out;
  unsigned ow, oh;
  CHECK(stripedpng::encode(out, ref, 0, h,
                           stripedpng::EncodeOptions(8))
        == stripedpng::ERR_BAD_ARGUMENT);
  CHECK(stripedpng::encode(out, ref, w, h,
                           stripedpng::EncodeOptions(0))
        == stripedpng::ERR_BAD_ARGUMENT);
  std::vector<unsigned char> wrong_size = ref;
  wrong_size.push_back(0);
  CHECK(stripedpng::encode(out, wrong_size, w, h,
                           stripedpng::EncodeOptions(8))
        == stripedpng::ERR_BAD_ARGUMENT);

  /* decode of a non-container fails cleanly */
  CHECK(stripedpng::decode(out, ow, oh, bad, 1)
        == stripedpng::ERR_BAD_MAGIC);
}

static void test_bloat_report() {
  /* 10% growth on 1000 bytes exceeds the 5% threshold */
  stripedpng::BloatReport r1 =
      stripedpng::check_bloat(1000, 1100, 512, 64, 5.0);
  CHECK(r1.delta_bytes == 100);
  CHECK(r1.bloat_flagged && !r1.strip_too_large);

  /* small overhead stays under the threshold */
  stripedpng::BloatReport r2 =
      stripedpng::check_bloat(100000, 100100, 512, 64, 5.0);
  CHECK(!r2.bloat_flagged && !r2.strip_too_large);

  /* strip height covering the whole image is reported as too large */
  stripedpng::BloatReport r3 =
      stripedpng::check_bloat(1000, 1060, 512, 512, 5.0);
  CHECK(r3.strip_too_large);

  /* striped file smaller than the single stream is never bloat */
  stripedpng::BloatReport r4 =
      stripedpng::check_bloat(1000, 990, 512, 64, 5.0);
  CHECK(!r4.bloat_flagged && r4.delta_bytes == -10);
}

/* ---------- Part 2: benchmark ---------- */

typedef void (*ImageGen)(std::vector<unsigned char>&, unsigned, unsigned);

static double now_ms() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
}

template <typename F>
static double time_best(int iters, F f) {
  for(int w = 0; w < 3; ++w) f(); /* warmup */
  double best = 1e300;
  for(int i = 0; i < iters; ++i) {
    double t0 = now_ms();
    f();
    double dt = now_ms() - t0;
    if(dt < best) best = dt;
  }
  return best;
}

static void run_benchmark() {
  const unsigned W = 512, H = 512;
  const int ITERS = 25;
  const unsigned strip_heights[] = {32, 64, 256, H};
  const unsigned timed_sh = 64; /* configuration used for decode timings */

  struct Case { const char* name; ImageGen gen; };
  Case cases[] = {
    {"photo",      gen_photo},
    {"screenshot", gen_screenshot},
    {"gradient",   gen_gradient},
    {"noise",      gen_noise}
  };

  unsigned hw = std::thread::hardware_concurrency();
  std::cout << "\n=== Benchmark (image " << W << "x" << H
            << " RGBA8, best of " << ITERS
            << " iterations, hardware_concurrency="
            << (hw ? hw : 0) << ") ===" << std::endl;

  for(size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ++ci) {
    std::vector<unsigned char> ref = make_rgba(W, H);
    cases[ci].gen(ref, W, H);

    std::vector<unsigned char> single;
    if(lodepng::encode(single, ref, W, H) != 0) {
      std::cout << "single-stream encode failed for "
                << cases[ci].name << std::endl;
      ++g_failures;
      continue;
    }

    std::cout << "\n[" << cases[ci].name << "]\n"
              << "  size comparison (single-stream PNG = "
              << single.size() << " bytes):\n"
              << "    strip_h  strips  striped_bytes  delta_bytes  "
                 "delta_%   report\n";

    std::vector<unsigned char> timed_file;
    for(size_t k = 0; k < sizeof(strip_heights) / sizeof(strip_heights[0]);
        ++k) {
      unsigned sh = strip_heights[k];
      std::vector<unsigned char> spng;
      unsigned err = stripedpng::encode(spng, ref, W, H,
                                        stripedpng::EncodeOptions(sh));
      CHECK(err == 0);
      stripedpng::Info info;
      CHECK(stripedpng::inspect(spng, info) == 0);

      stripedpng::BloatReport br =
          stripedpng::check_bloat((unsigned long long)single.size(),
                                  (unsigned long long)spng.size(),
                                  H, sh, 5.0);
      std::printf("    %7u  %6u  %13zu  %11lld  %7.2f%%  %s\n",
                  sh, info.num_strips, spng.size(),
                  (long long)br.delta_bytes, br.delta_percent,
                  br.message.c_str());
      if(sh == timed_sh) timed_file = spng;
    }

    stripedpng::Info tinfo;
    CHECK(stripedpng::inspect(timed_file, tinfo) == 0);
    unsigned workers = hw ? hw : 4;
    if(workers > tinfo.num_strips) workers = tinfo.num_strips;

    std::vector<unsigned char> dec;
    unsigned dw, dh;
    double t_seq = time_best(ITERS, [&]() {
      CHECK(stripedpng::decode(dec, dw, dh, timed_file, 1) == 0);
    });
    double t_par = time_best(ITERS, [&]() {
      CHECK(stripedpng::decode(dec, dw, dh, timed_file, 0) == 0);
    });
    CHECK(dw == W && dh == H && dec == ref);

    unsigned mid = tinfo.num_strips / 2;
    std::vector<unsigned char> one;
    unsigned sw, shh, row0;
    double t_one = time_best(ITERS, [&]() {
      CHECK(stripedpng::decode_strip(one, sw, shh, row0,
                                     timed_file, mid) == 0);
    });
    CHECK(sw == W && row0 == mid * timed_sh);

    std::printf("  decode timings (strip_h=%u, %u strips, %u workers):\n"
                "    full sequential : %8.3f ms\n"
                "    full parallel   : %8.3f ms   speedup %.2fx\n"
                "    single strip %2u : %8.3f ms   "
                "(%.1f%% of sequential full decode)\n",
                timed_sh, tinfo.num_strips, workers,
                t_seq, t_par, t_seq / t_par, mid, t_one,
                100.0 * t_one / t_seq);
  }
}

int main() {
  std::cout << "=== SPNG1 striped PNG correctness tests ===" << std::endl;
  test_roundtrip();
  test_single_stream_byte_identical();
  test_error_handling();
  test_bloat_report();

  if(g_failures) {
    std::cout << "\n" << g_failures << " FAILURE(S) among "
              << g_checks << " checks" << std::endl;
    return 1;
  }
  std::cout << "all " << g_checks
            << " correctness checks passed" << std::endl;

  run_benchmark();

  std::cout << "\nstriped PNG tests successful" << std::endl;
  return 0;
}
