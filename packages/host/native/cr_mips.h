// Separable 4-tap Catmull-Rom ([-1, 9, 9, -1] / 16) mip chain for 8-bit 4-channel images.
// Channel-order agnostic on channels 0..2 (RGBA or BGRA); channel 3 is alpha (premultiplied clamped
// c <= a when opaque == 0, forced to 255 when opaque != 0).
#pragma once
#include <stdint.h>
#include <string.h>

static inline uint8_t mb_cr_clamp_u8(int v) {
  return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static inline int mb_cr_mip_count(int w, int h) {
  int mips = 1;
  unsigned int m = (unsigned int)(w > h ? w : h);
  while (m > 1u && mips < 16) {
    m >>= 1;
    mips++;
  }
  return mips;
}

static inline void mb_cr_mip_step(const uint8_t *prev, int mw, int mh, uint8_t *dst, uint8_t *tmp, int opaque) {
  int nw = mw > 1 ? (mw >> 1) : 1;
  int nh = mh > 1 ? (mh >> 1) : 1;
  for (int y = 0; y < mh; y++) {
    const uint8_t *r = prev + (size_t)y * (size_t)mw * 4u;
    uint8_t *out = tmp + (size_t)y * (size_t)nw * 4u;
    if (mw == 1) {
      memcpy(out, r, 4);
      if (opaque) out[3] = 255;
    } else {
      for (int x = 0; x < nw; x++) {
        int x0 = x * 2;
        int xm1 = x0 > 0 ? x0 - 1 : 0;
        int xp1 = x0 + 1 < mw ? x0 + 1 : mw - 1;
        int xp2 = x0 + 2 < mw ? x0 + 2 : mw - 1;
        const uint8_t *a = r + (size_t)xm1 * 4u, *b = r + (size_t)x0 * 4u;
        const uint8_t *c = r + (size_t)xp1 * 4u, *d = r + (size_t)xp2 * 4u;
        uint8_t al = opaque ? 255 : mb_cr_clamp_u8((-(int)a[3] + 9 * (int)b[3] + 9 * (int)c[3] - (int)d[3] + 8) >> 4);
        uint8_t c0 = mb_cr_clamp_u8((-(int)a[0] + 9 * (int)b[0] + 9 * (int)c[0] - (int)d[0] + 8) >> 4);
        uint8_t c1 = mb_cr_clamp_u8((-(int)a[1] + 9 * (int)b[1] + 9 * (int)c[1] - (int)d[1] + 8) >> 4);
        uint8_t c2 = mb_cr_clamp_u8((-(int)a[2] + 9 * (int)b[2] + 9 * (int)c[2] - (int)d[2] + 8) >> 4);
        if (!opaque) {
          if (c0 > al) c0 = al;
          if (c1 > al) c1 = al;
          if (c2 > al) c2 = al;
        }
        out[(size_t)x * 4u + 0] = c0;
        out[(size_t)x * 4u + 1] = c1;
        out[(size_t)x * 4u + 2] = c2;
        out[(size_t)x * 4u + 3] = al;
      }
    }
  }
  for (int y = 0; y < nh; y++) {
    uint8_t *out = dst + (size_t)y * (size_t)nw * 4u;
    if (mh == 1) {
      memcpy(out, tmp, (size_t)nw * 4u);
    } else {
      int y0 = y * 2;
      int ym1 = y0 > 0 ? y0 - 1 : 0;
      int yp1 = y0 + 1 < mh ? y0 + 1 : mh - 1;
      int yp2 = y0 + 2 < mh ? y0 + 2 : mh - 1;
      const uint8_t *ra = tmp + (size_t)ym1 * (size_t)nw * 4u;
      const uint8_t *rb = tmp + (size_t)y0 * (size_t)nw * 4u;
      const uint8_t *rc = tmp + (size_t)yp1 * (size_t)nw * 4u;
      const uint8_t *rd = tmp + (size_t)yp2 * (size_t)nw * 4u;
      for (int x = 0; x < nw; x++) {
        const uint8_t *a = ra + (size_t)x * 4u, *b = rb + (size_t)x * 4u;
        const uint8_t *c = rc + (size_t)x * 4u, *d = rd + (size_t)x * 4u;
        uint8_t al = opaque ? 255 : mb_cr_clamp_u8((-(int)a[3] + 9 * (int)b[3] + 9 * (int)c[3] - (int)d[3] + 8) >> 4);
        uint8_t c0 = mb_cr_clamp_u8((-(int)a[0] + 9 * (int)b[0] + 9 * (int)c[0] - (int)d[0] + 8) >> 4);
        uint8_t c1 = mb_cr_clamp_u8((-(int)a[1] + 9 * (int)b[1] + 9 * (int)c[1] - (int)d[1] + 8) >> 4);
        uint8_t c2 = mb_cr_clamp_u8((-(int)a[2] + 9 * (int)b[2] + 9 * (int)c[2] - (int)d[2] + 8) >> 4);
        if (!opaque) {
          if (c0 > al) c0 = al;
          if (c1 > al) c1 = al;
          if (c2 > al) c2 = al;
        }
        out[(size_t)x * 4u + 0] = c0;
        out[(size_t)x * 4u + 1] = c1;
        out[(size_t)x * 4u + 2] = c2;
        out[(size_t)x * 4u + 3] = al;
      }
    }
  }
}
