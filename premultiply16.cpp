/*
Premultiply / unpremultiply alpha for 16-bit per channel RGBA images, with a
precision controlled fixed point intermediate representation.

Copyright (c) 2005-2026 Lode Vandevenne

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software in a
    product, an acknowledgment in the product documentation would be
    appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.

    3. This notice may not be removed or altered from any source
    distribution.
*/

#include "premultiply16.h"

/*
All arithmetic below stays inside 64-bit unsigned:
  - largest premultiply product: 65535 * 65535 * 65536 < 2^48
  - largest unpremultiply product: 65535 * 65536 * 65535 < 2^48
No overflow is possible for valid 16-bit inputs.
*/

void pm16_stats_init(PM16Stats* stats) {
  if (!stats) return;
  stats->max_abs_error = 0;
  stats->error_channels = 0;
  stats->worst_color = 0;
  stats->worst_alpha = 0;
  stats->total = 0;
}

/*
Q16 premultiplied color: q = round(c * a * 2^16 / 65535).
The division is quotient plus remainder, the remainder rounds the quotient to
nearest instead of truncating.
*/
uint32_t pm16_premultiply_channel(unsigned c, unsigned a) {
  uint64_t numerator;
  uint64_t quotient;
  uint64_t remainder;

  if (a == 0) {
    /* Fully transparent: keep the color itself in Q16 so it survives the
       round trip; the premultiplied color of a transparent pixel is unbound. */
    return (uint32_t)c << PM16_FRAC_BITS;
  }

  numerator = (uint64_t)c * (uint64_t)a * (uint64_t)PM16_ONE_Q16;
  quotient = numerator / (uint64_t)PM16_MAX_VALUE;
  remainder = numerator % (uint64_t)PM16_MAX_VALUE;
  /* Remainder compensation: round to nearest, halves round up. */
  if (remainder * 2 >= (uint64_t)PM16_MAX_VALUE) quotient++;
  return (uint32_t)quotient;
}

/*
Recover the straight color: c' = round(q * 65535 / (a * 2^16)).
Again quotient plus remainder, nearest rounding. For a == 0 the Q16 value
stores the color directly, so shifting it back is exact.
*/
unsigned pm16_unpremultiply_channel(uint32_t q, unsigned a) {
  uint64_t numerator;
  uint64_t denominator;
  uint64_t quotient;
  uint64_t remainder;

  if (a == 0) {
    return (unsigned)(q >> PM16_FRAC_BITS);
  }

  numerator = (uint64_t)q * (uint64_t)PM16_MAX_VALUE;
  denominator = (uint64_t)a * (uint64_t)PM16_ONE_Q16;
  quotient = numerator / denominator;
  remainder = numerator % denominator;
  /* Remainder compensation: round to nearest, halves round up. */
  if (remainder * 2 >= denominator) quotient++;
  /* q <= 65535 * 65536 guarantees quotient <= 65535. */
  return (unsigned)quotient;
}

void pm16_premultiply_rgba16(const unsigned short* straight,
                             uint32_t* premult_q16,
                             size_t pixel_count) {
  size_t i;
  for (i = 0; i < pixel_count; i++) {
    unsigned a = (unsigned)straight[4 * i + 3];
    premult_q16[4 * i + 0] =
        pm16_premultiply_channel((unsigned)straight[4 * i + 0], a);
    premult_q16[4 * i + 1] =
        pm16_premultiply_channel((unsigned)straight[4 * i + 1], a);
    premult_q16[4 * i + 2] =
        pm16_premultiply_channel((unsigned)straight[4 * i + 2], a);
    premult_q16[4 * i + 3] = (uint32_t)a;
  }
}

void pm16_unpremultiply_rgba16(const uint32_t* premult_q16,
                               unsigned short* straight,
                               size_t pixel_count) {
  size_t i;
  for (i = 0; i < pixel_count; i++) {
    unsigned a = (unsigned)premult_q16[4 * i + 3];
    straight[4 * i + 0] =
        (unsigned short)pm16_unpremultiply_channel(premult_q16[4 * i + 0], a);
    straight[4 * i + 1] =
        (unsigned short)pm16_unpremultiply_channel(premult_q16[4 * i + 1], a);
    straight[4 * i + 2] =
        (unsigned short)pm16_unpremultiply_channel(premult_q16[4 * i + 2], a);
    straight[4 * i + 3] = (unsigned short)a;
  }
}

void pm16_roundtrip_rgba16(const unsigned short* straight,
                           uint32_t* premult_q16,
                           unsigned short* reconstructed,
                           size_t pixel_count,
                           PM16Stats* stats) {
  size_t i;
  pm16_premultiply_rgba16(straight, premult_q16, pixel_count);
  pm16_unpremultiply_rgba16(premult_q16, reconstructed, pixel_count);

  if (stats) {
    pm16_stats_init(stats);
    stats->total = (uint64_t)pixel_count * 3u;
    for (i = 0; i < pixel_count; i++) {
      size_t j;
      for (j = 0; j < 3; j++) {
        unsigned original = (unsigned)straight[4 * i + j];
        unsigned recovered = (unsigned)reconstructed[4 * i + j];
        unsigned error = (original >= recovered)
                         ? (original - recovered)
                         : (recovered - original);
        if (error) stats->error_channels++;
        if (error > stats->max_abs_error) {
          stats->max_abs_error = error;
          stats->worst_color = original;
          stats->worst_alpha = (unsigned)straight[4 * i + 3];
        }
      }
    }
  }
}
