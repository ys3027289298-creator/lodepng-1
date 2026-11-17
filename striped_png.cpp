/*
 * striped_png.cpp - implementation of the SPNG1 striped PNG container.
 * See striped_png.h for the format and rationale.
 *
 * Only the LodePNG public C++ API (lodepng::encode / lodepng::decode) and
 * the C++ standard library are used; parallelism is std::thread.
 */

#include "striped_png.h"

#include "lodepng.h"

#include <cstring>
#include <algorithm>
#include <thread>

namespace stripedpng {

namespace {

const unsigned char MAGIC[5] = {'S', 'P', 'N', 'G', '1'};
const unsigned RAW_BPP = 4; /* 8-bit RGBA */

/* Fixed header size before the offset table: magic(5) + six u32 fields. */
const size_t HEADER_FIXED_SIZE = 5u + 6u * 4u;

void put_u32(std::vector<unsigned char>& b, unsigned v) {
  b.push_back((unsigned char)(v & 0xffu));
  b.push_back((unsigned char)((v >> 8) & 0xffu));
  b.push_back((unsigned char)((v >> 16) & 0xffu));
  b.push_back((unsigned char)((v >> 24) & 0xffu));
}

void put_u64(std::vector<unsigned char>& b, unsigned long long v) {
  for(int i = 0; i < 8; ++i) {
    b.push_back((unsigned char)((v >> (8 * i)) & 0xffull));
  }
}

unsigned get_u32(const std::vector<unsigned char>& b, size_t pos) {
  return (unsigned)b[pos]
       | ((unsigned)b[pos + 1] << 8)
       | ((unsigned)b[pos + 2] << 16)
       | ((unsigned)b[pos + 3] << 24);
}

unsigned long long get_u64(const std::vector<unsigned char>& b, size_t pos) {
  unsigned long long v = 0;
  for(int i = 0; i < 8; ++i) {
    v |= (unsigned long long)b[pos + i] << (8 * i);
  }
  return v;
}

size_t header_size(unsigned num_strips) {
  return HEADER_FIXED_SIZE + ((size_t)num_strips + 1u) * 8u;
}

} /* anonymous namespace */

unsigned encode(std::vector<unsigned char>& out,
                const std::vector<unsigned char>& rgba,
                unsigned width, unsigned height,
                const EncodeOptions& options) {
  if(width == 0 || height == 0 || options.strip_height == 0) {
    return ERR_BAD_ARGUMENT;
  }
  /* Overflow-safe size check for the tightly packed RGBA8 buffer. */
  if((size_t)width * (size_t)height >
     (size_t)-1 / (size_t)RAW_BPP) {
    return ERR_BAD_ARGUMENT;
  }
  size_t expected = (size_t)width * (size_t)height * (size_t)RAW_BPP;
  if(rgba.size() != expected) return ERR_BAD_ARGUMENT;

  size_t sh = options.strip_height;
  size_t num_strips = ((size_t)height + sh - 1u) / sh;

  out.clear();
  out.reserve(expected / 4u);

  for(size_t i = 0; i < sizeof(MAGIC); ++i) out.push_back(MAGIC[i]);
  put_u32(out, width);
  put_u32(out, height);
  put_u32(out, (unsigned)sh);
  put_u32(out, (unsigned)num_strips);
  put_u32(out, (unsigned)LCT_RGBA);
  put_u32(out, 8u);

  /* Offset table placeholder; patched as each strip is appended. */
  size_t table_pos = out.size();
  for(size_t i = 0; i <= num_strips; ++i) put_u64(out, 0);

  std::vector<unsigned long long> offsets(num_strips + 1, 0);
  for(size_t s = 0; s < num_strips; ++s) {
    offsets[s] = (unsigned long long)out.size();

    size_t row0 = s * sh;
    size_t rows = std::min(sh, (size_t)height - row0);
    const unsigned char* strip_pixels =
        rgba.data() + row0 * (size_t)width * RAW_BPP;

    std::vector<unsigned char> strip_png;
    /* This is the untouched single-stream LodePNG encoder: each strip gets
     * its own complete, standalone PNG and therefore its own zlib stream. */
    unsigned error = lodepng::encode(strip_png, strip_pixels,
                                     width, (unsigned)rows,
                                     LCT_RGBA, 8u);
    if(error) {
      out.clear();
      return error;
    }
    out.insert(out.end(), strip_png.begin(), strip_png.end());
  }
  offsets[num_strips] = (unsigned long long)out.size();

  for(size_t i = 0; i <= num_strips; ++i) {
    unsigned long long v = offsets[i];
    for(int k = 0; k < 8; ++k) {
      out[table_pos + i * 8u + k] =
          (unsigned char)((v >> (8 * k)) & 0xffull);
    }
  }
  return ERR_OK;
}

unsigned inspect(const std::vector<unsigned char>& in, Info& info) {
  if(in.size() < sizeof(MAGIC)) return ERR_BAD_MAGIC;
  for(size_t i = 0; i < sizeof(MAGIC); ++i) {
    if(in[i] != MAGIC[i]) return ERR_BAD_MAGIC;
  }
  if(in.size() < HEADER_FIXED_SIZE) return ERR_TRUNCATED;

  info.width = get_u32(in, 5);
  info.height = get_u32(in, 9);
  info.strip_height = get_u32(in, 13);
  info.num_strips = get_u32(in, 17);
  info.colortype = get_u32(in, 21);
  info.bitdepth = get_u32(in, 25);

  if(info.width == 0 || info.height == 0 ||
     info.strip_height == 0 || info.num_strips == 0 ||
     info.colortype != (unsigned)LCT_RGBA || info.bitdepth != 8u) {
    return ERR_BAD_HEADER;
  }
  size_t hsize;
  if((size_t)info.num_strips >
     (size_t)-1 / 8u - HEADER_FIXED_SIZE) {
    return ERR_BAD_HEADER;
  }
  hsize = header_size(info.num_strips);
  if(in.size() < hsize) return ERR_TRUNCATED;

  info.offsets.resize((size_t)info.num_strips + 1);
  size_t table_pos = HEADER_FIXED_SIZE;
  for(size_t i = 0; i <= (size_t)info.num_strips; ++i) {
    info.offsets[i] = get_u64(in, table_pos + i * 8u);
  }

  /* The table must describe exactly this file, with nonempty strips. */
  if(info.offsets[0] != hsize ||
     info.offsets[info.num_strips] != in.size()) {
    return ERR_BAD_HEADER;
  }
  for(size_t i = 0; i < (size_t)info.num_strips; ++i) {
    if(info.offsets[i + 1] <= info.offsets[i]) {
      return ERR_BAD_HEADER;
    }
  }
  size_t expected_strips =
      ((size_t)info.height + (size_t)info.strip_height - 1u)
      / (size_t)info.strip_height;
  if(expected_strips != (size_t)info.num_strips) return ERR_BAD_HEADER;

  return ERR_OK;
}

namespace {

/* Decode one strip straight from its slice of the container and place the
 * pixels into its row range of the full-image output buffer. */
unsigned decode_one(std::vector<unsigned char>& rgba,
                    unsigned width, unsigned strip_height,
                    const std::vector<unsigned char>& in,
                    const Info& info, unsigned s) {
  size_t begin = (size_t)info.offsets[s];
  size_t end = (size_t)info.offsets[s + 1];

  std::vector<unsigned char> strip_rgba;
  unsigned sw = 0, sh = 0;
  unsigned error = lodepng::decode(strip_rgba, sw, sh,
                                   in.data() + begin, end - begin,
                                   LCT_RGBA, 8u);
  if(error) return error;
  if(sw != width || sh != strip_height) return ERR_BAD_HEADER;
  if(strip_rgba.size() !=
     (size_t)sw * (size_t)sh * (size_t)RAW_BPP) {
    return ERR_BAD_HEADER;
  }

  size_t row0 = (size_t)s * (size_t)info.strip_height;
  std::memcpy(rgba.data() + row0 * (size_t)width * RAW_BPP,
              strip_rgba.data(), strip_rgba.size());
  return ERR_OK;
}

unsigned strip_rows(const Info& info, unsigned s) {
  size_t row0 = (size_t)s * (size_t)info.strip_height;
  size_t rows = std::min((size_t)info.strip_height,
                         (size_t)info.height - row0);
  return (unsigned)rows;
}

} /* anonymous namespace */

unsigned decode(std::vector<unsigned char>& rgba,
                unsigned& width, unsigned& height,
                const std::vector<unsigned char>& in,
                unsigned num_threads) {
  Info info;
  unsigned error = inspect(in, info);
  if(error) return error;

  width = info.width;
  height = info.height;
  rgba.assign((size_t)width * (size_t)height * RAW_BPP, 0);

  unsigned n = info.num_strips;
  unsigned workers = num_threads;
  if(workers == 0) {
    workers = std::thread::hardware_concurrency();
    if(workers == 0) workers = 4;
  }
  if(workers > n) workers = n;

  if(workers <= 1) {
    for(unsigned s = 0; s < n; ++s) {
      error = decode_one(rgba, width, strip_rows(info, s), in, info, s);
      if(error) return error;
    }
    return ERR_OK;
  }

  /* Parallel path: workers own disjoint strip sets and write disjoint row
   * ranges, so the only shared state is the read-only container. Thread
   * creation failure (or an allocation failure inside a worker) is reported
   * as ERR_THREAD rather than terminating the process. */
  std::vector<unsigned> errors(n, 0);
  std::vector<unsigned> worker_error(workers, 0);
  try {
    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for(unsigned t = 1; t < workers; ++t) {
      pool.emplace_back([&, t]() {
        try {
          for(unsigned s = t; s < n; s += workers) {
            errors[s] = decode_one(rgba, width, strip_rows(info, s),
                                   in, info, s);
          }
        } catch(...) {
          worker_error[t] = ERR_THREAD;
        }
      });
    }
    try {
      for(unsigned s = 0; s < n; s += workers) {
        errors[s] = decode_one(rgba, width, strip_rows(info, s),
                               in, info, s);
      }
    } catch(...) {
      worker_error[0] = ERR_THREAD;
    }
    for(std::thread& th : pool) th.join();
  } catch(...) {
    return ERR_THREAD;
  }

  for(unsigned t = 0; t < workers; ++t) {
    if(worker_error[t]) return worker_error[t];
  }
  for(unsigned s = 0; s < n; ++s) {
    if(errors[s]) return errors[s];
  }
  return ERR_OK;
}

unsigned decode_strip(std::vector<unsigned char>& strip_rgba,
                      unsigned& strip_width, unsigned& strip_height,
                      unsigned& row_offset,
                      const std::vector<unsigned char>& in,
                      unsigned strip_index) {
  Info info;
  unsigned error = inspect(in, info);
  if(error) return error;
  if(strip_index >= info.num_strips) return ERR_STRIP_INDEX;

  size_t begin = (size_t)info.offsets[strip_index];
  size_t end = (size_t)info.offsets[strip_index + 1];

  unsigned sw = 0, sh = 0;
  error = lodepng::decode(strip_rgba, sw, sh,
                          in.data() + begin, end - begin,
                          LCT_RGBA, 8u);
  if(error) return error;

  unsigned expected_h = strip_rows(info, strip_index);
  if(sw != info.width || sh != expected_h) return ERR_BAD_HEADER;

  strip_width = sw;
  strip_height = sh;
  row_offset = strip_index * info.strip_height;
  return ERR_OK;
}

BloatReport check_bloat(unsigned long long single_stream_size,
                        unsigned long long striped_size,
                        unsigned image_height, unsigned strip_height,
                        double threshold_percent) {
  BloatReport r;
  r.single_stream_size = single_stream_size;
  r.striped_size = striped_size;
  r.delta_bytes = (long long)striped_size - (long long)single_stream_size;
  r.delta_percent = single_stream_size == 0
      ? 0.0
      : 100.0 * (double)r.delta_bytes / (double)single_stream_size;
  r.strip_too_large = strip_height >= image_height && image_height > 0;
  r.bloat_flagged = r.delta_percent > threshold_percent;

  if(r.strip_too_large) {
    r.message = "WARNING: strip height >= image height: the file has a "
                "single strip, no parallel decode is possible";
  } else if(r.bloat_flagged) {
    r.message = "WARNING: striped size is ";
    r.message += r.delta_percent >= 0 ? "+" : "";
    r.message += std::to_string(r.delta_percent);
    r.message += "% vs the single-stream PNG (threshold ";
    r.message += std::to_string(threshold_percent);
    r.message += "%)";
  } else {
    r.message = "ok";
  }
  return r;
}

} /* namespace stripedpng */
