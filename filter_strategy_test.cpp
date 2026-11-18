/*Tests for the PNG filter strategy interface of lodepng.cpp.

The test:
1. Encodes five small synthetic samples (photo, screenshot, line art, gradient,
   alpha) with every filter strategy, with and without interlacing, for fixed
   RGBA/RGB output and for the automatic color mode.
2. Compares the SHA-256 of every encoded PNG against golden hashes that were
   generated with the implementation from before the strategy interface was
   refactored. This proves the refactor is byte-for-byte identical.
3. Decodes the result again and checks the pixels survived the roundtrip.
4. Checks that a misconfigured strategy interface reports an error instead of
   silently falling back to a default.

This file only needs the standard library and lodepng/lodepng_util.
*/

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "filter_strategy_test_common.h"
#include "filter_strategy_golden.h"
#include "lodepng.h"
#include "lodepng_util.h"

#ifdef LODEPNG_TEST_FILTER_INTERFACE
/*test-only hook compiled into lodepng.cpp when LODEPNG_TEST_FILTER_INTERFACE is defined*/
void lodepng_test_filter_set_fault(unsigned fault);
#endif

static int g_failures = 0;

#define CHECK(cond) do { \
  if(!(cond)) { \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_failures; \
  } \
} while(0)

static void checkEquals(unsigned expected, unsigned actual, const char* what, int line) {
  if(expected != actual) {
    std::printf("FAIL %s:%d: %s: expected %u, got %u\n", __FILE__, line, what, expected, actual);
    ++g_failures;
  }
}

#define CHECK_EQUALS(expected, actual) checkEquals((unsigned)(expected), (unsigned)(actual), #actual, __LINE__)

/*description of one encode configuration to test*/
struct TestConfig {
  std::string label;
  LodePNGFilterStrategy strategy;
  unsigned interlace;
  unsigned colorMode; /*0: auto_convert, 1: fixed RGBA8, 2: fixed RGB8*/
};

static std::string makeLabel(unsigned colorMode, unsigned interlace, const char* strategyName) {
  char buf[64];
  std::sprintf(buf, "%s/i%u/%s",
               colorMode == 0 ? "auto" : (colorMode == 1 ? "rgba" : "rgb"),
               interlace, strategyName);
  return std::string(buf);
}

static std::vector<TestConfig> makeTestConfigs() {
  static const char* names[9] = {
    "zero", "sub", "up", "average", "paeth",
    "minsum", "entropy", "bruteforce", "predefined"
  };
  std::vector<TestConfig> configs;
  unsigned mode, interlace, s;

  for(mode = 1; mode <= 2; ++mode) {
    unsigned maxInterlace = mode == 1 ? 2 : 1;
    for(interlace = 0; interlace < maxInterlace; ++interlace) {
      for(s = 0; s < 9; ++s) {
        TestConfig config;
        config.label = makeLabel(mode, interlace, names[s]);
        config.strategy = (LodePNGFilterStrategy)s;
        config.interlace = interlace;
        config.colorMode = mode;
        configs.push_back(config);
      }
    }
  }

  /*default settings: auto color conversion with the default filter palette zero behavior*/
  for(interlace = 0; interlace < 2; ++interlace) {
    TestConfig config;
    config.label = makeLabel(0, interlace, "minsum");
    config.strategy = LFS_MINSUM;
    config.interlace = interlace;
    config.colorMode = 0;
    configs.push_back(config);
  }

  return configs;
}

static unsigned encodeSample(std::vector<unsigned char>& png, const FilterTestSample& sample,
                             const TestConfig& config) {
  lodepng::State state;
  std::vector<unsigned char> predefined;

  state.info_raw.colortype = LCT_RGBA;
  state.info_raw.bitdepth = 8;
  state.encoder.filter_strategy = config.strategy;
  state.encoder.filter_palette_zero = 1;
  state.info_png.interlace_method = config.interlace;

  if(config.colorMode != 0) {
    /*force the output color mode so that filter_palette_zero cannot override the
    chosen strategy and all five filter types get exercised*/
    state.encoder.auto_convert = 0;
    state.encoder.filter_palette_zero = 0;
    state.info_png.color.colortype = config.colorMode == 1 ? LCT_RGBA : LCT_RGB;
    state.info_png.color.bitdepth = 8;
  }

  if(config.strategy == LFS_PREDEFINED) {
    unsigned i;
    predefined.resize(sample.h);
    for(i = 0; i < sample.h; ++i) predefined[i] = (unsigned char)(i % 5);
    state.encoder.predefined_filters = &predefined[0];
  }

  return lodepng::encode(png, &sample.rgba[0], sample.w, sample.h, state);
}

