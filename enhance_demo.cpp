/*
Demo for the grayscale enhancement pipeline added to lodepng_util.cpp.
Builds one small grayish, unevenly distributed sample and prints, in one run:
- step-by-step output of three pixel groups
- histogram variance increments after stretch and after equalization
- numeric effect of the sRGB toggle on the intermediate result
- status for all-black / all-white images (no division by zero)
- errors for out-of-range parameters of each step
- byte-identical check of the original PNG encoder/decoder
*/

#include "lodepng.h"
#include <iostream>
#include <vector>

namespace lodepng {
/* Implemented in lodepng_util.cpp */
double histogramVarianceGray8(const unsigned char* data, size_t n);
unsigned quantileClipGray8(unsigned char* data, size_t n, double low, double high);
unsigned linearStretchGray8(unsigned char* data, size_t n, double out_min, double out_max);
unsigned histogramEqualizeGray8(unsigned char* data, size_t n);
unsigned srgbConvertGray8(unsigned char* data, size_t n, unsigned direction);
unsigned enhancePipelineGray8(unsigned char* data, size_t n,
                              bool clip_on, double clip_low, double clip_high,
                              bool stretch_on, double stretch_min, double stretch_max,
                              bool equalize_on,
                              bool srgb_on, unsigned srgb_direction);
}

static const unsigned W = 16, H = 16, N = W * H;

/* grayish and unevenly distributed: values clustered in 96..132 */
static std::vector<unsigned char> makeSample() {
  std::vector<unsigned char> img(N);
  for(unsigned y = 0; y < H; ++y)
    for(unsigned x = 0; x < W; ++x)
      img[y * W + x] = (unsigned char)(96 + (x * x * 3 + y * 7 + x * y) % 37);
  return img;
}

/* three pixel groups, three consecutive pixels each */
static const size_t GROUPS[3][3] = {{0, 1, 2}, {87, 88, 89}, {253, 254, 255}};

static void printGroups(const char* label, const std::vector<unsigned char>& img) {
  std::cout << "  " << label << ": ";
  for(int g = 0; g < 3; ++g) {
    std::cout << "G" << g << "=[";
    for(int k = 0; k < 3; ++k) {
      std::cout << (int)img[GROUPS[g][k]] << (k < 2 ? "," : "");
    }
    std::cout << "] ";
  }
  std::cout << std::endl;
}

