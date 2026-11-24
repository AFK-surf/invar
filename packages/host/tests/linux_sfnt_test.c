// The Linux host's WOFF / WOFF2 unwrapping (linux_text.c mb_sfnt_of_face) on a real web font (web/inter.woff2, or the
// files named): every table's directory checksum (head's with checkSumAdjustment taken as 0), the whole file's sum
// 0xB1B0AFBA, the directory in tag order with each table 4-aligned and in the file, and FreeType reading the SFNT
// back as the same face. scripts/check-linux-host.sh builds and runs it.
#define MB_SFNT_ONLY
#include "linux_text.c"

static int failed;
#define CHECK(cond, ...)                 \
  do {                                   \
    if (!(cond)) {                       \
      fprintf(stderr, "  FAIL: ");       \
      fprintf(stderr, __VA_ARGS__);      \
      fprintf(stderr, "\n");             \
      failed = 1;                        \
    }                                    \
  } while (0)

static uint32_t get32(const unsigned char *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static uint32_t get16(const unsigned char *p) { return (uint32_t)p[0] << 8 | (uint32_t)p[1]; }

static void check_font(FT_Library lib, const char *path) {
  printf("==> %s\n", path);
  FT_Face web;
  if (FT_New_Face(lib, path, 0, &web) != 0) {
    CHECK(0, "FreeType cannot open %s", path);
    return;
  }
  size_t len = 0;
  unsigned char *sf = mb_sfnt_of_face(web, &len);
  CHECK(sf != NULL && len > 12, "no SFNT made");
  if (!sf) {
    FT_Done_Face(web);
    return;
  }
  uint32_t n = get16(sf + 4);
  CHECK(len >= 12 + 16 * (size_t)n, "directory past the file");
  uint32_t sel = 0;
  while ((2U << sel) <= n) sel++;
  CHECK(get16(sf + 6) == (16U << sel) && get16(sf + 8) == sel && get16(sf + 10) == n * 16 - (16U << sel),
        "searchRange / entrySelector / rangeShift");
  int heads = 0;
  for (uint32_t i = 0; i < n && 12 + 16 * (size_t)i + 16 <= len; i++) {
    const unsigned char *d = sf + 12 + 16 * i;
    uint32_t tag = get32(d), sum = get32(d + 4), off = get32(d + 8), tl = get32(d + 12);
    char t[5] = {(char)(tag >> 24), (char)(tag >> 16), (char)(tag >> 8), (char)tag, 0};
    if (i > 0) CHECK(get32(d - 16) < tag, "directory not in ascending tag order at '%s'", t);
    CHECK(off % 4 == 0, "'%s' not 4-aligned (%u)", t, off);
    size_t padded = ((size_t)tl + 3) & ~(size_t)3;
    if (!(off >= 12 + 16 * (size_t)n && (size_t)off + padded <= len)) {
      CHECK(0, "'%s' outside the file", t);
      continue;
    }
    unsigned char *copy = malloc(padded);
    memcpy(copy, sf + off, padded);
    if (tag == FT_MAKE_TAG('h', 'e', 'a', 'd')) {
      heads++;
      memset(copy + 8, 0, 4);
    }
    uint32_t want = mb_sfnt_sum(copy, padded);
    free(copy);
    CHECK(sum == want, "'%s' checksum %08X, its bytes sum to %08X", t, sum, want);
    // (and the table as FreeType reads it from the web font, byte for byte; head but its adjustment)
    FT_ULong wl = 0;
    if (FT_Load_Sfnt_Table(web, tag, 0, NULL, &wl) == 0 && wl == tl) {
      unsigned char *orig = malloc(wl ? wl : 1);
      FT_Load_Sfnt_Table(web, tag, 0, orig, &wl);
      int same = tag == FT_MAKE_TAG('h', 'e', 'a', 'd')
                     ? (memcmp(orig, sf + off, 8) == 0 && memcmp(orig + 12, sf + off + 12, wl - 12) == 0)
                     : memcmp(orig, sf + off, wl) == 0;
      CHECK(same, "'%s' differs from the web font's", t);
      free(orig);
    } else {
      CHECK(0, "'%s' not in the web font as long", t);
    }
  }
  CHECK(heads == 1, "one head table (%d)", heads);
  uint32_t whole = mb_sfnt_sum(sf, len);
  CHECK(whole == 0xB1B0AFBAU, "whole-file checksum %08X, not B1B0AFBA", whole);
  printf("  %u tables, %zu bytes, whole-file checksum %08X\n", n, len, whole);
  FT_Face back;
  if (FT_New_Memory_Face(lib, sf, (FT_Long)len, 0, &back) == 0) {
    CHECK(back->num_glyphs == web->num_glyphs, "glyphs %ld, the web font's %ld", back->num_glyphs, web->num_glyphs);
    CHECK(back->family_name && web->family_name && strcmp(back->family_name, web->family_name) == 0, "family");
    CHECK(back->units_per_EM == web->units_per_EM, "units per em");
    CHECK(FT_Load_Char(back, 'A', FT_LOAD_NO_SCALE) == 0 && back->glyph->outline.n_points > 0, "'A' has an outline");
    FT_Done_Face(back);
  } else {
    CHECK(0, "FreeType cannot read the SFNT made");
  }
  free(sf);
  FT_Done_Face(web);
}

int main(int argc, char **argv) {
  FT_Library lib;
  if (FT_Init_FreeType(&lib)) {
    fprintf(stderr, "no FreeType\n");
    return 1;
  }
  if (argc < 2) {
    check_font(lib, "web/inter.woff2");
  } else {
    for (int i = 1; i < argc; i++) check_font(lib, argv[i]);
  }
  FT_Done_FreeType(lib);
  if (failed) {
    fprintf(stderr, "==> linux_sfnt_test FAILED\n");
    return 1;
  }
  printf("==> linux_sfnt_test PASSED\n");
  return 0;
}