/*decode the PNG back to RGBA and compare against the sample. For RGB output the
alpha channel is expected to have been dropped.*/
static void checkRoundtrip(const FilterTestSample& sample, const std::vector<unsigned char>& png,
                           unsigned colorMode) {
  std::vector<unsigned char> decoded;
  unsigned w = 0, h = 0;
  unsigned error = lodepng::decode(decoded, w, h, png, LCT_RGBA, 8);
  CHECK_EQUALS(0, error);
  CHECK_EQUALS(sample.w, w);
  CHECK_EQUALS(sample.h, h);
  if(error || w != sample.w || h != sample.h) return;
  CHECK(decoded.size() == sample.rgba.size());
  if(decoded.size() != sample.rgba.size()) return;

  {
    size_t i;
    size_t bad = 0;
    for(i = 0; i < decoded.size(); i += 4) {
      unsigned channels = colorMode == 2 ? 3 : 4;
      size_t c;
      for(c = 0; c < channels; ++c) {
        if(decoded[i + c] != sample.rgba[i + c]) ++bad;
      }
    }
    if(bad) {
      std::printf("FAIL roundtrip of sample '%s' (%s): %lu differing channel values\n",
                  sample.name.c_str(), colorMode == 2 ? "rgb" : "rgba", (unsigned long)bad);
      ++g_failures;
    }
  }
}

/*For non-interlaced fixed-filter strategies the filter type of every scanline must be
exactly the requested one. This checks that the strategy interface actually dispatches
the chosen implementation.*/
static void checkFilterTypes(const FilterTestSample& sample, const std::vector<unsigned char>& png,
                             const TestConfig& config) {
  std::vector<unsigned char> types;
  unsigned error;
  unsigned y;
  if(config.interlace != 0 || config.colorMode == 0) return;
  if(config.strategy > LFS_FOUR && config.strategy != LFS_PREDEFINED) return;
  error = lodepng::getFilterTypes(types, png);
  CHECK_EQUALS(0, error);
  if(error) return;
  CHECK_EQUALS(sample.h, types.size());
  if(types.size() != sample.h) return;
  for(y = 0; y < sample.h; ++y) {
    unsigned expected = config.strategy == LFS_PREDEFINED ? (y % 5) : (unsigned)config.strategy;
    if(types[y] != expected) {
      std::printf("FAIL sample '%s' config '%s': scanline %u uses filter %u, expected %u\n",
                  sample.name.c_str(), config.label.c_str(), y, (unsigned)types[y], expected);
      ++g_failures;
      break;
    }
  }
}

static const GoldenEntry* findGolden(const std::string& sample, const std::string& label) {
  size_t i;
  for(i = 0; i < kGoldenCount; ++i) {
    if(sample == kGolden[i].sample && label == kGolden[i].label) return &kGolden[i];
  }
  return 0;
}

/*returns nonzero on failure*/
/*Extract the uncompressed filtered scanline bytes from the IDAT chunks of a PNG.
This lets the test compare the filter output itself, independent of the deflate
encoder, against what the original implementation produced.*/
static bool extractFilteredBytes(std::vector<unsigned char>& out, const std::vector<unsigned char>& png) {
  std::vector<unsigned char> idat;
  size_t pos = 8; /*skip the PNG signature*/
  out.clear();
  while(pos + 12 <= png.size()) {
    unsigned length = ((unsigned)png[pos] << 24) | ((unsigned)png[pos + 1] << 16) |
                      ((unsigned)png[pos + 2] << 8) | (unsigned)png[pos + 3];
    char typebuf[5];
    size_t i;
    for(i = 0; i < 4; ++i) typebuf[i] = (char)png[pos + 4 + i];
    typebuf[4] = 0;
    if(pos + 12 + length > png.size()) return false;
    if(typebuf[0] == 'I' && typebuf[1] == 'D' && typebuf[2] == 'A' && typebuf[3] == 'T') {
      idat.insert(idat.end(), png.begin() + (long)pos + 8, png.begin() + (long)(pos + 8 + length));
    }
    if(typebuf[0] == 'I' && typebuf[1] == 'E' && typebuf[2] == 'N' && typebuf[3] == 'D') break;
    pos += 12 + length;
  }
  if(idat.empty()) return false;
  if(lodepng::decompress(out, idat) != 0) return false;
  return true;
}

