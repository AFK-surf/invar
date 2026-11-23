// MoonBit C runtime shim + zero-copy Pion FFI bridges for mbui native multi-platform hosts
// (macOS, iOS, Linux, Windows). Lets unmodified upstream libs/mbui/src/host/{apple_*.m,mac_app.m,ios_app.m,linux_*.c,
// win_*.{c,cpp},*.h} link directly into Pion with zero MoonBit runtime dependency.
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "moonbit.h"
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include <time.h>
extern void mb_images_upload(void);
#elif defined(_WIN32)
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// ---- UTF-8 <-> UTF-16 (moonbit_string_t) & raw bytes <-> moonbit_bytes_t ----

moonbit_string_t pion_mb_str_from_utf8(const char *utf8, int64_t byte_len) {
  if (!utf8 || byte_len <= 0) return moonbit_make_string_raw(0);
  const uint8_t *src = (const uint8_t *)utf8;
  size_t n = (size_t)byte_len;
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

static char *mb_str_to_utf8_buf(moonbit_string_t s, int64_t *out_len) {
  if (!s) {
    char *empty = (char *)malloc(1);
    if (empty) empty[0] = '\0';
    *out_len = 0;
    return empty;
  }
  int32_t u16_len = Moonbit_array_length(s);
  size_t max_bytes = (size_t)u16_len * 3 + 1;
  char *buf = (char *)malloc(max_bytes);
  if (!buf) {
    *out_len = 0;
    return NULL;
  }
  size_t k = 0;
  for (int32_t i = 0; i < u16_len; i++) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < u16_len) {
      uint32_t lo = s[i + 1];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
        i += 1;
      }
    }
    if (cp < 0x80) {
      buf[k++] = (char)cp;
    } else if (cp < 0x800) {
      buf[k++] = (char)(0xC0 | (cp >> 6));
      buf[k++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      buf[k++] = (char)(0xE0 | (cp >> 12));
      buf[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[k++] = (char)(0x80 | (cp & 0x3F));
    } else {
      buf[k++] = (char)(0xF0 | (cp >> 18));
      buf[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      buf[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[k++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  buf[k] = '\0';
  *out_len = (int64_t)k;
  return buf;
}

static void *mb_str_to_pion(moonbit_string_t s) {
  if (!s) return NULL;
  int64_t utf8_len = 0;
  char *buf = mb_str_to_utf8_buf(s, &utf8_len);
  moonbit_decref(s);
  return (void *)buf;
}

moonbit_bytes_t pion_mb_bytes_from_raw(const uint8_t *data, int64_t len) {
  if (!data || len <= 0) return moonbit_make_bytes(0, 0);
  moonbit_bytes_t b = moonbit_make_bytes((int32_t)len, 0);
  if (b) memcpy(b, data, (size_t)len);
  return b;
}

static void *mb_bytes_to_pion(moonbit_bytes_t b) {
  return (void *)b;
}

int32_t pion_mb_bytes_len(void *p) {
  if (!p) return 0;
  return Moonbit_array_length(p);
}

// ---- Upstream native declarations (unmodified native/*.m) ----

typedef void (*mb_frame_fn)(double, double, double, double);
typedef void (*mb_pointer_fn)(int32_t, double, double, int32_t);
typedef void (*mb_wheel_fn)(double, double, double, double, int32_t);
typedef void (*mb_key_fn)(int32_t, int32_t);
typedef void (*mb_text_fn)(int32_t, moonbit_string_t, int32_t, int32_t);

extern int mb_render_init_offscreen(double w, double h, double scale);
extern void mb_render_present(const double *inst, int32_t count, double r, double g, double b);
extern int mb_render_snapshot(const char *path);
extern int32_t mb_render_drawn_pixels(int32_t x, int32_t y, int32_t w, int32_t h, moonbit_bytes_t out);
extern int32_t mb_render_pixels_of(const double *inst, int32_t count, double r, double g, double b,
                                   double scale, int32_t w, int32_t h, moonbit_bytes_t out);

extern void mb_host_run(moonbit_string_t title, mb_frame_fn frame, mb_pointer_fn pointer,
                        mb_wheel_fn wheel, mb_key_fn key, mb_text_fn text);
extern void mb_host_present(double *buf, int32_t count, double r, double g, double b);
extern void mb_host_wake_now(void);

void *mb_host_wake_now_ptr(void) {
  return (void *)(uintptr_t)mb_host_wake_now;
}

extern int32_t mb_host_shape(moonbit_string_t text, double size, int32_t weight, int32_t style, double *out);
extern int32_t mb_host_text_key(moonbit_string_t text, double size, int32_t weight, int32_t style);
extern int32_t mb_host_glyph_picture(int32_t key, moonbit_bytes_t out);
extern int32_t mb_host_atlas_put(int32_t x, int32_t y, int32_t w, int32_t h, moonbit_bytes_t px);
extern void mb_host_glyphs_ahead(int32_t *keys, int32_t n);
extern int32_t mb_host_face_add(int32_t face, moonbit_string_t path, int32_t weight);
extern int32_t mb_host_face_add_bytes(int32_t face, moonbit_bytes_t data, int32_t weight);
extern int32_t mb_host_face_add_family(int32_t face, moonbit_string_t family);
extern moonbit_string_t mb_host_local_font_families(void);

extern void mb_host_text_menu_own(moonbit_string_t words);
extern void mb_host_set_ime(int32_t active, double x, double y, double w, double h,
                            moonbit_string_t text, int32_t a, int32_t b);
extern void mb_host_clipboard_set(moonbit_string_t s);
extern void mb_host_clipboard_image(moonbit_bytes_t data);
extern void mb_host_clipboard_secret(moonbit_string_t s, double clear_ms);
extern void mb_host_log(moonbit_string_t s);
extern void mb_host_frame_awaiting(int32_t awaiting, moonbit_string_t what);
extern void mb_host_perf_line(moonbit_string_t line);
extern moonbit_string_t mb_host_perf_where(void);
extern int32_t mb_host_reveal_file(moonbit_string_t path, int32_t go);
extern moonbit_bytes_t mb_host_perf_read(void);
extern moonbit_string_t mb_host_env(moonbit_string_t name);
extern int32_t mb_host_drive(int32_t kind, moonbit_string_t text);
extern void mb_host_a11y_tree(moonbit_string_t s);
extern int32_t mb_host_share(moonbit_string_t text, moonbit_string_t name, moonbit_string_t mime, moonbit_bytes_t data);
extern moonbit_string_t mb_host_shared_in(void);
extern moonbit_bytes_t mb_host_shared_in_file(int32_t i);
extern double mb_host_word_at(moonbit_string_t text, int32_t off);
extern int32_t mb_host_open_url(moonbit_string_t url);
extern int32_t mb_host_open_tab(moonbit_string_t url);
extern void mb_host_hold_words(moonbit_string_t channel, moonbit_string_t title, moonbit_string_t body);
extern void mb_host_update_fetch(moonbit_string_t url, moonbit_string_t sha256, double size,
                                 moonbit_string_t name, int32_t now, moonbit_string_t delta);
extern void mb_host_update_apply(moonbit_string_t said);
extern moonbit_string_t mb_host_update_state(void);
extern void mb_host_install_package(moonbit_string_t name, moonbit_bytes_t data);
extern int32_t mb_host_window_open(moonbit_string_t what);
extern int32_t mb_host_window_front(moonbit_string_t what);
extern int32_t mb_host_windows(int32_t op, int32_t wid, moonbit_string_t what);
extern void mb_host_menu_setup(moonbit_string_t json);
extern moonbit_string_t mb_host_app_said(moonbit_string_t name);
extern void mb_host_app_tell(moonbit_string_t name, moonbit_string_t arg);
extern void mb_host_set_site_name(moonbit_string_t name);

extern int32_t mb_host_image_load(moonbit_string_t src, int32_t max_px);
extern int32_t mb_host_image_load_bytes(moonbit_bytes_t data, int32_t max_px);
extern int32_t mb_host_image_pixels(int32_t id, moonbit_bytes_t out);

extern int32_t mb_host_browser_login_start(moonbit_string_t path, moonbit_string_t done);
extern int32_t mb_host_browser_login_open(int32_t id, moonbit_string_t url);
extern moonbit_string_t mb_host_browser_login_back(int32_t id);

extern int32_t mb_host_passwords_run(moonbit_string_t args);
extern moonbit_string_t mb_host_passwords_out(int32_t id);

extern int32_t mb_host_autofill_ask(moonbit_string_t host);
extern moonbit_string_t mb_host_autofill_take(void);

extern int32_t mb_host_passkeys_get(moonbit_string_t ask);
extern moonbit_string_t mb_host_passkeys_out(int32_t id);

extern moonbit_string_t mb_host_pick_name(int32_t id, int32_t i);
extern moonbit_bytes_t mb_host_pick_bytes(int32_t id, int32_t i);
extern moonbit_string_t mb_host_pick_info(int32_t id, int32_t i);
extern int32_t mb_host_pick_photo(moonbit_string_t src);

extern int32_t mb_host_keep_begin(moonbit_string_t name, moonbit_string_t mime, int32_t way);
extern int32_t mb_host_keep_append(int32_t id, moonbit_bytes_t data);
extern moonbit_string_t mb_host_keep_where(int32_t id);

extern moonbit_string_t mb_host_gallery_read_rows(int32_t id);

extern int32_t mb_host_charset_known(moonbit_string_t charset);
extern moonbit_string_t mb_host_charset_decode(moonbit_bytes_t data, moonbit_string_t charset);

extern int32_t mb_host_marks_start(moonbit_bytes_t image, moonbit_string_t marks);
extern moonbit_bytes_t mb_host_marks_png(int32_t id);

extern void mb_host_notice_show(moonbit_string_t channel, moonbit_string_t tag, moonbit_string_t title,
                                moonbit_string_t body, moonbit_string_t opens);
extern void mb_host_notice_cancel(moonbit_string_t tag);
extern moonbit_string_t mb_host_notice_text(int32_t what);

extern int32_t mb_host_nav_bar(moonbit_string_t spec);

extern int32_t mb_host_view_open(int32_t kind, moonbit_string_t arg);
extern void mb_host_view_read_answer(int32_t id, int32_t req, moonbit_bytes_t data, double total, int32_t more);
extern moonbit_string_t mb_host_view_site_next(int32_t id);
extern moonbit_bytes_t mb_host_view_site_body(int32_t id, int32_t req);
extern void mb_host_view_site_head(int32_t id, int32_t req, int32_t status, moonbit_string_t headers);
extern void mb_host_view_site_chunk(int32_t id, int32_t req, moonbit_bytes_t data);
extern void mb_host_view_site_end(int32_t id, int32_t req, moonbit_string_t error);
extern moonbit_string_t mb_host_view_site_state(int32_t id);
extern void mb_host_view_site_load(int32_t id, moonbit_string_t path);
extern void mb_host_view_tell(int32_t id, int32_t what, moonbit_string_t text);
extern int32_t mb_host_pdf_open(moonbit_bytes_t data);
extern moonbit_string_t mb_host_view_text(int32_t id, int32_t what);
extern moonbit_bytes_t mb_host_view_still(int32_t id);
extern int32_t mb_host_save_file(moonbit_string_t name, moonbit_bytes_t data);

extern moonbit_string_t mb_host_post_next(void);
extern moonbit_string_t mb_host_post_json(void);
extern moonbit_bytes_t mb_host_post_bytes(void);

extern int32_t mb_host_hotkey_set(moonbit_string_t id, moonbit_string_t accel);
extern void mb_host_hotkey_clear(moonbit_string_t id);

// device_native
extern moonbit_string_t mb_host_clip_text(int32_t id);
extern moonbit_bytes_t mb_host_clip_png(int32_t id);
extern moonbit_string_t mb_host_clip_file_name(int32_t id, int32_t i);
extern moonbit_bytes_t mb_host_clip_file(int32_t id, int32_t i);
extern void mb_host_clip_set(moonbit_string_t text, moonbit_bytes_t png, moonbit_string_t label);

// stream_native
extern void mb_host_stream_h264(int32_t id, moonbit_bytes_t avcc, int32_t w, int32_t h);
extern void mb_host_stream_frame(int32_t id, moonbit_bytes_t data, int32_t key, double ts);
extern void mb_host_stream_jpeg(int32_t id, moonbit_bytes_t data, double ts);
extern moonbit_string_t mb_host_stream_next(int32_t id);
extern void mb_host_stream_codec(int32_t id, moonbit_string_t cmd, moonbit_bytes_t data);
extern void mb_host_stream_hevc(int32_t id, moonbit_bytes_t hvcc, int32_t w, int32_t h);
extern void mb_host_stream_av1(int32_t id, moonbit_bytes_t av1c, int32_t w, int32_t h);
extern void mb_host_stream_audio(int32_t id, moonbit_bytes_t data);
extern void mb_host_stream_pointer(int32_t id, moonbit_bytes_t png, double w, double h, double hx, double hy);
extern int32_t mb_host_stream_frame_shown(int32_t id, moonbit_bytes_t data, int32_t key, double ts, int32_t show);

// textref_native
extern void mb_host_ref_text(moonbit_string_t text, double size, int32_t weight, int32_t style,
                             double x, double base, int32_t color, double *out);
extern int32_t mb_host_ref_end(moonbit_string_t name);
extern int32_t mb_host_ref_frame(moonbit_string_t name);

// terminal_native
extern void mb_host_term_write(int32_t id, moonbit_bytes_t data);
extern moonbit_bytes_t mb_host_term_frame(int32_t id, int32_t flags);
extern moonbit_string_t mb_host_term_text(int32_t id, int32_t sel);
extern moonbit_bytes_t mb_host_term_mouse(int32_t id, int32_t action, int32_t button, int32_t mods,
                                          int32_t x, int32_t y, int32_t w, int32_t h, int32_t cw, int32_t ch);

// native_native
extern int32_t mb_host_native_call(moonbit_string_t name, moonbit_string_t params);
extern moonbit_string_t mb_host_native_result(int32_t id);

// ---- Offscreen & event loop helpers ----

static int pion_offscreen_active = 0;

int32_t pion_host_init_offscreen(double w, double h, double scale) {
  int ok = mb_render_init_offscreen(w, h, scale);
  pion_offscreen_active = ok ? 1 : 0;
  return ok ? 1 : 0;
}

void pion_host_present(const double *buf, int32_t count, double r, double g, double b) {
  if (pion_offscreen_active) {
    mb_render_present(buf, count, r, g, b);
  } else {
    mb_host_present((double *)buf, count, r, g, b);
  }
}

int32_t pion_host_snapshot(const char *path, int64_t path_len) {
  if (!path || path_len <= 0) return 0;
  char *cpath = (char *)malloc((size_t)path_len + 1);
  if (!cpath) return 0;
  memcpy(cpath, path, (size_t)path_len);
  cpath[path_len] = '\0';
  int ok = mb_render_snapshot(cpath);
  free(cpath);
  return ok ? 1 : 0;
}

void pion_host_pump_main_loop(double ms) {
#ifdef __APPLE__
  double sec = ms > 0.0 ? ms / 1000.0 : 0.001;
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, sec, true);
#elif defined(__linux__)
  if (ms > 0.0) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1000000.0);
    if (ts.tv_nsec < 0) ts.tv_nsec = 0;
    nanosleep(&ts, NULL);
  }
  if (pion_offscreen_active) {
    mb_images_upload();
  }
#elif defined(_WIN32)
  // Deliver what is waiting on this thread's queue (decodes and other posted callbacks), then wait up to `ms` for more.
  DWORD until = GetTickCount() + (DWORD)(ms > 0.0 ? ms : 1.0);
  for (;;) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    DWORD now = GetTickCount();
    if ((int32_t)(until - now) <= 0) break;
    MsgWaitForMultipleObjects(0, NULL, FALSE, until - now, QS_ALLINPUT);
  }
#else
  (void)ms;
#endif
}

// ---- Run callback bridge ----

typedef void (*pion_text_cb_fn)(int32_t, const char *, int32_t, int32_t);
static pion_text_cb_fn pion_text_cb = NULL;

static void pion_text_trampoline(int32_t kind, moonbit_string_t s, int32_t a, int32_t b) {
  if (!pion_text_cb) {
    if (s) moonbit_decref(s);
    return;
  }
  int64_t utf8_len = 0;
  char *buf = mb_str_to_utf8_buf(s, &utf8_len);
  if (s) moonbit_decref(s);
  pion_text_cb(kind, buf ? buf : "", a, b);
  free(buf);
}

void pion_host_run(const char *title, int64_t title_len, mb_frame_fn frame, mb_pointer_fn pointer,
                   mb_wheel_fn wheel, mb_key_fn key, pion_text_cb_fn text_cb) {
  pion_offscreen_active = 0;
  pion_text_cb = text_cb;
  moonbit_string_t mt = pion_mb_str_from_utf8(title, title_len);
  mb_host_run(mt, frame, pointer, wheel, key, pion_text_trampoline);
  moonbit_decref(mt);
}

// ---- Text & Glyph Atlas bridges ----

int32_t pion_host_shape(const char *utf8, int64_t utf8_len, double size, int32_t weight, int32_t style,
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

int32_t pion_host_text_key(const char *utf8, int64_t utf8_len, double size, int32_t weight, int32_t style) {
  moonbit_string_t s = pion_mb_str_from_utf8(utf8, utf8_len);
  int32_t r = mb_host_text_key(s, size, weight, style);
  moonbit_decref(s);
  return r;
}

// `Array[Byte]` is a packed `uint8_t` buffer (element stride 1), not 8-byte slots.
int32_t pion_host_glyph_picture(int32_t key, uint8_t *out, int32_t out_len) {
  if (!out || out_len <= 0) return 0;
  moonbit_bytes_t mb_out = moonbit_make_bytes(out_len, 0);
  if (!mb_out) return 0;
  int32_t ok = mb_host_glyph_picture(key, mb_out);
  if (ok == 1) memcpy(out, mb_out, (size_t)out_len);
  moonbit_decref(mb_out);
  return ok;
}

int32_t pion_host_atlas_put(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *px, int32_t px_len) {
  if (!px || px_len <= 0) return 0;
  moonbit_bytes_t mb_px = moonbit_make_bytes(px_len, 0);
  if (!mb_px) return 0;
  memcpy(mb_px, px, (size_t)px_len);
  int32_t ok = mb_host_atlas_put(x, y, w, h, mb_px);
  moonbit_decref(mb_px);
  return ok;
}

void pion_host_glyphs_ahead(const int64_t *keys_slots, int32_t n) {
  if (!keys_slots || n <= 0) return;
  int32_t *keys = (int32_t *)malloc((size_t)n * sizeof(int32_t));
  if (!keys) return;
  for (int32_t i = 0; i < n; i++) keys[i] = (int32_t)keys_slots[i];
  mb_host_glyphs_ahead(keys, n);
  free(keys);
}

int32_t pion_host_face_add(int32_t face, const char *path, int64_t path_len, int32_t weight) {
  moonbit_string_t mp = pion_mb_str_from_utf8(path, path_len);
  int32_t r = mb_host_face_add(face, mp, weight);
  moonbit_decref(mp);
  return r;
}

int32_t pion_host_face_add_bytes(int32_t face, const uint8_t *data, int64_t len, int32_t weight) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  int32_t r = mb_host_face_add_bytes(face, mb, weight);
  moonbit_decref(mb);
  return r;
}

int32_t pion_host_face_add_family(int32_t face, const char *family, int64_t family_len) {
  moonbit_string_t mf = pion_mb_str_from_utf8(family, family_len);
  int32_t r = mb_host_face_add_family(face, mf);
  moonbit_decref(mf);
  return r;
}

void *pion_host_local_font_families(void) {
  return mb_str_to_pion(mb_host_local_font_families());
}

// ---- IME, Clipboard, Log, Perf, Env, Drive, A11y, Share, Update, Window, App ----

void pion_host_text_menu_own(const char *words, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(words, len);
  mb_host_text_menu_own(ms);
  moonbit_decref(ms);
}

void pion_host_set_ime(int32_t active, double x, double y, double w, double h,
                       const char *text, int64_t text_len, int32_t a, int32_t b) {
  moonbit_string_t ms = pion_mb_str_from_utf8(text, text_len);
  mb_host_set_ime(active, x, y, w, h, ms, a, b);
  moonbit_decref(ms);
}

void pion_host_clipboard_set(const char *s, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(s, len);
  mb_host_clipboard_set(ms);
  moonbit_decref(ms);
}

void pion_host_clipboard_image(const uint8_t *data, int64_t len) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  mb_host_clipboard_image(mb);
  moonbit_decref(mb);
}

