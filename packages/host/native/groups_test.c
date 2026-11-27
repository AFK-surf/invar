// groups.h on its own: what it keeps stays as large as what is kept at once, and headers are drawn out as it says.
//   cc -Ipackages/host/native packages/host/native/groups_test.c -o groups_test -lm && ./groups_test
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// (each table made: groups.h makes them with calloc alone)
static int tables, calloc_fails;
static void *counted_calloc(size_t n, size_t size) {
  if (calloc_fails) return NULL;
  tables++;
  return calloc(n, size);
}
#define calloc counted_calloc
#include "groups.h"
#undef calloc

static int checks;
#define CHECK(c)                                                  \
  do {                                                            \
    checks++;                                                     \
    if (!(c)) {                                                   \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);     \
      exit(1);                                                    \
    }                                                             \
  } while (0)

#define N MB_GROUP_INST
#define POISON 12345.5

static void inst_at(double *e, double x, double y, double kind) {
  memset(e, 0, sizeof(double) * N);
  e[0] = x;
  e[1] = y;
  e[2] = 10;
  e[3] = 20;
  e[5] = kind;
  e[18] = e[19] = 1e9;
}

static void head_at(double *e, double x, double y, int id, int first, int n) {
  memset(e, 0, sizeof(double) * N);
  e[0] = x;
  e[1] = y;
  e[5] = MB_GROUP_HEAD;
  e[12] = id;
  e[13] = first;
  e[14] = n;
}

static void poison(double *e, int n) {
  for (int j = 0; j < n * N; j++) e[j] = j % 2 ? POISON : NAN;
}

static void forget(void) {
  for (size_t i = 0; i < mb_groups_cap; i++)
    if (mb_groups[i].id > 0) mb_host_group_drop(mb_groups[i].id);
  CHECK(mb_groups_live == 0);
  free(mb_groups);
  mb_groups = NULL;
  mb_groups_cap = mb_groups_taken = 0;
  tables = 0;
}

static void counted(void) {
  size_t live = 0, gone = 0;
  for (size_t i = 0; i < mb_groups_cap; i++) {
    live += mb_groups[i].id > 0;
    gone += mb_groups[i].id < 0;
  }
  CHECK(live == mb_groups_live && live + gone == mb_groups_taken);
  CHECK(mb_groups_taken * 2 < mb_groups_cap + 1);
}

static void test_churn(void) {
  double e[N], m[1] = {0};
  inst_at(e, 1, 2, 0);
  for (int id = 1; id <= 100000; id++) {
    mb_host_group_set(id, e, m, 1);
    CHECK(mb_group_find(id));
    mb_host_group_drop(id);
  }
  counted();
  CHECK(mb_groups_live == 0);
  // (never more places; each time put again, a quarter of them at least taken since)
  CHECK(mb_groups_cap <= 256);
  CHECK(tables <= 1 + 100000 / 64);
  printf("churn, 0 kept: cap %zu, tables %d\n", mb_groups_cap, tables);
  forget();

  // a window of rows over a long list, the oldest let go as one more comes in
  static const int windows[] = {1, 60, 62, 63, 64, 100, 120, 125, 126, 127, 128, 140, 154, 160, 179, 200, 254, 256, 310, 358, 600};
  for (size_t w = 0; w < sizeof windows / sizeof *windows; w++) {
    int held = windows[w];
    for (int id = 1; id <= 100000; id++) {
      e[0] = id;
      mb_host_group_set(id, e, m, 1);
      if (id > held) mb_host_group_drop(id - held);
    }
    counted();
    CHECK(mb_groups_live == (size_t)held);
    size_t fits = 256;
    while (fits < (size_t)(held + 1) * 8) fits *= 2;
    CHECK(mb_groups_cap <= fits);
    CHECK((size_t)tables <= 5 + 100000 / (mb_groups_cap / 4));
    for (int id = 100000 - held + 1; id <= 100000; id++) {
      mb_group *g = mb_group_find(id);
      CHECK(g && g->inst[0] == id);
    }
    CHECK(!mb_group_find(100000 - held));
    printf("churn, %d kept: cap %zu, tables %d\n", held, mb_groups_cap, tables);
    forget();
  }

  // the same group set again and again
  for (int i = 0; i < 100000; i++) mb_host_group_set(7, e, m, 1);
  CHECK(mb_groups_live == 1 && mb_groups_taken == 1 && tables == 1);
  forget();
}

