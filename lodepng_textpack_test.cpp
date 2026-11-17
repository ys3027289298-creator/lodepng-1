/*
Packed text chunk (txMt) tests and measurements for LodePNG.

Build (the repository Makefile is intentionally not modified):
  g++ -I ./ -W -Wall -Wextra -pedantic -ansi -O3 \
      lodepng.cpp lodepng_textpack_test.cpp -o textpack_test

The program exits with code 0 only when every assertion passes. It prints:
  1) chunk sizes for four sample kinds across the feature switch combinations
  2) write-then-read-back consistency checks
  3) read/write timing
  4) a threshold sweep curve
  5) single-chunk size limit enforcement and corrupt-data rejection
  6) a byte-for-byte check that the classic tEXt path is unchanged
*/

#include "lodepng.h"

#include <ctime>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
  ++g_checks; \
  if(!(cond)) { \
    ++g_failures; \
    std::cout << "FAIL: " << #cond << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
  } \
} while(0)

static std::string repeated(const std::string& s, int times) {
  std::string out;
  for(int i = 0; i < times; ++i) out += s;
  return out;
}

/*count how many standalone chunks are in a chunk stream (no PNG signature)*/
static size_t count_chunks(const std::vector<unsigned char>& data) {
  size_t n = 0;
  size_t pos = 0;
  while(pos + 12 <= data.size()) {
    size_t len = (size_t)lodepng_chunk_length(&data[pos]);
    pos += 12 + len;
    ++n;
  }
  return n;
}

struct Sample {
  std::string name;
  std::string key;
  std::vector<unsigned char> value;
};

static Sample make_short() {
  Sample s;
  s.name = "short";
  s.key = "Title";
  std::string t = "hello";
  s.value.assign(t.begin(), t.end());
  return s;
}

static Sample make_long() {
  Sample s;
  s.name = "long";
  s.key = "Description";
  /*repetitive prose: compresses strongly, still small (about 1 KiB)*/
  std::string t = repeated("The quick brown fox jumps over the lazy dog. ", 20);
  s.value.assign(t.begin(), t.end());
  return s;
}

static Sample make_chinese() {
  Sample s;
  s.name = "chinese";
  s.key = "Comment";
  /*UTF-8 Chinese text, repeated so it is long enough to compress*/
  const char* hanzi =
      "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\xef\xbc\x8c"
      "\xe8\xbf\x99\xe6\x98\xaf\xe4\xb8\x80\xe6\xae\xb5\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe6\x9c\xac\xef\xbc\x81";
  std::string t;
  for(int i = 0; i < 12; ++i) t += hanzi;
  s.value.assign(t.begin(), t.end());
  return s;
}

static Sample make_compressed() {
  Sample s;
  s.name = "already-compressed";
  s.key = "RawData";
  /*build real zlib data with lodepng itself, then hand it in as opaque bytes*/
  std::string t = repeated("abcdefghijklmnopqrstuvwxyz0123456789", 8);
  std::vector<unsigned char> in(t.begin(), t.end());
  unsigned char* out = 0;
  size_t outsize = 0;
  unsigned error = lodepng_zlib_compress(&out, &outsize,
                                         in.empty() ? 0 : &in[0], in.size(),
                                         &lodepng_default_compress_settings);
  if(error) {
    std::cout << "sample setup zlib error " << error << "\n";
    ++g_failures;
    return s;
  }
  /*make it incompressible-looking by XOR cycling the deflate payload bytes*/
  for(size_t i = 2; i + 4 < outsize; ++i) out[i] = (unsigned char)(out[i] ^ (unsigned char)(i * 37 + 11));
  s.value.assign(out, out + outsize);
  free(out);
  return s;
}

struct SwitchCombo {
  const char* name;
  int compress;
  int merge;
  int threshold;
};