void pion_host_clipboard_secret(const char *s, int64_t len, double clear_ms) {
  moonbit_string_t ms = pion_mb_str_from_utf8(s, len);
  mb_host_clipboard_secret(ms, clear_ms);
  moonbit_decref(ms);
}

void pion_host_log(const char *s, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(s, len);
  mb_host_log(ms);
  moonbit_decref(ms);
}

void pion_host_frame_awaiting(int32_t awaiting, const char *what, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(what, len);
  mb_host_frame_awaiting(awaiting, ms);
  moonbit_decref(ms);
}

void pion_host_perf_line(const char *line, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(line, len);
  mb_host_perf_line(ms);
  moonbit_decref(ms);
}

void *pion_host_perf_where(void) {
  return mb_str_to_pion(mb_host_perf_where());
}

int32_t pion_host_reveal_file(const char *path, int64_t len, int32_t go) {
  moonbit_string_t ms = pion_mb_str_from_utf8(path, len);
  int32_t r = mb_host_reveal_file(ms, go);
  moonbit_decref(ms);
  return r;
}

void *pion_host_perf_read(void) {
  return mb_bytes_to_pion(mb_host_perf_read());
}

void *pion_host_env(const char *name, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(name, len);
  moonbit_string_t r = mb_host_env(ms);
  moonbit_decref(ms);
  return mb_str_to_pion(r);
}