static void test_insert(void) {
  double e[N], m[1] = {0};
  inst_at(e, 1, 2, 0);
  size_t cap = 0;
  for (int id = 1; id <= 1000; id++) {
    int was = tables;
    mb_host_group_set(id, e, m, 1);
    if (tables != was) {
      // (each table made twice the one before it, never as many again)
      CHECK(tables == was + 1);
      CHECK(mb_groups_cap == (cap ? cap * 2 : 256));
    }
    cap = mb_groups_cap;
  }
  counted();
  // 256, 512, 1024, 2048
  CHECK(mb_groups_cap == 2048 && tables == 4 && mb_groups_live == 1000);
  for (int id = 1; id <= 1000; id++) CHECK(mb_group_find(id));
  printf("1000 kept: cap %zu, tables %d\n", mb_groups_cap, tables);
  forget();
}

static void test_no_group(void) {
  double e[N], m[1] = {0};
  inst_at(e, 1, 2, 0);
  mb_host_group_drop(3);
  mb_host_group_set(3, e, m, 1);
  mb_host_group_drop(0);
  mb_host_group_drop(-1);
  mb_host_group_set(0, e, m, 1);
  mb_host_group_set(-1, e, m, 1);
  mb_host_group_drop(4);
  counted();
  CHECK(mb_groups_live == 1 && mb_groups_taken == 1);
  mb_host_group_drop(3);
  mb_host_group_drop(3);
  mb_host_group_drop(-1);
  counted();
  CHECK(mb_groups_live == 0);
  forget();
}

static void test_part(void) {
  // three of which the second and third are said, moved, the band's pieces on, the clip's y alone moved
  double g[3 * N], m[3] = {0, 2 | 8, 0};
  inst_at(g, 1, 1, 0);
  inst_at(g + N, 0.1, 0.7, 11);
  g[N + 12] = 3;
  g[N + 13] = 2;
  g[N + 16] = 1, g[N + 17] = 2, g[N + 18] = 300, g[N + 19] = 400;
  inst_at(g + 2 * N, 5, 6, 0);
  mb_host_group_set(7, g, m, 3);
  // (kept as its own: what it was made from may go)
  poison(g, 3);

  // a ground, the header and its poisoned room, one after it, the pieces of shapes at the tail
  double f[6 * N];
  inst_at(f, 0, 0, 0);
  head_at(f + N, 0.2, 0.1, 7, 1, 2);
  f[N + 15] = 5;
  poison(f + 2 * N, 1);
  inst_at(f + 3 * N, 8, 9, 1);
  for (int k = 4; k < 6; k++) {
    for (int j = 0; j < N; j++) f[k * N + j] = k * 100 + j + 0.1;
    f[k * N + 5] = 12;
  }
  float o[6 * N];
  mb_groups_floats(o, f, 6);
  for (int j = 0; j < N; j++) {
    CHECK(o[j] == (float)f[j]);
    for (int k = 3; k < 6; k++) CHECK(o[k * N + j] == (float)f[k * N + j]);
  }
  const float *a = o + N, *b = o + 2 * N;
  // (moved in doubles, then made a float)
  CHECK(a[0] == (float)(0.1 + 0.2) && a[1] == (float)(0.7 + 0.1));
  CHECK(a[12] == 8 && a[13] == 2);
  CHECK(a[16] == 1 && a[17] == (float)(2 + 0.1) && a[18] == 300 && a[19] == (float)(400 + 0.1));
  CHECK(b[0] == (float)5.2 && b[1] == (float)(6 + 0.1) && b[12] == 0 && b[5] == 0);
  for (int j = 0; j < 2 * N; j++) CHECK(!isnan(o[N + j]) && o[N + j] != (float)POISON);

  // a place where a float kept and then moved rounds twice, a double does not
  int differ = 0;
  for (int i = 1; i < 1000; i++) {
    double x = i * 0.1, dx = i * 0.37;
    inst_at(g, x, 0, 0);
    mb_host_group_set(8, g, m, 1);
    head_at(f, dx, 0, 8, 0, 1);
    mb_groups_floats(o, f, 1);
    CHECK(o[0] == (float)(x + dx));
    differ += (float)(x + dx) != (float)((float)x + (float)dx);
  }
  CHECK(differ > 0);
  forget();
}

