// Shared MBRAWPRE binary wire format parser, bounds validator, and premultiplied RGBA8888 downscaler.
// Pure C99/C++ compatible, zero platform dependencies.
//
// Wire format (version 1, little-endian, 16 + w * h * 4 bytes):
//   [0..8)   ASCII magic "MBRAWPRE"
//   [8..12)  uint32_t w (1..=16384)
//   [12..16) uint32_t h (1..=16384)
//   [16..)   tight row-major top-down premultiplied sRGB RGBA8888 (stride = w * 4)
#ifndef MB_RAW_PREMUL_H
#define MB_RAW_PREMUL_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MB_RAW_PREMUL_HEADER_BYTES 16u
#define MB_RAW_PREMUL_MAX_DIM 16384u

static inline int mb_raw_premul_is(const uint8_t *d, size_t n) {
  return d != NULL && n >= 8u && memcmp(d, "MBRAWPRE", 8u) == 0;
}

static inline int mb_raw_premul_parse(const uint8_t *d, size_t n, int *w, int *h) {
  if (!mb_raw_premul_is(d, n) || n < MB_RAW_PREMUL_HEADER_BYTES) return 0;
  uint32_t rw = (uint32_t)d[8] | ((uint32_t)d[9] << 8) | ((uint32_t)d[10] << 16) | ((uint32_t)d[11] << 24);
  uint32_t rh = (uint32_t)d[12] | ((uint32_t)d[13] << 8) | ((uint32_t)d[14] << 16) | ((uint32_t)d[15] << 24);
  if (rw == 0u || rh == 0u || rw > MB_RAW_PREMUL_MAX_DIM || rh > MB_RAW_PREMUL_MAX_DIM) return 0;
  uint64_t expected = (uint64_t)rw * (uint64_t)rh * 4ULL;
  if (expected != (uint64_t)(n - MB_RAW_PREMUL_HEADER_BYTES)) return 0;
  if (w) *w = (int)rw;
  if (h) *h = (int)rh;
  return 1;
}

static inline void mb_raw_premul_target_size(int w, int h, int max_px, int device_max, int *tw, int *th) {
  int dmax = device_max > 0 ? device_max : (int)MB_RAW_PREMUL_MAX_DIM;
  int lim = (max_px > 0 && max_px < dmax) ? max_px : dmax;
  if (w <= lim && h <= lim) {
    *tw = w;
    *th = h;
    return;
  }
  double k = (double)lim / (double)(w > h ? w : h);
  int rw = (int)lround((double)w * k);
  int rh = (int)lround((double)h * k);
  if (rw < 1) rw = 1;
  if (rw > lim) rw = lim;
  if (rh < 1) rh = 1;
  if (rh > lim) rh = lim;
  *tw = rw;
  *th = rh;
}

