/*Shared helpers for the PNG filter strategy tests.

This header only depends on the C++ standard library and lodepng.h. It provides
five small synthetic test images (photo, screenshot, line art, gradient, alpha)
and a self-contained SHA-256 implementation so that the tests can compare the
exact encoded bytes without any third-party dependency.
*/

#ifndef FILTER_STRATEGY_TEST_COMMON_H
#define FILTER_STRATEGY_TEST_COMMON_H

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "lodepng.h"

struct FilterTestSample {
  std::string name;
  unsigned w;
  unsigned h;
  std::vector<unsigned char> rgba; /*w * h * 4, 8 bit per channel*/
};

/*Simple deterministic 32-bit LCG, so every platform generates the same samples.*/
static unsigned filterTestRand(unsigned& seed) {
  seed = seed * 1664525u + 1013904223u;
  return seed;
}

static unsigned char filterTestClamp(int v) {
  if(v < 0) return 0;
  if(v > 255) return 255;
  return (unsigned char)v;
}

/*Bilinear sample of a small grid, used to make a smooth "photo-like" image.*/
static unsigned char filterTestBilinear(const std::vector<unsigned char>& grid,
                                        unsigned gw, unsigned gh,
                                        unsigned x, unsigned y,
                                        unsigned w, unsigned h) {
  double fx = (double)x * (double)(gw - 1) / (double)(w > 1 ? w - 1 : 1);
  double fy = (double)y * (double)(gh - 1) / (double)(h > 1 ? h - 1 : 1);
  unsigned x0 = (unsigned)fx;
  unsigned y0 = (unsigned)fy;
  unsigned x1 = x0 + 1 < gw ? x0 + 1 : x0;
  unsigned y1 = y0 + 1 < gh ? y0 + 1 : y0;
  double ax = fx - (double)x0;
  double ay = fy - (double)y0;
  double v00 = grid[(size_t)y0 * gw + x0];
  double v10 = grid[(size_t)y0 * gw + x1];
  double v01 = grid[(size_t)y1 * gw + x0];
  double v11 = grid[(size_t)y1 * gw + x1];
  double top = v00 * (1.0 - ax) + v10 * ax;
  double bottom = v01 * (1.0 - ax) + v11 * ax;
  return filterTestClamp((int)(top * (1.0 - ay) + bottom * ay + 0.5));
}

static void filterTestSetPixel(std::vector<unsigned char>& rgba, unsigned w, unsigned h,
                               unsigned x, unsigned y,
                               unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
  if(x >= w || y >= h) return;
  size_t i = ((size_t)y * w + x) * 4;
  rgba[i + 0] = r;
  rgba[i + 1] = g;
  rgba[i + 2] = b;
  rgba[i + 3] = a;
}

static void filterTestFillRect(std::vector<unsigned char>& rgba, unsigned w, unsigned h,
                               unsigned x0, unsigned y0, unsigned x1, unsigned y1,
                               unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
  unsigned x, y;
  for(y = y0; y < y1; ++y)
    for(x = x0; x < x1; ++x)
      filterTestSetPixel(rgba, w, h, x, y, r, g, b, a);
}

static FilterTestSample filterTestMakePhoto(unsigned w, unsigned h) {
  FilterTestSample s;
  unsigned seed = 0x12345678u;
  unsigned gw = 17, gh = 17;
  std::vector<unsigned char> gr(gw * gh), gg(gw * gh), gb(gw * gh);
  unsigned i;
  s.name = "photo";
  s.w = w;
  s.h = h;
  s.rgba.resize((size_t)w * h * 4);
  for(i = 0; i < gw * gh; ++i) {
    gr[i] = (unsigned char)(40 + (filterTestRand(seed) >> 24) % 190);
    gg[i] = (unsigned char)(40 + (filterTestRand(seed) >> 24) % 190);
    gb[i] = (unsigned char)(40 + (filterTestRand(seed) >> 24) % 190);
  }
  for(i = 0; i < w * h; ++i) {
    unsigned x = i % w, y = i / w;
    int noise = (int)((filterTestRand(seed) >> 28) & 15) - 8;
    s.rgba[i * 4 + 0] = filterTestClamp((int)filterTestBilinear(gr, gw, gh, x, y, w, h) + noise);
    s.rgba[i * 4 + 1] = filterTestClamp((int)filterTestBilinear(gg, gw, gh, x, y, w, h) + noise);
    s.rgba[i * 4 + 2] = filterTestClamp((int)filterTestBilinear(gb, gw, gh, x, y, w, h) + noise);
    s.rgba[i * 4 + 3] = 255;
  }
  return s;
}