static void test_moves(void) {
  // each bit of an instance's moves moves its own numbers alone
  static const int slots[13][2] = {{16, 0}, {17, 0}, {18, 0}, {19, 0}, {40, 0}, {41, 0}, {42, 0},
                                   {43, 0}, {33, 34}, {52, 0}, {54, 0}, {56, 0}, {58, 0}};
  static const int down[13] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0};
  for (int bit = 0; bit < 13; bit++) {
    double g[N], m[1] = {1 << bit}, f[N];
    for (int j = 0; j < N; j++) g[j] = 1000 + j;
    g[5] = 0;
    mb_host_group_set(5, g, m, 1);
    head_at(f, 0.25, 0.5, 5, 0, 1);
    float o[N];
    mb_groups_floats(o, f, 1);
    for (int j = 0; j < N; j++) {
      double want = g[j];
      if (j == 0) want += 0.25;
      if (j == 1) want += 0.5;
      if (j == slots[bit][0]) want += down[bit] ? 0.5 : 0.25;
      if (slots[bit][1] && j == slots[bit][1]) want += 0.5;
      CHECK(o[j] == (float)want);
    }
  }
  forget();
}

static void test_clip(void) {
  double g[3 * N], m[3] = {0, 0, 0}, f[3 * N];
  // a round clip with no outer: cut, the corners of the sides that moved square
  inst_at(g, 0, 0, 0);
  g[16] = 0, g[17] = 0, g[18] = 100, g[19] = 100;
  for (int j = 24; j < 32; j++) g[j] = 8;
  // a round clip with an outer: the outer cut, the clip as it was
  memcpy(g + N, g, sizeof(double) * N);
  g[N + 40] = 0, g[N + 41] = 0, g[N + 42] = 100, g[N + 43] = 100;
  // a square clip with an outer: the clip cut
  memcpy(g + 2 * N, g + N, sizeof(double) * N);
  for (int j = 24; j < 32; j++) g[2 * N + j] = 0;
  mb_host_group_set(5, g, m, 3);
  head_at(f, 0, 0, 5, 0, 3);
  f[16] = -5, f[17] = 10, f[18] = 1000, f[19] = 100;
  poison(f + N, 2);
  float o[3 * N];
  mb_groups_floats(o, f, 3);
  CHECK(o[16] == 0 && o[17] == 10 && o[18] == 100 && o[19] == 100);
  CHECK(o[24] == 0 && o[25] == 0 && o[26] == 8 && o[27] == 8 && o[28] == 0 && o[29] == 0 && o[30] == 8 && o[31] == 8);
  CHECK(o[N + 17] == 0 && o[N + 24] == 8 && o[N + 40] == 0 && o[N + 41] == 10 && o[N + 43] == 100);
  CHECK(o[2 * N + 17] == 10 && o[2 * N + 41] == 0);
  forget();
}

// A rounded clip (0, 0)-(100, 100), its corners 26, cut to (-50, -50)-(150, `foot`): the corners at its foot, -1 where
// the two differ.
static double cut_foot_corner(double foot) {
  double e[N] = {0};
  e[18] = 100.0;
  e[19] = 100.0;
  for (int j = 24; j < 32; j++) e[j] = 26.0;
  mb_group_inst_moved(e, 0, 0.0, 0.0, 0.0, 1, -50.0, -50.0, 150.0, foot);
  CHECK(e[24] == 26.0 && e[25] == 26.0 && e[28] == 26.0 && e[29] == 26.0);
  return e[26] == e[27] && e[26] == e[30] && e[26] == e[31] ? e[26] : -1.0;
}