int32_t pion_host_drive(int32_t kind, const char *text, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(text, len);
  int32_t r = mb_host_drive(kind, ms);
  moonbit_decref(ms);
  return r;
}

void pion_host_a11y_tree(const char *s, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(s, len);
  mb_host_a11y_tree(ms);
  moonbit_decref(ms);
}

int32_t pion_host_share(const char *text, int64_t text_len, const char *name, int64_t name_len,
                        const char *mime, int64_t mime_len, const uint8_t *data, int64_t data_len) {
  moonbit_string_t mt = pion_mb_str_from_utf8(text, text_len);
  moonbit_string_t mn = pion_mb_str_from_utf8(name, name_len);
  moonbit_string_t mm = pion_mb_str_from_utf8(mime, mime_len);
  moonbit_bytes_t md = pion_mb_bytes_from_raw(data, data_len);
  int32_t r = mb_host_share(mt, mn, mm, md);
  moonbit_decref(mt);
  moonbit_decref(mn);
  moonbit_decref(mm);
  moonbit_decref(md);
  return r;
}

void *pion_host_shared_in(void) {
  return mb_str_to_pion(mb_host_shared_in());
}

void *pion_host_shared_in_file(int32_t i) {
  return mb_bytes_to_pion(mb_host_shared_in_file(i));
}

