// Thin C bridge between Pion UTF-8 strings/Arrays and packages/host/native/apple_film.m (mbf_*).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint16_t *moonbit_string_t;

extern moonbit_string_t pion_mb_str_from_utf8(const char *utf8, int64_t byte_len);
extern void moonbit_decref(void *p);

extern int32_t mbf_supported(void);
extern void mbf_set_view(double w, double h, double scale);
extern void mbf_text_scale(double scale);
extern void mbf_dirty(double x, double y, double w, double h);
extern double mbf_now_ms(void);
extern void mbf_spin(double ms);
extern int32_t mbf_open(moonbit_string_t path, int32_t w, int32_t h, int32_t fps);
extern int32_t mbf_draw(const double *inst, int32_t count, double r, double g, double b);
extern int32_t mbf_put(int32_t k);
extern int32_t mbf_close(int32_t frames);
extern int32_t mbf_finish(int32_t publish);
extern int32_t mbf_snapshot(moonbit_string_t path);
extern int32_t mbf_snapshot_transparent(moonbit_string_t path);
extern void mbf_say(moonbit_string_t s);

int32_t pion_film_supported(void) {
  return mbf_supported();
}

void pion_film_set_view(double w, double h, double scale) {
  mbf_text_scale(scale);
  mbf_set_view(w, h, scale);
}

void pion_film_dirty(double x, double y, double w, double h) {
  mbf_dirty(x, y, w, h);
}

double pion_film_now_ms(void) {
  return mbf_now_ms();
}

void pion_film_spin(double ms) {
  mbf_spin(ms);
}

int32_t pion_film_open(const char *path, int64_t path_len, int32_t w, int32_t h, int32_t fps) {
  moonbit_string_t s = pion_mb_str_from_utf8(path, path_len);
  int32_t ok = mbf_open(s, w, h, fps);
  moonbit_decref(s);
  return ok;
}

int32_t pion_film_draw(const double *inst, int32_t count, double r, double g, double b) {
  return mbf_draw(inst, count, r, g, b);
}

int32_t pion_film_put(int32_t k) {
  return mbf_put(k);
}

int32_t pion_film_close(int32_t frames) {
  return mbf_close(frames);
}

int32_t pion_film_finish(int32_t publish) {
  return mbf_finish(publish);
}

int32_t pion_film_snapshot(const char *path, int64_t path_len, int32_t transparent) {
  moonbit_string_t s = pion_mb_str_from_utf8(path, path_len);
  int32_t ok = transparent ? mbf_snapshot_transparent(s) : mbf_snapshot(s);
  moonbit_decref(s);
  return ok;
}

void pion_film_say(const char *msg, int64_t msg_len) {
  moonbit_string_t s = pion_mb_str_from_utf8(msg, msg_len);
  mbf_say(s);
  moonbit_decref(s);
}

// Read a local file into a malloc'd buffer; returns byte length (>= 0) or -1 on error.
int64_t pion_film_file_size(const char *path, int64_t path_len) {
  char *cpath = (char *)malloc((size_t)path_len + 1);
  if (!cpath) return -1;
  memcpy(cpath, path, (size_t)path_len);
  cpath[path_len] = '\0';
  FILE *f = fopen(cpath, "rb");
  free(cpath);
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  long sz = ftell(f);
  fclose(f);
  return (int64_t)sz;
}

int64_t pion_film_read_file(const char *path, int64_t path_len, uint8_t *out, int64_t cap) {
  char *cpath = (char *)malloc((size_t)path_len + 1);
  if (!cpath) return -1;
  memcpy(cpath, path, (size_t)path_len);
  cpath[path_len] = '\0';
  FILE *f = fopen(cpath, "rb");
  free(cpath);
  if (!f) return -1;
  size_t n = fread(out, 1, (size_t)cap, f);
  fclose(f);
  return (int64_t)n;
}