static void test_clip_hair(void) {
  // a cut by a hair (MB_CLIP_HAIR) or less keeps the corners round, one by more squares them
  CHECK(cut_foot_corner(99.7) == 26.0);
  CHECK(cut_foot_corner(99.5) == 26.0);
  CHECK(cut_foot_corner(99.4) == 0.0);
  CHECK(cut_foot_corner(98.0) == 0.0);
  // the same through a header: a top cut by 0.3 keeps the top's corners, a left cut by 2 squares the left's
  double g[N], m[1] = {0}, f[N];
  inst_at(g, 0, 0, 0);
  g[16] = 0, g[17] = 0, g[18] = 100, g[19] = 100;
  for (int j = 24; j < 32; j++) g[j] = 26;
  mb_host_group_set(6, g, m, 1);
  head_at(f, 0, 0, 6, 0, 1);
  f[16] = 2, f[17] = 0.3, f[18] = 1000, f[19] = 1000;
  float o[N];
  mb_groups_floats(o, f, 1);
  CHECK(o[16] == 2 && o[17] == 0.3f && o[18] == 100 && o[19] == 100);
  CHECK(o[24] == 0 && o[25] == 26 && o[26] == 26 && o[27] == 0);
  CHECK(o[28] == 0 && o[29] == 26 && o[30] == 26 && o[31] == 0);
  forget();
}

static void test_nested(void) {
  double row[2 * N], m[4] = {0, 0, 0, 0};
  inst_at(row, 1, 2, 0);
  inst_at(row + N, 3, 4, 11);
  row[N + 12] = 1;
  mb_host_group_set(7, row, m, 2);
  // a list of two of the row and one of its own between, in a list of that
  double list[3 * N];
  head_at(list, 10, 20, 7, 0, 2);
  list[15] = 100;
  list[16] = 5, list[17] = 6, list[18] = 70, list[19] = 80;
  inst_at(list + N, 30, 40, 1);
  head_at(list + 2 * N, 50, 60, 7, 1, 1);
  mb_host_group_set(8, list, m, 3);
  double outer[N];
  head_at(outer, 1000, 2000, 8, 0, 4);
  mb_host_group_set(9, outer, m, 1);

  double f[6 * N];
  head_at(f, 10000, 20000, 9, 0, 6);
  f[15] = 7;
  f[16] = 1, f[17] = 10, f[18] = 60, f[19] = 1000;
  poison(f + N, 5);
  float o[6 * N];
  mb_groups_floats(o, f, 6);
  CHECK(o[0] == 11011 && o[1] == 22022);
  CHECK(o[N] == 11013 && o[N + 1] == 22024 && o[N + 12] == 108);
  CHECK(o[2 * N] == 11030 && o[2 * N + 1] == 22040 && o[2 * N + 5] == 1);
  CHECK(o[3 * N] == 11053 && o[3 * N + 1] == 22064 && o[3 * N + 12] == 8);
  // (the header's own clip and the outer's, as much of both; none of its own: the outer's)
  CHECK(o[16] == 5 && o[17] == 10 && o[18] == 60 && o[19] == 80);
  CHECK(o[3 * N + 16] == 1 && o[3 * N + 17] == 10 && o[3 * N + 18] == 60 && o[3 * N + 19] == 1000);
  // (it says more than it holds: nothing there)
  for (int j = 4 * N; j < 6 * N; j++) CHECK(o[j] == 0);

  // fewer than it holds: cut off there, what is after it as it is
  head_at(f, 0, 0, 8, 0, 1);
  inst_at(f + N, 77, 78, 0);
  mb_groups_floats(o, f, 2);
  CHECK(o[0] == 11 && o[N] == 77);
  // more than the frame has room for; a part of a group past what it holds, one not kept, of no number: nothing
  head_at(f, 0, 0, 8, 0, 100);
  poison(f + N, 1);
  mb_groups_floats(o, f, 2);
  CHECK(o[0] == 11 && o[N] == 13);
  static const int none[4][3] = {{7, 1, 2}, {7, -1, 2}, {6, 0, 2}, {0, 0, 2}};
  for (int i = 0; i < 4; i++) {
    head_at(f, 0, 0, none[i][0], none[i][1], none[i][2]);
    poison(f + N, 1);
    mb_groups_floats(o, f, 2);
    for (int j = 0; j < 2 * N; j++) CHECK(o[j] == 0);
  }
  // how many said as none: one
  head_at(f, 0, 0, 7, 0, 0);
  mb_groups_floats(o, f, 2);
  CHECK(o[0] == 1);
  forget();
}