double pion_host_word_at(const char *text, int64_t len, int32_t off) {
  moonbit_string_t ms = pion_mb_str_from_utf8(text, len);
  double r = mb_host_word_at(ms, off);
  moonbit_decref(ms);
  return r;
}

int32_t pion_host_open_url(const char *url, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(url, len);
  int32_t r = mb_host_open_url(ms);
  moonbit_decref(ms);
  return r;
}

int32_t pion_host_open_tab(const char *url, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(url, len);
  int32_t r = mb_host_open_tab(ms);
  moonbit_decref(ms);
  return r;
}

void pion_host_hold_words(const char *ch, int64_t ch_len, const char *ti, int64_t ti_len,
                          const char *bo, int64_t bo_len) {
  moonbit_string_t mc = pion_mb_str_from_utf8(ch, ch_len);
  moonbit_string_t mt = pion_mb_str_from_utf8(ti, ti_len);
  moonbit_string_t mb = pion_mb_str_from_utf8(bo, bo_len);
  mb_host_hold_words(mc, mt, mb);
  moonbit_decref(mc);
  moonbit_decref(mt);
  moonbit_decref(mb);
}

void pion_host_update_fetch(const char *url, int64_t url_len, const char *sha, int64_t sha_len,
                            double size, const char *name, int64_t name_len, int32_t now_flag,
                            const char *delta, int64_t delta_len) {
  moonbit_string_t mu = pion_mb_str_from_utf8(url, url_len);
  moonbit_string_t ms = pion_mb_str_from_utf8(sha, sha_len);
  moonbit_string_t mn = pion_mb_str_from_utf8(name, name_len);
  moonbit_string_t md = pion_mb_str_from_utf8(delta, delta_len);
  mb_host_update_fetch(mu, ms, size, mn, now_flag, md);
  moonbit_decref(mu);
  moonbit_decref(ms);
  moonbit_decref(mn);
  moonbit_decref(md);
}