static int testByteIdenticalOutput(bool printOnly) {
  std::vector<FilterTestSample> samples = filterTestMakeSamples();
  std::vector<TestConfig> configs = makeTestConfigs();
  size_t si, ci;

  for(si = 0; si < samples.size(); ++si) {
    for(ci = 0; ci < configs.size(); ++ci) {
      const FilterTestSample& sample = samples[si];
      const TestConfig& config = configs[ci];
      std::vector<unsigned char> png;
      std::string hash;
      unsigned error = encodeSample(png, sample, config);
      CHECK_EQUALS(0, error);
      if(error) {
        std::printf("  sample '%s' config '%s' failed with error %u: %s\n",
                    sample.name.c_str(), config.label.c_str(), error,
                    lodepng_error_text(error));
        continue;
      }
      hash = filterTestSha256(png);

      if(printOnly) {
        std::vector<unsigned char> filtered;
        std::string filteredHash = "decompress-failed";
        if(extractFilteredBytes(filtered, png)) filteredHash = filterTestSha256(filtered);
        std::printf("%s|%s|%s|%s|%lu\n", sample.name.c_str(), config.label.c_str(),
                    hash.c_str(), filteredHash.c_str(), (unsigned long)png.size());
        continue;
      }

      {
        const GoldenEntry* golden = findGolden(sample.name, config.label);
        std::vector<unsigned char> filtered;
        std::string filteredHash;
        bool haveFiltered = extractFilteredBytes(filtered, png);
        if(haveFiltered) filteredHash = filterTestSha256(filtered);
        if(!golden) {
          std::printf("FAIL sample '%s' config '%s': no golden hash available\n",
                      sample.name.c_str(), config.label.c_str());
          ++g_failures;
        } else if(hash != golden->hash) {
          std::printf("FAIL sample '%s' config '%s': encoded bytes differ from before the refactor\n"
                      "  expected sha256 %s\n  actual   sha256 %s (%lu bytes)\n",
                      sample.name.c_str(), config.label.c_str(), golden->hash.c_str(), hash.c_str(),
                      (unsigned long)png.size());
          ++g_failures;
        } else if(png.size() != golden->size) {
          std::printf("FAIL sample '%s' config '%s': size %lu, golden says %lu\n",
                      sample.name.c_str(), config.label.c_str(),
                      (unsigned long)png.size(), (unsigned long)golden->size);
          ++g_failures;
        } else if(!haveFiltered) {
          std::printf("FAIL sample '%s' config '%s': could not decompress the IDAT data\n",
                      sample.name.c_str(), config.label.c_str());
          ++g_failures;
        } else if(filteredHash != golden->filteredHash) {
          std::printf("FAIL sample '%s' config '%s': filtered scanline bytes differ\n"
                      "  expected sha256 %s\n  actual   sha256 %s\n",
                      sample.name.c_str(), config.label.c_str(),
                      golden->filteredHash.c_str(), filteredHash.c_str());
          ++g_failures;
        }
      }

      checkRoundtrip(sample, png, config.colorMode);
      checkFilterTypes(sample, png, config);
    }
  }
  return g_failures;
}

/*misconfigured strategies must return an error instead of using a default*/
static void testMisconfiguredStrategy() {
  std::vector<FilterTestSample> samples = filterTestMakeSamples();
  const FilterTestSample& sample = samples[0];
  static const int invalid[] = { -1, 9, 10, 100, 1000, 0x7fffffff };
  size_t i;
  unsigned interlace;

  for(i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    for(interlace = 0; interlace < 2; ++interlace) {
      lodepng::State state;
      std::vector<unsigned char> png;
      unsigned error;
      state.encoder.filter_strategy = (LodePNGFilterStrategy)invalid[i];
      state.info_png.interlace_method = interlace;
      error = lodepng::encode(png, &sample.rgba[0], sample.w, sample.h, state);
      CHECK_EQUALS(88, error);
    }
  }

  /*the palette/low bitdepth override must not hide an invalid strategy*/
  {
    unsigned w = 16, h = 16, x, y;
    std::vector<unsigned char> image(w * h * 4);
    lodepng::State state;
    std::vector<unsigned char> png;
    for(y = 0; y < h; ++y) {
      for(x = 0; x < w; ++x) {
        unsigned char v = (unsigned char)(((x / 4) + (y / 4)) % 2 ? 0 : 255);
        size_t p = ((size_t)y * w + x) * 4;
        image[p + 0] = v;
        image[p + 1] = v;
        image[p + 2] = v;
        image[p + 3] = 255;
      }
    }
    state.encoder.filter_palette_zero = 1;
    state.encoder.filter_strategy = (LodePNGFilterStrategy)9;
    CHECK_EQUALS(88, lodepng::encode(png, &image[0], w, h, state));
  }
}

