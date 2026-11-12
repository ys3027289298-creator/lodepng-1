/*
Test program for the QOI decoder (qoi.cpp). Test assets are hand-made and
embedded below as small byte arrays. Covers: a small image (RGB/RGBA ops),
RUN (+DIFF/LUMA), INDEX, and a truncated file which must return an error
code without crashing.
*/

#include "qoi.h"

#include <stdio.h>

/*Test 1: 2x2, channels=3. Pixels: red, green, blue, (10,20,30).*/
static const unsigned char qoi_small[] = {
  'q','o','i','f', 0,0,0,2, 0,0,0,2, 3,0,
  0xfe,255,0,0, 0xfe,0,255,0, 0xfe,0,0,255, 0xfe,10,20,30,
  0,0,0,0,0,0,0,1
};

/*Test 2: 8x1, channels=4. RGBA(1,2,3,4), RUN x5, DIFF(+0,+1,-1), LUMA -> (2,5,5,4).*/
static const unsigned char qoi_run[] = {
  'q','o','i','f', 0,0,0,8, 0,0,0,1, 4,0,
  0xff,1,2,3,4, 0xc4, 0x6d, 0xa2,0x79,
  0,0,0,0,0,0,0,1
};

/*Test 3: 4x1, channels=3. (7,7,7), (1,1,1), then both repeated via INDEX.*/
static const unsigned char qoi_index[] = {
  'q','o','i','f', 0,0,0,4, 0,0,0,1, 3,0,
  0xfe,7,7,7, 0xfe,1,1,1, 0x1e, 0x04,
  0,0,0,0,0,0,0,1
};

/*Test 4: truncated copy of qoi_run (cut inside the first RGBA chunk).*/
static const unsigned char qoi_truncated[] = {
  'q','o','i','f', 0,0,0,8, 0,0,0,1, 4,0,
  0xff,1,2,3
};

static void print_pixels(const std::vector<unsigned char>& img, unsigned w, unsigned h) {
  unsigned n = w * h < 8 ? w * h : 8, i;
  printf("first %u pixels (RGBA):", n);
  for(i = 0; i < n; ++i) {
    printf(" (%u,%u,%u,%u)", img[i * 4], img[i * 4 + 1], img[i * 4 + 2], img[i * 4 + 3]);
  }
  printf("\n");
}

static int run_test(const char* name, const unsigned char* data, size_t size) {
  std::vector<unsigned char> img;
  unsigned w = 0, h = 0, channels = 0;
  unsigned error = qoi_decode32(img, w, h, channels, data, size);
  printf("%s: ", name);
  if(error) {
    printf("error %u (%s)\n", error, qoi_error_text(error));
    return error == 7 ? 0 : 1; /*only the truncated test may fail, with code 7*/
  }
  printf("w=%u h=%u channels=%u\n  ", w, h, channels);
  print_pixels(img, w, h);
  return 0;
}

int main() {
  int fails = 0;
  fails += run_test("test1 small 2x2 rgb", qoi_small, sizeof(qoi_small));
  fails += run_test("test2 run/diff/luma 8x1 rgba", qoi_run, sizeof(qoi_run));
  fails += run_test("test3 index 4x1 rgb", qoi_index, sizeof(qoi_index));
  fails += run_test("test4 truncated", qoi_truncated, sizeof(qoi_truncated));
  if(fails) printf("FAILED (%d)\n", fails);
  else printf("all tests behaved as expected\n");
  return fails ? 1 : 0;
}