void pion_host_update_apply(const char *said, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(said, len);
  mb_host_update_apply(ms);
  moonbit_decref(ms);
}

void *pion_host_update_state(void) {
  return mb_str_to_pion(mb_host_update_state());
}

void pion_host_install_package(const char *name, int64_t name_len, const uint8_t *data, int64_t data_len) {
  moonbit_string_t mn = pion_mb_str_from_utf8(name, name_len);
  moonbit_bytes_t md = pion_mb_bytes_from_raw(data, data_len);
  mb_host_install_package(mn, md);
  moonbit_decref(mn);
  moonbit_decref(md);
}

int32_t pion_host_window_open(const char *what, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(what, len);
  int32_t r = mb_host_window_open(ms);
  moonbit_decref(ms);
  return r;
}

int32_t pion_host_window_front(const char *what, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(what, len);
  int32_t r = mb_host_window_front(ms);
  moonbit_decref(ms);
  return r;
}

int32_t pion_host_windows(int32_t op, int32_t wid, const char *what, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(what, len);
  int32_t r = mb_host_windows(op, wid, ms);
  moonbit_decref(ms);
  return r;
}

void pion_host_menu_setup(const char *json, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(json, len);
  mb_host_menu_setup(ms);
  moonbit_decref(ms);
}

void *pion_host_app_said(const char *name, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(name, len);
  moonbit_string_t r = mb_host_app_said(ms);
  moonbit_decref(ms);
  return mb_str_to_pion(r);
}

void pion_host_app_tell(const char *name, int64_t name_len, const char *arg, int64_t arg_len) {
  moonbit_string_t mn = pion_mb_str_from_utf8(name, name_len);
  moonbit_string_t ma = pion_mb_str_from_utf8(arg, arg_len);
  mb_host_app_tell(mn, ma);
  moonbit_decref(mn);
  moonbit_decref(ma);
}

void pion_host_set_site_name(const char *name, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(name, len);
  mb_host_set_site_name(ms);
  moonbit_decref(ms);
}

// ---- Images & Drawn Pixels ----

int32_t pion_host_image_load(const char *src, int64_t len, int32_t max_px) {
  moonbit_string_t ms = pion_mb_str_from_utf8(src, len);
  int32_t r = mb_host_image_load(ms, max_px);
  moonbit_decref(ms);
  return r;
}

int32_t pion_host_image_load_bytes(const uint8_t *data, int64_t len, int32_t max_px) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  int32_t r = mb_host_image_load_bytes(mb, max_px);
  moonbit_decref(mb);
  return r;
}

int32_t pion_host_image_pixels(int32_t id, uint8_t *out, int32_t out_len) {
  if (!out || out_len <= 0) return 0;
  moonbit_bytes_t mb_out = moonbit_make_bytes(out_len, 0);
  if (!mb_out) return 0;
  int32_t ok = mb_host_image_pixels(id, mb_out);
  if (ok == 1) memcpy(out, mb_out, (size_t)out_len);
  moonbit_decref(mb_out);
  return ok;
}