static unsigned encode_one(const Sample& sample, const SwitchCombo& combo,
                           unsigned level, size_t threshold,
                           std::vector<unsigned char>& out) {
  LodePNGTextPackSettings settings;
  lodepng_text_pack_settings_init(&settings);
  settings.compress_enabled = combo.compress;
  settings.merge_enabled = combo.merge;
  settings.threshold_enabled = combo.threshold;
  settings.compression_level = level;
  settings.compress_threshold = threshold;

  const char* keys[1] = { sample.key.c_str() };
  size_t key_sizes[1] = { sample.key.size() };
  const unsigned char* vals[1] = { sample.value.empty() ? 0 : &sample.value[0] };
  size_t val_sizes[1] = { sample.value.size() };

  unsigned char* buf = 0;
  size_t bufsize = 0;
  unsigned error = lodepng_text_pack_encode(&buf, &bufsize, keys, key_sizes,
                                            vals, val_sizes, 1, &settings);
  out.assign(buf, buf + bufsize);
  free(buf);
  return error;
}

/*encode three copies of the sample to make merging observable*/
static unsigned encode_three(const Sample& sample, const SwitchCombo& combo,
                             unsigned level, size_t threshold, size_t payload_limit,
                             std::vector<unsigned char>& out) {
  LodePNGTextPackSettings settings;
  lodepng_text_pack_settings_init(&settings);
  settings.compress_enabled = combo.compress;
  settings.merge_enabled = combo.merge;
  settings.threshold_enabled = combo.threshold;
  settings.compression_level = level;
  settings.compress_threshold = threshold;
  settings.max_chunk_payload = payload_limit;

  std::vector<const char*> keys(3, sample.key.c_str());
  std::vector<size_t> key_sizes(3, sample.key.size());
  std::vector<const unsigned char*> vals(3, sample.value.empty() ? 0 : &sample.value[0]);
  std::vector<size_t> val_sizes(3, sample.value.size());

  unsigned char* buf = 0;
  size_t bufsize = 0;
  unsigned error = lodepng_text_pack_encode(&buf, &bufsize,
                                            &keys[0], &key_sizes[0],
                                            &vals[0], &val_sizes[0], 3, &settings);
  out.assign(buf, buf + bufsize);
  free(buf);
  return error;
}

static unsigned roundtrip_decode(const std::vector<unsigned char>& in,
                                 std::vector<std::string>& keys,
                                 std::vector<std::vector<unsigned char> >& values) {
  char** dkeys = 0;
  size_t* dks = 0;
  unsigned char** dvalues = 0;
  size_t* dvs = 0;
  size_t n = 0;
  unsigned error = lodepng_text_pack_decode(in.empty() ? 0 : &in[0], in.size(),
                                            &dkeys, &dks, &dvalues, &dvs, &n);
  keys.clear();
  values.clear();
  if(!error) {
    for(size_t i = 0; i < n; ++i) {
      keys.push_back(std::string(dkeys[i], dks[i]));
      values.push_back(std::vector<unsigned char>(dvalues[i], dvalues[i] + dvs[i]));
    }
  }
  lodepng_text_pack_free(dkeys, dks, dvalues, dvs, n);
  return error;
}

static double seconds_since(clock_t start) {
  return (double)(clock() - start) / CLOCKS_PER_SEC;
}

/* ---- table 1: chunk sizes across switch combinations -------------------- */
static void test_sizes(const std::vector<Sample>& samples,
                       const std::vector<SwitchCombo>& combos) {
  std::cout << "\n=== 1) chunk sizes (bytes; single entry on the left, 3 entries on the right) ===\n";
  std::cout << std::setw(18) << "sample" << std::setw(20) << "switches"
            << std::setw(10) << "1: total" << std::setw(10) << "1: chunks"
            << std::setw(10) << "3: total" << std::setw(10) << "3: chunks"
            << "\n";
  for(size_t si = 0; si < samples.size(); ++si) {
    for(size_t ci = 0; ci < combos.size(); ++ci) {
      std::vector<unsigned char> one, three;
      unsigned e1 = encode_one(samples[si], combos[ci], 6, 128, one);
      unsigned e3 = encode_three(samples[si], combos[ci], 6, 128,
                                 LODEPNG_TEXT_PACK_MAX_CHUNK_PAYLOAD, three);
      CHECK(e1 == 0);
      CHECK(e3 == 0);
      std::cout << std::setw(18) << samples[si].name
                << std::setw(20) << combos[ci].name
                << std::setw(10) << one.size()
                << std::setw(10) << count_chunks(one)
                << std::setw(10) << three.size()
                << std::setw(10) << count_chunks(three) << "\n";
      if(combos[ci].merge) CHECK(count_chunks(three) == 1);
      else CHECK(count_chunks(three) == 3);
    }
  }
}