int main() {
  int failures = 0;
  const double clip_lo = 0.05, clip_hi = 0.95;
  const double st_min = 0.0, st_max = 255.0;

  std::cout << "== step-by-step pipeline (clip 5%/95% -> stretch 0..255 -> equalize) ==" << std::endl;
  std::vector<unsigned char> img = makeSample();
  printGroups("input   ", img);
  double var0 = lodepng::histogramVarianceGray8(&img[0], N);

  unsigned e = lodepng::quantileClipGray8(&img[0], N, clip_lo, clip_hi);
  std::cout << "clip status=" << e << std::endl;
  printGroups("clip    ", img);
  double var1 = lodepng::histogramVarianceGray8(&img[0], N);

  e = lodepng::linearStretchGray8(&img[0], N, st_min, st_max);
  std::cout << "stretch status=" << e << std::endl;
  printGroups("stretch ", img);
  double var2 = lodepng::histogramVarianceGray8(&img[0], N);

  e = lodepng::histogramEqualizeGray8(&img[0], N);
  std::cout << "equalize status=" << e << std::endl;
  printGroups("equalize", img);
  double var3 = lodepng::histogramVarianceGray8(&img[0], N);

  std::cout << "== histogram variance increments ==" << std::endl;
  std::cout << "  var(input)=" << var0 << " var(clip)=" << var1
            << " var(stretch)=" << var2 << " var(equalize)=" << var3 << std::endl;
  std::cout << "  increment after stretch:  " << (var2 - var1) << std::endl;
  std::cout << "  increment after equalize: " << (var3 - var2) << std::endl;

  std::cout << "== sRGB toggle effect on the intermediate (post-equalize) result ==" << std::endl;
  std::vector<unsigned char> srgb_on = img, srgb_off = img;
  e = lodepng::srgbConvertGray8(&srgb_on[0], N, 0); /* linear -> sRGB */
  std::cout << "srgb status=" << e << std::endl;
  printGroups("srgb off", srgb_off);
  printGroups("srgb on ", srgb_on);
  int maxdiff = 0;
  double vard = lodepng::histogramVarianceGray8(&srgb_on[0], N) - var3;
  for(size_t i = 0; i < N; ++i) {
    int d = (int)srgb_on[i] - (int)srgb_off[i];
    if(d < 0) d = -d;
    if(d > maxdiff) maxdiff = d;
  }
  std::cout << "  max |on-off| per pixel: " << maxdiff
            << ", variance change from sRGB step: " << vard << std::endl;

  std::cout << "== chained pipeline equals manual chain ==" << std::endl;
  std::vector<unsigned char> chained = makeSample();
  e = lodepng::enhancePipelineGray8(&chained[0], N,
                                    true, clip_lo, clip_hi,
                                    true, st_min, st_max,
                                    true,
                                    false, 0);
  std::cout << "pipeline status=" << e << std::endl;
  printGroups("chained ", chained);
  if(e != 0 || chained != img) { std::cout << "  MISMATCH!" << std::endl; ++failures; }
  else std::cout << "  matches manual step-by-step chain" << std::endl;

  std::cout << "== uniform images return a clear status, no division by zero ==" << std::endl;
  std::vector<unsigned char> black(N, 0), white(N, 255);
  unsigned eb = lodepng::enhancePipelineGray8(&black[0], N, true, 0.05, 0.95,
                                              true, 0.0, 255.0, true, false, 0);
  unsigned ew = lodepng::enhancePipelineGray8(&white[0], N, true, 0.05, 0.95,
                                              true, 0.0, 255.0, true, false, 0);
  std::cout << "  all-black status=" << eb << " (10=uniform), all-white status=" << ew << std::endl;
  if(eb != 10 || ew != 10) { std::cout << "  UNEXPECTED STATUS!" << std::endl; ++failures; }

  std::cout << "== out-of-range parameters are rejected per step ==" << std::endl;
  std::vector<unsigned char> t = makeSample();
  unsigned e1 = lodepng::quantileClipGray8(&t[0], N, -0.1, 0.95);
  unsigned e2 = lodepng::quantileClipGray8(&t[0], N, 0.9, 0.5);
  unsigned e3 = lodepng::linearStretchGray8(&t[0], N, 0.0, 300.0);
  unsigned e4 = lodepng::linearStretchGray8(&t[0], N, 200.0, 100.0);
  unsigned e5 = lodepng::srgbConvertGray8(&t[0], N, 2);
  std::cout << "  clip low<0: " << e1 << ", clip low>high: " << e2
            << ", stretch max>255: " << e3 << ", stretch min>max: " << e4
            << ", srgb dir=2: " << e5 << std::endl;
  if(e1 != 1 || e2 != 1 || e3 != 2 || e4 != 2 || e5 != 3) {
    std::cout << "  UNEXPECTED ERROR CODE!" << std::endl; ++failures;
  }

  std::cout << "== original codec is byte-identical ==" << std::endl;
  {
    std::vector<unsigned char> rgba(32 * 32 * 4), png1, png2, back;
    unsigned w = 32, h = 32;
    for(size_t i = 0; i < 32 * 32; ++i) {
      rgba[i * 4 + 0] = (unsigned char)(i * 3);
      rgba[i * 4 + 1] = (unsigned char)(i * 5);
      rgba[i * 4 + 2] = (unsigned char)(i * 7);
      rgba[i * 4 + 3] = 255;
    }
    unsigned ec1 = lodepng::encode(png1, rgba, w, h);
    unsigned ed = lodepng::decode(back, w, h, png1);
    unsigned ec2 = lodepng::encode(png2, back, w, h);
    bool same_bytes = (png1 == png2);
    bool same_pixels = (back == rgba);
    std::cout << "  encode/decode errors: " << ec1 << "," << ed << "," << ec2
              << " re-encode byte-identical: " << (same_bytes ? "yes" : "no")
              << " roundtrip pixels identical: " << (same_pixels ? "yes" : "no") << std::endl;
    if(ec1 || ed || ec2 || !same_bytes || !same_pixels) ++failures;
  }

  std::cout << (failures == 0 ? "DEMO OK" : "DEMO FAILED") << std::endl;
  return failures == 0 ? 0 : 1;
}