int32_t pion_host_drawn_pixels(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t *out, int32_t out_len) {
  if (!out || out_len <= 0 || w <= 0 || h <= 0) return 0;
  int64_t need = (int64_t)w * (int64_t)h * 4;
  if ((int64_t)out_len < need) return 0;
  moonbit_bytes_t mb_out = moonbit_make_bytes((int32_t)need, 0);
  if (!mb_out) return 0;
  int32_t ok = mb_render_drawn_pixels(x, y, w, h, mb_out);
  if (ok == 1) memcpy(out, mb_out, (size_t)need);
  moonbit_decref(mb_out);
  return ok;
}

int32_t pion_host_drawn_pixels_of(const double *inst, int32_t count, double r, double g, double b,
                                  double scale, int32_t w, int32_t h, uint8_t *out, int32_t out_len) {
  if (!out || out_len <= 0 || w <= 0 || h <= 0) return 0;
  int64_t need = (int64_t)w * (int64_t)h * 4;
  if ((int64_t)out_len < need) return 0;
  moonbit_bytes_t mb_out = moonbit_make_bytes((int32_t)need, 0);
  if (!mb_out) return 0;
  int32_t ok = mb_render_pixels_of(inst, count, r, g, b, scale, w, h, mb_out);
  if (ok == 1) memcpy(out, mb_out, (size_t)need);
  moonbit_decref(mb_out);
  return ok;
}

// ---- Browser login, Passwords, Autofill, Passkeys, Pick, Keep, Gallery, Charset, Marks, Notices, Nav ----

int32_t pion_host_bl_start(const char *path, int64_t path_len, const char *done, int64_t done_len) {
  moonbit_string_t mp = pion_mb_str_from_utf8(path, path_len);
  moonbit_string_t md = pion_mb_str_from_utf8(done, done_len);
  int32_t r = mb_host_browser_login_start(mp, md);
  moonbit_decref(mp);
  moonbit_decref(md);
  return r;
}

int32_t pion_host_bl_open(int32_t id, const char *url, int64_t len) {
  moonbit_string_t mu = pion_mb_str_from_utf8(url, len);
  int32_t r = mb_host_browser_login_open(id, mu);
  moonbit_decref(mu);
  return r;
}

void *pion_host_bl_back(int32_t id) {
  return mb_str_to_pion(mb_host_browser_login_back(id));
}

int32_t pion_host_pw_run(const char *args, int64_t len) {
  moonbit_string_t ma = pion_mb_str_from_utf8(args, len);
  int32_t r = mb_host_passwords_run(ma);
  moonbit_decref(ma);
  return r;
}

void *pion_host_pw_out(int32_t id) {
  return mb_str_to_pion(mb_host_passwords_out(id));
}

int32_t pion_host_af_ask(const char *host, int64_t len) {
  moonbit_string_t mh = pion_mb_str_from_utf8(host, len);
  int32_t r = mb_host_autofill_ask(mh);
  moonbit_decref(mh);
  return r;
}

void *pion_host_af_take(void) {
  return mb_str_to_pion(mb_host_autofill_take());
}

int32_t pion_host_wa_get(const char *ask, int64_t len) {
  moonbit_string_t ma = pion_mb_str_from_utf8(ask, len);
  int32_t r = mb_host_passkeys_get(ma);
  moonbit_decref(ma);
  return r;
}

void *pion_host_wa_out(int32_t id) {
  return mb_str_to_pion(mb_host_passkeys_out(id));
}

void *pion_host_pk_name(int32_t id, int32_t i) {
  return mb_str_to_pion(mb_host_pick_name(id, i));
}

void *pion_host_pk_bytes(int32_t id, int32_t i) {
  return mb_bytes_to_pion(mb_host_pick_bytes(id, i));
}

void *pion_host_pk_info(int32_t id, int32_t i) {
  return mb_str_to_pion(mb_host_pick_info(id, i));
}

int32_t pion_host_pk_photo(const char *src, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(src, len);
  int32_t r = mb_host_pick_photo(ms);
  moonbit_decref(ms);
  return r;
}

int32_t pion_host_kp_begin(const char *name, int64_t name_len, const char *mime, int64_t mime_len, int32_t way) {
  moonbit_string_t mn = pion_mb_str_from_utf8(name, name_len);
  moonbit_string_t mm = pion_mb_str_from_utf8(mime, mime_len);
  int32_t r = mb_host_keep_begin(mn, mm, way);
  moonbit_decref(mn);
  moonbit_decref(mm);
  return r;
}

int32_t pion_host_kp_append(int32_t id, const uint8_t *data, int64_t len) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  int32_t r = mb_host_keep_append(id, mb);
  moonbit_decref(mb);
  return r;
}

void *pion_host_kp_where(int32_t id) {
  return mb_str_to_pion(mb_host_keep_where(id));
}

void *pion_host_ga_read_rows(int32_t id) {
  return mb_str_to_pion(mb_host_gallery_read_rows(id));
}

int32_t pion_host_cs_known(const char *cs, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(cs, len);
  int32_t r = mb_host_charset_known(ms);
  moonbit_decref(ms);
  return r;
}

void *pion_host_cs_decode(const uint8_t *data, int64_t data_len, const char *cs, int64_t cs_len) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, data_len);
  moonbit_string_t ms = pion_mb_str_from_utf8(cs, cs_len);
  moonbit_string_t r = mb_host_charset_decode(mb, ms);
  moonbit_decref(mb);
  moonbit_decref(ms);
  return mb_str_to_pion(r);
}

int32_t pion_host_mk_start(const uint8_t *image, int64_t image_len, const char *marks, int64_t marks_len) {
  moonbit_bytes_t mi = pion_mb_bytes_from_raw(image, image_len);
  moonbit_string_t mm = pion_mb_str_from_utf8(marks, marks_len);
  int32_t r = mb_host_marks_start(mi, mm);
  moonbit_decref(mi);
  moonbit_decref(mm);
  return r;
}