/* ---- table 2: write/read-back consistency ------------------------------- */
static void test_consistency(const std::vector<Sample>& samples,
                             const std::vector<SwitchCombo>& combos) {
  std::cout << "\n=== 2) write-then-read-back consistency ===\n";
  for(size_t si = 0; si < samples.size(); ++si) {
    for(size_t ci = 0; ci < combos.size(); ++ci) {
      std::vector<unsigned char> encoded;
      unsigned error = encode_three(samples[si], combos[ci], 6, 128,
                                    LODEPNG_TEXT_PACK_MAX_CHUNK_PAYLOAD, encoded);
      CHECK(error == 0);
      std::vector<std::string> keys;
      std::vector<std::vector<unsigned char> > values;
      error = roundtrip_decode(encoded, keys, values);
      CHECK(error == 0);
      CHECK(keys.size() == 3);
      CHECK(values.size() == 3);
      for(int k = 0; k < 3; ++k) {
        CHECK(keys[k] == samples[si].key);
        CHECK(values[k] == samples[si].value);
      }
    }
    std::cout << "  " << samples[si].name << ": all 8 switch combos round-tripped, "
              << "3/3 entries byte-identical each time\n";
  }
}

/* ---- table 3: timing ----------------------------------------------------- */
static void test_timing(const std::vector<Sample>& samples) {
  std::cout << "\n=== 3) timing (50 encode + 50 decode iterations, seconds) ===\n";
  std::cout << std::setw(18) << "sample" << std::setw(16) << "raw bytes"
            << std::setw(14) << "encode s" << std::setw(14) << "decode s" << "\n";
  SwitchCombo all_on = { "all-on", 1, 1, 1 };
  for(size_t si = 0; si < samples.size(); ++si) {
    std::vector<unsigned char> encoded;
    unsigned error = encode_three(samples[si], all_on, 6, 128,
                                  LODEPNG_TEXT_PACK_MAX_CHUNK_PAYLOAD, encoded);
    CHECK(error == 0);

    clock_t t0 = clock();
    for(int rep = 0; rep < 50; ++rep) {
      std::vector<unsigned char> tmp;
      unsigned e = encode_three(samples[si], all_on, 6, 128,
                                LODEPNG_TEXT_PACK_MAX_CHUNK_PAYLOAD, tmp);
      CHECK(e == 0);
    }
    double enc_time = seconds_since(t0);

    t0 = clock();
    for(int rep = 0; rep < 50; ++rep) {
      std::vector<std::string> keys;
      std::vector<std::vector<unsigned char> > values;
      unsigned e = roundtrip_decode(encoded, keys, values);
      CHECK(e == 0);
      CHECK(keys.size() == 3);
    }
    double dec_time = seconds_since(t0);

    std::cout << std::setw(18) << samples[si].name
              << std::setw(16) << samples[si].value.size()
              << std::setw(14) << std::fixed << std::setprecision(4) << enc_time
              << std::setw(14) << dec_time << "\n";
  }
}

