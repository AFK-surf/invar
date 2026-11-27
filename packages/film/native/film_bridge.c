// Thin C bridge between Pion UTF-8 strings/Arrays and native film backends (mbf_*).
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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
extern int32_t mbf_snapshot_inst(const double *inst, int32_t count, double r, double g, double b, int32_t transparent,
                                 int32_t async_write, moonbit_string_t path);
extern int32_t mbf_flush_snapshots(void);
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

int32_t pion_film_snapshot_inst(const double *inst, int32_t count, double r, double g, double b, int32_t transparent,
                                int32_t async_write, const char *path, int64_t path_len) {
  moonbit_string_t s = pion_mb_str_from_utf8(path, path_len);
  int32_t ok = mbf_snapshot_inst(inst, count, r, g, b, transparent, async_write, s);
  moonbit_decref(s);
  return ok;
}

int32_t pion_film_flush_snapshots(void) {
  return mbf_flush_snapshots();
}

// What a test holds film's PNG writer to (Apple: ImageIO's PNG of the same pixels, apple_render.m); -1 elsewhere.
#ifdef __APPLE__
#include <zlib.h>
extern int mb_png_write_bgra_fast(const uint8_t *px, int w, int h, const char *path);
extern int mb_png_write_bgra_imageio(const uint8_t *px, int w, int h, const char *path);
extern int mb_render_snapshot_reference(const char *path, int transparent);

static char *film_cpath(const char *path, int64_t path_len) {
  char *c = (char *)malloc((size_t)path_len + 1);
  if (!c) return NULL;
  memcpy(c, path, (size_t)path_len);
  c[path_len] = '\0';
  return c;
}
#endif

// Pixels `bgra` (premultiplied, w × h, rows from the top) written by the fast writer to `fast_path` and by ImageIO to
// `ref_path`: 1 when both were written, 0 when not, -1 off Apple.
int32_t pion_film_png_pair(const uint8_t *bgra, int32_t w, int32_t h, const char *fast_path, int64_t fast_len,
                           const char *ref_path, int64_t ref_len) {
#ifdef __APPLE__
  char *f = film_cpath(fast_path, fast_len), *r = film_cpath(ref_path, ref_len);
  int ok = f && r && mb_png_write_bgra_fast(bgra, w, h, f) && mb_png_write_bgra_imageio(bgra, w, h, r);
  free(f);
  free(r);
  return ok;
#else
  (void)bgra; (void)w; (void)h; (void)fast_path; (void)fast_len; (void)ref_path; (void)ref_len;
  return -1;
#endif
}

#ifdef __APPLE__
static uint32_t film_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// The PNG in `b` (`n` bytes) checked strictly: its signature; every chunk's CRC-32; IHDR first (8-bit RGB or RGBA,
// not interlaced), the IDATs one run, IEND last with nothing after; the IDATs joined inflated whole as one zlib stream
// (its RFC 1950 header and Adler-32 checked by zlib) to exactly the rows IHDR says, each with a filter of 0 to 4.
static int film_png_strict(const uint8_t *b, size_t n) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (n < 8 || memcmp(b, sig, 8) != 0) return 0;
  size_t p = 8, idat_n = 0;
  uint32_t w = 0, h = 0;
  int ch = 0, seen_ihdr = 0, seen_iend = 0, idat_state = 0;  // 0 none yet, 1 in the run, 2 after it
  uint8_t *idat = NULL;
  while (p < n) {
    if (seen_iend || n - p < 12) {
      free(idat);
      return 0;
    }
    uint32_t len = film_be32(b + p);
    if (len > n - p - 12) {
      free(idat);
      return 0;
    }
    const uint8_t *type = b + p + 4, *data = b + p + 8;
    uLong crc = crc32(crc32(0L, type, 4), data, len);
    if (crc != film_be32(data + len)) {
      free(idat);
      return 0;
    }
    int is_idat = memcmp(type, "IDAT", 4) == 0;
    if (!seen_ihdr) {
      if (memcmp(type, "IHDR", 4) != 0 || len != 13) {
        free(idat);
        return 0;
      }
      w = film_be32(data);
      h = film_be32(data + 4);
      ch = data[9] == 6 ? 4 : data[9] == 2 ? 3 : 0;
      if (w == 0 || h == 0 || data[8] != 8 || !ch || data[10] != 0 || data[11] != 0 || data[12] != 0) {
        free(idat);
        return 0;
      }
      seen_ihdr = 1;
    } else if (memcmp(type, "IHDR", 4) == 0) {
      free(idat);
      return 0;
    } else if (is_idat) {
      if (idat_state == 2) {
        free(idat);
        return 0;
      }
      idat_state = 1;
      uint8_t *more = realloc(idat, idat_n + len + 1);
      if (!more) {
        free(idat);
        return 0;
      }
      idat = more;
      memcpy(idat + idat_n, data, len);
      idat_n += len;
    } else {
      if (idat_state == 1) idat_state = 2;
      if (memcmp(type, "IEND", 4) == 0) {
        if (len != 0) {
          free(idat);
          return 0;
        }
        seen_iend = 1;
      }
    }
    p += 12 + (size_t)len;
  }
  if (!seen_iend || !idat_state || !idat) {
    free(idat);
    return 0;
  }
  size_t row = (size_t)w * (size_t)ch + 1, want = row * h;
  uint8_t *raw = malloc(want + 1);
  int ok = 0;
  z_stream zs;
  memset(&zs, 0, sizeof zs);
  if (raw && inflateInit(&zs) == Z_OK) {
    zs.next_in = idat;
    zs.avail_in = (uInt)idat_n;
    zs.next_out = raw;
    zs.avail_out = (uInt)(want + 1);
    int rc = inflate(&zs, Z_FINISH);
    ok = rc == Z_STREAM_END && zs.total_out == want && zs.avail_in == 0;
    inflateEnd(&zs);
    for (uint32_t y = 0; ok && y < h; y++) ok = raw[(size_t)y * row] <= 4;
  }
  free(raw);
  free(idat);
  return ok;
}
#endif