/*a strategy whose inputs are not set up must error, not crash or silently use defaults*/
static void testMisconfiguredPredefinedFilters() {
  std::vector<FilterTestSample> samples = filterTestMakeSamples();
  const FilterTestSample& sample = samples[0];
  unsigned y;

  /*LFS_PREDEFINED without a buffer*/
  {
    lodepng::State state;
    std::vector<unsigned char> png;
    state.encoder.filter_strategy = LFS_PREDEFINED;
    state.encoder.filter_palette_zero = 0;
    state.encoder.predefined_filters = 0;
    CHECK_EQUALS(88, lodepng::encode(png, &sample.rgba[0], sample.w, sample.h, state));
  }
  /*LFS_PREDEFINED with an invalid filter type in the buffer*/
  {
    std::vector<unsigned char> predefined(sample.h, 5);
    lodepng::State state;
    std::vector<unsigned char> png;
    state.encoder.filter_strategy = LFS_PREDEFINED;
    state.encoder.filter_palette_zero = 0;
    state.encoder.predefined_filters = &predefined[0];
    CHECK_EQUALS(36, lodepng::encode(png, &sample.rgba[0], sample.w, sample.h, state));
  }
  {
    std::vector<unsigned char> predefined(sample.h, 255);
    lodepng::State state;
    std::vector<unsigned char> png;
    state.encoder.filter_strategy = LFS_PREDEFINED;
    state.encoder.filter_palette_zero = 0;
    state.encoder.predefined_filters = &predefined[0];
    CHECK_EQUALS(36, lodepng::encode(png, &sample.rgba[0], sample.w, sample.h, state));
  }

  /*a valid predefined list still works*/
  {
    std::vector<unsigned char> predefined(sample.h);
    lodepng::State state;
    std::vector<unsigned char> png;
    std::vector<unsigned char> types;
    for(y = 0; y < sample.h; ++y) predefined[y] = (unsigned char)(y % 5);
    state.encoder.filter_strategy = LFS_PREDEFINED;
    state.encoder.filter_palette_zero = 0;
    state.encoder.predefined_filters = &predefined[0];
    CHECK_EQUALS(0, lodepng::encode(png, &sample.rgba[0], sample.w, sample.h, state));
    if(lodepng::getFilterTypes(types, png) == 0) {
      CHECK_EQUALS(sample.h, types.size());
      for(y = 0; y < sample.h && y < types.size(); ++y) {
        CHECK_EQUALS(predefined[y], types[y]);
      }
    }
  }
}

/*Simulate a miswired strategy interface through the test-only fault hook in
lodepng.cpp. A missing implementation table, a missing selection function, a
selection function returning an invalid filter type, and a selection function
returning an error must all be reported, never silently replaced by a default.*/
static void testMiswiredStrategyInterface() {
  std::vector<FilterTestSample> samples = filterTestMakeSamples();
  const FilterTestSample& sample = samples[0];

  /*fault 1: no filter implementation table*/
  lodepng_test_filter_set_fault(1);
  {
    lodepng::State state;
    std::vector<unsigned char> png;
    CHECK_EQUALS(88, lodepng::encode(png, &sample.rgba[0], sample.w, sample.h, state));
  }

  /*fault 2: no selection function*/
  lodepng_test_filter_set_fault(2);
  {
    lodepng::State state;
    std::vector<unsigned char> png;
    CHECK_EQUALS(88, lodepng::encode(png, &sample.rgba[0], sample.w, sample.h, state));
  }

  /*fault 3: selection function returns a filter type that does not exist*/
  lodepng_test_filter_set_fault(3);
  {
    lodepng::State state;
    std::vector<unsigned char> png;
    CHECK_EQUALS(88, lodepng::encode(png, &sample.rgba[0], sample.w, sample.h, state));
  }

  /*fault 4: selection function itself reports an error*/
  lodepng_test_filter_set_fault(4);
  {
    lodepng::State state;
    std::vector<unsigned char> png;
    CHECK_EQUALS(36, lodepng::encode(png, &sample.rgba[0], sample.w, sample.h, state));
  }

  /*the hook must not stay stuck on a fault*/
  lodepng_test_filter_set_fault(0);
  {
    lodepng::State state;
    std::vector<unsigned char> png;
    CHECK_EQUALS(0, lodepng::encode(png, &sample.rgba[0], sample.w, sample.h, state));
    CHECK(filterTestSha256(png) == findGolden(sample.name, "auto/i0/minsum")->hash);
  }
}

int main(int argc, char** argv) {
  bool printOnly = argc > 1 && std::string(argv[1]) == "--print";
  int failed;

  if(printOnly) {
    testByteIdenticalOutput(true);
    return 0;
  }

  std::printf("filter_strategy_test: byte-identical output and roundtrip\n");
  failed = testByteIdenticalOutput(false);
  std::printf("filter_strategy_test: misconfigured strategy errors\n");
  testMisconfiguredStrategy();
  std::printf("filter_strategy_test: misconfigured predefined filters\n");
  testMisconfiguredPredefinedFilters();
#ifdef LODEPNG_TEST_FILTER_INTERFACE
  std::printf("filter_strategy_test: miswired strategy interface errors\n");
  testMiswiredStrategyInterface();
#endif

  if(g_failures || failed) {
    std::printf("\n*** FILTER STRATEGY TEST FAILED (%d failure(s)) ***\n", g_failures);
    return 1;
  }
  std::printf("filter strategy test successful\n");
  return 0;
}