static void test_round(void) {
  // groups that come round to themselves: a header first, one after an instance, two of each other
  double g[3 * N], m[3] = {0}, f[40 * N];
  static float o[40 * N];
  head_at(g, 1, 1, 7, 0, 40);
  inst_at(g + N, 5, 5, 0);
  mb_host_group_set(7, g, m, 2);
  head_at(f, 0, 0, 7, 0, 40);
  poison(f + N, 39);
  mb_groups_floats(o, f, 40);
  for (int j = 0; j < 40 * N; j++) CHECK(!isnan(o[j]) && o[j] != (float)POISON);
  inst_at(g, 5, 5, 0);
  head_at(g + N, 1, 1, 7, 0, 40);
  head_at(g + 2 * N, 2, 2, 7, 0, 40);
  mb_host_group_set(7, g, m, 3);
  mb_groups_floats(o, f, 40);
  for (int i = 0; i < 8; i++) CHECK(o[i * N] == 5 + i);
  for (int j = 8 * N; j < 40 * N; j++) CHECK(o[j] == 0);
  head_at(g, 0, 0, 8, 0, 40);
  mb_host_group_set(7, g, m, 1);
  head_at(g, 0, 0, 7, 0, 40);
  mb_host_group_set(8, g, m, 1);
  mb_groups_floats(o, f, 40);
  for (int j = 0; j < 40 * N; j++) CHECK(o[j] == 0);
  forget();
}

static void test_no_room(void) {
  // no table to be made: none kept at first, then those here as they are, as many as fit, the rest not kept
  double e[N], m[1] = {0}, f[N];
  float o[N];
  inst_at(e, 1, 2, 0);
  calloc_fails = 1;
  mb_host_group_set(1, e, m, 1);
  CHECK(mb_groups_cap == 0 && !mb_group_find(1));
  mb_host_group_drop(1);
  head_at(f, 0, 0, 1, 0, 1);
  mb_groups_floats(o, f, 1);
  CHECK(o[0] == 0);
  calloc_fails = 0;
  mb_host_group_set(1, e, m, 1);
  calloc_fails = 1;
  for (int id = 2; id <= 1000; id++) mb_host_group_set(id, e, m, 1);
  CHECK(mb_groups_cap == 256 && mb_groups_live == 255 && mb_groups_taken == 255);
  for (int id = 1; id <= 1000; id++) CHECK(!mb_group_find(id) == (id > 255));
  for (int id = 1; id <= 255; id++) mb_host_group_drop(id);
  mb_host_group_set(2000, e, m, 1);
  CHECK(!mb_group_find(2000));
  calloc_fails = 0;
  for (int id = 1; id <= 1000; id++) mb_host_group_set(id, e, m, 1);
  for (int id = 1; id <= 1000; id++) CHECK(mb_group_find(id));
  counted();
  forget();
}

int main(void) {
  test_round();
  test_no_room();
  test_churn();
  test_insert();
  test_no_group();
  test_part();
  test_moves();
  test_clip();
  test_clip_hair();
  test_nested();
  printf("groups_test: ok, %d checks\n", checks);
  return 0;
}