/* ---- table 4: threshold sweep ------------------------------------------- */
static void test_threshold_curve(const std::vector<Sample>& samples) {
  std::cout << "\n=== 4) threshold sweep (compress=on, merge=on, 3 entries; total chunk bytes) ===\n";
  size_t thresholds[] = {0, 32, 64, 128, 256, 512, 1024, 2048};
  std::cout << std::setw(18) << "sample";
  for(size_t i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]); ++i)
    std::cout << std::setw(8) << thresholds[i];
  std::cout << "   (bytes)\n";
  SwitchCombo sweep = { "sweep", 1, 1, 1 };
  for(size_t si = 0; si < samples.size(); ++si) {
    std::cout << std::setw(18) << samples[si].name;
    for(size_t i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]); ++i) {
      std::vector<unsigned char> encoded;
      unsigned error = encode_three(samples[si], sweep, 6, thresholds[i],
                                    LODEPNG_TEXT_PACK_MAX_CHUNK_PAYLOAD, encoded);
      CHECK(error == 0);
      std::vector<std::string> keys;
      std::vector<std::vector<unsigned char> > values;
      CHECK(roundtrip_decode(encoded, keys, values) == 0);
      CHECK(values.size() == 3 && values[0] == samples[si].value);
      std::cout << std::setw(8) << encoded.size();
    }
    std::cout << "\n";
  }
  /*threshold switch off must equal threshold 0 behavior (everything eligible)*/
  SwitchCombo no_threshold = { "no-thr", 1, 1, 0 };
  for(size_t si = 0; si < samples.size(); ++si) {
    std::vector<unsigned char> a, b;
    CHECK(encode_three(samples[si], sweep, 6, 0,
                       LODEPNG_TEXT_PACK_MAX_CHUNK_PAYLOAD, a) == 0);
    CHECK(encode_three(samples[si], no_threshold, 6, 128,
                       LODEPNG_TEXT_PACK_MAX_CHUNK_PAYLOAD, b) == 0);
    CHECK(a == b);
  }
  std::cout << "  threshold_enabled=0 produces identical bytes to threshold=0\n";
}

/* ---- table 5: chunk size limit ------------------------------------------ */
static void test_size_limit(const Sample& long_sample) {
  std::cout << "\n=== 5) single-chunk payload limit ===\n";
  std::cout << "  default limit = " << LODEPNG_TEXT_PACK_MAX_CHUNK_PAYLOAD
            << " bytes (8 MiB), hard cap = "
            << LODEPNG_TEXT_PACK_MAX_CHUNK_PAYLOAD_LIMIT << " bytes (PNG per-chunk max)\n";

  /*a single frame larger than a tiny custom limit must be rejected, not split*/
  LodePNGTextPackSettings settings;
  lodepng_text_pack_settings_init(&settings);
  settings.merge_enabled = 1;
  settings.max_chunk_payload = 64; /*smaller than this entry's frame*/
  const char* keys[1] = { long_sample.key.c_str() };
  size_t key_sizes[1] = { long_sample.key.size() };
  const unsigned char* vals[1] = { &long_sample.value[0] };
  size_t val_sizes[1] = { long_sample.value.size() };
  unsigned char* buf = 0;
  size_t bufsize = 0;
  unsigned error = lodepng_text_pack_encode(&buf, &bufsize, keys, key_sizes,
                                            vals, val_sizes, 1, &settings);
  free(buf);
  CHECK(error == 124);
  std::cout << "  oversized entry rejected with error " << error
            << " (\"" << lodepng_error_text(error) << "\")\n";

  /*a limit large enough lets the same entry through*/
  settings.max_chunk_payload = LODEPNG_TEXT_PACK_MAX_CHUNK_PAYLOAD;
  error = lodepng_text_pack_encode(&buf, &bufsize, keys, key_sizes,
                                   vals, val_sizes, 1, &settings);
  CHECK(error == 0);
  CHECK(count_chunks(std::vector<unsigned char>(buf, buf + bufsize)) == 1);
  free(buf);
  std::cout << "  same entry within the default limit: 1 chunk, error 0\n";
}

