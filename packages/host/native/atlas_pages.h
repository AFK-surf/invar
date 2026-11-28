// How many pages a glyph atlas may have: a texture array of squares `side` texels a side (hostapi's
// zz_invar_atlas_pages.pion places glyphs in it, a glyph on page p said p * side down past its place in it). At least
// 2, at most 32 and 128 MiB, and what the device takes; or what the program says (mb_render_set_atlas_max_pages,
// else MBUI_ATLAS_PAGES: 1 is one page, as before pages).
#ifndef MB_ATLAS_PAGES_H
#define MB_ATLAS_PAGES_H

#include <stdint.h>
#include <stdlib.h>
#ifdef _WIN32
#include <wchar.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MB_ATLAS_PAGES_BUDGET (128LL * 1024 * 1024)
#define MB_ATLAS_PAGES_MOST 32

// what the program said (mb_render_set_atlas_max_pages; 0: nothing), the host's render file's
extern int mb_atlas_pages_said;

#ifdef _WIN32
const wchar_t *mb_win_env(const wchar_t *name);
static inline int mb_atlas_pages_env(void) {
  const wchar_t *v = mb_win_env(L"MBUI_ATLAS_PAGES");
  return v && *v ? (int)wcstol(v, NULL, 10) : 0;
}
#else
static inline int mb_atlas_pages_env(void) {
  const char *v = getenv("MBUI_ATLAS_PAGES");
  return v && *v ? atoi(v) : 0;
}
#endif

static inline int mb_atlas_pages_cap(int side, int texel_bytes, int device_most) {
  if (side <= 0) return 0;
  int said = mb_atlas_pages_said > 0 ? mb_atlas_pages_said : mb_atlas_pages_env();
  int n;
  if (said > 0) {
    n = said > MB_ATLAS_PAGES_MOST ? MB_ATLAS_PAGES_MOST : said;
  } else {
    long long page = (long long)side * side * (texel_bytes > 0 ? texel_bytes : 1);
    long long fit = MB_ATLAS_PAGES_BUDGET / page;
    n = fit > MB_ATLAS_PAGES_MOST ? MB_ATLAS_PAGES_MOST : (int)fit;
    if (n < 2) n = 2;
  }
  if (device_most > 0 && n > device_most) n = device_most;
  return n < 1 ? 1 : n;
}

#ifdef __cplusplus
}
#endif

#endif
