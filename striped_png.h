/*
 * striped_png.h - Chunked (striped) PNG container built on the LodePNG public API.
 *
 * A regular PNG holds one zlib/deflate stream for the whole image, so its
 * decompression is inherently sequential. This module splits an image into
 * horizontal strips. Every strip is encoded independently as its own
 * complete PNG (and therefore its own independent zlib stream); the strips
 * are stored in one self-describing container:
 *
 *   +--------------------+
 *   | header + offsets   |  magic "SPNG1", geometry, per-strip offset table
 *   +--------------------+
 *   | strip 0 PNG bytes  |  standalone PNG, independently decodable
 *   +--------------------+
 *   | strip 1 PNG bytes  |
 *   +--------------------+
 *   | ...                |
 *   +--------------------+
 *
 * The offset table gives random access: a single strip can be decoded
 * without touching any other strip. A full-image decode runs the strips in
 * parallel on std::thread workers, or sequentially with one worker.
 *
 * This is a new, self-contained module. It only calls the documented
 * LodePNG API (lodepng::encode / lodepng::decode) and the C++ standard
 * library. No LodePNG source file, public signature or build script is
 * modified, and the single-stream PNG bytes produced by lodepng::encode are
 * untouched (the embedded per-strip bytes are exactly that output).
 *
 * Container layout (all multi-byte integers little-endian):
 *
 *   char   magic[5]       = 'S','P','N','G','1'
 *   u32    width
 *   u32    height
 *   u32    strip_height   (requested height; the last strip may be shorter)
 *   u32    num_strips
 *   u32    colortype      (LodePNGColorType, currently LCT_RGBA)
 *   u32    bitdepth       (currently 8)
 *   u64    offsets[num_strips + 1]
 *                          offsets[i] is the file offset of strip i;
 *                          offsets[num_strips] equals the total file size,
 *                          so strip i spans offsets[i] .. offsets[i+1].
 *   bytes  strip PNGs, concatenated in strip order
 *
 * Supported raw pixel format: 8-bit RGBA (LCT_RGBA, bitdepth 8),
 * 4 bytes per pixel, rows tightly packed.
 */

#ifndef STRIPED_PNG_HEADER
#define STRIPED_PNG_HEADER

#include <cstddef>
#include <vector>
#include <string>

namespace stripedpng {

/* Error codes (nonzero). Kept above 1000 so they never collide with the
 * LodePNG error codes that may be returned from the underlying codec. */
enum StripedError {
  ERR_OK = 0,
  ERR_BAD_MAGIC = 1001,      /* data is not an SPNG1 container */
  ERR_TRUNCATED = 1002,      /* header or strip data is cut short */
  ERR_BAD_HEADER = 1003,     /* inconsistent geometry / offset table */
  ERR_STRIP_INDEX = 1004,    /* requested strip index is out of range */
  ERR_BAD_ARGUMENT = 1005,   /* zero/unsupported image geometry etc. */
  ERR_THREAD = 1006          /* worker thread could not be created */
};

struct EncodeOptions {
  /* Height of each strip in rows, except the last one which may be shorter.
   * Configurable. Values >= image height produce a single-strip file. */
  unsigned strip_height;
  EncodeOptions() : strip_height(64) {}
  explicit EncodeOptions(unsigned sh) : strip_height(sh) {}
};

struct Info {
  unsigned width;
  unsigned height;
  unsigned strip_height;
  unsigned num_strips;
  unsigned colortype;
  unsigned bitdepth;
  /* File offsets of every strip, size num_strips + 1. The last entry is the
   * total file size; strip i occupies offsets[i] .. offsets[i+1]. */
  std::vector<unsigned long long> offsets;
  Info() : width(0), height(0), strip_height(0), num_strips(0),
           colortype(0), bitdepth(0) {}
};

/* Encode an RGBA8 image into the striped container. Returns ERR_OK or a
 * nonzero error code (which may be a LodePNG codec error). Output is
 * deterministic for identical inputs. */
unsigned encode(std::vector<unsigned char>& out,
                const std::vector<unsigned char>& rgba,
                unsigned width, unsigned height,
                const EncodeOptions& options = EncodeOptions());

/* Parse the header and offset table without decoding any pixel data. */
unsigned inspect(const std::vector<unsigned char>& in, Info& info);

/* Decode every strip and reassemble the full RGBA8 image.
 * num_threads: 0 = hardware concurrency, 1 = strictly sequential,
 * otherwise clamped to num_strips. Strips are independent jobs; workers
 * write into disjoint row ranges, so no per-pixel locking is needed. */
unsigned decode(std::vector<unsigned char>& rgba,
                unsigned& width, unsigned& height,
                const std::vector<unsigned char>& in,
                unsigned num_threads = 0);

/* Decode exactly one strip. Nothing outside its offset range is touched,
 * so this is the random-access path. row_offset receives the image row at
 * which the strip begins; strip_width/strip_height describe the returned
 * pixels (the last strip may be shorter than the configured strip_height). */
unsigned decode_strip(std::vector<unsigned char>& strip_rgba,
                      unsigned& strip_width, unsigned& strip_height,
                      unsigned& row_offset,
                      const std::vector<unsigned char>& in,
                      unsigned strip_index);

/* Result of comparing a striped file against the single-stream PNG of the
 * same image. Used by callers (and the benchmark) to report size inflation. */
struct BloatReport {
  unsigned long long single_stream_size;
  unsigned long long striped_size;
  long long delta_bytes;            /* striped_size - single_stream_size */
  double delta_percent;             /* delta as % of single-stream size */
  bool bloat_flagged;               /* delta_percent exceeds the threshold */
  bool strip_too_large;             /* strip covers (nearly) the whole image,
                                       so striping provides no parallelism */
  std::string message;              /* human-readable explanation */
};

/* Policy helper: decide whether a striped encoding is bloated relative to
 * the single-stream PNG, or whether the configured strip height is so large
 * that the image is effectively not striped at all. threshold_percent is
 * e.g. 5.0 for a 5% size-inflation warning. */
BloatReport check_bloat(unsigned long long single_stream_size,
                        unsigned long long striped_size,
                        unsigned image_height, unsigned strip_height,
                        double threshold_percent = 5.0);

} /* namespace stripedpng */

#endif /* STRIPED_PNG_HEADER */
