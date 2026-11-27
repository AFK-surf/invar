// Standalone Windows D3D11 WARP shader-bake & Liquid Glass numerical smoke test driver.
// Zero Pion/MoonBit runtime dependency (uses MB_NO_MOONBIT_H).
//
// Build (MSVC Developer Command Prompt):
//   cl /nologo /O2 /EHsc /DMB_NO_MOONBIT_H /Ipackages/host/native \
//     packages/host/tests/win_render_test.cpp packages/host/native/win_render.cpp \
//     /Fe:win_render_test.exe
//
// Modes:
//   --pass1 : expects INVAR_IGNORE_BAKED=1 + empty MBUI_SHADERS=<gen_dir>; compiles all 21
//             shaders via D3DCompile -> writes <key>.cso to <gen_dir> & runs 5-vector WARP smoke.
//   --pass2 : expects INVAR_IGNORE_BAKED unset + empty MBUI_SHADERS=<verify_dir>; asserts
//             baked_hits == 21, disk_hits == 0, runtime_compiles == 0 & runs 5-vector WARP smoke.

#ifndef MB_NO_MOONBIT_H
#define MB_NO_MOONBIT_H
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win.h"

static size_t g_test_buf_len = 0;

extern "C" {
int mb_render_init_offscreen(double w, double h, double scale);
void mb_host_render_dirty(double x, double y, double w, double h);
void invar_win_shader_stats(int *baked_hits, int *disk_hits, int *runtime_compiles);
void invar_win_shader_stats_reset(void);

size_t invar_test_array_length(const void *p) {
  (void)p;
  return g_test_buf_len;
}

const wchar_t *mb_win_env(const wchar_t *name) {
  static wchar_t buf[8][MAX_PATH * 2];
  static int slot = 0;
  wchar_t *dst = buf[(slot++) & 7];
  DWORD n = GetEnvironmentVariableW(name, dst, MAX_PATH * 2);
  if (n == 0 || n >= MAX_PATH * 2) return NULL;
  return dst;
}

const wchar_t *mb_win_app_id(void) { return L""; }
void *mb_image_srv(int id) { (void)id; return NULL; }
uint64_t mb_image_generation(void) { return 0; }
uint64_t mb_image_free_generation(void) { return 0; }
int32_t mb_atlas_gen_now(void) { return 0; }
void *mb_stream_srv(int id) { (void)id; return NULL; }
void mb_text_set_scale(double scale) { (void)scale; }
int mb_views_count(void) { return 0; }
void mb_views_draw_under(uint8_t *out, int w, int h, double scale) {
  (void)out; (void)w; (void)h; (void)scale;
}
double mb_app_scale(void) { return 1.0; }
void mb_host_request_frame(void) {}
}

static const int INST_SIZE = 60;
static const int EXPECTED_SHADER_KEYS = 21;

static void push_inst(
    double *buf, int *count,
    double x, double y, double w, double h,
    double radius, double kind, double p6, double p7,
    double r, double g, double b, double a,
    double c2a, double c2b, double c2c, double c2d,
    double tex_id, double pad1, double pad2, double pad3) {
  double *o = buf + (size_t)(*count) * INST_SIZE;
  memset(o, 0, sizeof(double) * INST_SIZE);
  o[0] = x; o[1] = y; o[2] = w; o[3] = h;
  o[4] = radius; o[5] = kind; o[6] = p6; o[7] = p7;
  o[8] = r; o[9] = g; o[10] = b; o[11] = a;
  o[12] = c2a; o[13] = c2b; o[14] = c2c; o[15] = c2d;
  o[16] = 0.0; o[17] = 0.0; o[18] = 1e9; o[19] = 1e9;
  o[20] = tex_id; o[21] = pad1; o[22] = pad2; o[23] = pad3;
  o[36] = radius; o[37] = radius; o[38] = radius; o[39] = radius;
  (*count)++;
}

static int render_and_sample(
    const double *inst, int count,
    double bg_r, double bg_g, double bg_b,
    int px, int py,
    int *out_r, int *out_g, int *out_b) {
  const int W = 320, H = 240;
  const double SCALE = 2.0;
  static uint8_t rgba[320 * 240 * 4];
  memset(rgba, 0, sizeof rgba);
  g_test_buf_len = sizeof rgba;
  int ok = mb_render_pixels_of(inst, count, bg_r, bg_g, bg_b, SCALE, W, H, rgba);
  if (!ok) return 0;
  size_t idx = ((size_t)py * W + (size_t)px) * 4;
  *out_r = rgba[idx + 0];
  *out_g = rgba[idx + 1];
  *out_b = rgba[idx + 2];
  return 1;
}

static int check_rgb(const char *name, int r, int g, int b, int er, int eg, int eb, int tol) {
  int dr = abs(r - er), dg = abs(g - eg), db = abs(b - eb);
  printf("  [%s] %s: got=(%d,%d,%d) expected=(%d,%d,%d) tol=+/-%d\n",
         (dr <= tol && dg <= tol && db <= tol) ? "PASS" : "FAIL",
         name, r, g, b, er, eg, eb, tol);
  return (dr <= tol && dg <= tol && db <= tol) ? 1 : 0;
}

static int render_rgba(
    const double *inst, int count,
    double bg_r, double bg_g, double bg_b,
    double scale, int w, int h, uint8_t *rgba) {
  memset(rgba, 0, (size_t)w * (size_t)h * 4);
  g_test_buf_len = (size_t)w * (size_t)h * 4;
  return mb_render_pixels_of(inst, count, bg_r, bg_g, bg_b, scale, w, h, rgba);
}

static int px_luma_of(const uint8_t *rgba, int w, int x, int y) {
  size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 4;
  return (rgba[idx] * 2126 + rgba[idx + 1] * 7152 + rgba[idx + 2] * 722) / 10000;
}

