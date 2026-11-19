// Benchmark driver for the lodepng multi-pass iterative encoder.
//
// For every raw input image it produces:
//   - baseline PNG : stock single-pass lodepng encoder
//   - mp-serial    : multi-pass encoder, block-parallel disabled (parallel_jobs=1)
//   - mp-parallel-N: multi-pass encoder with N worker threads
// and verifies that mp-serial and mp-parallel IDAT payloads are byte-for-byte
// identical (the zlib streams, hence the spliced deflate blocks).
//
// Build: see run_bench.sh
#include "lodepng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

namespace {

double now_sec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

bool read_binary(const std::string& path, std::vector<unsigned char>& out) {
  std::ifstream f(path.c_str(), std::ios::binary);
  if(!f) return false;
  f.seekg(0, std::ios::end);
  std::streamoff n = f.tellg();
  f.seekg(0, std::ios::beg);
  if(n <= 0) return false;
  out.resize((size_t)n);
  return bool(f.read((char*)out.data(), n));
}

// Parse a PAM (RGBA), PGM (8-bit gray) or PGM16 (16-bit gray) raw file.
// Returns false on anything unexpected.
bool load_raw(const std::string& path, std::vector<unsigned char>& px,
              unsigned& w, unsigned& h, LodePNGColorType& ct, unsigned& bd) {
  std::vector<unsigned char> raw;
  if(!read_binary(path, raw)) return false;
  size_t pos = 0;
  auto token = [&](std::string& tok) -> bool {
    while(pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\n' ||
                               raw[pos] == '\t' || raw[pos] == '\r')) ++pos;
    size_t start = pos;
    while(pos < raw.size() && raw[pos] != ' ' && raw[pos] != '\n' &&
          raw[pos] != '\t' && raw[pos] != '\r') ++pos;
    if(pos == start) return false;
    tok.assign((const char*)raw.data() + start, pos - start);
    return true;
  };
  std::string magic;
  if(!token(magic)) return false;
  if(magic == "P7") {
    unsigned depth = 0, maxval = 0;
    w = h = 0;
    std::string key;
    while(token(key)) {
      if(key == "ENDHDR") break;
      std::string val;
      if(!token(val)) return false;
      if(key == "WIDTH") w = (unsigned)atoi(val.c_str());
      else if(key == "HEIGHT") h = (unsigned)atoi(val.c_str());
      else if(key == "DEPTH") depth = (unsigned)atoi(val.c_str());
      else if(key == "MAXVAL") maxval = (unsigned)atoi(val.c_str());
      (void)maxval;
    }
    if(w == 0 || h == 0 || depth != 4) return false;
    ++pos; /*single whitespace after ENDHDR*/
    if(raw.size() - pos < (size_t)w * h * 4) return false;
    px.assign(raw.begin() + pos, raw.begin() + pos + (size_t)w * h * 4);
    ct = LCT_RGBA;
    bd = 8;
    return true;
  } else if(magic == "P5") {
    std::string sw, sh, smax;
    if(!token(sw) || !token(sh) || !token(smax)) return false;
    w = (unsigned)atoi(sw.c_str());
    h = (unsigned)atoi(sh.c_str());
    unsigned maxv = (unsigned)atoi(smax.c_str());
    ++pos;
    if(maxv == 255) {
      if(raw.size() - pos < (size_t)w * h) return false;
      px.assign(raw.begin() + pos, raw.begin() + pos + (size_t)w * h);
      ct = LCT_GREY;
      bd = 8;
    } else if(maxv == 65535) {
      if(raw.size() - pos < (size_t)w * h * 2) return false;
      px.assign(raw.begin() + pos, raw.begin() + pos + (size_t)w * h * 2);
      ct = LCT_GREY;
      bd = 16;
    } else return false;
    return true;
  }
  return false;
}

struct Result {
  std::string mode;
  size_t png_size;
  double seconds;
  std::vector<unsigned char> png;
};

unsigned encode_with(const std::vector<unsigned char>& px, unsigned w, unsigned h,
                     LodePNGColorType ct, unsigned bd,
                     bool multipass, unsigned passes, unsigned jobs,
                     Result& res) {
  lodepng::State state;
  if(multipass) {
    state.encoder.zlibsettings.multipass = 1;
    state.encoder.zlibsettings.multipass_passes = passes;
    state.encoder.zlibsettings.parallel_jobs = jobs;
  }
  /*request the exact output color type so gray/16-bit inputs stay gray/16-bit*/
  state.info_raw.colortype = ct;
  state.info_raw.bitdepth = bd;
  state.info_png.color.colortype = ct;
  state.info_png.color.bitdepth = bd;
  state.encoder.auto_convert = 0;

  double t0 = now_sec();
  unsigned err = lodepng::encode(res.png, px, w, h, state);
  double t1 = now_sec();
  res.seconds = t1 - t0;
  res.png_size = res.png.size();
  return err;
}