static FilterTestSample filterTestMakeScreenshot(unsigned w, unsigned h) {
  FilterTestSample s;
  unsigned x, y;
  s.name = "screenshot";
  s.w = w;
  s.h = h;
  s.rgba.resize((size_t)w * h * 4);
  filterTestFillRect(s.rgba, w, h, 0, 0, w, h, 238, 238, 238, 255);
  /*title bar*/
  filterTestFillRect(s.rgba, w, h, 0, 0, w, 7, 30, 90, 170, 255);
  /*window edge*/
  filterTestFillRect(s.rgba, w, h, 0, 7, w, 8, 190, 190, 190, 255);
  /*side panel*/
  filterTestFillRect(s.rgba, w, h, 0, 8, 14, h, 214, 214, 214, 255);
  /*button-like flat rectangles*/
  filterTestFillRect(s.rgba, w, h, 20, 14, 46, 21, 245, 245, 245, 255);
  filterTestFillRect(s.rgba, w, h, 50, 14, 62, 21, 200, 220, 240, 255);
  filterTestFillRect(s.rgba, w, h, 20, 28, 46, 35, 245, 245, 245, 255);
  filterTestFillRect(s.rgba, w, h, 50, 28, 62, 35, 200, 220, 240, 255);
  /*text-like dark stripes*/
  for(y = 42; y < h - 4; y += 6) {
    for(x = 20; x < w - 6; x += 2) {
      if(((x + y) / 2) % 7 != 0) filterTestSetPixel(s.rgba, w, h, x, y, 40, 40, 40, 255);
    }
  }
  /*cursor*/
  for(y = 0; y < 9; ++y)
    for(x = 0; x <= y && 30 + x < w; ++x)
      filterTestSetPixel(s.rgba, w, h, 30 + x, 45 + y, 0, 0, 0, 255);
  return s;
}

static FilterTestSample filterTestMakeLineArt(unsigned w, unsigned h) {
  FilterTestSample s;
  unsigned i, x, y;
  s.name = "lineart";
  s.w = w;
  s.h = h;
  s.rgba.resize((size_t)w * h * 4);
  for(i = 0; i < w * h; ++i) {
    s.rgba[i * 4 + 0] = 255;
    s.rgba[i * 4 + 1] = 255;
    s.rgba[i * 4 + 2] = 255;
    s.rgba[i * 4 + 3] = 255;
  }
  /*rectangle outline*/
  for(x = 4; x < w - 4; ++x) {
    filterTestSetPixel(s.rgba, w, h, x, 4, 0, 0, 0, 255);
    filterTestSetPixel(s.rgba, w, h, x, h - 5, 0, 0, 0, 255);
  }
  for(y = 4; y < h - 4; ++y) {
    filterTestSetPixel(s.rgba, w, h, 4, y, 0, 0, 0, 255);
    filterTestSetPixel(s.rgba, w, h, w - 5, y, 0, 0, 0, 255);
  }
  /*diagonal lines*/
  for(i = 0; i < w && i < h; ++i) {
    filterTestSetPixel(s.rgba, w, h, i, h - 1 - i, 0, 0, 0, 255);
    filterTestSetPixel(s.rgba, w, h, 10 + i % (w - 20), 10 + i * 2 % (h - 20), 0, 0, 0, 255);
  }
  /*staircase-ish polyline*/
  for(x = 12; x < w - 12; ++x) {
    unsigned yy = 20 + ((x / 4) % 2) * 8;
    filterTestSetPixel(s.rgba, w, h, x, yy, 0, 0, 0, 255);
    filterTestSetPixel(s.rgba, w, h, x, yy + 1, 0, 0, 0, 255);
  }
  return s;
}

