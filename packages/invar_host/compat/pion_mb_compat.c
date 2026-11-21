// MoonBit C runtime shim + zero-copy Pion FFI bridges for mbui Apple host.
#include "moonbit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Upstream host declarations (linked from unmodified upstream/apple_*.m)
extern void mb_render_init_offscreen(double w, double h, double scale);
extern void mb_render_set_size(double w, double h, double scale);
extern uint8_t *mb_render_read(const double *inst, int32_t count, double r, double g, double b, int *pw, int *ph);
extern int mb_render_snapshot(const char *path);
extern void mb_text_set_scale(double scale);
extern void mb_host_font_metrics(double size, int32_t weight, int32_t style, double *out);
extern int32_t mb_host_shape(moonbit_string_t text, double size, int32_t weight, int32_t style, double *out);
extern void mb_host_glyph(int32_t key, double *out);
extern void mb_host_glyph_put(int32_t key, int32_t x, int32_t y);
extern int32_t mb_host_atlas_size(int32_t colour);

// ---- 8-byte header allocator matching Moonbit_array_length / moonbit_decref ----

static void *mb_alloc(int32_t len, size_t elem_size) {
  if (len < 0) len = 0;
  size_t total = 8 + ((size_t)len + 1) * elem_size;
  uint32_t *hdr = (uint32_t *)calloc(1, total);
  if (!hdr) return NULL;
  hdr[0] = (uint32_t)len;
  hdr[1] = 1;
  return (void *)(hdr + 2);
}

moonbit_string_t moonbit_make_string_raw(int32_t len) {
  return (moonbit_string_t)mb_alloc(len, sizeof(uint16_t));
}

moonbit_string_t moonbit_make_string(int32_t len, uint16_t fill) {
  moonbit_string_t s = moonbit_make_string_raw(len);
  if (s && fill != 0) {
    for (int32_t i = 0; i < len; i++) s[i] = fill;
  }
  return s;
}

moonbit_bytes_t moonbit_make_bytes(int32_t len, int fill) {
  moonbit_bytes_t b = (moonbit_bytes_t)mb_alloc(len, sizeof(uint8_t));
  if (b && fill != 0 && len > 0) {
    memset(b, fill, (size_t)len);
  }
  return b;
}

double *pion_mb_make_double_array(int32_t len) {
  return (double *)mb_alloc(len, sizeof(double));
}

void moonbit_incref(void *p) {
  if (!p) return;
  uint32_t *hdr = ((uint32_t *)p) - 2;
  hdr[1] += 1;
}

void moonbit_decref(void *p) {
  if (!p) return;
  uint32_t *hdr = ((uint32_t *)p) - 2;
  if (hdr[1] <= 1) {
    free(hdr);
  } else {
    hdr[1] -= 1;
  }
}

// ---- UTF-8 -> UTF-16 (moonbit_string_t) decoder ----

moonbit_string_t pion_mb_str_from_utf8(const char *utf8, int64_t byte_len) {
  if (!utf8 || byte_len <= 0) return moonbit_make_string_raw(0);
  const uint8_t *src = (const uint8_t *)utf8;
  size_t n = (size_t)byte_len;
  // Pass 1: count UTF-16 code units
  int32_t u16_len = 0;
  size_t i = 0;
  while (i < n) {
    uint8_t c = src[i];
    if (c < 0x80) {
      u16_len += 1;
      i += 1;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
      u16_len += 1;
      i += 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
      u16_len += 1;
      i += 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
      u16_len += 2;
      i += 4;
    } else {
      u16_len += 1;
      i += 1;
    }
  }
  moonbit_string_t out = moonbit_make_string_raw(u16_len);
  if (!out) return NULL;
  int32_t k = 0;
  i = 0;
  while (i < n && k < u16_len) {
    uint8_t c = src[i];
    if (c < 0x80) {
      out[k++] = (uint16_t)c;
      i += 1;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
      uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(src[i + 1] & 0x3F);
      out[k++] = (uint16_t)cp;
      i += 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
      uint32_t cp = ((uint32_t)(c & 0x0F) << 12) |
                    ((uint32_t)(src[i + 1] & 0x3F) << 6) |
                    (uint32_t)(src[i + 2] & 0x3F);
      out[k++] = (uint16_t)cp;
      i += 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
      uint32_t cp = ((uint32_t)(c & 0x07) << 18) |
                    ((uint32_t)(src[i + 1] & 0x3F) << 12) |
                    ((uint32_t)(src[i + 2] & 0x3F) << 6) |
                    (uint32_t)(src[i + 3] & 0x3F);
      cp -= 0x10000;
      out[k++] = (uint16_t)(0xD800 + (cp >> 10));
      if (k < u16_len) out[k++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
      i += 4;
    } else {
      out[k++] = 0xFFFD;
      i += 1;
    }
  }
  return out;
}

// ---- Pion FFI bridge functions ----

void pion_mb_init_offscreen(double w, double h, double scale) {
  mb_render_init_offscreen(w, h, scale);
}

int32_t pion_mb_render_frame(const double *inst, int32_t count, double r, double g, double b) {
  int pw = 0, ph = 0;
  uint8_t *px = mb_render_read(inst, count, r, g, b, &pw, &ph);
  if (!px) return 0;
  free(px);
  return (pw > 0 && ph > 0) ? 1 : 0;
}

int32_t pion_mb_snapshot_utf8(const char *path_utf8, int64_t path_len) {
  if (!path_utf8 || path_len <= 0) return 0;
  char *cpath = (char *)malloc((size_t)path_len + 1);
  if (!cpath) return 0;
  memcpy(cpath, path_utf8, (size_t)path_len);
  cpath[path_len] = '\0';
  int ok = mb_render_snapshot(cpath);
  free(cpath);
  return ok ? 1 : 0;
}

int32_t pion_mb_shape_utf8(const char *utf8, int64_t utf8_len, double size, int32_t weight, int32_t style,
                           double *out, int32_t out_doubles) {
  if (!out || out_doubles < 4) return 0;
  moonbit_string_t s = pion_mb_str_from_utf8(utf8, utf8_len);
  if (!s) return 0;
  double *mb_out = pion_mb_make_double_array(out_doubles);
  if (!mb_out) {
    moonbit_decref(s);
    return 0;
  }
  int32_t k = mb_host_shape(s, size, weight, style, mb_out);
  if (k > 0) {
    int32_t copy_n = k * 4;
    if (copy_n > out_doubles) copy_n = out_doubles;
    memcpy(out, mb_out, (size_t)copy_n * sizeof(double));
  }
  moonbit_decref(mb_out);
  moonbit_decref(s);
  return k;
}

void pion_mb_font_metrics(double size, int32_t weight, int32_t style, double *out) {
  mb_host_font_metrics(size, weight, style, out);
}

void pion_mb_glyph(int32_t key, double *out) {
  mb_host_glyph(key, out);
}

void pion_mb_glyph_put(int32_t key, int32_t x, int32_t y) {
  mb_host_glyph_put(key, x, y);
}

int32_t pion_mb_atlas_size(int32_t colour) {
  return mb_host_atlas_size(colour);
}

// Exercises P26-B export extern "C" fn + c_fn_ptr callback bridge
int32_t pion_mb_invoke_frame_cb(int32_t (*cb)(double, double, double), double w, double h, double scale) {
  if (!cb) return 0;
  return cb(w, h, scale);
}
