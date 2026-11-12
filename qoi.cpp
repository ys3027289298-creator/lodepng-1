#include "qoi.h"

#include <stdio.h>

#define QOI_OP_INDEX 0x00 /* 00xxxxxx */
#define QOI_OP_DIFF  0x40 /* 01xxxxxx */
#define QOI_OP_LUMA  0x80 /* 10xxxxxx */
#define QOI_OP_RUN   0xc0 /* 11xxxxxx */
#define QOI_OP_RGB   0xfe
#define QOI_OP_RGBA  0xff
#define QOI_MASK_2   0xc0

static unsigned qoi_read32be(const unsigned char* p) {
  return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static unsigned qoi_hash(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
  return ((unsigned)r * 3u + (unsigned)g * 5u + (unsigned)b * 7u + (unsigned)a * 11u) % 64u;
}

unsigned qoi_decode32(std::vector<unsigned char>& out, unsigned& w, unsigned& h,
                      unsigned& channels, const unsigned char* in, size_t insize) {
  size_t p, numpixels, px;
  unsigned char index[64][4];
  unsigned char r, g, b, a;
  unsigned run, i;

  if(!in) return 1;
  if(insize < 14) return 2;
  if(in[0] != 'q' || in[1] != 'o' || in[2] != 'i' || in[3] != 'f') return 3;

  w = qoi_read32be(in + 4);
  h = qoi_read32be(in + 8);
  channels = in[12];
  if(channels != 3 && channels != 4) return 4;
  if(in[13] > 1) return 5;
  if(w == 0 || h == 0) return 6;
  /*w*h*4 must fit in size_t (and in 32-bit unsigned for the loop counters)*/
  if(w > 0x3fffffffu / h) return 6;

  numpixels = (size_t)w * (size_t)h;
  try {
    out.resize(numpixels * 4);
  } catch(...) {
    return 9;
  }

  for(i = 0; i < 64; ++i) {
    index[i][0] = 0; index[i][1] = 0; index[i][2] = 0; index[i][3] = 255;
  }
  r = 0; g = 0; b = 0; a = 255;

  p = 14;
  px = 0;
  run = 0;
  while(px < numpixels) {
    if(run > 0) {
      --run;
    } else {
      unsigned char b1, b2;
      if(p >= insize) return 7;
      b1 = in[p++];
      if(b1 == QOI_OP_RGB) {
        if(p + 3 > insize) return 7;
        r = in[p]; g = in[p + 1]; b = in[p + 2];
        p += 3;
      } else if(b1 == QOI_OP_RGBA) {
        if(p + 4 > insize) return 7;
        r = in[p]; g = in[p + 1]; b = in[p + 2]; a = in[p + 3];
        p += 4;
      } else if((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
        const unsigned char* c = index[b1 & 0x3f];
        r = c[0]; g = c[1]; b = c[2]; a = c[3];
      } else if((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
        r = (unsigned char)(r + ((int)((b1 >> 4) & 0x03) - 2));
        g = (unsigned char)(g + ((int)((b1 >> 2) & 0x03) - 2));
        b = (unsigned char)(b + ((int)(b1 & 0x03) - 2));
      } else if((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
        int dg, drdg, dbdg;
        if(p >= insize) return 7;
        b2 = in[p++];
        dg = (int)(b1 & 0x3f) - 32;
        drdg = (int)((b2 >> 4) & 0x0f) - 8;
        dbdg = (int)(b2 & 0x0f) - 8;
        r = (unsigned char)(r + dg + drdg);
        g = (unsigned char)(g + dg);
        b = (unsigned char)(b + dg + dbdg);
      } else { /*QOI_OP_RUN*/
        run = (unsigned)(b1 & 0x3f); /*stored value is run-1; one pixel emitted below*/
        if(run > numpixels - px - 1) return 8;
      }
      index[qoi_hash(r, g, b, a)][0] = r;
      index[qoi_hash(r, g, b, a)][1] = g;
      index[qoi_hash(r, g, b, a)][2] = b;
      index[qoi_hash(r, g, b, a)][3] = a;
    }
    out[px * 4 + 0] = r;
    out[px * 4 + 1] = g;
    out[px * 4 + 2] = b;
    out[px * 4 + 3] = a;
    ++px;
  }
  return 0;
}

unsigned qoi_decode32_file(std::vector<unsigned char>& out, unsigned& w, unsigned& h,
                           unsigned& channels, const char* filename) {
  std::vector<unsigned char> data;
  FILE* file = fopen(filename, "rb");
  long size;
  if(!file) return 1;
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if(size < 0) { fclose(file); return 1; }
  try {
    data.resize((size_t)size);
  } catch(...) {
    fclose(file);
    return 9;
  }
  if(size > 0 && fread(&data[0], 1, (size_t)size, file) != (size_t)size) {
    fclose(file);
    return 7;
  }
  fclose(file);
  return qoi_decode32(out, w, h, channels, data.empty() ? 0 : &data[0], data.size());
}

const char* qoi_error_text(unsigned code) {
  switch(code) {
    case 0: return "no error, everything went ok";
    case 1: return "null input pointer or file could not be opened";
    case 2: return "input too small to contain a QOI header";
    case 3: return "bad magic bytes, not a QOI file";
    case 4: return "invalid channels value in QOI header (must be 3 or 4)";
    case 5: return "invalid colorspace value in QOI header (must be 0 or 1)";
    case 6: return "invalid or too large image dimensions";
    case 7: return "truncated QOI data";
    case 8: return "QOI RUN chunk exceeds remaining pixel count";
    case 9: return "memory allocation failed";
    default: return "unknown error code";
  }
}