static FilterTestSample filterTestMakeGradient(unsigned w, unsigned h) {
  FilterTestSample s;
  unsigned x, y;
  s.name = "gradient";
  s.w = w;
  s.h = h;
  s.rgba.resize((size_t)w * h * 4);
  for(y = 0; y < h; ++y) {
    for(x = 0; x < w; ++x) {
      filterTestSetPixel(s.rgba, w, h, x, y,
                         (unsigned char)(x * 255 / (w > 1 ? w - 1 : 1)),
                         (unsigned char)(y * 255 / (h > 1 ? h - 1 : 1)),
                         (unsigned char)((x + y) * 255 / (w + h > 2 ? w + h - 2 : 1)),
                         255);
    }
  }
  return s;
}

static FilterTestSample filterTestMakeAlpha(unsigned w, unsigned h) {
  FilterTestSample s;
  unsigned x, y;
  s.name = "alpha";
  s.w = w;
  s.h = h;
  s.rgba.resize((size_t)w * h * 4);
  for(y = 0; y < h; ++y) {
    for(x = 0; x < w; ++x) {
      unsigned char a;
      unsigned dx = x > w / 2 ? x - w / 2 : w / 2 - x;
      unsigned dy = y > h / 2 ? y - h / 2 : h / 2 - y;
      a = (unsigned char)((x * 255) / (w > 1 ? w - 1 : 1));
      if(dx * dx + dy * dy < (w / 5) * (h / 5)) a = 0; /*fully transparent hole*/
      filterTestSetPixel(s.rgba, w, h, x, y,
                         (unsigned char)(255 - x * 3),
                         (unsigned char)(x * 2 + y),
                         (unsigned char)(y * 3),
                         a);
    }
  }
  return s;
}

static std::vector<FilterTestSample> filterTestMakeSamples() {
  std::vector<FilterTestSample> samples;
  samples.push_back(filterTestMakePhoto(64, 64));
  samples.push_back(filterTestMakeScreenshot(64, 64));
  samples.push_back(filterTestMakeLineArt(64, 64));
  samples.push_back(filterTestMakeGradient(64, 64));
  samples.push_back(filterTestMakeAlpha(64, 64));
  return samples;
}

/*Self-contained SHA-256 (FIPS 180-4), only used by the tests.*/
class FilterTestSha256 {
  public:
    FilterTestSha256() {
      state[0] = 0x6a09e667u; state[1] = 0xbb67ae85u;
      state[2] = 0x3c6ef372u; state[3] = 0xa54ff53au;
      state[4] = 0x510e527fu; state[5] = 0x9b05688cu;
      state[6] = 0x1f83d9abu; state[7] = 0x5be0cd19u;
      datalen = 0;
      bitlen = 0;
    }

    void update(const unsigned char* data, size_t len) {
      size_t i;
      for(i = 0; i < len; ++i) {
        buffer[datalen++] = data[i];
        if(datalen == 64) {
          transform(buffer);
          bitlen += 512;
          datalen = 0;
        }
      }
    }

