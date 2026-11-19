# Multi-pass iterative optimal encoder for LodePNG

This work adds an iterative, feedback-driven encoder and a block-parallel
deflate path to LodePNG **without changing the PNG format or the decoder**.
Every produced file is a standard PNG whose IDAT is a standard zlib/deflate
stream readable by any conformant decoder.

## Public API (`lodepng.h`, `LodePNGCompressSettings`)

Three new fields, all defaulting to the classic behavior (zero regression):

| field | meaning | default |
|---|---|---|
| `multipass` | enable the iterative multi-pass encoder | `0` (off) |
| `multipass_passes` | max iterations (clamped to 2..8) | `4` |
| `parallel_jobs` | worker threads for block deflate; `1` = serial | `1` |

Enable with:

```cpp
lodepng::State state;
state.encoder.zlibsettings.multipass = 1;
state.encoder.zlibsettings.multipass_passes = 4;
state.encoder.zlibsettings.parallel_jobs = 8;
lodepng::encode(png, pixels, w, h, state);
```

Threading is compiled in only with `-DLODEPNG_USE_PTHREAD -pthread`. Without it
the code is pure ANSI C++ as before and `parallel_jobs` is ignored.

## How it works

1. **Five-filter row cache and cost model.** For every scanline all five PNG
   filters (None/Sub/Up/Average/Paeth) are computed once. Each filtered row is
   described by four features — absolute-difference sum, zero-byte fraction,
   byte-entropy proxy, and byte-value spread — and each filter has a linear
   cost model over those features. The cheapest filter per row is selected.
2. **Bitstream-feedback iterations.** After each pass the real deflate
   bitstream is measured (literal/match counts, average match length, zero
   fraction, produced bytes). Filters whose rows compressed poorly have their
   penalties raised and their zero reward strengthened, so the next pass
   reassigns borderline rows toward better-behaving filters. The LZ77 strategy
   (window size, `nicematch`, lazy matching) is likewise retuned per pass from
   the match statistics.
3. **Stock anchor, never a regression.** Before iterating, the exact
   single-pass stock output (MINSUM filtering + stock zlib) is generated and
   kept as the first candidate. Iterative results replace it only when strictly
   smaller, so enabling the feature can never enlarge the PNG.
4. **Block-parallel deflate with bit-exact splicing.** The filtered stream is
   split into independent deflate blocks; each block uses a fresh hash (no
   cross-block references), so blocks are pure functions of their byte range and
   can run on worker threads. Deflate blocks form one *bit* stream, so after
   compression the blocks are spliced bit-by-bit in fixed order and the whole
   stream is byte-aligned once at the end. Because the same per-block function
   and fixed ordering are used, parallel output is **byte-for-byte identical to
   serial output** (asserted in the benchmark and verified across job counts).
5. **Convergence.** Iteration stops early when a pass improves the size by less
   than 0.1%, or after `multipass_passes` runs. The smallest stream seen wins.

Unsupported configurations (Adam7 interlacing, non-dynamic btype, custom zlib)
silently fall back to the stock encoder.

## Benchmark (12 images, see `artifacts/bench/results.csv`)

Measurements on this machine, best of 3 runs (`bench/run_bench.sh 8 3`):

- **12/12 images decode exactly** with the independent Python standard-library
  `zlib` decoder plus a from-spec unfilter, and with LodePNG's own decoder.
  All reconstructed pixels match the source bit-for-bit (36 PNGs verified).
- **0 regressions**: 8 images smaller, 4 tied via the anchor, none larger.
- **4.01% smaller total** IDAT across the 12 images; individual wins up to
  ~72% smaller on smooth/gradient content.
- Parallel (8 jobs) is **3.18x faster** than the serial multi-pass on this set,
  while producing identical bytes.
- The default (feature-off) encoder output is **byte-identical** to the
  unmodified LodePNG for all 12 images.

## Reproduce

```sh
make ENABLE_PTHREAD=1 multipass_bench     # build only
bash bench/run_bench.sh 8 3               # images + benchmark + read-back
```

Validation tooling (all under `bench/`, run with `python3 -I`):

- `gen_images.py` — generates 12 diverse raw PAM/PGM images (no PNG dependency).
- `bench_multipass.cpp` — baseline vs serial vs parallel, with memcmp identity.
- `verify_pngs.py` — independent standard-decoder read-back and pixel check.
- `edge_tests.cpp` — 1x1/single-row/single-column/multi-block/Adam7/all color
  types, cross-checked against both LodePNG and system zlib.
