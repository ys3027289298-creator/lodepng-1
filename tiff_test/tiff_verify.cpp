/*
TIFF decoder verification harness for the lodepng TIFF extension.

Build (from the repository root):
  g++ -I ./ -O3 -Wall -Wextra -ansi -pedantic \
      lodepng.cpp tiff_test/tiff_verify.cpp -o tiff_test/bin/tiff_verify

It exercises the three TIFF compressions (none/LZW/deflate), strip and tile
layouts, chunky and planar configurations, palette, 1-bit, 16-bit, big and
little endian, multi-page (first IFD only), and the failure paths. Both the C
API (lodepng_decode32) and the C++ wrapper (lodepng::decode) are checked and
must give byte-identical output. Peak heap usage and leaks are measured with
instrumented lodepng allocators.

Output lines start with keywords so tiff_check.py can parse them.
*/

/*
This harness defines the lodepng allocators itself, so lodepng.cpp must be
compiled with -DLODEPNG_NO_COMPILE_ALLOCATORS (see tiff_test/run.sh).
*/

#include "lodepng.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0)
#include <time.h>
static double now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#else
static double now_ms() {
  return (double)std::clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}
#endif

/* ---------------- instrumented lodepng allocators ---------------- */

static long g_live = 0;     /*bytes currently held through lodepng allocations*/
static long g_peak = 0;     /*maximum of g_live observed*/
static long g_allocs = 0;
static long g_frees = 0;

/*lodepng.cpp is compiled as C++ here, so its allocator symbols are C++
mangled; the definitions must match (no extern "C")*/
extern void* lodepng_malloc(size_t size) {
  if(size == 0) size = 1;
  size_t* hdr = reinterpret_cast<size_t*>(malloc(size + sizeof(size_t)));
  if(!hdr) return 0;
  *hdr = size;
  g_live += (long)size;
  if(g_live > g_peak) g_peak = g_live;
  g_allocs++;
  return hdr + 1;
}

extern void lodepng_free(void* p) {
  if(!p) return;
  g_frees++;
  /*the allocation size is stored just before the block by lodepng_realloc and
  lodepng_malloc below*/
  size_t* hdr = reinterpret_cast<size_t*>(p) - 1;
  g_live -= (long)*hdr;
  if(g_live < 0) g_live = 0;
  free(hdr);
}

extern void* lodepng_realloc(void* p, size_t size) {
  if(size == 0) size = 1;
  if(!p) return lodepng_malloc(size);
  size_t* old_hdr = reinterpret_cast<size_t*>(p) - 1;
  size_t old_size = *old_hdr;
  size_t* new_hdr = reinterpret_cast<size_t*>(realloc(old_hdr, size + sizeof(size_t)));
  if(!new_hdr) return 0;
  g_live += (long)size - (long)old_size;
  if(g_live > g_peak) g_peak = g_live;
  *new_hdr = size;
  return new_hdr + 1;
}

/* ---------------- helpers ---------------- */

static bool read_file(const std::string& path, std::vector<unsigned char>& out) {
  std::ifstream f(path.c_str(), std::ios::binary);
  if(!f) return false;
  f.seekg(0, std::ios::end);
  std::streamoff n = f.tellg();
  f.seekg(0, std::ios::beg);
  if(n <= 0) return false;
  out.resize((size_t)n);
  f.read(reinterpret_cast<char*>(&out[0]), n);
  return f.good() || f.eof();
}