/* ---- table 6: corrupt compressed data must error, never produce garbage - */
static void test_corrupt() {
  std::cout << "\n=== 6) corrupt data rejection ===\n";

  /*(a) valid compressed chunk, then a flipped byte inside the zlib stream*/
  Sample s = make_long();
  SwitchCombo combo = { "all-on", 1, 1, 0 }; /*force compression*/
  std::vector<unsigned char> encoded;
  CHECK(encode_one(s, combo, 6, 128, encoded) == 0);
  CHECK(count_chunks(encoded) == 1);
  std::vector<unsigned char> corrupted = encoded;
  /*chunk layout: 4 len, 4 type, 1 flag, frame... corrupt a late data byte
  but stay inside the chunk so the CRC check also fires if reached*/
  size_t chunk_len = lodepng_chunk_length(&corrupted[0]);
  size_t corrupt_at = 8 + chunk_len - 6;
  corrupted[corrupt_at] = (unsigned char)(corrupted[corrupt_at] ^ 0xFF);

  char** dkeys = 0;
  size_t* dks = 0;
  unsigned char** dvalues = 0;
  size_t* dvs = 0;
  size_t n = 0;
  unsigned error = lodepng_text_pack_decode(&corrupted[0], corrupted.size(),
                                            &dkeys, &dks, &dvalues, &dvs, &n);
  lodepng_text_pack_free(dkeys, dks, dvalues, dvs, n);
  CHECK(error != 0);
  std::cout << "  (a) corrupted zlib/CRC byte -> error " << error
            << " (\"" << lodepng_error_text(error) << "\"), no output accepted\n";

  /*(a2) corrupt the zlib stream itself but regenerate a valid CRC, so the
  rejection must come from the zlib decoder rather than the CRC check*/
  std::vector<unsigned char> zcorrupt = encoded;
  size_t zchunk_len = lodepng_chunk_length(&zcorrupt[0]);
  /*the value zlib stream starts after: flag(1) + key-len(4) + key(11) +
  value-len(4); corrupt a byte in its middle, well clear of the adler32 tail*/
  size_t z_at = 8 + 1 + 4 + (size_t)strlen(s.key.c_str()) + 4 + 6;
  CHECK(z_at < 8 + zchunk_len - 6);
  zcorrupt[z_at] = (unsigned char)(zcorrupt[z_at] ^ 0x5A);
  lodepng_chunk_generate_crc(&zcorrupt[0]);
  error = lodepng_text_pack_decode(&zcorrupt[0], zcorrupt.size(),
                                   &dkeys, &dks, &dvalues, &dvs, &n);
  lodepng_text_pack_free(dkeys, dks, dvalues, dvs, n);
  CHECK(error != 0);
  CHECK(error != 57); /*a valid CRC means this is a genuine zlib failure*/
  std::cout << "  (a2) corrupt zlib, valid CRC -> error " << error
            << " (\"" << lodepng_error_text(error) << "\"), decoded data rejected\n";

  /*(b) truncated stream: cut the chunk (and its CRC) short*/
  std::vector<unsigned char> truncated(encoded.begin(), encoded.begin() + (long)(encoded.size() - 8));
  /*fix the declared length so parsing reaches the broken zlib tail*/
  size_t new_len = truncated.size() - 8;
  truncated[0]=(unsigned char)((new_len>>24)&0xFF); truncated[1]=(unsigned char)((new_len>>16)&0xFF);
  truncated[2]=(unsigned char)((new_len>>8)&0xFF); truncated[3]=(unsigned char)(new_len&0xFF);
  error = lodepng_text_pack_decode(&truncated[0], truncated.size(),
                                   &dkeys, &dks, &dvalues, &dvs, &n);
  lodepng_text_pack_free(dkeys, dks, dvalues, dvs, n);
  CHECK(error != 0);
  std::cout << "  (b) truncated zlib stream     -> error " << error
            << " (\"" << lodepng_error_text(error) << "\")\n";

  /*(c) unknown reserved flag bit*/
  std::vector<unsigned char> badflag = encoded;
  badflag[8] = 0x7F;
  /*repair CRC so the failure is specifically the flag check*/
  lodepng_chunk_generate_crc(&badflag[0]);
  error = lodepng_text_pack_decode(&badflag[0], badflag.size(),
                                   &dkeys, &dks, &dvalues, &dvs, &n);
  lodepng_text_pack_free(dkeys, dks, dvalues, dvs, n);
  CHECK(error == 125);
  std::cout << "  (c) reserved flag bit set     -> error " << error
            << " (\"" << lodepng_error_text(error) << "\")\n";

  /*(d) malformed frame: claim a huge key length that runs past the payload*/
  std::vector<unsigned char> badframe = encoded;
  badframe[9] = 0xFF; /*first key-length byte*/
  badframe[10] = 0xFF;
  lodepng_chunk_generate_crc(&badframe[0]);
  error = lodepng_text_pack_decode(&badframe[0], badframe.size(),
                                   &dkeys, &dks, &dvalues, &dvs, &n);
  lodepng_text_pack_free(dkeys, dks, dvalues, dvs, n);
  CHECK(error == 125);
  std::cout << "  (d) out-of-range frame length -> error " << error
            << " (\"" << lodepng_error_text(error) << "\")\n";

  /*(e) invalid compression level is rejected at encode time*/
  LodePNGTextPackSettings settings;
  lodepng_text_pack_settings_init(&settings);
  settings.compression_level = 10;
  const char* keys[1] = { s.key.c_str() };
  size_t key_sizes[1] = { s.key.size() };
  const unsigned char* vals[1] = { &s.value[0] };
  size_t val_sizes[1] = { s.value.size() };
  unsigned char* buf = 0;
  size_t bufsize = 0;
  error = lodepng_text_pack_encode(&buf, &bufsize, keys, key_sizes,
                                   vals, val_sizes, 1, &settings);
  free(buf);
  CHECK(error == 61);
  std::cout << "  (e) level 10 rejected         -> error " << error << "\n";
}

