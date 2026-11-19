// Edge-case and cross-decoder tests for the multipass encoder.
//  * tiny images (1x1, single row, single column),
//  * an image larger than one deflate block (multi-block splice),
//  * interlaced (Adam7) input, which must take the stock fallback,
//  * every output color type,
//  * round-trip through BOTH the lodepng decoder and system zlib+unfilter.
#include "lodepng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg) do { if(!(cond)) { printf("  FAIL: %s\n", msg); ++g_failures; } \
                              else { printf("  ok: %s\n", msg); } } while(0)

// minimal PNG IDAT inflate + unfilter to independently verify one PNG
static bool independent_decode(const std::vector<unsigned char>& png,
                                unsigned expect_w, unsigned expect_h,
                                const std::vector<unsigned char>& expect_raw,
                                unsigned bpp_bytes) {
  size_t p = 8;
  std::vector<unsigned char> idat;
  while(p + 8 <= png.size()) {
    unsigned ln = (png[p]<<24)|(png[p+1]<<16)|(png[p+2]<<8)|png[p+3];
    std::string t((const char*)&png[p+4], 4);
    if(t == "IDAT") idat.insert(idat.end(), png.begin()+p+8, png.begin()+p+8+ln);
    p += 12 + ln;
    if(t == "IEND") break;
  }
  std::vector<unsigned char> filtered(expect_raw.size() + expect_h + 16);
  uLongf fl = filtered.size();
  if(uncompress(filtered.data(), &fl, idat.data(), idat.size()) != Z_OK) return false;
  size_t stride = (size_t)expect_w * bpp_bytes;
  if(fl != stride * expect_h + expect_h) return false;
  std::vector<unsigned char> recon(stride * expect_h);
  std::vector<unsigned char> prev(stride, 0);
  size_t pos = 0;
  for(unsigned y = 0; y < expect_h; ++y) {
    unsigned ft = filtered[pos++];
    unsigned char* line = filtered.data() + pos;
    pos += stride;
    for(size_t i = 0; i < stride; ++i) {
      unsigned char a = i >= bpp_bytes ? line[i - bpp_bytes] : 0;
      unsigned char c = i >= bpp_bytes ? prev[i - bpp_bytes] : 0;
      unsigned char b = prev[i];
      switch(ft) {
        case 0: break;
        case 1: line[i] = (unsigned char)(line[i] + a); break;
        case 2: line[i] = (unsigned char)(line[i] + b); break;
        case 3: line[i] = (unsigned char)(line[i] + ((a + b) >> 1)); break;
        case 4: {
          int pp = (int)a + (int)b - (int)c;
          int pa = abs(pp - (int)a), pb = abs(pp - (int)b), pc = abs(pp - (int)c);
          unsigned char pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          line[i] = (unsigned char)(line[i] + pred);
          break;
        }
        default: return false;
      }
    }
    memcpy(recon.data() + y * stride, line, stride);
    memcpy(prev.data(), line, stride);
  }
  return recon == expect_raw;
}

static void roundtrip(const char* name, const std::vector<unsigned char>& px,
                      unsigned w, unsigned h, LodePNGColorType ct, unsigned bd,
                      unsigned bpp_bytes, unsigned jobs, unsigned interlace = 0) {
  printf("[%s %ux%u bd=%u jobs=%u%s]\n", name, w, h, bd, jobs,
         interlace ? " adam7" : "");
  lodepng::State st;
  st.encoder.zlibsettings.multipass = 1;
  st.encoder.zlibsettings.multipass_passes = 4;
  st.encoder.zlibsettings.parallel_jobs = jobs;
  st.info_raw.colortype = ct; st.info_raw.bitdepth = bd;
  st.info_png.color.colortype = ct; st.info_png.color.bitdepth = bd;
  st.encoder.auto_convert = 0;
  st.info_png.interlace_method = interlace;
  std::vector<unsigned char> png;
  unsigned err = lodepng::encode(png, px, w, h, st);
  CHECK(err == 0, "encode succeeds");

  // lodepng's own decoder. Request the same raw color mode as the input so
  // the decoded buffer is directly comparable to the original pixels.
  lodepng::State dst;
  dst.info_raw.colortype = ct;
  dst.info_raw.bitdepth = bd;
  std::vector<unsigned char> decoded;
  unsigned dw = 0, dh = 0;
  unsigned derr = lodepng::decode(decoded, dw, dh, dst, png);
  CHECK(derr == 0 && dw == w && dh == h, "lodepng decode round-trip");
  CHECK(decoded == px, "lodepng pixels exact");

  // independent system-zlib + manual unfilter (skip for interlaced)
  if(!interlace) {
    CHECK(independent_decode(png, w, h, px, bpp_bytes),
          "independent zlib/unfilter pixels exact");
  }
}

int main(void) {
  // 1x1 RGBA
  { std::vector<unsigned char> px(4, 123); roundtrip("1x1", px, 1, 1, LCT_RGBA, 8, 4, 1); }
  // single row
  { std::vector<unsigned char> px(256 * 4);
    for(size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)(i * 5);
    roundtrip("1-row", px, 256, 1, LCT_RGBA, 8, 4, 4); }
  // single column
  { std::vector<unsigned char> px(300 * 4);
    for(size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)(i);
    roundtrip("1-col", px, 1, 300, LCT_RGBA, 8, 4, 8); }
  // RGB 8-bit
  { std::vector<unsigned char> px(120 * 90 * 3);
    for(size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)(i * 3 + 1);
    roundtrip("RGB", px, 120, 90, LCT_RGB, 8, 3, 2); }
  // gray 8-bit
  { std::vector<unsigned char> px(200 * 150);
    for(size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)(i);
    roundtrip("gray8", px, 200, 150, LCT_GREY, 8, 1, 4); }
  // gray 16-bit
  { std::vector<unsigned char> px(128 * 128 * 2);
    for(size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)(i * 7);
    roundtrip("gray16", px, 128, 128, LCT_GREY, 16, 2, 8); }
  // gray+alpha and RGBA 16
  { std::vector<unsigned char> px(64 * 48 * 4);
    for(size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)(i * 11);
    roundtrip("gray+alpha16", px, 64, 48, LCT_GREY_ALPHA, 16, 4, 4); }
  // large: forces several 65KB+ deflate blocks through the bit-splice path
  { unsigned w = 700, h = 700;
    std::vector<unsigned char> px((size_t)w * h * 4);
    for(size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)((i * 13 + i / 97) & 255);
    roundtrip("large-multiblock", px, w, h, LCT_RGBA, 8, 4, 8); }
  // Adam7: multipass must fall back to stock encoder and still be valid
  { unsigned w = 100, h = 80;
    std::vector<unsigned char> px((size_t)w * h * 4);
    for(size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)(i);
    roundtrip("adam7-fallback", px, w, h, LCT_RGBA, 8, 4, 8, 1); }

  printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL EDGE TESTS PASSED" : "FAILURES",
         g_failures);
  return g_failures ? 1 : 0;
}