static inline void mb_raw_premul_downscale(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
  double sx_scale = (double)sw / (double)dw;
  double sy_scale = (double)sh / (double)dh;
  if (dw <= sw && dh <= sh) {
    // Exact integer-weight box / area averaging (Fant filter): prevents aliasing, vanishing 1px lines, and animated
    // flicker at >2x downscale ratios while keeping bit-exact half-up rounding across all hosts with zero allocation.
    uint64_t sw64 = (uint64_t)sw;
    uint64_t sh64 = (uint64_t)sh;
    uint64_t dw64 = (uint64_t)dw;
    uint64_t total_w = sw64 * sh64;
    uint64_t den = total_w * 2ULL;
    for (int y = 0; y < dh; y++) {
      uint64_t sy0 = (uint64_t)y * sh64;
      uint64_t sy1 = (uint64_t)(y + 1) * sh64;
      int iy0 = (int)(sy0 / (uint64_t)dh);
      int iy1 = (int)((sy1 - 1ULL) / (uint64_t)dh);
      uint8_t *out = dst + (size_t)y * (size_t)dw * 4u;
      for (int x = 0; x < dw; x++) {
        uint64_t sx0 = (uint64_t)x * sw64;
        uint64_t sx1 = (uint64_t)(x + 1) * sw64;
        int ix0 = (int)(sx0 / dw64);
        int ix1 = (int)((sx1 - 1ULL) / dw64);
        uint64_t wx0 = (ix0 == ix1) ? sw64 : ((uint64_t)(ix0 + 1) * dw64 - sx0);
        uint64_t wx1 = (ix0 == ix1) ? 0ULL : (sx1 - (uint64_t)ix1 * dw64);
        uint64_t sr = 0ULL, sg = 0ULL, sb = 0ULL, sa = 0ULL;
        for (int iy = iy0; iy <= iy1; iy++) {
          uint64_t y_lo = (uint64_t)iy * (uint64_t)dh;
          if (sy0 > y_lo) y_lo = sy0;
          uint64_t y_hi = (uint64_t)(iy + 1) * (uint64_t)dh;
          if (sy1 < y_hi) y_hi = sy1;
          uint64_t wy = y_hi - y_lo;
          const uint8_t *row = src + (size_t)iy * (size_t)sw * 4u;
          const uint8_t *p0 = row + (size_t)ix0 * 4u;
          uint64_t rr = (uint64_t)p0[0] * wx0;
          uint64_t rg = (uint64_t)p0[1] * wx0;
          uint64_t rb = (uint64_t)p0[2] * wx0;
          uint64_t ra = (uint64_t)p0[3] * wx0;
          if (ix0 + 1 < ix1) {
            uint32_t ir = 0u, ig = 0u, ib = 0u, ia = 0u;
            for (int ix = ix0 + 1; ix < ix1; ix++) {
              const uint8_t *p = row + (size_t)ix * 4u;
              ir += p[0];
              ig += p[1];
              ib += p[2];
              ia += p[3];
            }
            rr += (uint64_t)ir * dw64;
            rg += (uint64_t)ig * dw64;
            rb += (uint64_t)ib * dw64;
            ra += (uint64_t)ia * dw64;
          }
          if (ix1 > ix0) {
            const uint8_t *p1 = row + (size_t)ix1 * 4u;
            rr += (uint64_t)p1[0] * wx1;
            rg += (uint64_t)p1[1] * wx1;
            rb += (uint64_t)p1[2] * wx1;
            ra += (uint64_t)p1[3] * wx1;
          }
          sr += rr * wy;
          sg += rg * wy;
          sb += rb * wy;
          sa += ra * wy;
        }
        int a = (int)((sa * 2ULL + total_w) / den);
        if (a > 255) a = 255;
        int r = (int)((sr * 2ULL + total_w) / den);
        int g = (int)((sg * 2ULL + total_w) / den);
        int b = (int)((sb * 2ULL + total_w) / den);
        if (r > a) r = a;
        if (g > a) g = a;
        if (b > a) b = a;
        out[(size_t)x * 4u + 0u] = (uint8_t)r;
        out[(size_t)x * 4u + 1u] = (uint8_t)g;
        out[(size_t)x * 4u + 2u] = (uint8_t)b;
        out[(size_t)x * 4u + 3u] = (uint8_t)a;
      }
    }
    return;
  }
  for (int y = 0; y < dh; y++) {
    double fy = ((double)y + 0.5) * sy_scale - 0.5;
    int y0 = fy < 0.0 ? 0 : (int)fy;
    if (y0 >= sh) y0 = sh - 1;
    int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
    double wy = (fy < 0.0 || fy >= (double)(sh - 1)) ? 0.0 : (fy - (double)y0);
    double wy0 = 1.0 - wy;
    const uint8_t *row0 = src + (size_t)y0 * (size_t)sw * 4u;
    const uint8_t *row1 = src + (size_t)y1 * (size_t)sw * 4u;
    uint8_t *out = dst + (size_t)y * (size_t)dw * 4u;
    for (int x = 0; x < dw; x++) {
      double fx = ((double)x + 0.5) * sx_scale - 0.5;
      int x0 = fx < 0.0 ? 0 : (int)fx;
      if (x0 >= sw) x0 = sw - 1;
      int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
      double wx = (fx < 0.0 || fx >= (double)(sw - 1)) ? 0.0 : (fx - (double)x0);
      double wx0 = 1.0 - wx;
      const uint8_t *p00 = row0 + (size_t)x0 * 4u;
      const uint8_t *p10 = row0 + (size_t)x1 * 4u;
      const uint8_t *p01 = row1 + (size_t)x0 * 4u;
      const uint8_t *p11 = row1 + (size_t)x1 * 4u;
      double w00 = wx0 * wy0;
      double w10 = wx * wy0;
      double w01 = wx0 * wy;
      double w11 = wx * wy;
      int a = (int)lround(p00[3] * w00 + p10[3] * w10 + p01[3] * w01 + p11[3] * w11);
      if (a < 0) a = 0;
      if (a > 255) a = 255;
      for (int c = 0; c < 3; c++) {
        int v = (int)lround(p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11);
        if (v < 0) v = 0;
        if (v > a) v = a;
        out[(size_t)x * 4u + (size_t)c] = (uint8_t)v;
      }
      out[(size_t)x * 4u + 3u] = (uint8_t)a;
    }
  }
}

#endif