/* ---- table 7: classic tEXt path byte-for-byte unchanged ----------------- */
static void test_legacy_text_identical() {
  std::cout << "\n=== 7) classic tEXt path byte-for-byte unchanged ===\n";
  /*hand-build the exact bytes a tEXt chunk for key "Title"/text "hello" must have:
  length=11, "tEXt", "Title", 0, "hello", CRC32 over type+data*/
  const char* key = "Title";
  const char* text = "hello";
  std::vector<unsigned char> expected;
  size_t data_len = strlen(key) + 1 + strlen(text);
  std::vector<unsigned char> data(data_len);
  size_t p = 0;
  for(size_t i = 0; i < strlen(key); ++i) data[p++] = (unsigned char)key[i];
  data[p++] = 0;
  for(size_t i = 0; i < strlen(text); ++i) data[p++] = (unsigned char)text[i];

  unsigned char* chunk = 0;
  size_t chunk_size = 0;
  unsigned error = lodepng_chunk_create(&chunk, &chunk_size, data.size(), "tEXt", &data[0]);
  CHECK(error == 0);
  expected.assign(chunk, chunk + chunk_size);
  free(chunk);

  /*produce a tEXt chunk through the public state API the same way the
  encoder does, and compare every byte*/
  LodePNGState state;
  lodepng_state_init(&state);
  state.encoder.text_compression = 0; /*force the plain tEXt path, not zTXt*/
  error = lodepng_add_text(&state.info_png, key, text);
  CHECK(error == 0);
  std::vector<unsigned char> image(4 * 4); /*1x1 RGBA*/
  std::vector<unsigned char> png;
  unsigned char* pngbuf = 0;
  size_t pngsize = 0;
  error = lodepng_encode(&pngbuf, &pngsize, &image[0], 1, 1, &state);
  CHECK(error == 0);
  png.assign(pngbuf, pngbuf + pngsize);
  free(pngbuf);

  const unsigned char* found = lodepng_chunk_find_const(&png[8], &png[0] + png.size(), "tEXt");
  CHECK(found != 0);
  size_t found_len = lodepng_chunk_length(found);
  CHECK(found_len == data_len);
  bool identical = true;
  for(size_t i = 0; i < found_len + 12; ++i) {
    if(found[i] != expected[i]) identical = false;
  }
  CHECK(identical);
  std::cout << "  encoder-emitted tEXt chunk (" << (found_len + 12)
            << " bytes) matches hand-built reference byte-for-byte: "
            << (identical ? "yes" : "NO") << "\n";

  /*the new txMt decoder must ignore standard tEXt chunks, not misread them*/
  char** dkeys = 0;
  size_t* dks = 0;
  unsigned char** dvalues = 0;
  size_t* dvs = 0;
  size_t n = 0;
  error = lodepng_text_pack_decode(found, found_len + 12,
                                   &dkeys, &dks, &dvalues, &dvs, &n);
  lodepng_text_pack_free(dkeys, dks, dvalues, dvs, n);
  CHECK(error == 0);
  CHECK(n == 0);
  std::cout << "  txMt decoder ignores a tEXt chunk (0 entries, error 0)\n";
  lodepng_state_cleanup(&state);
}