// Extract the concatenated IDAT payload (the zlib stream) from a PNG.
bool idat_payload(const std::vector<unsigned char>& png, std::vector<unsigned char>& out) {
  out.clear();
  size_t p = 8; /*skip signature*/
  bool found = false;
  while(p + 8 <= png.size()) {
    unsigned len = (png[p] << 24) | (png[p + 1] << 16) | (png[p + 2] << 8) | png[p + 3];
    if(p + 12 + len > png.size()) return false;
    std::string type((const char*)png.data() + p + 4, 4);
    if(type == "IDAT") {
      out.insert(out.end(), png.begin() + p + 8, png.begin() + p + 8 + len);
      found = true;
    }
    p += 12 + len;
    if(type == "IEND") break;
  }
  return found;
}

} // namespace

int main(int argc, char** argv) {
  if(argc < 4) {
    fprintf(stderr, "usage: %s manifest.txt outdir jobs [repeats]\n", argv[0]);
    return 2;
  }
  const char* manifest = argv[1];
  std::string outdir = argv[2];
  unsigned jobs = (unsigned)atoi(argv[3]);
  unsigned repeats = argc > 4 ? (unsigned)atoi(argv[4]) : 3;
  if(repeats < 1) repeats = 1;

  std::ifstream mf(manifest);
  if(!mf) {
    fprintf(stderr, "cannot open %s\n", manifest);
    return 2;
  }
  // machine-readable summary plus a human table
  std::ofstream csv(outdir + "/results.csv");
  csv << "image,mode,png_bytes,seconds\n";

  std::string line;
  int all_ok = 1;
  while(std::getline(mf, line)) {
    if(line.empty()) continue;
    std::istringstream ls(line);
    std::string name, kind, fname;
    unsigned w, h;
    ls >> name >> kind >> w >> h >> fname;
    std::string inpath;
    /*manifest paths are relative to the image directory (dirname of manifest)*/
    std::string mdir = manifest;
    size_t slash = mdir.find_last_of('/');
    mdir = slash == std::string::npos ? "." : mdir.substr(0, slash);
    inpath = mdir + "/" + fname;

    std::vector<unsigned char> px;
    LodePNGColorType ct;
    unsigned bd;
    if(!load_raw(inpath, px, w, h, ct, bd)) {
      fprintf(stderr, "FAIL load %s\n", inpath.c_str());
      all_ok = 0;
      continue;
    }

    Result base, serial, parallel;
    unsigned e0 = 0, e1 = 0, e2 = 0;
    double base_t = 1e9, ser_t = 1e9, par_t = 1e9;
    size_t base_sz = 0, ser_sz = 0, par_sz = 0;
    for(unsigned r = 0; r < repeats; ++r) {
      Result b, s, p;
      e0 = encode_with(px, w, h, ct, bd, false, 0, 1, b);
      e1 = encode_with(px, w, h, ct, bd, true, 4, 1, s);
      e2 = encode_with(px, w, h, ct, bd, true, 4, jobs, p);
      if(e0 || e1 || e2) break;
      if(b.seconds < base_t) { base_t = b.seconds; base_sz = b.png_size; base.png = b.png; }
      if(s.seconds < ser_t) { ser_t = s.seconds; ser_sz = s.png_size; serial.png = s.png; }
      if(p.seconds < par_t) { par_t = p.seconds; par_sz = p.png_size; parallel.png = p.png; }
    }
    if(e0 || e1 || e2) {
      fprintf(stderr, "FAIL encode %s errors %u %u %u\n", name.c_str(), e0, e1, e2);
      all_ok = 0;
      continue;
    }

    // byte-identity of serial vs parallel IDAT zlib payload
    std::vector<unsigned char> z_ser, z_par;
    bool ok_id = idat_payload(serial.png, z_ser) && idat_payload(parallel.png, z_par)
                 && z_ser == z_par;
    if(!ok_id) all_ok = 0;

    // full PNG files must also be identical between serial and parallel
    bool ok_png = serial.png == parallel.png;
    if(!ok_png) all_ok = 0;

    // save all three PNGs for independent decoding checks
    auto save = [&](const std::string& suffix, const std::vector<unsigned char>& buf) {
      std::ofstream f(outdir + "/" + name + "." + suffix + ".png", std::ios::binary);
      f.write((const char*)buf.data(), buf.size());
    };
    save("base", base.png);
    save("mpserial", serial.png);
    save("mppar", parallel.png);

    double ratio = (double)ser_sz / (double)base_sz;
    printf("%-18s base=%8zu (%.4fs)  mp=%8zu (%.4fs ser / %.4fs par x%u)  ratio=%.4f  identity=%s/%s\n",
           name.c_str(), base_sz, base_t, ser_sz, ser_t, par_t, jobs, ratio,
           ok_id ? "IDAT-OK" : "IDAT-DIFF", ok_png ? "PNG-OK" : "PNG-DIFF");
    csv << name << ",baseline," << base_sz << "," << base_t << "\n";
    csv << name << ",mp_serial," << ser_sz << "," << ser_t << "\n";
    csv << name << ",mp_parallel_" << jobs << "," << par_sz << "," << par_t << "\n";
  }
  csv.close();
  printf("\nOVERALL IDENTITY CHECK: %s\n", all_ok ? "PASS" : "FAIL");
  return all_ok ? 0 : 1;
}