    std::string hexDigest() {
      unsigned char digest[32];
      std::string hex;
      static const char* digits = "0123456789abcdef";
      size_t i;
      /*total message length in bits, must be measured before padding is added*/
      size_t totalbits = bitlen + datalen * 8;

      /*pad the remaining data*/
      if(datalen < 56) {
        buffer[datalen++] = (unsigned char)0x80;
        while(datalen < 56) buffer[datalen++] = 0;
      } else {
        buffer[datalen++] = (unsigned char)0x80;
        while(datalen < 64) buffer[datalen++] = 0;
        transform(buffer);
        for(i = 0; i < 56; ++i) buffer[i] = 0;
      }
      buffer[63] = (unsigned char)(totalbits);
      buffer[62] = (unsigned char)(totalbits >> 8);
      buffer[61] = (unsigned char)(totalbits >> 16);
      buffer[60] = (unsigned char)(totalbits >> 24);
      buffer[59] = (unsigned char)(totalbits >> 32);
      buffer[58] = (unsigned char)(totalbits >> 40);
      buffer[57] = (unsigned char)(totalbits >> 48);
      buffer[56] = (unsigned char)(totalbits >> 56);
      transform(buffer);

      for(i = 0; i < 4; ++i) {
        digest[i]      = (unsigned char)(state[0] >> (24 - i * 8));
        digest[i + 4]  = (unsigned char)(state[1] >> (24 - i * 8));
        digest[i + 8]  = (unsigned char)(state[2] >> (24 - i * 8));
        digest[i + 12] = (unsigned char)(state[3] >> (24 - i * 8));
        digest[i + 16] = (unsigned char)(state[4] >> (24 - i * 8));
        digest[i + 20] = (unsigned char)(state[5] >> (24 - i * 8));
        digest[i + 24] = (unsigned char)(state[6] >> (24 - i * 8));
        digest[i + 28] = (unsigned char)(state[7] >> (24 - i * 8));
      }
      for(i = 0; i < 32; ++i) {
        hex += digits[digest[i] >> 4];
        hex += digits[digest[i] & 15];
      }
      return hex;
    }

  private:
    unsigned state[8];
    unsigned char buffer[64];
    size_t datalen;
    size_t bitlen;

    static unsigned rotr(unsigned x, unsigned n) {
      return (x >> n) | (x << (32 - n));
    }

    static unsigned choose(unsigned e, unsigned f, unsigned g) {
      return (e & f) ^ ((~e) & g);
    }

    static unsigned majority(unsigned a, unsigned b, unsigned c) {
      return (a & b) ^ (a & c) ^ (b & c);
    }

    static unsigned bigSigma0(unsigned x) {
      return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
    }

    static unsigned bigSigma1(unsigned x) {
      return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
    }

    static unsigned smallSigma0(unsigned x) {
      return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
    }

    static unsigned smallSigma1(unsigned x) {
      return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
    }

    void transform(const unsigned char* chunk) {
      static const unsigned k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
      };
      unsigned w[64];
      unsigned a, b, c, d, e, f, g, h;
      unsigned t1, t2;
      unsigned i;

      for(i = 0; i < 16; ++i) {
        w[i] = ((unsigned)chunk[i * 4] << 24) |
               ((unsigned)chunk[i * 4 + 1] << 16) |
               ((unsigned)chunk[i * 4 + 2] << 8) |
               ((unsigned)chunk[i * 4 + 3]);
      }
      for(i = 16; i < 64; ++i) {
        w[i] = smallSigma1(w[i - 2]) + w[i - 7] + smallSigma0(w[i - 15]) + w[i - 16];
      }

      a = state[0]; b = state[1]; c = state[2]; d = state[3];
      e = state[4]; f = state[5]; g = state[6]; h = state[7];

      for(i = 0; i < 64; ++i) {
        t1 = h + bigSigma1(e) + choose(e, f, g) + k[i] + w[i];
        t2 = bigSigma0(a) + majority(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
      }

      state[0] += a; state[1] += b; state[2] += c; state[3] += d;
      state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
};

static std::string filterTestSha256(const std::vector<unsigned char>& data) {
  FilterTestSha256 sha;
  if(!data.empty()) sha.update(&data[0], data.size());
  return sha.hexDigest();
}

#endif /*FILTER_STRATEGY_TEST_COMMON_H*/
