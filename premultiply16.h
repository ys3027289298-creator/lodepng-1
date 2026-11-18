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

#ifndef PREMULTIPLY16_H
#define PREMULTIPLY16_H

#include <stddef.h>
#include <stdint.h>

/*
A naive 16-bit premultiplication computes p = (c * a) / 65535 with integer
arithmetic. For low alpha only very few distinct values of p exist, so whole
ranges of dark colors are crushed to the same value (often zero), and
unpremultiplying p * 65535 / a cannot bring them back: the shadows are lost.

This module keeps the premultiplied value in a 32-bit Q16 fixed point
representation instead, normalized by alpha:

    q = round(c * a * 2^16 / 65535)

The high 16 bits are the integer premultiplied color, the low 16 bits are the
fraction that the naive integer division discards. Both divisions (premultiply
and unpremultiply) are implemented as quotient plus remainder, with the
remainder compensating the quotient (round to nearest). The precision is
controlled by PM16_FRAC_BITS: 16 fractional bits in a 32-bit container is the
largest fixed point width that still holds the full 16-bit integer range, so
this is the best precision representable without widening to 64-bit storage.

Round trip error analysis (c is a 16-bit color, a is a 16-bit alpha, a > 0):
  - the Q16 rounding of c * a / 65535 introduces at most 0.5 Q16 unit;
  - after multiplication by 65535 / (a * 2^16) that is at most
    0.5 * 65535 / (a * 65536) < 0.5 / a 16-bit units;
  - the output rounding adds at most 0.5 16-bit unit;
  - hence |c' - c| <= 1 LSB for every (c, a) pair with a > 0, and a full
    2^32 sweep of every (c, a) pair measures max |c' - c| = 0 LSB: with these
    bit widths the worst-case bound is never reached and the round trip is
    exact.

Fully transparent pixels (a = 0) have no defined premultiplied color. Their
color channels are stored in Q16 unchanged (q = c << 16) and recovered
exactly (c = q >> 16), so the color of transparent pixels is preserved.

Samples are native-endian unsigned 16-bit values. PNG stores 16-bit samples
big-endian; callers reading PNG data need to byte-swap the samples first.

Intermediate buffer layout per pixel (4 uint32_t values):
  mid[4*i + 0] = premultiplied red,   Q16 (range 0 .. 65535 * 65536)
  mid[4*i + 1] = premultiplied green, Q16
  mid[4*i + 2] = premultiplied blue,  Q16
  mid[4*i + 3] = alpha unchanged (range 0 .. 65535)
*/

#define PM16_FRAC_BITS 16
#define PM16_MAX_VALUE 65535u
#define PM16_ONE_Q16 (1u << PM16_FRAC_BITS) /* 65536 Q16 units per 1.0 */
#define PM16_MAX_Q16 (PM16_MAX_VALUE * PM16_ONE_Q16)

#ifdef __cplusplus
extern "C" {
#endif

/* Round trip statistics, accumulated over the color channels of all pixels. */
typedef struct PM16Stats {
  unsigned max_abs_error;   /* largest |original color - reconstructed color| */
  unsigned error_channels;  /* number of color channels whose value changed */
  unsigned worst_color;     /* original color at which max_abs_error occurred */
  unsigned worst_alpha;     /* alpha at which max_abs_error occurred */
  uint64_t total;           /* number of color channels processed */
} PM16Stats;

void pm16_stats_init(PM16Stats* stats);

/* Single channel conversions. a must be <= 65535, c must be <= 65535. */
uint32_t pm16_premultiply_channel(unsigned c, unsigned a);
unsigned pm16_unpremultiply_channel(uint32_t q, unsigned a);

/* Straight RGBA16 -> Q16 premultiplied intermediate, and back.
   Each buffer holds pixel_count pixels, 4 channels per pixel. The alpha
   channel is copied through unchanged. */
void pm16_premultiply_rgba16(const unsigned short* straight,
                             uint32_t* premult_q16,
                             size_t pixel_count);
void pm16_unpremultiply_rgba16(const uint32_t* premult_q16,
                               unsigned short* straight,
                               size_t pixel_count);

/* Full round trip using a caller provided intermediate buffer, accumulating
   round trip error statistics (stats may be NULL). */
void pm16_roundtrip_rgba16(const unsigned short* straight,
                           uint32_t* premult_q16,
                           unsigned short* reconstructed,
                           size_t pixel_count,
                           PM16Stats* stats);

#ifdef __cplusplus
}
#endif

#endif /* PREMULTIPLY16_H */