/* ---- compression level effect on long text ------------------------------ */
static void test_levels(const Sample& long_sample) {
  std::cout << "\n=== 8) compression level sweep (single long entry, chunk bytes) ===\n";
  SwitchCombo combo = { "all-on", 1, 1, 0 };
  std::cout << std::setw(10) << "level";
  for(unsigned lvl = 0; lvl <= 9; ++lvl) std::cout << std::setw(8) << lvl;
  std::cout << "\n" << std::setw(10) << "bytes";
  for(unsigned lvl = 0; lvl <= 9; ++lvl) {
    std::vector<unsigned char> encoded;
    unsigned error = encode_one(long_sample, combo, lvl, 128, encoded);
    CHECK(error == 0);
    std::vector<std::string> keys;
    std::vector<std::vector<unsigned char> > values;
    CHECK(roundtrip_decode(encoded, keys, values) == 0);
    CHECK(values.size() == 1 && values[0] == long_sample.value);
    std::cout << std::setw(8) << encoded.size();
  }
  std::cout << "\n";
}

int main() {
  std::vector<Sample> samples;
  samples.push_back(make_short());
  samples.push_back(make_long());
  samples.push_back(make_chinese());
  samples.push_back(make_compressed());
  for(size_t i = 0; i < samples.size(); ++i) {
    if(samples[i].value.empty() && samples[i].name != "short") {
      std::cout << "sample setup failed for " << samples[i].name << "\n";
      return 1;
    }
  }

  SwitchCombo combos[8] = {
    {"c0 m0 t0", 0, 0, 0},
    {"c0 m0 t1", 0, 0, 1},
    {"c0 m1 t0", 0, 1, 0},
    {"c0 m1 t1", 0, 1, 1},
    {"c1 m0 t0", 1, 0, 0},
    {"c1 m0 t1", 1, 0, 1},
    {"c1 m1 t0", 1, 1, 0},
    {"c1 m1 t1", 1, 1, 1}
  };
  std::vector<SwitchCombo> combo_vec(combos, combos + 8);

  test_sizes(samples, combo_vec);
  test_consistency(samples, combo_vec);
  test_timing(samples);
  test_threshold_curve(samples);
  test_size_limit(samples[1]);
  test_corrupt();
  test_legacy_text_identical();
  test_levels(samples[1]);

  std::cout << "\nchecks: " << g_checks << ", failures: " << g_failures << "\n";
  if(g_failures) {
    std::cout << "TEST FAILED\n";
    return 1;
  }
  std::cout << "ALL TESTS PASSED\n";
  return 0;
}
