// Native C helpers for packages/render on macOS (Apple Metal offscreen + file/env I/O).
// Ported from libs/mbui/src/render/render_stub.c for Pion FFI.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

extern void mb_render_draw_timed(const double *inst, int32_t count, double r, double g, double b, double *out);
extern int mb_render_snapshot_drawn(const char *path);

static char *dup_cstr(const char *s, int64_t len) {
  if (!s || len <= 0) {
    char *empty = (char *)malloc(1);
    empty[0] = '\0';
    return empty;
  }
  char *out = (char *)malloc((size_t)len + 1);
  memcpy(out, s, (size_t)len);
  out[len] = '\0';
  return out;
}

static unsigned char *slurp_file(const char *path, int64_t path_len, long *out_len) {
  char *p = dup_cstr(path, path_len);
  FILE *f = fopen(p, "rb");
  free(p);
  *out_len = -1;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  unsigned char *buf = (unsigned char *)malloc((size_t)n + 1);
  if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(buf);
    return NULL;
  }
  buf[n] = '\0';
  fclose(f);
  *out_len = n;
  return buf;
}

char *pion_render_read_text(const char *path, int64_t path_len) {
  long n = 0;
  unsigned char *b = slurp_file(path, path_len, &n);
  if (!b || n < 0) {
    if (b) free(b);
    return dup_cstr("", 0);
  }
  return (char *)b;
}

unsigned char *pion_render_read_bytes(const char *path, int64_t path_len, int64_t *out_len) {
  long n = 0;
  unsigned char *b = slurp_file(path, path_len, &n);
  if (!b || n < 0) {
    if (b) free(b);
    *out_len = 0;
    return NULL;
  }
  *out_len = (int64_t)n;
  return b;
}

char *pion_render_arg(const char *name, int64_t name_len) {
  char *n = dup_cstr(name, name_len);
  const char *v = getenv(n);
  free(n);
  if (!v) return dup_cstr("", 0);
  return dup_cstr(v, (int64_t)strlen(v));
}

void pion_render_say(const char *s, int64_t len) {
  if (!s || len <= 0) {
    fprintf(stderr, "\n");
    return;
  }
  fwrite(s, 1, (size_t)len, stderr);
  fputc('\n', stderr);
}

void pion_render_spin(double ms) {
#if defined(__APPLE__)
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, ms / 1000.0, false);
#else
  (void)ms;
#endif
}

void pion_render_draw_timed(const double *inst, int32_t count, double r, double g, double b, double *out) {
  mb_render_draw_timed(inst, count, r, g, b, out);
}

int32_t pion_render_snapshot_drawn(const char *path, int64_t path_len) {
  char *p = dup_cstr(path, path_len);
  int ok = mb_render_snapshot_drawn(p);
  free(p);
  return ok;
}
