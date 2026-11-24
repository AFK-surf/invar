// Standalone Linux EGL/GLES3 offscreen pixel-readback test for `ensure_blur_layer` FBO failure recovery.
// Verifies that when an offscreen group_alpha (kind 16/17) layer FBO creation fails and is deleted,
// GL_FRAMEBUFFER is restored to the parent offscreen FBO (not left at 0) so the failed group leaks
// zero pixels AND subsequent sibling draws still render cleanly to the parent target.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "moonbit.h"

#define INST_SIZE 60

extern int32_t mb_render_init_offscreen(double w, double h, double scale);
extern int32_t mb_render_pixels_of(const double *inst, int32_t count, double r, double g, double b, double scale,
                                   int32_t w, int32_t h, moonbit_bytes_t out);
extern void invar_gl_test_fail_blur_depth(int depth);

static void push_inst(double *buf, int *n, double x, double y, double w, double h, double kind,
                      double r, double g, double b, double a) {
  double *p = buf + (*n) * INST_SIZE;
  memset(p, 0, INST_SIZE * sizeof(double));
  p[0] = x; p[1] = y; p[2] = w; p[3] = h;
  p[5] = kind;
  p[8] = r; p[9] = g; p[10] = b; p[11] = a;
  p[16] = 0.0; p[17] = 0.0; p[18] = 160.0; p[19] = 120.0;
  (*n)++;
}

static int check_px(const char *label, const uint8_t *rgba, int w, int x, int y,
                    int er, int eg, int eb, int tol) {
  const uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
  int ok = abs((int)p[0] - er) <= tol && abs((int)p[1] - eg) <= tol && abs((int)p[2] - eb) <= tol;
  printf("  [%s] %s at (%d,%d): got=(%d,%d,%d) expected=(%d,%d,%d)+/-%d\n",
         ok ? "PASS" : "FAIL", label, x, y, p[0], p[1], p[2], er, eg, eb, tol);
  return ok;
}

int main(void) {
  if (!mb_render_init_offscreen(160.0, 120.0, 2.0)) {
    fprintf(stderr, "FAIL: mb_render_init_offscreen failed\n");
    return 1;
  }
  const int W = 320, H = 240;
  moonbit_bytes_t rgba = moonbit_make_bytes(W * H * 4, 0);
  if (!rgba) return 1;

  double inst[8 * INST_SIZE];
  int n = 0;
  // Failing kind=16 group at (10,20,60,80) containing solid red child, followed by solid green sibling at (90,40,40,40)
  push_inst(inst, &n, 10.0, 20.0, 60.0, 80.0, 16.0, 0.0, 0.0, 0.0, 1.0);
  push_inst(inst, &n, 10.0, 20.0, 60.0, 80.0, 0.0, 1.0, 0.0, 0.0, 1.0);
  push_inst(inst, &n, 10.0, 20.0, 60.0, 80.0, 17.0, 1.0, 1.0, 1.0, 0.5);
  push_inst(inst, &n, 90.0, 40.0, 40.0, 40.0, 0.0, 0.0, 1.0, 0.0, 1.0);

  invar_gl_test_fail_blur_depth(0);
  int rc1 = mb_render_pixels_of(inst, n, 0.0, 0.0, 1.0, 2.0, W, H, rgba);
  invar_gl_test_fail_blur_depth(-1);
  if (!rc1) {
    fprintf(stderr, "FAIL: mb_render_pixels_of (fail path) returned 0\n");
    moonbit_decref(rgba);
    return 1;
  }
  int ok = 1;
  // Inside skipped group (40pt, 60pt) -> (80px, 120px): must remain parent blue (0,0,255), zero red leaked
  ok &= check_px("GLES FBO failure skips group (parent stays blue)", rgba, W, 80, 120, 0, 0, 255, 2);
  // Subsequent sibling (110pt, 60pt) -> (220px, 120px): parent FBO restored, sibling draws green (0,255,0)
  ok &= check_px("GLES FBO failure restores parent FBO for sibling (green)", rgba, W, 220, 120, 0, 255, 0, 2);

  // Control run without failure: red group at 0.5 over blue -> (128, 0, 128), sibling still green (0, 255, 0)
  int rc2 = mb_render_pixels_of(inst, n, 0.0, 0.0, 1.0, 2.0, W, H, rgba);
  if (!rc2) {
    fprintf(stderr, "FAIL: mb_render_pixels_of (normal path) returned 0\n");
    moonbit_decref(rgba);
    return 1;
  }
  ok &= check_px("GLES normal group_alpha=0.5 over blue", rgba, W, 80, 120, 128, 0, 128, 2);
  ok &= check_px("GLES normal sibling green", rgba, W, 220, 120, 0, 255, 0, 2);

  moonbit_decref(rgba);
  if (!ok) return 1;
  printf("linux_gles_fbo_fail_test: ALL PASSED\n");
  return 0;
}