void *pion_host_mk_png(int32_t id) {
  return mb_bytes_to_pion(mb_host_marks_png(id));
}

void pion_host_nt_show(const char *ch, int64_t ch_len, const char *tag, int64_t tag_len,
                       const char *ti, int64_t ti_len, const char *bo, int64_t bo_len,
                       const char *op, int64_t op_len) {
  moonbit_string_t mc = pion_mb_str_from_utf8(ch, ch_len);
  moonbit_string_t mt = pion_mb_str_from_utf8(tag, tag_len);
  moonbit_string_t mti = pion_mb_str_from_utf8(ti, ti_len);
  moonbit_string_t mb = pion_mb_str_from_utf8(bo, bo_len);
  moonbit_string_t mo = pion_mb_str_from_utf8(op, op_len);
  mb_host_notice_show(mc, mt, mti, mb, mo);
  moonbit_decref(mc);
  moonbit_decref(mt);
  moonbit_decref(mti);
  moonbit_decref(mb);
  moonbit_decref(mo);
}

void pion_host_nt_cancel(const char *tag, int64_t len) {
  moonbit_string_t mt = pion_mb_str_from_utf8(tag, len);
  mb_host_notice_cancel(mt);
  moonbit_decref(mt);
}

void *pion_host_nt_text(int32_t what) {
  return mb_str_to_pion(mb_host_notice_text(what));
}

int32_t pion_host_nv_bar(const char *spec, int64_t len) {
  moonbit_string_t ms = pion_mb_str_from_utf8(spec, len);
  int32_t r = mb_host_nav_bar(ms);
  moonbit_decref(ms);
  return r;
}

// ---- Native views & PDFs & Save file & Post & Hotkeys ----

int32_t pion_host_view_open(int32_t kind, const char *arg, int64_t len) {
  moonbit_string_t ma = pion_mb_str_from_utf8(arg, len);
  int32_t r = mb_host_view_open(kind, ma);
  moonbit_decref(ma);
  return r;
}

void pion_host_view_read_answer(int32_t id, int32_t req, const uint8_t *data, int64_t len, double total, int32_t more) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  mb_host_view_read_answer(id, req, mb, total, more);
  moonbit_decref(mb);
}

void *pion_host_view_site_next(int32_t id) {
  return mb_str_to_pion(mb_host_view_site_next(id));
}

void *pion_host_view_site_body(int32_t id, int32_t req) {
  return mb_bytes_to_pion(mb_host_view_site_body(id, req));
}

void pion_host_view_site_head(int32_t id, int32_t req, int32_t status, const char *headers, int64_t len) {
  moonbit_string_t mh = pion_mb_str_from_utf8(headers, len);
  mb_host_view_site_head(id, req, status, mh);
  moonbit_decref(mh);
}

void pion_host_view_site_chunk(int32_t id, int32_t req, const uint8_t *data, int64_t len) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  mb_host_view_site_chunk(id, req, mb);
  moonbit_decref(mb);
}

void pion_host_view_site_end(int32_t id, int32_t req, const char *err, int64_t len) {
  moonbit_string_t me = pion_mb_str_from_utf8(err, len);
  mb_host_view_site_end(id, req, me);
  moonbit_decref(me);
}

void *pion_host_view_site_state(int32_t id) {
  return mb_str_to_pion(mb_host_view_site_state(id));
}

void pion_host_view_site_load(int32_t id, const char *path, int64_t len) {
  moonbit_string_t mp = pion_mb_str_from_utf8(path, len);
  mb_host_view_site_load(id, mp);
  moonbit_decref(mp);
}

void pion_host_view_tell(int32_t id, int32_t what, const char *text, int64_t len) {
  moonbit_string_t mt = pion_mb_str_from_utf8(text, len);
  mb_host_view_tell(id, what, mt);
  moonbit_decref(mt);
}

int32_t pion_host_pdf_open(const uint8_t *data, int64_t len) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  int32_t r = mb_host_pdf_open(mb);
  moonbit_decref(mb);
  return r;
}

void *pion_host_view_text(int32_t id, int32_t what) {
  return mb_str_to_pion(mb_host_view_text(id, what));
}

void *pion_host_view_still(int32_t id) {
  return mb_bytes_to_pion(mb_host_view_still(id));
}

int32_t pion_host_save_file(const char *name, int64_t name_len, const uint8_t *data, int64_t data_len) {
  moonbit_string_t mn = pion_mb_str_from_utf8(name, name_len);
  moonbit_bytes_t md = pion_mb_bytes_from_raw(data, data_len);
  int32_t r = mb_host_save_file(mn, md);
  moonbit_decref(mn);
  moonbit_decref(md);
  return r;
}

void *pion_host_post_next(void) {
  return mb_str_to_pion(mb_host_post_next());
}

void *pion_host_post_json(void) {
  return mb_str_to_pion(mb_host_post_json());
}

void *pion_host_post_bytes(void) {
  return mb_bytes_to_pion(mb_host_post_bytes());
}

int32_t pion_host_hotkey_set(const char *id, int64_t id_len, const char *accel, int64_t accel_len) {
  moonbit_string_t mi = pion_mb_str_from_utf8(id, id_len);
  moonbit_string_t ma = pion_mb_str_from_utf8(accel, accel_len);
  int32_t r = mb_host_hotkey_set(mi, ma);
  moonbit_decref(mi);
  moonbit_decref(ma);
  return r;
}

void pion_host_hotkey_clear(const char *id, int64_t id_len) {
  moonbit_string_t mi = pion_mb_str_from_utf8(id, id_len);
  mb_host_hotkey_clear(mi);
  moonbit_decref(mi);
}

// ---- Device / Clipboard bridges ----

