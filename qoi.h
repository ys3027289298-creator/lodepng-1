/*
Minimal QOI (Quite OK Image) decoder, companion to LodePNG.
Decodes QOI images to 32-bit RGBA (RGBA8). Independent from the PNG codec.

Error codes (0 = success):
  1: null input pointer
  2: input too small to contain a QOI header
  3: bad magic bytes (not "qoif")
  4: invalid channels value (must be 3 or 4)
  5: invalid colorspace value (must be 0 or 1)
  6: invalid dimensions (zero, or w*h*4 overflows)
  7: truncated pixel data (input ended before all pixels were decoded)
  8: RUN chunk exceeds the remaining pixel count
  9: memory allocation failed
*/

#ifndef QOI_H
#define QOI_H

#include <string.h> /*for size_t*/
#include <vector>

/*
Decodes a QOI image from memory. The output is always RGBA8 (4 bytes per
pixel), regardless of the channels value stored in the QOI header.
out: filled with w*h*4 bytes on success.
w, h: image dimensions on success.
channels: the channels value from the QOI header (3 or 4), informational only.
Returns 0 on success, or one of the error codes listed above.
*/
unsigned qoi_decode32(std::vector<unsigned char>& out, unsigned& w, unsigned& h,
                      unsigned& channels, const unsigned char* in, size_t insize);

/*Same as qoi_decode32, but reads the QOI data from a file on disk.*/
unsigned qoi_decode32_file(std::vector<unsigned char>& out, unsigned& w, unsigned& h,
                           unsigned& channels, const char* filename);

/*Returns a static string describing the given qoi_decode32 error code.*/
const char* qoi_error_text(unsigned code);

#endif /*QOI_H*/