static int run_v8_v9_2d_roi(void) {
  const int W3 = 480, H3 = 300;
  const double S3 = 3.0;
  static uint8_t rgba[480 * 300 * 4];
  double inst[48 * INST_SIZE];
  int ok = 1;

  // V8-A: Slider Dark 2D ROI
  {
    int n = 0;
    push_inst(inst, &n, 10.0, 47.0, 70.0, 6.0, 3.0, 0.0, 0.0, 0.0, 0.0, 136.0/255.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 80.0, 47.0, 70.0, 6.0, 3.0, 0.0, 0.0, 0.0, 104.0/255.0, 104.0/255.0, 110.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 51.0, 31.0, 58.0, 38.0, 19.0, 6.0, 1.0, -16.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 28.0/255.0, 28.0/255.0, 30.0/255.0, S3, W3, H3, rgba)) return 0;

    double best_blue_h = 0.0, best_grey_h = 0.0;
    double blue_peak_dx = -1.0, grey_peak_dx = -1.0;
    for (int i = 0; i <= 18; i++) {
      int xb = 153 + i, xg = 326 - i;
      double dx_pt = (i + 0.5) / S3;
      int cntB = 0, cntG = 0;
      for (int y = 93; y < 207; y++) {
        size_t ib = ((size_t)y * W3 + (size_t)xb) * 4;
        if ((int)rgba[ib + 2] - (int)rgba[ib] > 100) cntB++;
        if (px_luma_of(rgba, W3, xg, y) > 60) cntG++;
      }
      if (cntB / S3 > best_blue_h) { best_blue_h = cntB / S3; blue_peak_dx = dx_pt; }
      if (cntG / S3 > best_grey_h) { best_grey_h = cntG / S3; grey_peak_dx = dx_pt; }
    }
    int cnt55_b = 0, cnt55_g = 0;
    for (int y = 93; y < 207; y++) {
      size_t i55 = ((size_t)y * W3 + 169) * 4;
      if ((int)rgba[i55 + 2] - (int)rgba[i55] > 100) cnt55_b++;
      if (px_luma_of(rgba, W3, 310, y) > 72) cnt55_g++;
    }
    double h55_b = cnt55_b / S3, h55_g = cnt55_g / S3;
    size_t il = (150 * W3 + 237) * 4, ir = (150 * W3 + 243) * 4;
    size_t it = (105 * W3 + 156) * 4, ib = (195 * W3 + 156) * 4;
    int split_ok = ((int)rgba[il + 2] - (int)rgba[il] > 100) && ((int)rgba[ir + 2] - (int)rgba[ir] < 25);
    int zero_blue_ok = ((int)rgba[it + 2] - (int)rgba[it] < 25) && ((int)rgba[ib + 2] - (int)rgba[ib] < 25);
    int y_hair = px_luma_of(rgba, W3, 240, 93);
    int y_rim = px_luma_of(rgba, W3, 240, 95);
    int y_sh = px_luma_of(rgba, W3, 240, 99);
    int y_dome = px_luma_of(rgba, W3, 240, 120);
    int y_low = px_luma_of(rgba, W3, 240, 165);
    int v8a_ok = (best_blue_h >= 9.5 && best_blue_h <= 12.5) &&
                 (blue_peak_dx >= 0.33 && blue_peak_dx <= 1.50) &&
                 (best_grey_h >= 9.5 && best_grey_h <= 12.5) &&
                 (grey_peak_dx >= 0.33 && grey_peak_dx <= 1.50) &&
                 (h55_b >= 5.6 && h55_b <= 6.4) && (h55_g >= 5.6 && h55_g <= 6.4) &&
                 split_ok && zero_blue_ok &&
                 (y_dome < y_low) && (y_rim > y_dome) && (y_rim > y_sh) && (y_hair < y_rim);
    printf("  [%s] V8-A slider dark 2D ROI: bluePeak=%.2f@%.3f greyPeak=%.2f@%.3f h55=(%.2f,%.2f) 5pt=(%d,%d,%d,%d,%d)\n",
           v8a_ok ? "PASS" : "FAIL", best_blue_h, blue_peak_dx, best_grey_h, grey_peak_dx, h55_b, h55_g, y_hair, y_rim, y_sh, y_dome, y_low);
    ok &= v8a_ok;
  }

  // V8-B: Slider vertical monotonicity (zero folding)
  {
    int n = 0;
    for (int si = 0; si < 38; si++) {
      push_inst(inst, &n, 45.0, 31.0 + si, 70.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, (double)si / 37.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    }
    push_inst(inst, &n, 51.0, 31.0, 58.0, 38.0, 19.0, 6.0, 1.0, -16.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 0.0, 0.0, 0.0, S3, W3, H3, rgba)) return 0;
    int mono_ok = 1, prev_g = -1;
    for (int y = 138; y <= 162; y++) {
      int g = rgba[((size_t)y * W3 + 159) * 4 + 1];
      if (prev_g >= 0 && g < prev_g - 2) mono_ok = 0;
      prev_g = g;
    }
    printf("  [%s] V8-B slider vertical monotonicity (zero folding)\n", mono_ok ? "PASS" : "FAIL");
    ok &= mono_ok;
  }

  // V8-C & V8-D: Light Slider full blue/grey track + 5pt 3D thickness + 124x48 dropRole=0 + role0 zoom=1.18
  {
    int n = 0;
    push_inst(inst, &n, 10.0, 47.0, 70.0, 6.0, 3.0, 0.0, 0.0, 0.0, 0.0, 136.0/255.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 80.0, 47.0, 70.0, 6.0, 3.0, 0.0, 0.0, 0.0, 115.0/255.0, 115.0/255.0, 122.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 51.0, 31.0, 58.0, 38.0, 19.0, 6.0, 1.0, -16.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 1.0, 1.0, 1.0, S3, W3, H3, rgba)) return 0;
    double l_best_b = 0.0, l_best_g = 0.0, l_dx_b = -1.0, l_dx_g = -1.0;
    for (int i = 0; i <= 18; i++) {
      int xb = 153 + i, xg = 326 - i;
      double dx_pt = (i + 0.5) / S3;
      int cntB = 0, cntG = 0;
      for (int y = 93; y < 207; y++) {
        size_t ib = ((size_t)y * W3 + (size_t)xb) * 4;
        if ((int)rgba[ib + 2] - (int)rgba[ib] > 100) cntB++;
        int lg = px_luma_of(rgba, W3, xg, y);
        if (lg > 40 && lg < 175) cntG++;
      }
      if (cntB / S3 > l_best_b) { l_best_b = cntB / S3; l_dx_b = dx_pt; }
      if (cntG / S3 > l_best_g) { l_best_g = cntG / S3; l_dx_g = dx_pt; }
    }
    int lc55_b = 0, lc55_g = 0;
    for (int y = 93; y < 207; y++) {
      size_t i55 = ((size_t)y * W3 + 169) * 4;
      if ((int)rgba[i55 + 2] - (int)rgba[i55] > 100) lc55_b++;
      int lg55 = px_luma_of(rgba, W3, 310, y);
      if (lg55 > 40 && lg55 < 175) lc55_g++;
    }
    double lh55_b = lc55_b / S3, lh55_g = lc55_g / S3;
    size_t lil = (150 * W3 + 237) * 4, lir = (150 * W3 + 243) * 4;
    size_t lit = (105 * W3 + 156) * 4, lib = (195 * W3 + 156) * 4;
    int l_split_ok = ((int)rgba[lil + 2] - (int)rgba[lil] > 100) && ((int)rgba[lir + 2] - (int)rgba[lir] < 25);
    int l_zero_ok = ((int)rgba[lit + 2] - (int)rgba[lit] < 25) && ((int)rgba[lib + 2] - (int)rgba[lib] < 25);
    int y_hair = px_luma_of(rgba, W3, 240, 93), y_rim = px_luma_of(rgba, W3, 240, 95);
    int y_sh = px_luma_of(rgba, W3, 240, 99), y_dome = px_luma_of(rgba, W3, 240, 120), y_low = px_luma_of(rgba, W3, 240, 165);
    int v8c_ok = (l_best_b >= 9.5 && l_best_b <= 12.5) && (l_dx_b >= 0.33 && l_dx_b <= 1.50) &&
                 (l_best_g >= 9.5 && l_best_g <= 12.5) && (l_dx_g >= 0.33 && l_dx_g <= 1.50) &&
                 (lh55_b >= 5.6 && lh55_b <= 6.4) && (lh55_g >= 5.6 && lh55_g <= 6.4) &&
                 l_split_ok && l_zero_ok &&
                 (y_hair < y_low - 15) && (y_rim > y_hair) && (y_sh < y_low) && (y_dome < y_low);
    printf("  [%s] V8-C slider light 2D ROI: bluePeak=%.2f@%.3f greyPeak=%.2f@%.3f h55=(%.2f,%.2f) 5pt=(%d,%d,%d,%d,%d)\n",
           v8c_ok ? "PASS" : "FAIL", l_best_b, l_dx_b, l_best_g, l_dx_g, lh55_b, lh55_g, y_hair, y_rim, y_sh, y_dome, y_low);
    ok &= v8c_ok;

    n = 0;
    push_inst(inst, &n, 10.0, 47.0, 140.0, 6.0, 3.0, 0.0, 0.0, 0.0, 0.0, 136.0/255.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 18.0, 26.0, 124.0, 48.0, 24.0, 6.0, 1.0, 0.0, 1.0, 1.0, 1.0, 0.0, 1.0, 8.0, 0.0, 1.0, -1.0, 3.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 28.0/255.0, 28.0/255.0, 30.0/255.0, S3, W3, H3, rgba)) return 0;
    int cnt = 0;
    for (int y = 78; y < 222; y++) {
      size_t i = ((size_t)y * W3 + 57) * 4;
      if ((int)rgba[i + 2] - (int)rgba[i] > 100) cnt++;
    }
    double h_mag = cnt / S3;
    n = 0;
    push_inst(inst, &n, 40.0, 31.8, 80.0, 1.0, 0.0, 0.0, 0.0, 0.0, 48.0/255.0, 209.0/255.0, 88.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 40.0, 30.0, 80.0, 40.0, 20.0, 6.0, 1.0, -0.18, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 28.0/255.0, 28.0/255.0, 30.0/255.0, S3, W3, H3, rgba)) return 0;
    size_t ie = (90 * W3 + 240) * 4, im = (150 * W3 + 240) * 4;
    int edge_gr = (int)rgba[ie + 1] - (int)rgba[ie];
    int mid_gr = (int)rgba[im + 1] - (int)rgba[im];
    int v8d_ok = (h_mag < 7.0) && (edge_gr > 40) && (mid_gr < 15);
    printf("  [%s] V8-D role0 magnifier h=%.2f (<7.0) & zoom=1.18 edgeGR=%d midGR=%d\n", v8d_ok ? "PASS" : "FAIL", h_mag, edge_gr, mid_gr);
    ok &= v8d_ok;
  }

  // V9-A: Switch ON 3x 2D ROI (exact 3-row TIR + pinched core + moat from y_top+2 + right outer tip)
  {
    int n = 0;
    push_inst(inst, &n, 48.5, 35.2, 63.0, 28.0, 14.0, 0.0, 0.0, 0.0, 48.0/255.0, 209.0/255.0, 88.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 61.4, 30.0, 59.2, 38.4, 19.2, 6.0, 1.0, -32.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 28.0/255.0, 28.0/255.0, 30.0/255.0, S3, W3, H3, rgba)) return 0;
    size_t i0 = (90 * W3 + 273) * 4, i1 = (91 * W3 + 273) * 4, i2 = (92 * W3 + 273) * 4;
    int gr0 = (int)rgba[i0 + 1] - (int)rgba[i0];
    int gr1 = (int)rgba[i1 + 1] - (int)rgba[i1];
    int gr2 = (int)rgba[i2 + 1] - (int)rgba[i2];
    int core_top = -1, core_bot = -1;
    for (int y = 96; y < 199; y++) {
      size_t iy = ((size_t)y * W3 + 273) * 4;
      if ((int)rgba[iy + 1] - (int)rgba[iy] > 60) {
        if (core_top < 0) core_top = y;
        core_bot = y;
      }
    }
    double core_h = (core_bot - core_top + 1) / S3;
    double moat_pt = (core_top - 92) / S3;
    size_t itip = (148 * W3 + 360) * 4;
    int gr_tip = (int)rgba[itip + 1] - (int)rgba[itip];
    int v9a_ok = (gr0 < 15) && (gr1 > 60) && (gr2 < 15) && (core_h >= 20.0 && core_h <= 22.5) && (moat_pt >= 6.5) && (gr_tip < 15);
    printf("  [%s] V9-A switch ON 3x ROI: rows=(%d,%d,%d) core=%.2f moat=%.2f tipGR=%d\n",
           v9a_ok ? "PASS" : "FAIL", gr0, gr1, gr2, core_h, moat_pt, gr_tip);
    ok &= v9a_ok;
  }

  // V9-B: Switch ON 2x phase-shifted green mass
  {
    const double phases[2] = { 30.0, 30.25 };
    int v9b_ok = 1;
    for (int pi = 0; pi < 2; pi++) {
      int n = 0;
      push_inst(inst, &n, 48.5, phases[pi] + 5.2, 63.0, 28.0, 14.0, 0.0, 0.0, 0.0, 48.0/255.0, 209.0/255.0, 88.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
      push_inst(inst, &n, 61.4, phases[pi], 59.2, 38.4, 19.2, 6.0, 1.0, -32.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
      if (!render_rgba(inst, n, 28.0/255.0, 28.0/255.0, 30.0/255.0, 2.0, 320, 200, rgba)) return 0;
      int mass = 0;
      for (int y = 60; y <= 64; y++) {
        size_t iy = ((size_t)y * 320 + 182) * 4;
        if (rgba[iy + 1] > rgba[iy]) mass += (int)rgba[iy + 1] - (int)rgba[iy];
      }
      if (mass <= 40) v9b_ok = 0;
    }
    printf("  [%s] V9-B switch ON 2x phase-shifted integrated green mass\n", v9b_ok ? "PASS" : "FAIL");
    ok &= v9b_ok;
  }

  // V9-C: Switch OFF luminance contrast, rim profile & right end-cap inner row (dropDisp=0)
  {
    int n = 0;
    push_inst(inst, &n, 48.5, 35.2, 63.0, 28.0, 14.0, 0.0, 0.0, 0.0, 120.0/255.0, 120.0/255.0, 128.0/255.0, 173.0/255.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 39.4, 30.0, 59.2, 38.4, 19.2, 6.0, 1.0, -32.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 28.0/255.0, 28.0/255.0, 30.0/255.0, S3, W3, H3, rgba)) return 0;
    int yc = px_luma_of(rgba, W3, 10, 10), ycore = px_luma_of(rgba, W3, 207, 148);
    int yrim = px_luma_of(rgba, W3, 207, 92), ymoat = px_luma_of(rgba, W3, 207, 102);
    int rc1 = px_luma_of(rgba, W3, 291, 148), rc2 = px_luma_of(rgba, W3, 288, 148), rc3 = px_luma_of(rgba, W3, 285, 148);
    int v9c_ok = (ycore > yc + 15) && (yrim > ymoat) && (rc1 > yc + 10) && (rc2 > yc + 10) && (rc3 > yc + 10);
    printf("  [%s] V9-C switch OFF luminance & right cap: card=%d core=%d rim=%d moat=%d rcap=(%d,%d,%d)\n",
           v9c_ok ? "PASS" : "FAIL", yc, ycore, yrim, ymoat, rc1, rc2, rc3);
    ok &= v9c_ok;
  }

  // V9-D: Light Segmented Prism (45pt dropRole=3 over white card) + interior text ROI zero dispersion delta
  {
    int n = 0;
    push_inst(inst, &n, 25.0, 27.0, 110.0, 45.0, 22.5, 6.0, 1.0, -48.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.04, 0.0);
    if (!render_rgba(inst, n, 1.0, 1.0, 1.0, S3, W3, H3, rgba)) return 0;
    size_t itop = (84 * W3 + 240) * 4, ibot = (213 * W3 + 240) * 4, icap = (148 * W3 + 78) * 4;
    int cyan_top = (int)rgba[itop + 2] - (int)rgba[itop];
    int cyan = (int)rgba[ibot + 2] - (int)rgba[ibot];
    int amber = (int)rgba[icap] - (int)rgba[icap + 2];
    static uint8_t rgba_c0[W3 * H3 * 4];
    n = 0;
    push_inst(inst, &n, 72.0, 45.0, 16.0, 9.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 25.0, 27.0, 110.0, 45.0, 22.5, 6.0, 1.0, -48.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.04, 0.0);
    if (!render_rgba(inst, n, 1.0, 1.0, 1.0, S3, W3, H3, rgba)) return 0;
    n = 0;
    push_inst(inst, &n, 72.0, 45.0, 16.0, 9.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 25.0, 27.0, 110.0, 45.0, 22.5, 6.0, 1.0, -48.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 1.0, 1.0, 1.0, S3, W3, H3, rgba_c0)) return 0;
    int max_txt_diff = 0;
    for (int ty = 132; ty <= 165; ty++) {
      for (int tx = 210; tx <= 270; tx++) {
        size_t k = ((size_t)ty * W3 + (size_t)tx) * 4;
        for (int c = 0; c < 3; c++) {
          int d = abs((int)rgba[k + c] - (int)rgba_c0[k + c]);
          if (d > max_txt_diff) max_txt_diff = d;
        }
      }
    }
    int v9d_ok = (cyan_top >= 10) && (cyan >= 10) && (amber >= 20) && (max_txt_diff == 0);
    // Role 3 Segmented text-scale (12pt stroke -> 35..37px@3x) & outer track pinch (8.0pt white ring vs 11.5pt gray track)
    n = 0;
    push_inst(inst, &n, 15.0, 32.5, 130.0, 34.0, 17.0, 0.0, 0.0, 0.0, 195.0/255.0, 195.0/255.0, 198.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 78.0, 43.5, 4.0, 12.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 25.0, 27.0, 110.0, 45.0, 22.5, 6.0, 1.0, -48.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.04, 0.0);
    if (!render_rgba(inst, n, 1.0, 1.0, 1.0, S3, W3, H3, rgba)) return 0;
    int dark_span = 0;
    for (int sy = 110; sy <= 190; sy++) {
      if (px_luma_of(rgba, W3, 240, sy) < 120) dark_span++;
    }
    int l_ring = px_luma_of(rgba, W3, 240, 105);
    int l_track = px_luma_of(rgba, W3, 240, 116);
    int seg_scale_ok = (dark_span >= 35 && dark_span <= 37) && (l_ring >= 225 && l_track <= 210 && l_ring - l_track >= 20);
    v9d_ok &= seg_scale_ok;
    printf("  [%s] V9-D light segmented prism & scale: top(B-R)=%d bot(B-R)=%d cap(R-B)=%d textDiff=%d strokeSpan=%d(35..37) ring8=%d track11.5=%d\n",
           v9d_ok ? "PASS" : "FAIL", cyan_top, cyan, amber, max_txt_diff, dark_span, l_ring, l_track);
    ok &= v9d_ok;
  }

  // V9-Tab: Role 4 Tab monotonic outer meniscus (no fake badge copy from 6.7pt inward; continuous outer lip refraction)
  {
    static uint8_t rgba_empty[W3 * H3 * 4];
    static uint8_t rgba_lip[W3 * H3 * 4];
    int n = 0;
    push_inst(inst, &n, 10.0, 28.0, 140.0, 44.0, 22.0, 0.0, 0.0, 0.0, 42.0/255.0, 42.0/255.0, 48.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 30.0, 25.0, 88.0, 50.0, 25.0, 6.0, 1.0, -64.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 16.0/255.0, 16.0/255.0, 20.0/255.0, S3, W3, H3, rgba_empty)) return 0;
    n = 0;
    push_inst(inst, &n, 10.0, 28.0, 140.0, 44.0, 22.0, 0.0, 0.0, 0.0, 42.0/255.0, 42.0/255.0, 48.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 109.0, 28.0, 2.0, 44.0, 1.0, 0.0, 0.0, 0.0, 1.0, 149.0/255.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 30.0, 25.0, 88.0, 50.0, 25.0, 6.0, 1.0, -64.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 16.0/255.0, 16.0/255.0, 20.0/255.0, S3, W3, H3, rgba)) return 0;
    n = 0;
    push_inst(inst, &n, 10.0, 28.0, 140.0, 44.0, 22.0, 0.0, 0.0, 0.0, 42.0/255.0, 42.0/255.0, 48.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 112.5, 45.0, 2.0, 10.0, 0.0, 0.0, 0.0, 0.0, 1.0, 149.0/255.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 30.0, 25.0, 88.0, 50.0, 25.0, 6.0, 1.0, -64.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 16.0/255.0, 16.0/255.0, 20.0/255.0, S3, W3, H3, rgba_lip)) return 0;
    size_t k_cap = ((size_t)150 * W3 + 349) * 4;
    int ghost_delta = abs((int)rgba[k_cap] - (int)rgba_empty[k_cap]) + abs((int)rgba[k_cap+1] - (int)rgba_empty[k_cap+1]) + abs((int)rgba[k_cap+2] - (int)rgba_empty[k_cap+2]);
    int lip_delta = abs((int)rgba_lip[k_cap] - (int)rgba_empty[k_cap]) + abs((int)rgba_lip[k_cap+1] - (int)rgba_empty[k_cap+1]) + abs((int)rgba_lip[k_cap+2] - (int)rgba_empty[k_cap+2]);
    size_t k_g0x = ((size_t)87 * W3 + 349) * 4;
    int gate0_same_x = abs((int)rgba[k_g0x] - (int)rgba_empty[k_g0x]) + abs((int)rgba[k_g0x+1] - (int)rgba_empty[k_g0x+1]) + abs((int)rgba[k_g0x+2] - (int)rgba_empty[k_g0x+2]);
    size_t k_g0c = ((size_t)87 * W3 + 311) * 4;
    int gate0_in_cap = abs((int)rgba[k_g0c] - (int)rgba_empty[k_g0c]) + abs((int)rgba[k_g0c+1] - (int)rgba_empty[k_g0c+1]) + abs((int)rgba[k_g0c+2] - (int)rgba_empty[k_g0c+2]);
    int v9tab_ok = (ghost_delta == 0) && (lip_delta >= 40) && (gate0_same_x == 0) && (gate0_in_cap == 0);
    printf("  [%s] V9-Tab Role 4 monotonic outer meniscus: ghostDelta=%d (==0) lipDelta=%d (>=40) gate0=(%d,%d) (==0)\n", v9tab_ok ? "PASS" : "FAIL", ghost_delta, lip_delta, gate0_same_x, gate0_in_cap);
    ok &= v9tab_ok;
  }

  // V9-Tab-No-Ghost: Role 4 zero interior double-fold / ghosting at inward >= 5.5pt
  {
    int n = 0;
    push_inst(inst, &n, 41.0, 28.0, 2.0, 44.0, 0.0, 0.0, 0.0, 0.0, 90.0/255.0, 90.0/255.0, 90.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 32.0, 18.0, 95.0, 63.5, 31.75, 6.0, 1.0, -64.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 246.0/255.0, 244.0/255.0, 240.0/255.0, S3, W3, H3, rgba)) return 0;
    int l_bar = px_luma_of(rgba, W3, 126, 149);
    int l_clean = px_luma_of(rgba, W3, 114, 149);
    int ng_ok = (l_bar < 130) && (l_clean > 220);
    printf("  [%s] V9-Tab-No-Ghost Role 4 monotonic: barL@10pt=%d (<130) cleanL@6pt=%d (>220)\n", ng_ok ? "PASS" : "FAIL", l_bar, l_clean);
    ok &= ng_ok;
  }

  // V9-E: Switch half-press p = 0.5 / 0.6 / 1.0 (no pinchWeight collapse at p=0.5)
  {
    const double ps[3] = { 0.5, 0.6, 1.0 };
    double hs[3] = { 0.0, 0.0, 0.0 };
    for (int pi = 0; pi < 3; pi++) {
      double p = ps[pi];
      int n = 0;
      push_inst(inst, &n, 48.5, 35.2, 63.0, 28.0, 14.0, 0.0, 0.0, 0.0, 48.0/255.0, 209.0/255.0, 88.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
      push_inst(inst, &n, 61.4, 30.0, 59.2, 38.4, 19.2, 6.0, 1.0, -32.0, 1.0, 1.0, 1.0, 0.0, p, 9.0 * p, 0.0, 1.0, -1.0, 9.0 * p, 0.0, 0.0);
      if (!render_rgba(inst, n, 28.0/255.0, 28.0/255.0, 30.0/255.0, S3, W3, H3, rgba)) return 0;
      int top = -1, bot = -1;
      for (int y = 96; y < 199; y++) {
        size_t iy = ((size_t)y * W3 + 273) * 4;
        if ((int)rgba[iy + 1] - (int)rgba[iy] > 60) {
          if (top < 0) top = y;
          bot = y;
        }
      }
      hs[pi] = (bot - top + 1) / S3;
    }
    int v9e_ok = (hs[0] < 27.0) && (hs[2] < hs[1]) && (hs[1] <= hs[0]);
    printf("  [%s] V9-E switch half-press pinch: h(0.5)=%.2f h(0.6)=%.2f h(1.0)=%.2f\n", v9e_ok ? "PASS" : "FAIL", hs[0], hs[1], hs[2]);
    ok &= v9e_ok;
  }

  // V10: Regular Liquid Glass Light AA & Dark Bit-Exact Gate (@2x, R=24pt)
  {
    auto luma_f = [](const uint8_t *buf, int stride, int x, int y) -> double {
      size_t i = ((size_t)y * stride + (size_t)x) * 4;
      return 0.2126 * buf[i] + 0.7152 * buf[i + 1] + 0.0722 * buf[i + 2];
    };
    int n = 0;
    push_inst(inst, &n, 20.0, 20.0, 120.0, 80.0, 24.0, 6.0, 1.0, 2.1,
              253.0/255.0, 252.0/255.0, 253.0/255.0, 103.0/255.0,
              0.6, 14.0, 0.0, 1.0, -1.0, 18.0, 0.06, 0.0);
    if (!render_rgba(inst, n, 242.0/255.0, 240.0/255.0, 236.0/255.0, 2.0, 320, 240, rgba)) return 0;
    double masses[64];
    int nm = 0;
    const double bg_l = 0.2126 * 242.0 + 0.7152 * 240.0 + 0.0722 * 236.0;
    for (int py = 44; py <= 84; py++) {
      double mass = 0.0;
      for (int px = 200; px <= 284; px++) {
        double l = luma_f(rgba, 320, px, py);
        if (l < bg_l) mass += (bg_l - l);
      }
      masses[nm++] = mass / 2.0;
    }
    double max_d2 = 0.0;
    for (int k = 1; k < nm - 1; k++) {
      double d2 = fabs(masses[k - 1] - 2.0 * masses[k] + masses[k + 1]);
      if (d2 > max_d2) max_d2 = d2;
    }
    double side_min = 1e9;
    for (int sx = 270; sx <= 284; sx++) {
      double l = luma_f(rgba, 320, sx, 120);
      if (l < side_min) side_min = l;
    }
    double light_outer_top = luma_f(rgba, 320, 160, 39);
    double light_top = luma_f(rgba, 320, 160, 40);
    double light_mid = luma_f(rgba, 320, 160, 120);
    n = 0;
    push_inst(inst, &n, 20.0, 20.0, 120.0, 80.0, 24.0, 6.0, 1.0, 2.1,
              32.0/255.0, 32.0/255.0, 32.0/255.0, 103.0/255.0,
              0.6, 14.0, 0.0, 1.0, -1.0, 18.0, 0.06, 0.0);
    if (!render_rgba(inst, n, 28.0/255.0, 28.0/255.0, 30.0/255.0, 2.0, 320, 240, rgba)) return 0;
    double yd_mid = luma_f(rgba, 320, 160, 120), yd_top = luma_f(rgba, 320, 160, 40);
    uint32_t h1 = 2166136261u, h2 = 5381u;
    for (int ry = 38; ry <= 122; ry++) {
      for (int rx = 160; rx <= 283; rx++) {
        size_t idx = ((size_t)ry * 320 + (size_t)rx) * 4;
        for (int ch = 0; ch < 4; ch++) {
          uint32_t b = rgba[idx + ch];
          h1 = (h1 ^ b) * 16777619u;
          h2 = (h2 * 33u) ^ b;
        }
      }
    }
    int v10_hash_ok = (h1 == 2732478156u && h2 == 1261726368u);
    int v10_ok = (max_d2 <= 14.0) && (side_min >= 150.0 && side_min <= 176.0) &&
                 (light_outer_top <= light_mid - 15.0) && (light_top >= light_mid) &&
                 (yd_top > yd_mid + 5.0) && v10_hash_ok;
    printf("  [%s] V10 regular glass AA & dark ROI RGBA8 exact: light_d2=%.2f (<=14) side_min=%.1f ([150,176]) outer_top=%.1f top=%.1f mid=%.1f dark_top=%.1f>mid=%.1f roi_hash=(%u,%u)\n",
           v10_ok ? "PASS" : "FAIL", max_d2, side_min, light_outer_top, light_top, light_mid, yd_top, yd_mid, h1, h2);
    ok &= v10_ok;

    // V10-Dark-On-Bright: Dark Regular glass on #F2F3F0 bright backdrop has no dead-black contour (>= 120/255) and bright top rim.
    n = 0;
    push_inst(inst, &n, 20.0, 20.0, 120.0, 80.0, 24.0, 6.0, 1.0, 0.0,
              32.0/255.0, 32.0/255.0, 32.0/255.0, 103.0/255.0,
              0.6, 14.0, 0.0, 1.0, -72.0, 20.0, 0.0, 0.0);
    if (!render_rgba(inst, n, 242.0/255.0, 243.0/255.0, 240.0/255.0, 2.0, 320, 240, rgba)) return 0;
    size_t k_db_side = ((size_t)120 * 320 + 280) * 4;
    int db_out_min = rgba[k_db_side] < rgba[k_db_side + 1] ? rgba[k_db_side] : rgba[k_db_side + 1];
    if (rgba[k_db_side + 2] < db_out_min) db_out_min = rgba[k_db_side + 2];
    double db_l_mid = luma_f(rgba, 320, 160, 120);
    double db_l_top = luma_f(rgba, 320, 160, 40);
    int v10_dob_ok = (db_out_min >= 120) && (db_l_top > db_l_mid + 20.0);
    printf("  [%s] V10-Dark-On-Bright (#F2F3F0 @2x): out_min=%d (>=120) top=%.1f mid=%.1f\n",
           v10_dob_ok ? "PASS" : "FAIL", db_out_min, db_l_top, db_l_mid);
    ok &= v10_dob_ok;
  }

  // V11: 3D Slim Bevel Wall & Curved-Cap AA Gate (V11-AA, V11-Wall, V11-Top)
  {
    auto luma_f = [](const uint8_t *buf, int stride, int x, int y) -> double {
      size_t i = ((size_t)y * stride + (size_t)x) * 4;
      return 0.2126 * buf[i] + 0.7152 * buf[i + 1] + 0.0722 * buf[i + 2];
    };
    auto arc_d2 = [&](const uint8_t *buf, int stride, int x0, int x1, int y0, int y1) -> double {
      double mins[128];
      int nm = 0;
      for (int py = y0; py <= y1 && nm < 128; py++) {
        double rm = 1e9;
        for (int px = x0; px <= x1; px++) {
          double l = luma_f(buf, stride, px, py);
          if (l < rm) rm = l;
        }
        mins[nm++] = rm;
      }
      double max_d2 = 0.0;
      for (int k = 1; k < nm - 1; k++) {
        double d2 = fabs(mins[k - 1] - 2.0 * mins[k] + mins[k + 1]);
        if (d2 > max_d2) max_d2 = d2;
      }
      return max_d2;
    };
    int v11_ok = 1;
    for (int dark = 1; dark >= 0; dark--) {
      double bg_r = dark ? 28.0/255.0 : 1.0, bg_g = dark ? 28.0/255.0 : 1.0, bg_b = dark ? 30.0/255.0 : 1.0;
      double tr_r = dark ? 48.0/255.0 : 52.0/255.0, tr_g = dark ? 209.0/255.0 : 199.0/255.0, tr_b = dark ? 88.0/255.0 : 89.0/255.0;
      int n = 0;
      push_inst(inst, &n, 18.5, 18.0, 63.0, 28.0, 14.0, 0.0, 0.0, 0.0, tr_r, tr_g, tr_b, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
      push_inst(inst, &n, 31.58, 12.8, 59.2, 38.4, 19.2, 6.0, 1.0, -32.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
      if (!render_rgba(inst, n, bg_r, bg_g, bg_b, 3.0, 360, 195, rgba)) return 0;
      double d2_l3 = arc_d2(rgba, 360, 89, 109, 66, 125);
      double d2_r3 = arc_d2(rgba, 360, 259, 281, 66, 125);
      double lim3 = dark ? 10.0 : 12.0;
      int r_deep_x = 272;
      double r_deep_l = 1e9;
      for (int sx = 266; sx <= 274; sx++) {
        double l = luma_f(rgba, 360, sx, 96);
        if (l < r_deep_l) { r_deep_l = l; r_deep_x = sx; }
      }
      size_t ird = ((size_t)96 * 360 + (size_t)r_deep_x) * 4;
      int r_gr = (int)rgba[ird + 1] - (int)rgba[ird];
      double lin1 = luma_f(rgba, 360, r_deep_x - 1, 96);
      double lin2 = luma_f(rgba, 360, r_deep_x - 2, 96);
      double lin3 = luma_f(rgba, 360, r_deep_x - 3, 96);
      double lin4 = luma_f(rgba, 360, r_deep_x - 4, 96);
      int wall_ok = dark
        ? (r_deep_l <= 14.0 && r_gr <= 12 && lin1 >= r_deep_l && lin2 >= lin1 && lin3 >= lin2)
        : (r_deep_l >= 170.0 && r_deep_l <= 205.0 && lin1 >= r_deep_l && lin2 >= lin1 && lin3 >= lin2 && lin4 >= lin3 && lin4 >= 230.0);
      int horn_ok = 1;
      double prev_horn = 0.0;
      const int dxs[8] = { 6, 8, 10, 12, 14, 16, 18, 20 };
      for (int di = 0; di < 8; di++) {
        double dx = dxs[di];
        int pc = (int)((31.58 + dx) * 3.0 + 0.5);
        double rem = (19.2 - dx > 0.0) ? (19.2 - dx) : 0.0;
        double dyc = sqrt(19.2 * 19.2 - rem * rem);
        int y0 = (int)((32.0 - dyc + 1.0) * 3.0 + 0.5);
        int best = 0, cur = 0;
        for (int py = y0; py < 96; py++) {
          size_t ih = ((size_t)py * 360 + (size_t)pc) * 4;
          if ((int)rgba[ih + 1] - (int)rgba[ih] < 26) { cur++; if (cur > best) best = cur; } else cur = 0;
        }
        double moat = best / 3.0;
        if (dxs[di] == 6 || dxs[di] == 8) { if (moat != 0.0) horn_ok = 0; }
        else if (dxs[di] == 10) { if (moat < 1.5 || moat > 4.5) horn_ok = 0; }
        else { if (moat < prev_horn - 0.35 || moat > prev_horn + 3.5) horn_ok = 0; if (dxs[di] == 20 && moat < 7.0) horn_ok = 0; }
        prev_horn = moat;
      }
      int center_ok = 1;
      if (dark) {
        double inst_g[40];
        int ng = 0;
        push_inst(inst_g, &ng, 31.58, 12.8, 59.2, 38.4, 19.2, 6.0, 1.0, -32.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
        if (!render_rgba(inst_g, ng, 48.0/255.0, 209.0/255.0, 88.0/255.0, 3.0, 360, 195, rgba)) return 0;
        size_t ic = (96 * 360 + 184) * 4;
        center_ok = (abs((int)rgba[ic] - 57) <= 2 && abs((int)rgba[ic+1] - 218) <= 2 && abs((int)rgba[ic+2] - 97) <= 2);
        printf("  [%s] V11 Switch ON solid green center: got=(%d,%d,%d) expected=(57,218,97)+/-2\n",
               center_ok ? "PASS" : "FAIL", rgba[ic], rgba[ic+1], rgba[ic+2]);
        if (!render_rgba(inst, n, bg_r, bg_g, bg_b, 3.0, 360, 195, rgba)) return 0;
      }
      int emerald_ok = 0;
      for (int lx = 93; lx <= 99; lx++) {
        size_t ilx = ((size_t)96 * 360 + (size_t)lx) * 4;
        int lr = rgba[ilx], lg = rgba[ilx + 1];
        if (lg - lr >= 90 && lg >= 125 && lg <= 165) emerald_ok = 1;
      }
      if (!render_rgba(inst, n, bg_r, bg_g, bg_b, 2.0, 240, 130, rgba)) return 0;
      double d2_l2 = arc_d2(rgba, 240, 58, 74, 44, 83);
      double d2_r2 = arc_d2(rgba, 240, 170, 188, 44, 83);
      double lim2 = dark ? 12.0 : 14.0;
      int sub_ok = (d2_l3 <= lim3 && d2_r3 <= lim3 && d2_l2 <= lim2 && d2_r2 <= lim2 && wall_ok && emerald_ok && horn_ok && center_ok);
      printf("  [%s] V11-AA/Wall/Horn (%s): 3x=(%.2f,%.2f) 2x=(%.2f,%.2f) rDeep=%.1f emerald=%d horn=%d\n",
             sub_ok ? "PASS" : "FAIL", dark ? "dark" : "light", d2_l3, d2_r3, d2_l2, d2_r2, r_deep_l, emerald_ok, horn_ok);
      v11_ok &= sub_ok;

      const double offs[2] = { 5.00, 5.20 };
      for (int oi = 0; oi < 2; oi++) {
        double off = offs[oi], th = 38.4 - 2.0 * off;
        n = 0;
        push_inst(inst, &n, 18.5, 21.0 + off, 63.0, th, th * 0.5, 0.0, 0.0, 0.0, tr_r, tr_g, tr_b, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        push_inst(inst, &n, 31.4, 21.0, 59.2, 38.4, 19.2, 6.0, 1.0, -32.0, 1.0, 1.0, 1.0, 0.0, 1.0, 9.0, 0.0, 1.0, -1.0, 9.0, 0.0, 0.0);
        if (!render_rgba(inst, n, bg_r, bg_g, bg_b, 3.0, 360, 210, rgba)) return 0;
        size_t p0 = (63 * 360 + 183) * 4, p1 = (64 * 360 + 183) * 4, p2 = (65 * 360 + 183) * 4;
        int gr0 = (int)rgba[p0 + 1] - (int)rgba[p0], gr1 = (int)rgba[p1 + 1] - (int)rgba[p1], gr2 = (int)rgba[p2 + 1] - (int)rgba[p2];
        double l0 = luma_f(rgba, 360, 183, 63), l2 = luma_f(rgba, 360, 183, 65);
        int top3_ok = dark
          ? (gr0 < 15 && l0 <= 35.0 && gr1 >= 100 && gr2 < 15 && l2 >= 75.0)
          : (gr0 < 15 && l0 >= 180.0 && l0 <= 215.0 && gr1 >= 95 && gr2 < 15 && l2 >= 245.0);
        if (!render_rgba(inst, n, bg_r, bg_g, bg_b, 2.0, 240, 140, rgba)) return 0;
        size_t q1 = (43 * 240 + 122) * 4, q2 = (44 * 240 + 122) * 4;
        int g21 = (int)rgba[q1 + 1] - (int)rgba[q1], g22 = (int)rgba[q2 + 1] - (int)rgba[q2];
        int top2_ok = (g21 >= 70 && g22 < 15);
        printf("  [%s] V11-Top (%s %.2fpt): 3x=(%+d/%+.0f, %+d, %+d/%+.0f) 2x=(%+d, %+d)\n",
               (top3_ok && top2_ok) ? "PASS" : "FAIL", dark ? "dark" : "light", off, gr0, l0, gr1, gr2, l2, g21, g22);
        v11_ok &= (top3_ok && top2_ok);
      }
    }
    // V11-Frost-Continuity: Switch ON continuous frost sweep (0.0 -> 0.0001 -> 0.00011 -> 0.01 -> 0.05 -> 0.10 -> 0.13 -> 0.30)
    {
      static uint8_t fb0[360 * 195 * 4], fb00011[360 * 195 * 4], fb10[360 * 195 * 4], fb127[360 * 195 * 4], fb128[360 * 195 * 4], fb13[360 * 195 * 4], fb30[360 * 195 * 4];
      auto draw_frost = [&](double f, uint8_t *dst) -> int {
        int active = f > 0.0001;
        double a_frac = active ? 0.94 * f : 0.0;
        double blur_slot = (a_frac <= 0.12) ? -32.0 : fmax(0.01, 14.0 * ((a_frac - 0.12) / 0.88) * 0.5);
        double refr = active ? 9.0 * (1.0 - 0.55 * f) : 9.0;
        double trgb = active ? 1.0 + (240.0 / 255.0 - 1.0) * (0.5 * f) : 1.0;
        double ta = floor(a_frac * 255.0 + 0.5) / 255.0;
        int nf = 0;
        push_inst(inst, &nf, 18.5, 18.0, 63.0, 28.0, 14.0, 0.0, 0.0, 0.0, 48.0/255.0, 209.0/255.0, 88.0/255.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        push_inst(inst, &nf, 31.58, 12.8, 59.2, 38.4, 19.2, 6.0, 1.0, blur_slot, trgb, trgb, trgb, ta, 1.0, 9.0, 0.0, 1.0, -1.0, refr, 0.0, 0.0);
        return render_rgba(inst, nf, 28.0/255.0, 28.0/255.0, 30.0/255.0, 3.0, 360, 195, dst);
      };
      auto roi_diff = [](const uint8_t *a, const uint8_t *b) -> int {
        int md = 0;
        for (int py = 38; py <= 154; py++) {
          for (int px = 95; px <= 272; px++) {
            size_t k = ((size_t)py * 360 + (size_t)px) * 4;
            for (int c = 0; c < 3; c++) {
              int d = abs((int)a[k + c] - (int)b[k + c]);
              if (d > md) md = d;
            }
          }
        }
        return md;
      };
      auto mean_l = [&](const uint8_t *buf, int x0, int x1, int y0, int y1) -> double {
        double s = 0.0;
        int cnt = 0;
        for (int py = y0; py <= y1; py++)
          for (int px = x0; px <= x1; px++) { s += luma_f(buf, 360, px, py); cnt++; }
        return s / (double)cnt;
      };
      if (!draw_frost(0.0, fb0)) return 0;
      if (!draw_frost(0.0001, rgba)) return 0;
      if (!draw_frost(0.00011, fb00011)) return 0;
      if (!draw_frost(0.01, rgba)) return 0;
      if (!draw_frost(0.05, rgba)) return 0;
      if (!draw_frost(0.10, fb10)) return 0;
      if (!draw_frost(0.127, fb127)) return 0;
      if (!draw_frost(0.128, fb128)) return 0;
      if (!draw_frost(0.13, fb13)) return 0;
      if (!draw_frost(0.30, fb30)) return 0;
      int d_eps = roi_diff(fb0, fb00011);
      int d_blur = roi_diff(fb127, fb128);
      int d_cross = roi_diff(fb10, fb13);
      double ml10 = mean_l(fb10, 95, 272, 38, 154), ml13 = mean_l(fb13, 95, 272, 38, 154);
      double ml0_roi = mean_l(fb0, 95, 272, 38, 154), ml30_roi = mean_l(fb30, 95, 272, 38, 154);
      int frost_ok = (d_eps <= 2) && (d_blur <= 8) && (d_cross <= 90) && (fabs(ml13 - ml10) < 15.0) && (ml30_roi > ml0_roi + 15.0);
      printf("  [%s] V11-Frost continuity: d_eps(0->0.00011)=%d (<=2) d_blur(0.127->0.128)=%d (<=8) d_cross(0.10->0.13)=%d (<=90, dMeanL=%.1f) fog30=%.1f>%.1f+15\n",
             frost_ok ? "PASS" : "FAIL", d_eps, d_blur, d_cross, fabs(ml13 - ml10), ml30_roi, ml0_roi);
      v11_ok &= frost_ok;
    }
    ok &= v11_ok;

    // V12-Unified-Overlay-And-Hole: v9/v11d unified overlay (0x80 light/dark, 0xFF uniformity, monotonicity)
    // and kind == 6 liquid glass over kind == 9 native-view hole translucent premultiplied alpha.
    {
      static uint8_t pbuf[320 * 240 * 4];
      int np = 0;
      // 1. Light 0x80 unified panel over sky blue (211, 232, 253): preserves blue tint & bright rim
      push_inst(inst, &np, 20.0, 20.0, 120.0, 80.0, 24.0, 6.0, 1.0, 14.0,
                253.0/255.0, 252.0/255.0, 253.0/255.0, 128.0/255.0,
                0.6, 20.0, 0.0, 1.0, -1.0, 16.0, 0.0, 0.0);
      if (!render_rgba(inst, np, 211.0/255.0, 232.0/255.0, 253.0/255.0, 2.0, 320, 240, pbuf)) return 0;
      size_t kc = (88 * 320 + 160) * 4;
      int center_chroma = (int)pbuf[kc + 2] - (int)pbuf[kc];
      double top_l = luma_f(pbuf, 320, 160, 40);
      double center_l = luma_f(pbuf, 320, 160, 88);
      int bf_ok = (center_chroma >= 12) && ((top_l - center_l) >= 4.0);

      // 2. Opaque 0xFF uniformity: |top - center| <= 3
      np = 0;
      push_inst(inst, &np, 20.0, 20.0, 120.0, 80.0, 24.0, 6.0, 1.0, 14.0,
                253.0/255.0, 252.0/255.0, 253.0/255.0, 1.0,
                0.6, 20.0, 0.0, 1.0, -1.0, 16.0, 0.0, 0.0);
      if (!render_rgba(inst, np, 211.0/255.0, 232.0/255.0, 253.0/255.0, 2.0, 320, 240, pbuf)) return 0;
      double top_ff = luma_f(pbuf, 320, 160, 40);
      double center_ff = luma_f(pbuf, 320, 160, 88);
      int ff_ok = (fabs(top_ff - center_ff) <= 3.0);

      // 3. Light monotonicity over gray 0.60 vs 0.72
      np = 0;
      push_inst(inst, &np, 20.0, 20.0, 120.0, 80.0, 24.0, 6.0, 1.0, 14.0,
                253.0/255.0, 252.0/255.0, 253.0/255.0, 128.0/255.0,
                0.6, 20.0, 0.0, 1.0, -1.0, 16.0, 0.0, 0.0);
      if (!render_rgba(inst, np, 0.60, 0.60, 0.60, 2.0, 320, 240, pbuf)) return 0;
      double l_g60 = luma_f(pbuf, 320, 160, 88);
      if (!render_rgba(inst, np, 0.72, 0.72, 0.72, 2.0, 320, 240, pbuf)) return 0;
      double l_g72 = luma_f(pbuf, 320, 160, 88);
      int mono_ok = (l_g72 >= l_g60 + 2.0);

      // 4. Kind == 6 Light liquid_panel_glass (0xFDFCFD80) over kind == 9 native-view hole:
      // translucent premultiplied output (rgb <= a, a in 132..156)
      np = 0;
      push_inst(inst, &np, 0.0, 0.0, 160.0, 120.0, 0.0, 9.0, 0.0, 0.0,
                0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 8388608.0 + 7.0, 0.0, 0.0, 0.0);
      push_inst(inst, &np, 20.0, 20.0, 120.0, 80.0, 24.0, 6.0, 1.0, 14.0,
                253.0/255.0, 252.0/255.0, 253.0/255.0, 128.0/255.0,
                0.6, 20.0, 0.0, 1.0, -390.0, 16.0, 0.0, 0.0);
      if (!render_rgba(inst, np, 0.0, 0.0, 0.0, 2.0, 320, 240, pbuf)) return 0;
      size_t kh = (120 * 320 + 160) * 4;
      int lr = pbuf[kh], lg = pbuf[kh + 1], lb = pbuf[kh + 2], la = pbuf[kh + 3];
      int hole_light_ok = (la >= 132 && la <= 156 && lr <= la && lg <= la && lb <= la);

      // 5. Kind == 6 Dark liquid_panel_glass (0x20202080) over kind == 9 hole:
      // translucent premultiplied (a in 180..205, over-white <= 105)
      np = 0;
      push_inst(inst, &np, 0.0, 0.0, 160.0, 120.0, 0.0, 9.0, 0.0, 0.0,
                0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 8388608.0 + 7.0, 0.0, 0.0, 0.0);
      push_inst(inst, &np, 20.0, 20.0, 120.0, 80.0, 24.0, 6.0, 1.0, 14.0,
                32.0/255.0, 32.0/255.0, 32.0/255.0, 128.0/255.0,
                0.6, 20.0, 0.0, 1.0, -391.0, 16.0, 0.0, 0.0);
      if (!render_rgba(inst, np, 0.0, 0.0, 0.0, 2.0, 320, 240, pbuf)) return 0;
      int dr = pbuf[kh], dg = pbuf[kh + 1], db = pbuf[kh + 2], da = pbuf[kh + 3];
      int over_w = dr + (255 * (255 - da) + 127) / 255;
      int hole_dark_ok = (da >= 180 && da <= 205 && dr <= da && dg <= da && db <= da && over_w <= 105);

      // 6. Opaque canvas (no kind == 9 hole): stays fully opaque (a == 255)
      np = 0;
      push_inst(inst, &np, 0.0, 0.0, 160.0, 120.0, 0.0, 0.0, 0.0, 0.0,
                0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
      push_inst(inst, &np, 20.0, 20.0, 120.0, 80.0, 24.0, 6.0, 1.0, 14.0,
                253.0/255.0, 252.0/255.0, 253.0/255.0, 128.0/255.0,
                0.6, 20.0, 0.0, 1.0, -392.0, 16.0, 0.0, 0.0);
      if (!render_rgba(inst, np, 0.0, 0.0, 0.0, 2.0, 320, 240, pbuf)) return 0;
      int hole_opaque_ok = (pbuf[kh + 3] == 255);

      // 7. Regular controls (chroma=0.06 vs 0.0) under contrast=true (0x85) and frost=0.5 (0xAB): bit-for-bit 0-diff
      static uint8_t pbuf2[320 * 240 * 4];
      auto ctrl_max_diff = [&](double tr, double tg, double tb, double ta, double br, double bg, double bb) -> int {
        int nc = 0;
        push_inst(inst, &nc, 20.0, 20.0, 120.0, 80.0, 24.0, 6.0, 1.0, 1.2, tr, tg, tb, ta, 0.6, 14.0, 0.0, 1.0, -1.0, 20.0, 0.06, 0.0);
        if (!render_rgba(inst, nc, br, bg, bb, 2.0, 320, 240, pbuf)) return 999;
        nc = 0;
        push_inst(inst, &nc, 20.0, 20.0, 120.0, 80.0, 24.0, 6.0, 1.0, 1.2, tr, tg, tb, ta, 0.6, 14.0, 0.0, 1.0, -1.0, 20.0, 0.0, 0.0);
        if (!render_rgba(inst, nc, br, bg, bb, 2.0, 320, 240, pbuf2)) return 999;
        int md = 0;
        for (int py = 40; py <= 200; py++) {
          for (int px = 40; px <= 280; px++) {
            size_t k = ((size_t)py * 320 + (size_t)px) * 4;
            for (int c = 0; c < 4; c++) {
              int d = abs((int)pbuf[k + c] - (int)pbuf2[k + c]);
              if (d > md) md = d;
            }
          }
        }
        return md;
      };
      int d_l85 = ctrl_max_diff(253.0/255.0, 252.0/255.0, 253.0/255.0, 133.0/255.0, 211.0/255.0, 232.0/255.0, 253.0/255.0);
      int d_lab = ctrl_max_diff(253.0/255.0, 252.0/255.0, 253.0/255.0, 171.0/255.0, 211.0/255.0, 232.0/255.0, 253.0/255.0);
      int d_dab = ctrl_max_diff(32.0/255.0, 32.0/255.0, 32.0/255.0, 171.0/255.0, 60.0/255.0, 120.0/255.0, 210.0/255.0);
      int ctrl_ok = (d_l85 == 0 && d_lab == 0 && d_dab == 0);

      int v12_ok = bf_ok && ff_ok && mono_ok && hole_light_ok && hole_dark_ok && hole_opaque_ok && ctrl_ok;
      printf("  [%s] V12-Overlay-And-Hole: 0x80 B-R=%d dRim=%.1f mono=%.1f->%.1f 0xFF |dL|=%.2f; holeL=(%d,%d,%d,a=%d) holeD=(%d,%d,%d,a=%d,ow=%d) opqA=%d ctrlDiff=(%d,%d,%d)\n",
             v12_ok ? "PASS" : "FAIL", center_chroma, top_l - center_l, l_g60, l_g72, fabs(top_ff - center_ff),
             lr, lg, lb, la, dr, dg, db, da, over_w, (int)pbuf[kh + 3], d_l85, d_lab, d_dab);
      ok &= v12_ok;
    }
  }

  return ok;
}

static int run_warp_smoke(void) {
  double inst[16 * INST_SIZE];
  int ok = 1;
  int r = 0, g = 0, b = 0;

  // Center of glass rect (20, 20, 120, 80) at 2x scale is (80pt, 60pt) = (160px, 120px)
  const double gx = 20.0, gy = 20.0, gw = 120.0, gh = 80.0, gr = 20.0;
  const int cx = 160, cy = 120;

  // Vector 1: Dark Regular (#20202067) over #000000 -> (34, 34, 34)
  {
    int n = 0;
    push_inst(inst, &n, gx, gy, gw, gh, gr, 6.0, 1.0, 1.2,
              32.0/255.0, 32.0/255.0, 32.0/255.0, 103.0/255.0,
              0.6, 14.0, 0.0, 1.0,
              -1.0, 20.0, 0.06, 0.0);
    if (!render_and_sample(inst, n, 0.0, 0.0, 0.0, cx, cy, &r, &g, &b)) return 0;
    ok &= check_rgb("V1 dark regular on #000000", r, g, b, 34, 34, 34, 3);
  }

  // Vector 2: Dark Regular (#20202067) over #1B1C1F -> (58, 59, 62)
  {
    int n = 0;
    push_inst(inst, &n, gx, gy, gw, gh, gr, 6.0, 1.0, 1.2,
              32.0/255.0, 32.0/255.0, 32.0/255.0, 103.0/255.0,
              0.6, 14.0, 0.0, 1.0,
              -1.0, 20.0, 0.06, 0.0);
    if (!render_and_sample(inst, n, 27.0/255.0, 28.0/255.0, 31.0/255.0, cx, cy, &r, &g, &b)) return 0;
    ok &= check_rgb("V2 dark regular on #1B1C1F", r, g, b, 58, 59, 62, 3);
  }

  // Vector 3: Dark Adaptive Regular (c2.z = -1.0) over #FFFFFF (t=1) -> (174, 174, 174)
  {
    int n = 0;
    push_inst(inst, &n, gx, gy, gw, gh, gr, 6.0, 1.0, 1.2,
              32.0/255.0, 32.0/255.0, 32.0/255.0, 103.0/255.0,
              0.6, 14.0, -1.0, 1.0,
              -1.0, 20.0, 0.06, 0.0);
    if (!render_and_sample(inst, n, 1.0, 1.0, 1.0, cx, cy, &r, &g, &b)) return 0;
    ok &= check_rgb("V3 dark adaptive regular on #FFFFFF", r, g, b, 174, 174, 174, 3);
  }

  // Vector 4: Light Regular (#FDFCFD67) over #0088FF -> (89, 206, 255), HSV S >= 0.62
  {
    int n = 0;
    push_inst(inst, &n, gx, gy, gw, gh, gr, 6.0, 1.0, 1.2,
              253.0/255.0, 252.0/255.0, 253.0/255.0, 103.0/255.0,
              0.6, 14.0, 0.0, 1.0,
              -1.0, 20.0, 0.06, 0.0);
    if (!render_and_sample(inst, n, 0.0, 136.0/255.0, 1.0, cx, cy, &r, &g, &b)) return 0;
    ok &= check_rgb("V4 light regular on #0088FF", r, g, b, 89, 206, 255, 4);
    double mx = (double)(r > g ? (r > b ? r : b) : (g > b ? g : b));
    double mn = (double)(r < g ? (r < b ? r : b) : (g < b ? g : b));
    double sat = mx > 0.0 ? (mx - mn) / mx : 0.0;
    printf("  [%s] V4 HSV saturation=%.3f (>= 0.62)\n", sat >= 0.62 ? "PASS" : "FAIL", sat);
    ok &= (sat >= 0.62);
  }

  // Vector 5: Pure Clear (regMix=0, #FFFFFF00, blur=0, rim=0) over (0, 255, 64) -> (0, 255, 64)
  {
    int n = 0;
    push_inst(inst, &n, gx, gy, gw, gh, gr, 6.0, 1.0, 0.0,
              1.0, 1.0, 1.0, 0.0,
              0.0, 8.0, 0.0, 1.0,
              -1.0, 0.0, 0.0, 0.0);
    if (!render_and_sample(inst, n, 0.0, 1.0, 64.0/255.0, cx, cy, &r, &g, &b)) return 0;
    ok &= check_rgb("V5 pure clear on (0,255,64)", r, g, b, 0, 255, 64, 2);
  }

  // Vector 6: High-density Light (#FDFCFDD9, alpha=0xD9) over saturated blue (0, 0.4, 1.0) -> R >= 184
  {
    int n = 0;
    push_inst(inst, &n, gx, gy, gw, gh, gr, 6.0, 1.0, 2.0,
              253.0/255.0, 252.0/255.0, 253.0/255.0, 217.0/255.0,
              0.6, 14.0, 0.0, 1.0,
              -1.0, 20.0, 0.06, 0.0);
    if (!render_and_sample(inst, n, 0.0, 0.4, 1.0, cx, cy, &r, &g, &b)) return 0;
    printf("  [%s] V6 high-density light 0xD9 on (0,0.4,1): got=(%d,%d,%d) expected R>=184\n",
           r >= 184 ? "PASS" : "FAIL", r, g, b);
    ok &= (r >= 184);
  }

  // Vector 7: Opaque Light (#FDFCFDFF, alpha=0xFF) over saturated blue (0, 0.4, 1.0) -> (253, 252, 253) +/- 2
  {
    int n = 0;
    push_inst(inst, &n, gx, gy, gw, gh, gr, 6.0, 1.0, 2.0,
              253.0/255.0, 252.0/255.0, 253.0/255.0, 1.0,
              0.6, 14.0, 0.0, 1.0,
              -1.0, 20.0, 0.06, 0.0);
    if (!render_and_sample(inst, n, 0.0, 0.4, 1.0, cx, cy, &r, &g, &b)) return 0;
    ok &= check_rgb("V7 opaque light 0xFF on (0,0.4,1)", r, g, b, 253, 252, 253, 2);
  }

  // Extra pipeline coverage: rect (kind 0), shadow (kind 2), frosted (kind 5), progressive frosted, merged liquid (merge > 0)
  {
    int n = 0;
    push_inst(inst, &n, 4.0, 4.0, 30.0, 30.0, 0.0, 0.0, 0.0, 0.0,
              0.2, 0.4, 0.8, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 10.0, 10.0, 50.0, 40.0, 8.0, 2.0, 0.0, 6.0,
              0.0, 0.0, 0.0, 0.25, 1.0, 2.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 10.0, 60.0, 60.0, 40.0, 10.0, 5.0, 1.2, 6.0,
              1.0, 1.0, 1.0, 0.35, 0.0, 0.0, 0.0, 1.0, -2.0, 0.0, 0.0, 0.0);
    push_inst(inst, &n, 80.0, 60.0, 60.0, 40.0, 10.0, 5.0, 1.25, 8.0,
              1.0, 1.0, 1.0, 0.50, 0.0, 0.0, 0.0, 1.0, -3.0, 0.0, -1.0, 0.0);
    push_inst(inst, &n, 80.0, 10.0, 36.0, 36.0, 18.0, 6.0, 1.0, 2.0,
              32.0/255.0, 32.0/255.0, 32.0/255.0, 103.0/255.0,
              0.6, 14.0, 0.0, 1.0, -4.0, 20.0, 0.06, 12.0);
    push_inst(inst, &n, 110.0, 10.0, 36.0, 36.0, 18.0, 6.0, 1.0, 2.0,
              32.0/255.0, 32.0/255.0, 32.0/255.0, 103.0/255.0,
              0.6, 14.0, 0.0, 1.0, -4.0, 20.0, 0.06, 12.0);
    if (!render_and_sample(inst, n, 0.1, 0.1, 0.12, 20, 20, &r, &g, &b)) return 0;
  }

  if (!run_v8_v9_2d_roi()) ok = 0;

  // Stateful adaptive test 3: Light adaptive opacity suppresses white-on-light flip on D3D11 WARP.
  // (a) Full dark backdrop with 0x67 flips dark, raising tint_a to 0.913 on the same slot snaps immediately
  //     to light glass + dark ink with settling == 0;
  // (b) Fresh full dark capsule at 65% frost (0.752) stays light frosted glass + dark ink.
  {
    const int W = 320, H = 240;
    static uint8_t pxbuf[320 * 240 * 4];
    double ad_inst[4 * INST_SIZE];
    auto build_ad = [&](double tint_a) -> int {
      int n = 0;
      push_inst(ad_inst, &n, 0.0, 0.0, 160.0, 120.0, 0.0, 0.0, 0.0, 0.0,
                0.08, 0.08, 0.08, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
      push_inst(ad_inst, &n, 16.0, 12.0, 128.0, 96.0, 16.0, 6.0, 1.0, 1.0,
                253.0/255.0, 252.0/255.0, 253.0/255.0, tint_a,
                0.0, 4.0, -1.0, 1.0, -50.0, 0.0, 0.0, 0.0);
      push_inst(ad_inst, &n, 64.0, 52.0, 32.0, 16.0, 6.0, 10.0, 0.0, 0.0,
                0.14, 0.15, 0.17, 1.0,
                0.1, 0.5, 0.9, 0.5, 0.0, 80.0, 60.0, 48.0);
      return n;
    };
    auto present_and_read = [&](int n) -> int {
      mb_host_render_dirty(0.0, 0.0, 160.0, 120.0);
      mb_render_present(ad_inst, n, 0.08, 0.08, 0.08);
      memset(pxbuf, 0, sizeof pxbuf);
      g_test_buf_len = sizeof pxbuf;
      return mb_render_drawn_pixels(0, 0, W, H, pxbuf);
    };

    // Reset any stale adaptive slots with an empty whole-frame present
    mb_host_render_dirty(0.0, 0.0, -1.0, -1.0);
    mb_render_present(ad_inst, 0, 0.08, 0.08, 0.08);

    // 1. Baseline 0x67 over full dark flips dark (ink bright > 180, glass dark < 90)
    int n0 = build_ad(103.0 / 255.0);
    if (!present_and_read(n0)) return 0;
    size_t k_ink = (120 * W + 160) * 4;
    size_t k_gl = (64 * W + 64) * 4;
    int ink0 = pxbuf[k_ink], gl0 = pxbuf[k_gl];

    // 2. Raise tint_a to 0.913 on the same slot: snaps immediately to light glass (> 215) + dark ink (< 65), settling == 0
    int n1 = build_ad(0.913);
    if (!present_and_read(n1)) return 0;
    int ink1 = pxbuf[k_ink], gl1 = pxbuf[k_gl];
    int settling1 = mb_render_adaptive_settling(0);

    // 3. Reset slots and draw fresh 0.752 capsule over full dark: stays light glass (> 180) + dark ink (< 65)
    mb_host_render_dirty(0.0, 0.0, -1.0, -1.0);
    mb_render_present(ad_inst, 0, 0.08, 0.08, 0.08);
    int n65 = build_ad(0.752);
    if (!present_and_read(n65)) return 0;
    int ink65 = pxbuf[k_ink], gl65 = pxbuf[k_gl];

    int ad3_ok = (ink0 > 180 && gl0 < 90) &&
                 (ink1 < 65 && gl1 > 215 && settling1 == 0) &&
                 (ink65 < 65 && gl65 > 180);
    printf("  [%s] V13 light adaptive opacity: 0x67=(ink=%d,gl=%d) 0.913=(ink=%d,gl=%d,settling=%d) 0.752=(ink=%d,gl=%d)\n",
           ad3_ok ? "PASS" : "FAIL", ink0, gl0, ink1, gl1, settling1, ink65, gl65);
    ok &= ad3_ok;

    // V13-B: Uniform capsule ink across mixed dark/bright backdrop (no local per-glyph inversion)
    // and opaque adaptive kind==0 fill opacity preservation (pad.w < 0, alpha >= 0.75).
    {
      mb_host_render_dirty(0.0, 0.0, -1.0, -1.0);
      mb_render_present(ad_inst, 0, 1.0, 1.0, 1.0);
      double mix_inst[8 * INST_SIZE];
      int nm = 0;
      push_inst(mix_inst, &nm, 0.0, 0.0, 72.0, 120.0, 0.0, 0.0, 0.0, 0.0,
                0.05, 0.05, 0.06, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
      push_inst(mix_inst, &nm, 10.0, 38.0, 140.0, 44.0, 22.0, 6.0, 1.0, 1.0,
                253.0/255.0, 252.0/255.0, 253.0/255.0, 103.0/255.0,
                0.0, 4.0, -1.0, 1.0, -51.0, 0.0, 0.0, 0.0);
      push_inst(mix_inst, &nm, 24.0, 52.0, 32.0, 16.0, 6.0, 10.0, 0.0, 0.0,
                0.14, 0.15, 0.17, 1.0,
                0.1, 0.5, 0.9, 0.5, 0.0, 80.0, 60.0, 48.0);
      push_inst(mix_inst, &nm, 104.0, 52.0, 32.0, 16.0, 6.0, 10.0, 0.0, 0.0,
                0.14, 0.15, 0.17, 1.0,
                0.1, 0.5, 0.9, 0.5, 0.0, 80.0, 60.0, 48.0);
      mb_host_render_dirty(0.0, 0.0, 160.0, 120.0);
      mb_render_present(mix_inst, nm, 1.0, 1.0, 1.0);
      memset(pxbuf, 0, sizeof pxbuf);
      g_test_buf_len = sizeof pxbuf;
      if (!mb_render_drawn_pixels(0, 0, W, H, pxbuf)) return 0;
      int l_left = px_luma_of(pxbuf, W, 80, 120);
      int l_right = px_luma_of(pxbuf, W, 240, 120);
      int uniform_ok = (l_left < 80) && (l_right < 80) && (abs(l_left - l_right) < 25);

      // Opaque vs translucent adaptive kind==0 fills on flipped dark capsule
      mb_host_render_dirty(0.0, 0.0, -1.0, -1.0);
      mb_render_present(ad_inst, 0, 0.05, 0.05, 0.06);
      nm = 0;
      push_inst(mix_inst, &nm, 0.0, 0.0, 160.0, 120.0, 0.0, 0.0, 0.0, 0.0,
                0.05, 0.05, 0.06, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
      push_inst(mix_inst, &nm, 10.0, 38.0, 140.0, 44.0, 22.0, 6.0, 1.0, 1.0,
                253.0/255.0, 252.0/255.0, 253.0/255.0, 103.0/255.0,
                0.0, 4.0, -1.0, 1.0, -52.0, 0.0, 0.0, 0.0);
      push_inst(mix_inst, &nm, 26.0, 52.0, 36.0, 16.0, 4.0, 0.0, 0.0, 0.0,
                0.10, 0.10, 0.10, 1.0,
                0.0, 0.0, 0.0, 0.0, 0.0, 80.0, 60.0, -48.0);
      push_inst(mix_inst, &nm, 98.0, 52.0, 36.0, 16.0, 4.0, 0.0, 0.0, 0.0,
                0.10, 0.10, 0.10, 0.12,
                0.0, 0.0, 0.0, 0.0, 0.0, 80.0, 60.0, -48.0);
      mb_host_render_dirty(0.0, 0.0, 160.0, 120.0);
      mb_render_present(mix_inst, nm, 0.05, 0.05, 0.06);
      memset(pxbuf, 0, sizeof pxbuf);
      g_test_buf_len = sizeof pxbuf;
      if (!mb_render_drawn_pixels(0, 0, W, H, pxbuf)) return 0;
      int opq_l = px_luma_of(pxbuf, W, 88, 120);
      int trn_l = px_luma_of(pxbuf, W, 232, 120);
      int fill_ok = (opq_l >= 235) && (trn_l < 150);
      int v13b_ok = uniform_ok && fill_ok;
      printf("  [%s] V13-B uniform capsule ink & opaque adaptive fill: mix=(%d,%d) opq=%d (>=235) trn=%d (<150)\n",
             v13b_ok ? "PASS" : "FAIL", l_left, l_right, opq_l, trn_l);
      ok &= v13b_ok;
    }

    // Clean up adaptive slots
    mb_host_render_dirty(0.0, 0.0, -1.0, -1.0);
    mb_render_present(ad_inst, 0, 0.08, 0.08, 0.08);
  }
  return ok;
}

int main(int argc, char **argv) {
  int pass = 0;
  if (argc >= 2 && strcmp(argv[1], "--pass1") == 0) pass = 1;
  else if (argc >= 2 && strcmp(argv[1], "--pass2") == 0) pass = 2;
  else {
    fprintf(stderr, "usage: win_render_test --pass1 | --pass2\n");
    return 2;
  }

  invar_win_shader_stats_reset();
  if (!mb_render_init_offscreen(160.0, 120.0, 2.0)) {
    fprintf(stderr, "FAIL: mb_render_init_offscreen failed\n");
    return 1;
  }

  int baked = 0, disk = 0, compiled = 0;
  invar_win_shader_stats(&baked, &disk, &compiled);
  printf("win_render_test pass=%d stats: baked_hits=%d disk_hits=%d runtime_compiles=%d (expected=%d)\n",
         pass, baked, disk, compiled, EXPECTED_SHADER_KEYS);

  int ok = 1;
  if (pass == 1) {
    if (baked != 0 || disk != 0 || compiled != EXPECTED_SHADER_KEYS) {
      fprintf(stderr, "FAIL: pass1 expected baked=0 disk=0 runtime_compiles=%d\n", EXPECTED_SHADER_KEYS);
      ok = 0;
    }
  } else {
    if (baked != EXPECTED_SHADER_KEYS || disk != 0 || compiled != 0) {
      fprintf(stderr, "FAIL: pass2 expected baked=%d disk=0 runtime_compiles=0\n", EXPECTED_SHADER_KEYS);
      ok = 0;
    }
  }

  if (!run_warp_smoke()) {
    fprintf(stderr, "FAIL: WARP numerical smoke tests failed\n");
    ok = 0;
  }

  if (!ok) return 1;
  printf("win_render_test pass=%d: ALL PASSED\n", pass);
  return 0;
}