void *pion_host_clip_text(int32_t id) {
  return mb_str_to_pion(mb_host_clip_text(id));
}

void *pion_host_clip_png(int32_t id) {
  return mb_bytes_to_pion(mb_host_clip_png(id));
}

void *pion_host_clip_file_name(int32_t id, int32_t i) {
  return mb_str_to_pion(mb_host_clip_file_name(id, i));
}

void *pion_host_clip_file(int32_t id, int32_t i) {
  return mb_bytes_to_pion(mb_host_clip_file(id, i));
}

void pion_host_clip_set(const char *text, int64_t text_len, const uint8_t *png, int64_t png_len,
                        const char *label, int64_t label_len) {
  moonbit_string_t mt = pion_mb_str_from_utf8(text, text_len);
  moonbit_bytes_t mp = pion_mb_bytes_from_raw(png, png_len);
  moonbit_string_t ml = pion_mb_str_from_utf8(label, label_len);
  mb_host_clip_set(mt, mp, ml);
  moonbit_decref(mt);
  moonbit_decref(mp);
  moonbit_decref(ml);
}

// ---- Stream bridges ----

void pion_host_stream_h264(int32_t id, const uint8_t *avcc, int64_t len, int32_t w, int32_t h) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(avcc, len);
  mb_host_stream_h264(id, mb, w, h);
  moonbit_decref(mb);
}

void pion_host_stream_frame(int32_t id, const uint8_t *data, int64_t len, int32_t key, double ts) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  mb_host_stream_frame(id, mb, key, ts);
  moonbit_decref(mb);
}

void pion_host_stream_jpeg(int32_t id, const uint8_t *data, int64_t len, double ts) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  mb_host_stream_jpeg(id, mb, ts);
  moonbit_decref(mb);
}

void *pion_host_stream_next(int32_t id) {
  return mb_str_to_pion(mb_host_stream_next(id));
}

void pion_host_stream_codec(int32_t id, const char *cmd, int64_t cmd_len, const uint8_t *data, int64_t data_len) {
  moonbit_string_t mc = pion_mb_str_from_utf8(cmd, cmd_len);
  moonbit_bytes_t md = pion_mb_bytes_from_raw(data, data_len);
  mb_host_stream_codec(id, mc, md);
  moonbit_decref(mc);
  moonbit_decref(md);
}

void pion_host_stream_hevc(int32_t id, const uint8_t *hvcc, int64_t len, int32_t w, int32_t h) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(hvcc, len);
  mb_host_stream_hevc(id, mb, w, h);
  moonbit_decref(mb);
}

void pion_host_stream_av1(int32_t id, const uint8_t *av1c, int64_t len, int32_t w, int32_t h) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(av1c, len);
  mb_host_stream_av1(id, mb, w, h);
  moonbit_decref(mb);
}

void pion_host_stream_audio(int32_t id, const uint8_t *data, int64_t len) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  mb_host_stream_audio(id, mb);
  moonbit_decref(mb);
}

void pion_host_stream_pointer(int32_t id, const uint8_t *png, int64_t len, double w, double h, double hx, double hy) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(png, len);
  mb_host_stream_pointer(id, mb, w, h, hx, hy);
  moonbit_decref(mb);
}

int32_t pion_host_stream_frame_shown(int32_t id, const uint8_t *data, int64_t len, int32_t key, double ts, int32_t show) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  int32_t r = mb_host_stream_frame_shown(id, mb, key, ts, show);
  moonbit_decref(mb);
  return r;
}

// ---- TextRef bridges ----

void pion_host_ref_text(const char *text, int64_t len, double size, int32_t weight, int32_t style,
                        double x, double base, int32_t color, double *out) {
  moonbit_string_t mt = pion_mb_str_from_utf8(text, len);
  mb_host_ref_text(mt, size, weight, style, x, base, color, out);
  moonbit_decref(mt);
}

int32_t pion_host_ref_end(const char *name, int64_t len) {
  moonbit_string_t mn = pion_mb_str_from_utf8(name, len);
  int32_t r = mb_host_ref_end(mn);
  moonbit_decref(mn);
  return r;
}

int32_t pion_host_ref_frame(const char *name, int64_t len) {
  moonbit_string_t mn = pion_mb_str_from_utf8(name, len);
  int32_t r = mb_host_ref_frame(mn);
  moonbit_decref(mn);
  return r;
}

// ---- Terminal bridges ----

void pion_host_term_write(int32_t id, const uint8_t *data, int64_t len) {
  moonbit_bytes_t mb = pion_mb_bytes_from_raw(data, len);
  mb_host_term_write(id, mb);
  moonbit_decref(mb);
}

void *pion_host_term_frame(int32_t id, int32_t flags) {
  return mb_bytes_to_pion(mb_host_term_frame(id, flags));
}

void *pion_host_term_text(int32_t id, int32_t sel) {
  return mb_str_to_pion(mb_host_term_text(id, sel));
}

void *pion_host_term_mouse(int32_t id, int32_t action, int32_t button, int32_t mods,
                           int32_t x, int32_t y, int32_t w, int32_t h, int32_t cw, int32_t ch) {
  return mb_bytes_to_pion(mb_host_term_mouse(id, action, button, mods, x, y, w, h, cw, ch));
}

// ---- Native call bridges ----

int32_t pion_host_native_call(const char *name, int64_t name_len, const char *params, int64_t params_len) {
  moonbit_string_t mn = pion_mb_str_from_utf8(name, name_len);
  moonbit_string_t mp = pion_mb_str_from_utf8(params, params_len);
  int32_t r = mb_host_native_call(mn, mp);
  moonbit_decref(mn);
  moonbit_decref(mp);
  return r;
}

void *pion_host_native_result(int32_t id) {
  return mb_str_to_pion(mb_host_native_result(id));
}