static unsigned int crc32_calc(const unsigned char* data, size_t n) {
  static unsigned int table[256];
  static int init = 0;
  if(!init) {
    for(unsigned int i = 0; i < 256; i++) {
      unsigned int c = i;
      for(int k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = 1;
  }
  unsigned int c = 0xffffffffu;
  for(size_t i = 0; i < n; i++) c = table[(c ^ data[i]) & 0xffu] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

struct Result {
  unsigned error;
  unsigned w, h;
  std::vector<unsigned char> rgba;
  double ms;
  long peak_bytes;
};

static Result decode_timed(const std::vector<unsigned char>& buf, int iterations) {
  Result r;
  r.error = 0;
  r.w = r.h = 0;
  r.ms = 0.0;

  /*warmup*/
  unsigned char* warm = 0;
  unsigned ww = 0, wh = 0;
  lodepng_decode32(&warm, &ww, &wh, buf.empty() ? 0 : &buf[0], buf.size());
  lodepng_free(warm);

  double t0 = now_ms();
  for(int it = 0; it < iterations; it++) {
    unsigned char* out = 0;
    unsigned w = 0, h = 0;
    r.error = lodepng_decode32(&out, &w, &h, buf.empty() ? 0 : &buf[0], buf.size());
    if(it == iterations - 1) {
      r.w = w;
      r.h = h;
      if(out && !r.error) r.rgba.assign(out, out + (size_t)w * h * 4);
    }
    lodepng_free(out);
    if(r.error) break;
  }
  double t1 = now_ms();
  double total = t1 - t0;
  r.ms = r.error ? 0.0 : total / (double)iterations;
  return r;
}

struct CaseSpec {
  const char* name;
  int expect_error; /*0 = success, otherwise exact lodepng error code*/
};

int main(int argc, char** argv) {
  if(argc < 2) {
    std::cerr << "usage: tiff_verify <samples_dir> [out_dir]" << std::endl;
    return 2;
  }
  std::string dir = argv[1];
  std::string outdir = argc > 2 ? argv[2] : dir;

  const CaseSpec cases[] = {
      /*each compression per layout*/
      {"gray_1.tif", 0},        {"gray_5.tif", 0},        {"gray_8.tif", 0},
      {"rgb_1.tif", 0},         {"rgb_5.tif", 0},         {"rgb_8.tif", 0},
      {"rgba_1.tif", 0},        {"rgba_5.tif", 0},        {"rgba_8.tif", 0},
      {"gray16_1.tif", 0},      {"gray16_5.tif", 0},      {"gray16_8.tif", 0},
      {"gray1_1.tif", 0},       {"gray1_5.tif", 0},       {"gray1_8.tif", 0},
      {"palette_1.tif", 0},     {"palette_5.tif", 0},     {"palette_8.tif", 0},
      {"planar_1.tif", 0},      {"planar_5.tif", 0},      {"planar_8.tif", 0},
      {"tiles_1.tif", 0},       {"tiles_5.tif", 0},       {"tiles_8.tif", 0},
      {"multipage_1.tif", 0},   {"multipage_5.tif", 0},   {"multipage_8.tif", 0},
      {"miniswhite_1.tif", 0},
      /*failure paths: exact lodepng/TIFF error codes. corrupt_deflate8 may
      surface as any inflate error, so it is special-cased below*/
      {"corrupt_lzw5.tif", 132},        /*invalid LZW code*/
      {"corrupt_deflate8.tif", 1000},   /*sentinel: expect a nonzero error*/
      {"badcompress_1.tif", 133},       /*unsupported compression*/
      {"badphotometric_1.tif", 134},    /*unsupported photometric*/
      {"badoffset_1.tif", 129},         /*strip offset outside file*/
      {"truncated_1.tif", 129},         /*IFD runs past end of file*/
  };
  const size_t ncases = sizeof(cases) / sizeof(cases[0]);
  const int ITERATIONS = 50;

  int failures = 0;
  std::printf("# name error w h bytes crc32 ms/iter peak_bytes live_after\n");

  for(size_t ci = 0; ci < ncases; ci++) {
    std::string path = dir + "/" + cases[ci].name;
    std::vector<unsigned char> buf;
    if(!read_file(path, buf)) {
      std::printf("RESULT %s LOADFAIL\n", cases[ci].name);
      failures++;
      continue;
    }

    g_live = 0;
    g_peak = 0;
    g_allocs = 0;
    g_frees = 0;

    Result r = decode_timed(buf, ITERATIONS);
    long live_after = g_live;
    long peak = g_peak;

    /*sentinel 1000: only require a nonzero error (exact inflate code varies)*/
    int error_ok = (cases[ci].expect_error == 1000)
                       ? (r.error != 0)
                       : ((int)r.error == cases[ci].expect_error);
    if(!error_ok) {
      std::printf("RESULT %s ERROR_MISMATCH got=%u want=%d text=%s\n",
                  cases[ci].name, r.error, cases[ci].expect_error,
                  lodepng_error_text(r.error));
      failures++;
      continue;
    }
    if(r.error) {
      std::printf("RESULT %s OK_ERROR code=%u text=%s peak=%ld live=%ld\n",
                  cases[ci].name, r.error, lodepng_error_text(r.error),
                  peak, live_after);
      if(live_after != 0) {
        std::printf("  LEAK %s live=%ld after error\n", cases[ci].name, live_after);
        failures++;
      }
      continue;
    }

    /*C++ wrapper must produce byte-identical RGBA*/
    std::vector<unsigned char> cpp_out;
    unsigned cw = 0, ch = 0;
    unsigned cpp_error = lodepng::decode(cpp_out, cw, ch, buf);
    if(cpp_error) {
      std::printf("RESULT %s CPP_DECODE_FAIL code=%u\n", cases[ci].name, cpp_error);
      failures++;
      continue;
    }
    if(cw != r.w || ch != r.h || cpp_out.size() != r.rgba.size()
       || std::memcmp(&cpp_out[0], &r.rgba[0], r.rgba.size()) != 0) {
      std::printf("RESULT %s CPP_MISMATCH\n", cases[ci].name);
      failures++;
      continue;
    }

    unsigned int crc = crc32_calc(&r.rgba[0], r.rgba.size());
    std::printf("RESULT %s OK err=0 w=%u h=%u bytes=%u crc=%08x ms=%.4f peak=%ld live=%ld\n",
                cases[ci].name, r.w, r.h, (unsigned)r.rgba.size(), crc, r.ms,
                peak, live_after);

    if(live_after != 0) {
      std::printf("  LEAK %s live=%ld\n", cases[ci].name, live_after);
      failures++;
    }

    /*dump RGBA for the external PIL cross check*/
    std::string rawpath = outdir + "/" + cases[ci].name + ".rgba";
    std::ofstream rf(rawpath.c_str(), std::ios::binary);
    rf.write(reinterpret_cast<const char*>(&r.rgba[0]),
             (std::streamsize)r.rgba.size());
  }

  /*cross-compression pixel identity: same family, same RGBA across 1/5/8*/
  const char* families[] = {"gray", "rgb", "rgba", "gray16", "gray1",
                            "palette", "planar", "tiles", "multipage"};
  for(size_t fi = 0; fi < sizeof(families) / sizeof(families[0]); fi++) {
    std::vector<unsigned int> crcs;
    std::vector<size_t> sizes;
    for(int c = 0; c < 3; c++) {
      std::string name = std::string(families[fi]) + "_" +
                         (c == 0 ? "1" : c == 1 ? "5" : "8") + ".tif.rgba";
      std::vector<unsigned char> raw;
      if(!read_file(outdir + "/" + name, raw)) {
        std::printf("CROSS %s MISSING %s\n", families[fi], name.c_str());
        failures++;
        break;
      }
      crcs.push_back(crc32_calc(&raw[0], raw.size()));
      sizes.push_back(raw.size());
    }
    if(crcs.size() == 3 && (crcs[0] != crcs[1] || crcs[1] != crcs[2]
                            || sizes[0] != sizes[1] || sizes[1] != sizes[2])) {
      std::printf("CROSS %s MISMATCH crc=%08x,%08x,%08x\n", families[fi],
                  crcs[0], crcs[1], crcs[2]);
      failures++;
    } else if(crcs.size() == 3) {
      std::printf("CROSS %s IDENTICAL crc=%08x bytes=%u\n", families[fi],
                  crcs[0], (unsigned)sizes[0]);
    }
  }

  /*multi-page: output must equal the first page (gray reference RGBA)*/
  {
    std::vector<unsigned char> mp, gray;
    read_file(outdir + "/multipage_1.tif.rgba", mp);
    read_file(outdir + "/gray_1.tif.rgba", gray);
    if(mp.size() == gray.size() &&
       std::memcmp(&mp[0], &gray[0], mp.size()) == 0) {
      std::printf("MULTIPAGE first-page selection OK\n");
    } else {
      std::printf("MULTIPAGE first-page selection FAILED\n");
      failures++;
    }
  }

  /*routing: a non-TIFF buffer must not be claimed by the TIFF reader; it falls
  through to the PNG parser and fails with a PNG signature error (code 28)*/
  {
    std::vector<unsigned char> junk;
    if(read_file(dir + "/nottiff.bin", junk)) {
      unsigned char* out2 = 0;
      unsigned w2 = 0, h2 = 0;
      unsigned e = lodepng_decode32(&out2, &w2, &h2,
                                    junk.empty() ? 0 : &junk[0], junk.size());
      if(e == 28) {
        std::printf("ROUTING non-TIFF correctly rejected by PNG path (28)\n");
      } else {
        std::printf("ROUTING non-TIFF unexpected code=%u\n", e);
        failures++;
      }
      lodepng_free(out2);
    }
  }

  /*PNG path sanity: a PNG still decodes through the same entry point*/
  {
    std::vector<unsigned char> dummy_png;
    /*small 1x1 red RGBA PNG encoded by lodepng itself*/
    unsigned char* png = 0;
    size_t pngsize = 0;
    unsigned char px[4] = {255, 0, 0, 255};
    unsigned en = lodepng_encode32(&png, &pngsize, px, 1, 1);
    if(en == 0) {
      unsigned char* out = 0;
      unsigned w = 0, h = 0;
      unsigned de = lodepng_decode32(&out, &w, &h, png, pngsize);
      if(de || w != 1 || h != 1 || out[0] != 255 || out[1] != 0 || out[2] != 0
         || out[3] != 255) {
        std::printf("PNG sanity FAILED de=%u\n", de);
        failures++;
      } else {
        std::printf("PNG sanity OK (roundtrip through shared entry point)\n");
      }
      lodepng_free(out);
    }
    lodepng_free(png);
    (void)dummy_png;
  }

  std::printf(failures == 0 ? "ALL CHECKS PASSED\n" : "CHECKS FAILED: %d\n",
              failures);
  return failures == 0 ? 0 : 1;
}