// The PNG at `path` checked strictly (film_png_strict): 1 when it holds, 0 when not, -1 off Apple.
int32_t pion_film_png_verify_strict(const char *path, int64_t path_len) {
#ifdef __APPLE__
  char *c = film_cpath(path, path_len);
  if (!c) return 0;
  FILE *f = fopen(c, "rb");
  free(c);
  if (!f) return 0;
  uint8_t *b = NULL;
  size_t n = 0, cap = 0, got;
  uint8_t chunk[65536];
  while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
    if (n + got > cap) {
      cap = (n + got) * 2;
      uint8_t *more = realloc(b, cap);
      if (!more) {
        free(b);
        fclose(f);
        return 0;
      }
      b = more;
    }
    memcpy(b + n, chunk, got);
    n += got;
  }
  fclose(f);
  int ok = b && film_png_strict(b, n);
  free(b);
  return ok;
#else
  (void)path; (void)path_len;
  return -1;
#endif
}

// The last frame drawn again as a snapshot draws it and written by ImageIO to `path`: 1, or 0 (-1 off Apple).
int32_t pion_film_snapshot_reference(const char *path, int64_t path_len, int32_t transparent) {
#ifdef __APPLE__
  char *p = film_cpath(path, path_len);
  int ok = p && mb_render_snapshot_reference(p, transparent);
  free(p);
  return ok;
#else
  (void)path; (void)path_len; (void)transparent;
  return -1;
#endif
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

int32_t pion_film_pid(void) {
#ifdef _WIN32
  return (int32_t)_getpid();
#else
  return (int32_t)getpid();
#endif
}

int32_t pion_film_remove_file(const char *path, int64_t path_len) {
  if (!path || path_len <= 0) return 0;
  char *cpath = (char *)malloc((size_t)path_len + 1);
  if (!cpath) return 0;
  memcpy(cpath, path, (size_t)path_len);
  cpath[path_len] = '\0';
  int rc = remove(cpath);
  free(cpath);
  return rc == 0 ? 1 : 0;
}

static int film_mkdir_one(const char *dir) {
#ifdef _WIN32
  if (_mkdir(dir) == 0) return 1;
  if (errno == EEXIST) {
    struct _stat st;
    return _stat(dir, &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
  }
#else
  if (mkdir(dir, 0755) == 0) return 1;
  if (errno == EEXIST) {
    struct stat st;
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
  }
#endif
  return 0;
}

int32_t pion_film_ensure_parent_dir(const char *path, int64_t path_len) {
  if (!path || path_len <= 0) return 0;
  char *buf = (char *)malloc((size_t)path_len + 1);
  if (!buf) return 0;
  memcpy(buf, path, (size_t)path_len);
  buf[path_len] = '\0';
  char *last_sep = strrchr(buf, '/');
#ifdef _WIN32
  char *last_bsep = strrchr(buf, '\\');
  if (last_bsep && (!last_sep || last_bsep > last_sep)) last_sep = last_bsep;
#endif
  if (!last_sep || last_sep == buf) {
    free(buf);
    return 1;
  }
  *last_sep = '\0';
  for (char *p = buf + 1; *p; p++) {
    if (*p == '/'
#ifdef _WIN32
        || *p == '\\'
#endif
    ) {
#ifdef _WIN32
      if (p == buf + 2 && buf[1] == ':') continue;
#endif
      char saved = *p;
      *p = '\0';
      if (buf[0] != '\0' && !film_mkdir_one(buf)) {
        free(buf);
        return 0;
      }
      *p = saved;
    }
  }
  int ok = film_mkdir_one(buf);
  free(buf);
  return ok;
}
