// Headless EGL/GLES3 regression tests for packages/host/native/gles_render.h:
// verifies lookahead glass batching, direct-to-slot blur, VBO trailing kind-12 trimming,
// exact kind-12 shape-segment slicing with zero-copy fast-reject slot matching,
// conservative blocking rules, slot-pinning budget, and partial multi-rect redraws.
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#include "moonbit.h"
#include "corner_sl.h"

#define LOG(...) (fprintf(stderr, "gles_render_test: " __VA_ARGS__), fputc('\n', stderr))
#define MB_STREAM_TEX 4194304
#define MB_VIEW_TEX 8388608
#define MB_LAYER_TEX 12582912

#define MB_VIEWS_UNDER 1
static GLuint test_under_white_tex;
static int test_under_tid;
static int view_under(int tid) { return test_under_tid && tid == test_under_tid; }
static GLuint image_tex(int id) {
  if (id > MB_VIEW_TEX && id <= MB_LAYER_TEX && view_under(id)) {
    if (!test_under_white_tex) {
      static const uint8_t white[4] = {255, 255, 255, 255};
      glGenTextures(1, &test_under_white_tex);
      glBindTexture(GL_TEXTURE_2D, test_under_white_tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    return test_under_white_tex;
  }
  return 0;
}
static void view_parts_in(int tid, int32_t start, int32_t end) { (void)tid, (void)start, (void)end; }

// Detect texture-FBO feedback loops that strict GLES3 drivers reject even if llvmpipe allows them.
static int feedback_loops;
static void feedback_seen(void) {
  GLint fb = 0, type = 0, tex = 0, p = 0, n = 0, unit_was = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
  if (!fb) return;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
  if (type != GL_TEXTURE) return;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &tex);
  glGetIntegerv(GL_CURRENT_PROGRAM, &p);
  if (!p || !tex) return;
  glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &unit_was);
  for (GLint i = 0; i < n; i++) {
    char name[64];
    GLint size = 0, unit = 0, bound = 0;
    GLenum utype = 0;
    glGetActiveUniform(p, (GLuint)i, sizeof name, NULL, &size, &utype, name);
    if (utype != GL_SAMPLER_2D) continue;
    glGetUniformiv(p, glGetUniformLocation(p, name), &unit);
    glActiveTexture(GL_TEXTURE0 + unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (bound == tex) {
      if (!feedback_loops) {
        fprintf(stderr, "gles_render_test: feedback loop: %s on unit %d samples texture %d drawn into\n", name, unit,
                tex);
      }
      feedback_loops++;
    }
  }
  glActiveTexture((GLenum)unit_was);
}

static int vbo_uploads;
static size_t vbo_upload_bytes;
static int fb_blits;
static int fb_binds;
static int target_fb_resolves;
static GLuint tracked_target_fb;
static GLuint current_draw_fb;

static void checked_buffer_data(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
  if (target == GL_ARRAY_BUFFER) {
    vbo_uploads++;
    vbo_upload_bytes += (size_t)size;
  }
  epoxy_glBufferData(target, size, data, usage);
}

static void checked_bind_fb(GLenum target, GLuint fb) {
  fb_binds++;
  if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
    if (tracked_target_fb && current_draw_fb == tracked_target_fb && fb != tracked_target_fb) {
      target_fb_resolves++;
    }
    current_draw_fb = fb;
  }
  epoxy_glBindFramebuffer(target, fb);
}

static void checked_blit_fb(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                            GLbitfield mask, GLenum filter) {
  fb_blits++;
  epoxy_glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

static void checked_draw_arrays(GLenum mode, GLint first, GLsizei count) {
  feedback_seen();
  epoxy_glDrawArrays(mode, first, count);
}

static void checked_draw_arrays_instanced(GLenum mode, GLint first, GLsizei count, GLsizei n) {
  feedback_seen();
  epoxy_glDrawArraysInstanced(mode, first, count, n);
}

#undef glBufferData
#define glBufferData checked_buffer_data
#undef glBindFramebuffer
#define glBindFramebuffer checked_bind_fb
#undef glBlitFramebuffer
#define glBlitFramebuffer checked_blit_fb
#undef glDrawArrays
#define glDrawArrays checked_draw_arrays
#undef glDrawArraysInstanced
#define glDrawArraysInstanced checked_draw_arrays_instanced

#include "gles_render.h"

int mb_atlas_pages_said;

static int failed;

#define CHECK(c)                                                      \
  do {                                                                \
    if (!(c)) {                                                       \
      fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); \
      failed = 1;                                                     \
    }                                                                 \
  } while (0)

static void rect_at(double *d, double x, double y, double w, double h, int kind, double r, double g, double b,
                    double tid, double cw, double ch) {
  memset(d, 0, MB_INST * sizeof(double));
  d[0] = x;
  d[1] = y;
  d[2] = w;
  d[3] = h;
  d[5] = kind;
  d[8] = r;
  d[9] = g;
  d[10] = b;
  d[11] = 1.0;
  d[18] = cw;
  d[19] = ch;
  d[20] = tid;
}

static unsigned char *pixels_of_fb(GLuint fb) {
  unsigned char *px = (unsigned char *)malloc((size_t)fb_w * (size_t)fb_h * 4);
  epoxy_glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  return px;
}

static int max_pixel_diff(const unsigned char *a, const unsigned char *b, size_t nbytes) {
  int md = 0;
  for (size_t i = 0; i < nbytes; i++) {
    int d = (int)a[i] - (int)b[i];
    if (d < 0) d = -d;
    if (d > md) md = d;
  }
  return md;
}

static int surface_as_fresh(GLuint surface) {
  unsigned char *shown = pixels_of_fb(surface);
  glass_forget();
  draw_kept_read();
  unsigned char *fresh = pixels_of_fb(read_fb);
  epoxy_glBindFramebuffer(GL_FRAMEBUFFER, surface);
  current_draw_fb = surface;
  int same = memcmp(shown, fresh, (size_t)fb_w * (size_t)fb_h * 4) == 0;
  free(shown);
  free(fresh);
  return same;
}

static void reset_counters(GLuint fb) {
  vbo_uploads = 0;
  vbo_upload_bytes = 0;
  fb_blits = 0;
  fb_binds = 0;
  target_fb_resolves = 0;
  tracked_target_fb = fb;
  current_draw_fb = fb;
}

int main(void) {
  EGLDisplay dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
    fprintf(stderr, "gles_render_test: no surfaceless EGL display\n");
    return 1;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  static const EGLint cfg_want[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                    EGL_NONE};
  EGLConfig cfg = NULL;
  EGLint ncfg = 0;
  eglChooseConfig(dpy, cfg_want, &cfg, 1, &ncfg);
  static const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, ncfg > 0 ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
  if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
    fprintf(stderr, "gles_render_test: failed to create GLES3 context\n");
    return 1;
  }

  gl_setup();

  GLuint test_tex = 0, test_fb = 0;
  fb_w = 300;
  fb_h = 600;
  cur_w = 150.0;
  cur_h = 300.0;
  cur_scale = 2.0;
  glGenTextures(1, &test_tex);
  glBindTexture(GL_TEXTURE_2D, test_tex);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, fb_w, fb_h);
  glGenFramebuffers(1, &test_fb);
  glBindFramebuffer(GL_FRAMEBUFFER, test_fb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, test_tex, 0);
  CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  target_fb = test_fb;
  gl_set_fixed_dt(1.0 / 120.0);

  size_t px_bytes = (size_t)fb_w * (size_t)fb_h * 4;

  // 1) 5-capture scene (non-overlapping float + bottom scrim + overlapping composer + top scrim + top bar)
  //    with trailing kind-12 shape segments: verify byte-exact parity, lookahead batching, VBO trim, and blit/resolve reduction.
  {
    double scene[64 * MB_INST];
    int n = 0;
    for (int i = 0; i < 6; i++) {
      rect_at(scene + n * MB_INST, 4, i * 48.0, 142, 40, 0, 0.2 + 0.1 * i, 0.5, 0.8 - 0.1 * i, 0, 150.0, 300.0);
      n++;
    }
    // Shape A (under top bar glass, uses segments 0..5 -> kind-12 index 0)
    rect_at(scene + n * MB_INST, 20, 14, 16, 16, 11, 0.9, 0.2, 0.2, 0, 150.0, 300.0);
    scene[n * MB_INST + 12] = 0.0;
    scene[n * MB_INST + 13] = 6.0;
    n++;
    // Shape B (mid-screen outside all glass, uses segments 6..11 -> kind-12 index 1)
    rect_at(scene + n * MB_INST, 20, 140, 16, 16, 11, 0.2, 0.9, 0.2, 0, 150.0, 300.0);
    scene[n * MB_INST + 12] = 6.0;
    scene[n * MB_INST + 13] = 6.0;
    n++;
    // Shape C (under top bar glass, uses segments 18..23 -> kind-12 index 3; leaves index 2 unreferenced in between!)
    rect_at(scene + n * MB_INST, 50, 14, 16, 16, 11, 0.2, 0.2, 0.9, 0, 150.0, 300.0);
    scene[n * MB_INST + 12] = 18.0;
    scene[n * MB_INST + 13] = 6.0;
    n++;
    // Glass 1: float pill (tid=-1, y=180..210)
    rect_at(scene + n * MB_INST, 110, 180, 32, 30, 6, 1.0, 1.0, 1.0, -1, 150.0, 300.0);
    scene[n * MB_INST + 4] = 15.0;
    scene[n * MB_INST + 6] = 1.15;
    scene[n * MB_INST + 7] = 1.2;
    scene[n * MB_INST + 11] = 0.404;
    scene[n * MB_INST + 12] = 0.25;
    scene[n * MB_INST + 13] = 10.0;
    scene[n * MB_INST + 14] = -1.0;
    scene[n * MB_INST + 21] = 20.0;
    scene[n * MB_INST + 22] = 0.06;
    n++;
    // Glass 2: bottom progressive scrim (tid=-2, y=268..300) — independent of Glass 1
    rect_at(scene + n * MB_INST, 0, 268, 150, 32, 5, 0.96, 0.96, 0.97, -2, 150.0, 300.0);
    scene[n * MB_INST + 6] = 1.0;
    scene[n * MB_INST + 7] = 8.5;
    scene[n * MB_INST + 11] = 0.0;
    scene[n * MB_INST + 15] = 1.0;
    scene[n * MB_INST + 22] = -1.0;
    scene[n * MB_INST + 52] = 268.0;
    scene[n * MB_INST + 53] = 18.0;
    n++;
    // Glass 3: composer overlapping Glass 2 (tid=-3, y=246..292)
    rect_at(scene + n * MB_INST, 8, 246, 134, 46, 6, 1.0, 1.0, 1.0, -3, 150.0, 300.0);
    scene[n * MB_INST + 4] = 23.0;
    scene[n * MB_INST + 6] = 1.15;
    scene[n * MB_INST + 7] = 1.2;
    scene[n * MB_INST + 11] = 0.404;
    scene[n * MB_INST + 12] = 0.25;
    scene[n * MB_INST + 13] = 10.0;
    scene[n * MB_INST + 14] = -1.0;
    scene[n * MB_INST + 21] = 20.0;
    scene[n * MB_INST + 22] = 0.06;
    n++;
    // Glass 4: top progressive scrim (tid=-4, y=0..48) — independent of Glass 1..3
    rect_at(scene + n * MB_INST, 0, 0, 150, 48, 5, 0.96, 0.96, 0.97, -4, 150.0, 300.0);
    scene[n * MB_INST + 6] = 1.0;
    scene[n * MB_INST + 7] = 8.5;
    scene[n * MB_INST + 11] = 0.0;
    scene[n * MB_INST + 15] = 1.0;
    scene[n * MB_INST + 22] = -1.0;
    scene[n * MB_INST + 54] = 48.0;
    scene[n * MB_INST + 55] = 48.0;
    n++;
    // Glass 5: top bar overlapping Glass 4 (tid=-5, y=4..40)
    rect_at(scene + n * MB_INST, 8, 4, 134, 36, 6, 1.0, 1.0, 1.0, -5, 150.0, 300.0);
    scene[n * MB_INST + 4] = 18.0;
    scene[n * MB_INST + 6] = 1.15;
    scene[n * MB_INST + 7] = 1.2;
    scene[n * MB_INST + 11] = 0.404;
    scene[n * MB_INST + 12] = 0.25;
    scene[n * MB_INST + 13] = 10.0;
    scene[n * MB_INST + 14] = -1.0;
    scene[n * MB_INST + 21] = 20.0;
    scene[n * MB_INST + 22] = 0.06;
    n++;
    int drawn_pieces = n;
    for (int s = 0; s < 10; s++) {
      memset(scene + n * MB_INST, 0, MB_INST * sizeof(double));
      scene[n * MB_INST + 5] = 12.0;
      scene[n * MB_INST + 0] = 2.0;
      scene[n * MB_INST + 1] = 2.0;
      scene[n * MB_INST + 2] = 14.0;
      scene[n * MB_INST + 3] = 14.0;
      n++;
    }

    // Reference render with lookahead forced off
    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 1;
    reset_counters(test_fb);
    gl_present(scene, n, 0.95, 0.95, 0.96);
    int ref_resolves = target_fb_resolves;
    int ref_blits = fb_blits;
    int ref_binds = fb_binds;
    unsigned char *px_ref = pixels_of_fb(test_fb);

    // Optimized render with lookahead enabled
    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 0;
    lookahead_batched = 0;
    reset_counters(test_fb);
    gl_present(scene, n, 0.95, 0.95, 0.96);
    int opt_resolves = target_fb_resolves;
    int opt_blits = fb_blits;
    int opt_binds = fb_binds;
    unsigned char *px_opt = pixels_of_fb(test_fb);
    int md = max_pixel_diff(px_ref, px_opt, px_bytes);
    CHECK(md == 0);
    CHECK(memcmp(px_ref, px_opt, px_bytes) == 0);
    CHECK(lookahead_batched == 2);
    CHECK(opt_resolves < ref_resolves);
    CHECK(vbo_upload_bytes == (size_t)drawn_pieces * MB_INST * sizeof(float));
    // Direct-to-slot blur: 5 target_fb->lv[0] blits + 3 ad_fb blits = 8 total blits (zero slot_fill blits).
    CHECK(opt_blits == 8);
    printf("gles_render_test: 5-glass scene max_diff=%d resolves=%d->%d blits=%d->%d fb_binds=%d->%d vbo_bytes=%zu (full=%zu)\n",
           md, ref_resolves, opt_resolves, ref_blits, opt_blits, ref_binds, opt_binds, vbo_upload_bytes,
           (size_t)n * MB_INST * sizeof(float));
    free(px_ref);
    free(px_opt);

    // 2) Exact kind-12 segment slicing (including disjoint gap between index 0 and index 3):
    //    mutating Shape B (index 1) or unreferenced gap (index 2) must NOT invalidate any blur;
    //    mutating Shape A (index 0) or Shape C (index 3) MUST invalidate and match fresh render.
    int b0 = blurs_made;
    scene[(drawn_pieces + 1) * MB_INST + 2] = 10.0;
    scene[(drawn_pieces + 2) * MB_INST + 2] = 11.0;
    gl_present(scene, n, 0.95, 0.95, 0.96);
    CHECK(blurs_made == b0);
    scene[(drawn_pieces + 0) * MB_INST + 2] = 6.0;
    gl_present(scene, n, 0.95, 0.95, 0.96);
    CHECK(blurs_made > b0);
    CHECK(surface_as_fresh(test_fb));
    int b1 = blurs_made;
    scene[(drawn_pieces + 3) * MB_INST + 2] = 8.0;
    gl_present(scene, n, 0.95, 0.95, 0.96);
    CHECK(blurs_made > b1);
    CHECK(surface_as_fresh(test_fb));
  }

  // 3) 2-capture independent glass scene: top bar + bottom tab bar batch into 1 lookahead + 1 batched ad_fb update (3 blits).
  {
    double home[16 * MB_INST];
    int hn = 0;
    rect_at(home + hn * MB_INST, 0, 0, 150, 300, 0, 0.85, 0.88, 0.92, 0, 150.0, 300.0);
    hn++;
    rect_at(home + hn * MB_INST, 8, 4, 134, 36, 6, 1.0, 1.0, 1.0, -1, 150.0, 300.0);
    home[hn * MB_INST + 4] = 18.0;
    home[hn * MB_INST + 6] = 1.15;
    home[hn * MB_INST + 7] = 1.2;
    home[hn * MB_INST + 11] = 0.404;
    home[hn * MB_INST + 12] = 0.25;
    home[hn * MB_INST + 13] = 10.0;
    home[hn * MB_INST + 14] = -1.0;
    home[hn * MB_INST + 21] = 20.0;
    home[hn * MB_INST + 22] = 0.06;
    hn++;
    rect_at(home + hn * MB_INST, 8, 240, 134, 48, 6, 1.0, 1.0, 1.0, -2, 150.0, 300.0);
    home[hn * MB_INST + 4] = 24.0;
    home[hn * MB_INST + 6] = 1.15;
    home[hn * MB_INST + 7] = 1.2;
    home[hn * MB_INST + 11] = 0.404;
    home[hn * MB_INST + 12] = 0.25;
    home[hn * MB_INST + 13] = 10.0;
    home[hn * MB_INST + 14] = -1.0;
    home[hn * MB_INST + 21] = 20.0;
    home[hn * MB_INST + 22] = 0.06;
    hn++;

    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 1;
    reset_counters(test_fb);
    gl_present(home, hn, 0.95, 0.95, 0.96);
    int ref_resolves = target_fb_resolves;
    int ref_blits = fb_blits;
    unsigned char *h_ref = pixels_of_fb(test_fb);

    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 0;
    lookahead_batched = 0;
    reset_counters(test_fb);
    gl_present(home, hn, 0.95, 0.95, 0.96);
    int opt_resolves = target_fb_resolves;
    int opt_blits = fb_blits;
    unsigned char *h_opt = pixels_of_fb(test_fb);
    CHECK(memcmp(h_ref, h_opt, px_bytes) == 0);
    CHECK(lookahead_batched == 1);
    CHECK(opt_resolves < ref_resolves);
    CHECK(opt_blits == 3);
    printf("gles_render_test: 2-glass scene resolves=%d->%d blits=%d->%d\n", ref_resolves, opt_resolves, ref_blits,
           opt_blits);
    free(h_ref);
    free(h_opt);
  }

  // 4a) Intervening merged-liquid group (kind == 6, merge > 0) spanning across a later glass rect MUST block lookahead.
  {
    double merged[12 * MB_INST];
    int mn = 0;
    rect_at(merged + mn * MB_INST, 0, 0, 150, 300, 0, 0.3, 0.5, 0.7, 0, 150.0, 300.0);
    mn++;
    rect_at(merged + mn * MB_INST, 20, 10, 40, 30, 6, 1.0, 0.4, 0.2, -1, 150.0, 300.0);
    merged[mn * MB_INST + 4] = 10.0;
    merged[mn * MB_INST + 6] = 1.1;
    merged[mn * MB_INST + 7] = 1.2;
    merged[mn * MB_INST + 11] = 0.45;
    merged[mn * MB_INST + 21] = 12.0;
    merged[mn * MB_INST + 23] = 8.0;
    mn++;
    rect_at(merged + mn * MB_INST, 80, 230, 40, 30, 6, 1.0, 0.4, 0.2, -1, 150.0, 300.0);
    merged[mn * MB_INST + 4] = 10.0;
    merged[mn * MB_INST + 6] = 1.1;
    merged[mn * MB_INST + 7] = 1.2;
    merged[mn * MB_INST + 11] = 0.45;
    merged[mn * MB_INST + 21] = 12.0;
    merged[mn * MB_INST + 23] = 8.0;
    mn++;
    rect_at(merged + mn * MB_INST, 40, 110, 60, 40, 6, 0.2, 0.8, 1.0, -2, 150.0, 300.0);
    merged[mn * MB_INST + 4] = 12.0;
    merged[mn * MB_INST + 6] = 1.1;
    merged[mn * MB_INST + 7] = 1.2;
    merged[mn * MB_INST + 11] = 0.404;
    merged[mn * MB_INST + 14] = -1.0;
    merged[mn * MB_INST + 21] = 14.0;
    mn++;

    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 1;
    gl_present(merged, mn, 0.2, 0.2, 0.25);
    unsigned char *m_ref = pixels_of_fb(test_fb);
    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 0;
    lookahead_batched = 0;
    gl_present(merged, mn, 0.2, 0.2, 0.25);
    unsigned char *m_opt = pixels_of_fb(test_fb);
    CHECK(memcmp(m_ref, m_opt, px_bytes) == 0);
    CHECK(lookahead_batched == 0);
    free(m_ref);
    free(m_opt);
  }

  // 4b) Intervening kind-8 foreground lens overlapping a later glass rect MUST block lookahead.
  {
    double lens_sc[10 * MB_INST];
    int ln = 0;
    rect_at(lens_sc + ln * MB_INST, 0, 0, 150, 300, 0, 0.2, 0.3, 0.4, 0, 150.0, 300.0);
    ln++;
    rect_at(lens_sc + ln * MB_INST, 8, 4, 134, 36, 6, 1.0, 1.0, 1.0, -1, 150.0, 300.0);
    lens_sc[ln * MB_INST + 4] = 18.0;
    lens_sc[ln * MB_INST + 6] = 1.1;
    lens_sc[ln * MB_INST + 7] = 1.2;
    lens_sc[ln * MB_INST + 11] = 0.404;
    lens_sc[ln * MB_INST + 14] = -1.0;
    lens_sc[ln * MB_INST + 21] = 16.0;
    ln++;
    rect_at(lens_sc + ln * MB_INST, 20, 220, 100, 50, 8, 1.0, 1.0, 1.0, 0, 150.0, 300.0);
    lens_sc[ln * MB_INST + 4] = 16.0;
    lens_sc[ln * MB_INST + 7] = 1.0;
    lens_sc[ln * MB_INST + 11] = 1.0;
    lens_sc[ln * MB_INST + 12] = 1.0;
    lens_sc[ln * MB_INST + 13] = 8.0;
    lens_sc[ln * MB_INST + 14] = 10.0;
    ln++;
    rect_at(lens_sc + ln * MB_INST, 30, 230, 80, 30, 0, 0.95, 0.2, 0.2, 0, 150.0, 300.0);
    ln++;
    rect_at(lens_sc + ln * MB_INST, 12, 240, 126, 40, 6, 1.0, 1.0, 1.0, -2, 150.0, 300.0);
    lens_sc[ln * MB_INST + 4] = 18.0;
    lens_sc[ln * MB_INST + 6] = 1.1;
    lens_sc[ln * MB_INST + 7] = 1.2;
    lens_sc[ln * MB_INST + 11] = 0.404;
    lens_sc[ln * MB_INST + 14] = -1.0;
    lens_sc[ln * MB_INST + 21] = 16.0;
    ln++;

    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 1;
    gl_present(lens_sc, ln, 0.2, 0.2, 0.25);
    unsigned char *l_ref = pixels_of_fb(test_fb);
    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 0;
    lookahead_batched = 0;
    gl_present(lens_sc, ln, 0.2, 0.2, 0.25);
    unsigned char *l_opt = pixels_of_fb(test_fb);
    CHECK(memcmp(l_ref, l_opt, px_bytes) == 0);
    CHECK(lookahead_batched == 0);
    free(l_ref);
    free(l_opt);
  }

  // 4c) Intervening rotated rect overlapping a later glass rect MUST block lookahead.
  {
    double rot_sc[8 * MB_INST];
    int rn = 0;
    rect_at(rot_sc + rn * MB_INST, 0, 0, 150, 300, 0, 0.2, 0.3, 0.4, 0, 150.0, 300.0);
    rn++;
    rect_at(rot_sc + rn * MB_INST, 8, 4, 134, 36, 6, 1.0, 1.0, 1.0, -1, 150.0, 300.0);
    rot_sc[rn * MB_INST + 4] = 16.0;
    rot_sc[rn * MB_INST + 6] = 1.1;
    rot_sc[rn * MB_INST + 7] = 1.2;
    rot_sc[rn * MB_INST + 11] = 0.404;
    rot_sc[rn * MB_INST + 14] = -1.0;
    rot_sc[rn * MB_INST + 21] = 14.0;
    rn++;
    // Rotated rect whose unrotated box is y=180..210, rotated 45 deg around (75, 225) so it swings into y=240..280.
    rect_at(rot_sc + rn * MB_INST, 20, 180, 110, 30, 0, 0.9, 0.2, 0.2, 0, 150.0, 300.0);
    rot_sc[rn * MB_INST + 32] = 0.78539816339;
    rot_sc[rn * MB_INST + 33] = 75.0;
    rot_sc[rn * MB_INST + 34] = 225.0;
    rn++;
    rect_at(rot_sc + rn * MB_INST, 12, 240, 126, 40, 6, 1.0, 1.0, 1.0, -2, 150.0, 300.0);
    rot_sc[rn * MB_INST + 4] = 16.0;
    rot_sc[rn * MB_INST + 6] = 1.1;
    rot_sc[rn * MB_INST + 7] = 1.2;
    rot_sc[rn * MB_INST + 11] = 0.404;
    rot_sc[rn * MB_INST + 14] = -1.0;
    rot_sc[rn * MB_INST + 21] = 14.0;
    rn++;

    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 1;
    gl_present(rot_sc, rn, 0.2, 0.2, 0.25);
    unsigned char *r_ref = pixels_of_fb(test_fb);
    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 0;
    lookahead_batched = 0;
    gl_present(rot_sc, rn, 0.2, 0.2, 0.25);
    unsigned char *r_opt = pixels_of_fb(test_fb);
    CHECK(memcmp(r_ref, r_opt, px_bytes) == 0);
    CHECK(lookahead_batched == 0);
    free(r_ref);
    free(r_opt);
  }

  // 4d) Intervening kind-9 native view cutout overlapping a later glass rect MUST block lookahead.
  {
    test_under_tid = MB_VIEW_TEX + 1;
    double view_sc[8 * MB_INST];
    int vn = 0;
    rect_at(view_sc + vn * MB_INST, 0, 0, 150, 300, 0, 0.2, 0.3, 0.4, 0, 150.0, 300.0);
    vn++;
    rect_at(view_sc + vn * MB_INST, 8, 4, 134, 36, 6, 1.0, 1.0, 1.0, -1, 150.0, 300.0);
    view_sc[vn * MB_INST + 4] = 16.0;
    view_sc[vn * MB_INST + 6] = 1.1;
    view_sc[vn * MB_INST + 7] = 1.2;
    view_sc[vn * MB_INST + 11] = 0.404;
    view_sc[vn * MB_INST + 14] = -1.0;
    view_sc[vn * MB_INST + 21] = 14.0;
    vn++;
    rect_at(view_sc + vn * MB_INST, 20, 235, 110, 45, 9, 1.0, 1.0, 1.0, (double)test_under_tid, 150.0, 300.0);
    vn++;
    rect_at(view_sc + vn * MB_INST, 12, 240, 126, 40, 6, 1.0, 1.0, 1.0, -2, 150.0, 300.0);
    view_sc[vn * MB_INST + 4] = 16.0;
    view_sc[vn * MB_INST + 6] = 1.1;
    view_sc[vn * MB_INST + 7] = 1.2;
    view_sc[vn * MB_INST + 11] = 0.404;
    view_sc[vn * MB_INST + 14] = -1.0;
    view_sc[vn * MB_INST + 21] = 14.0;
    vn++;

    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 1;
    gl_present(view_sc, vn, 0.2, 0.2, 0.25);
    unsigned char *v_ref = pixels_of_fb(test_fb);
    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 0;
    lookahead_batched = 0;
    gl_present(view_sc, vn, 0.2, 0.2, 0.25);
    unsigned char *v_opt = pixels_of_fb(test_fb);
    CHECK(memcmp(v_ref, v_opt, px_bytes) == 0);
    CHECK(lookahead_batched == 0);
    free(v_ref);
    free(v_opt);
    test_under_tid = 0;
  }

  // 5) Direct-to-slot blur across raw_copy (k==0, sg<=0.3), contained (kind==5, a[7]<0), and regular blur:
  //    verify zero post-blur slot_fill blits, zero feedback loops, and fresh-render parity.
  {
    double modes[8 * MB_INST];
    int mn = 0;
    rect_at(modes + mn * MB_INST, 0, 0, 150, 300, 0, 0.35, 0.55, 0.75, 0, 150.0, 300.0);
    mn++;
    rect_at(modes + mn * MB_INST, 10, 10, 130, 80, 0, 0.9, 0.3, 0.2, 0, 150.0, 300.0);
    mn++;
    // raw_copy frosted glass (blur=0 -> k=0, sg=0)
    rect_at(modes + mn * MB_INST, 12, 12, 60, 40, 5, 0.9, 0.9, 0.95, -1, 150.0, 300.0);
    modes[mn * MB_INST + 6] = 1.0;
    modes[mn * MB_INST + 7] = 0.0;
    modes[mn * MB_INST + 11] = 0.25;
    mn++;
    // contained frosted glass (a[7] < 0)
    rect_at(modes + mn * MB_INST, 12, 100, 120, 50, 5, 0.9, 0.9, 0.95, -2, 150.0, 300.0);
    modes[mn * MB_INST + 6] = 1.0;
    modes[mn * MB_INST + 7] = -4.0;
    modes[mn * MB_INST + 11] = 0.3;
    mn++;

    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 0;
    reset_counters(test_fb);
    gl_present(modes, mn, 0.2, 0.2, 0.25);
    // Exactly 2 blits (one target_fb->out/lv[0] per glass capture, zero slot_fill blits).
    CHECK(fb_blits == 2);
    CHECK(surface_as_fresh(test_fb));
  }

  // 6) Slot-pinning stress test (> MB_GLASS_SLOTS = 32 groups, mixing 30 liquid glass pills + 4 progressive scrims).
  {
    double stress[40 * MB_INST];
    int sn = 0;
    rect_at(stress + sn * MB_INST, 0, 0, 150, 300, 0, 0.25, 0.35, 0.45, 0, 150.0, 300.0);
    sn++;
    for (int g = 0; g < 30; g++) {
      double gx = (g % 5) * 28.0 + 4.0;
      double gy = (g / 5) * 34.0 + 4.0;
      rect_at(stress + sn * MB_INST, gx, gy, 22, 24, 6, 1.0, 1.0, 1.0, -(g + 1), 150.0, 300.0);
      stress[sn * MB_INST + 4] = 8.0;
      stress[sn * MB_INST + 6] = 1.1;
      stress[sn * MB_INST + 7] = 1.2;
      stress[sn * MB_INST + 11] = 0.404;
      stress[sn * MB_INST + 14] = -1.0;
      stress[sn * MB_INST + 21] = 10.0;
      sn++;
    }
    for (int d = 0; d < 4; d++) {
      double dy = 216.0 + d * 20.0;
      rect_at(stress + sn * MB_INST, 0, dy, 150, 18, 5, 0.95, 0.95, 0.96, -(31 + d), 150.0, 300.0);
      stress[sn * MB_INST + 6] = 1.0;
      stress[sn * MB_INST + 7] = 6.0;
      stress[sn * MB_INST + 11] = 0.0;
      stress[sn * MB_INST + 15] = 1.0;
      stress[sn * MB_INST + 22] = -1.0;
      stress[sn * MB_INST + 52] = dy;
      stress[sn * MB_INST + 53] = 12.0;
      sn++;
    }
    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 1;
    gl_present(stress, sn, 0.25, 0.35, 0.45);
    unsigned char *st_ref = pixels_of_fb(test_fb);
    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 0;
    lookahead_batched = 0;
    gl_present(stress, sn, 0.25, 0.35, 0.45);
    unsigned char *st_opt = pixels_of_fb(test_fb);
    CHECK(lookahead_batched > 0);
    CHECK(memcmp(st_ref, st_opt, px_bytes) == 0);
    CHECK(surface_as_fresh(test_fb));
    free(st_ref);
    free(st_opt);
  }

  // 7) Partial multi-rect redraw (2 disjoint dirty rects, cut_on == 1) + intervening skind-6 ad_barrier.
  {
    double bar[12 * MB_INST];
    int bn = 0;
    rect_at(bar + bn * MB_INST, 0, 0, 150, 300, 0, 0.85, 0.88, 0.92, 0, 150.0, 300.0);
    bn++;
    rect_at(bar + bn * MB_INST, 8, 8, 60, 32, 6, 1.0, 1.0, 1.0, -1, 150.0, 300.0);
    bar[bn * MB_INST + 4] = 12.0;
    bar[bn * MB_INST + 6] = 1.1;
    bar[bn * MB_INST + 7] = 1.2;
    bar[bn * MB_INST + 11] = 0.404;
    bar[bn * MB_INST + 14] = -1.0;
    bar[bn * MB_INST + 21] = 14.0;
    bn++;
    rect_at(bar + bn * MB_INST, 20, 100, 100, 40, 0, 0.9, 0.3, 0.2, 0, 150.0, 300.0);
    bn++;
    rect_at(bar + bn * MB_INST, 24, 104, 92, 32, 6, 1.0, 1.0, 1.0, -2, 150.0, 300.0);
    bar[bn * MB_INST + 4] = 12.0;
    bar[bn * MB_INST + 6] = 1.1;
    bar[bn * MB_INST + 7] = 1.2;
    bar[bn * MB_INST + 11] = 0.404;
    bar[bn * MB_INST + 14] = -1.0;
    bar[bn * MB_INST + 21] = 14.0;
    bn++;
    rect_at(bar + bn * MB_INST, 8, 160, 134, 32, 6, 1.0, 1.0, 1.0, -3, 150.0, 300.0);
    bar[bn * MB_INST + 4] = 12.0;
    bar[bn * MB_INST + 6] = 1.1;
    bar[bn * MB_INST + 7] = 1.2;
    bar[bn * MB_INST + 11] = 0.404;
    bar[bn * MB_INST + 14] = -1.0;
    bar[bn * MB_INST + 21] = 14.0;
    bn++;
    rect_at(bar + bn * MB_INST, 8, 250, 134, 40, 6, 1.0, 1.0, 1.0, -4, 150.0, 300.0);
    bar[bn * MB_INST + 4] = 14.0;
    bar[bn * MB_INST + 6] = 1.1;
    bar[bn * MB_INST + 7] = 1.2;
    bar[bn * MB_INST + 11] = 0.404;
    bar[bn * MB_INST + 14] = -1.0;
    bar[bn * MB_INST + 21] = 14.0;
    bn++;

    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 1;
    gl_present(bar, bn, 0.85, 0.88, 0.92);
    bar[0 * MB_INST + 8] = 0.4;
    gl_dirty_add(0, 0, 150, 210);
    gl_dirty_add(0, 235, 150, 65);
    gl_present(bar, bn, 0.85, 0.88, 0.92);
    unsigned char *p_ref = pixels_of_fb(test_fb);

    bar[0 * MB_INST + 8] = 0.85;
    glass_forget();
    gl_adaptive_reset();
    lookahead_force_off = 0;
    gl_present(bar, bn, 0.85, 0.88, 0.92);
    bar[0 * MB_INST + 8] = 0.4;
    lookahead_batched = 0;
    gl_dirty_add(0, 0, 150, 210);
    gl_dirty_add(0, 235, 150, 65);
    gl_present(bar, bn, 0.85, 0.88, 0.92);
    unsigned char *p_opt = pixels_of_fb(test_fb);
    CHECK(lookahead_batched >= 1);
    CHECK(memcmp(p_ref, p_opt, px_bytes) == 0);
    free(p_ref);
    free(p_opt);
  }

  // 8) Same-polarity adaptive liquid glass, opt-in clear-invert, dark more_contrast (is_dark==2) easing, & .soft curve.
  {
    double sp[3 * MB_INST];
    // 8a) Default same-polarity light adaptive (a[6] = +1.0) on black: bright densified glass (> 0.62) + dark ink (< 0.25)
    memset(sp, 0, sizeof(sp));
    rect_at(sp, 0, 0, 150, 300, 0, 0.0, 0.0, 0.0, 0, 150.0, 300.0);
    rect_at(sp + MB_INST, 15, 20, 120, 80, 6, 253.0/255.0, 252.0/255.0, 253.0/255.0, -70, 150.0, 300.0);
    sp[MB_INST + 4] = 20.0; sp[MB_INST + 6] = 1.0; sp[MB_INST + 7] = 2.0;
    sp[MB_INST + 11] = 0x67 / 255.0; sp[MB_INST + 13] = 10.0; sp[MB_INST + 14] = -1.0; sp[MB_INST + 15] = 1.0;
    rect_at(sp + 2 * MB_INST, 55, 50, 24, 20, 10, 0.14, 0.15, 0.17, 0, 150.0, 300.0);
    sp[2 * MB_INST + 4] = 6.0;
    sp[2 * MB_INST + 12] = 0.1; sp[2 * MB_INST + 13] = 0.5; sp[2 * MB_INST + 14] = 0.9; sp[2 * MB_INST + 15] = 0.5;
    sp[2 * MB_INST + 21] = 75.0; sp[2 * MB_INST + 22] = 60.0; sp[2 * MB_INST + 23] = 40.0;
    glass_forget();
    gl_adaptive_reset();
    gl_present(sp, 3, 0.0, 0.0, 0.0);
    unsigned char *px_sp = pixels_of_fb(test_fb);
    int sp_gl = px_sp[((fb_h - 1 - 60 * 2) * fb_w + 100 * 2) * 4];
    int sp_ink = px_sp[((fb_h - 1 - 60 * 2) * fb_w + 67 * 2) * 4];
    free(px_sp);
    CHECK(sp_gl > 158);
    CHECK(sp_ink < 64);

    // 8b) Opt-in clear-invert light adaptive (a[6] = -1.0) on black: dark glass (< 90) + white ink (> 190)
    sp[MB_INST + 6] = -1.0;
    glass_forget();
    gl_adaptive_reset();
    gl_present(sp, 3, 0.0, 0.0, 0.0);
    unsigned char *px_inv = pixels_of_fb(test_fb);
    int inv_gl = px_inv[((fb_h - 1 - 60 * 2) * fb_w + 100 * 2) * 4];
    int inv_ink = px_inv[((fb_h - 1 - 60 * 2) * fb_w + 67 * 2) * 4];
    free(px_inv);
    CHECK(inv_gl < 90);
    CHECK(inv_ink > 190);

    // 8c) Dark more_contrast (tint_a = 0.52 -> is_dark == 2) over white densifies (60..112) and eases smoothly dark -> white
    double mc[2 * MB_INST];
    memset(mc, 0, sizeof(mc));
    rect_at(mc, 0, 0, 150, 300, 0, 0.05, 0.05, 0.05, 0, 150.0, 300.0);
    rect_at(mc + MB_INST, 15, 20, 120, 80, 6, 32.0/255.0, 32.0/255.0, 32.0/255.0, -71, 150.0, 300.0);
    mc[MB_INST + 4] = 20.0; mc[MB_INST + 6] = 1.0; mc[MB_INST + 7] = 2.0;
    mc[MB_INST + 11] = 0.52; mc[MB_INST + 13] = 10.0; mc[MB_INST + 14] = -1.0; mc[MB_INST + 15] = 1.0;
    gl_set_fixed_dt(1.0 / 60.0);
    glass_forget();
    gl_adaptive_reset();
    gl_present(mc, 2, 0.05, 0.05, 0.05);
    rect_at(mc, 0, 0, 150, 300, 0, 1.0, 1.0, 1.0, 0, 150.0, 300.0);
    gl_present(mc, 2, 1.0, 1.0, 1.0);
    CHECK(gl_adaptive_settling() == 1);
    unsigned char *px_mc1 = pixels_of_fb(test_fb);
    int mc_f1 = px_mc1[((fb_h - 1 - 60 * 2) * fb_w + 75 * 2) * 4];
    free(px_mc1);
    for (int k = 0; k < 40; k++) gl_present(mc, 2, 1.0, 1.0, 1.0);
    unsigned char *px_mc40 = pixels_of_fb(test_fb);
    int mc_f40 = px_mc40[((fb_h - 1 - 60 * 2) * fb_w + 75 * 2) * 4];
    free(px_mc40);
    CHECK(mc_f40 < mc_f1 - 3);
    CHECK(mc_f40 >= 60 && mc_f40 <= 112);
    CHECK(gl_adaptive_settling() == 0);

    // 8d) .soft progressive scroll-edge (kind == 5, a[22] = -1.0, col.a = 0.96): dark on dark matches ground; dark on white dissolves top & fades bottom
    double ap[2 * MB_INST];
    memset(ap, 0, sizeof(ap));
    rect_at(ap, 0, 0, 150, 300, 0, 0.08, 0.08, 0.08, 0, 150.0, 300.0);
    rect_at(ap + MB_INST, 0, 0, 150, 68.0, 5, 0.08, 0.08, 0.08, -72, 150.0, 300.0);
    ap[MB_INST + 6] = 1.0; ap[MB_INST + 7] = 12.0; ap[MB_INST + 11] = 0.96; ap[MB_INST + 15] = 1.0;
    ap[MB_INST + 22] = -1.0; ap[MB_INST + 54] = 68.0; ap[MB_INST + 55] = 20.0;
    glass_forget();
    gl_adaptive_reset();
    gl_present(ap, 2, 0.08, 0.08, 0.08);
    unsigned char *px_same = pixels_of_fb(test_fb);
    int same_r = px_same[((fb_h - 1 - 16 * 2) * fb_w + 75 * 2) * 4];
    free(px_same);
    rect_at(ap, 0, 0, 150, 300, 0, 1.0, 1.0, 1.0, 0, 150.0, 300.0);
    glass_forget();
    gl_adaptive_reset();
    gl_present(ap, 2, 1.0, 1.0, 1.0);
    unsigned char *px_opp = pixels_of_fb(test_fb);
    int opp_top_r = px_opp[((fb_h - 1 - 10 * 2) * fb_w + 75 * 2) * 4];
    int opp_bot_r = px_opp[((fb_h - 1 - 65 * 2) * fb_w + 75 * 2) * 4];
    free(px_opp);
    CHECK(abs(same_r - 20) <= 3);
    CHECK(opp_top_r <= 45);
    CHECK(opp_bot_r >= 225);
  }

  CHECK(feedback_loops == 0);
  CHECK(glGetError() == GL_NO_ERROR);

  gl_set_fixed_dt(0.0);
  if (canvas_fb) {
    glDeleteFramebuffers(1, &canvas_fb);
    glDeleteTextures(1, &canvas_tex);
    canvas_lost();
  }
  // Atlas pages: a page added keeps page 0's texels, a glyph whose v is a page down is drawn from page 1, and a page
  // given back leaves page 0 as it was.
  {
    const int side = MB_ATLAS;
    uint8_t c0[16 * 16], c1[16 * 16];
    memset(c0, 200, sizeof c0);
    memset(c1, 90, sizeof c1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, atlas);
    gl_atlas_sub(0, 0, 0, 16, 16, c0);
    CHECK(gl_atlas_have[0] == 1);
    CHECK(gl_atlas_pages(0, 2) == 1 && gl_atlas_have[0] == 2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, atlas);
    gl_atlas_sub(0, 0, side, 16, 16, c1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    double scene[2 * MB_INST];
    rect_at(scene, 10, 10, 40, 40, 1, 1.0, 1.0, 1.0, 0, 150.0, 300.0);
    scene[12] = 0.0;
    scene[13] = 0.0;
    scene[14] = 16.0;
    scene[15] = 16.0;
    rect_at(scene + MB_INST, 60, 10, 40, 40, 1, 1.0, 1.0, 1.0, 0, 150.0, 300.0);
    scene[MB_INST + 12] = 0.0;
    scene[MB_INST + 13] = (double)side;
    scene[MB_INST + 14] = 16.0;
    scene[MB_INST + 15] = 16.0;
    glass_forget();
    reset_counters(test_fb);
    gl_present(scene, 2, 0.0, 0.0, 0.0);
    unsigned char *px = pixels_of_fb(test_fb);
    // (rows from the bottom: the quads' middles, 30 points down, 30 and 80 across, at 2 pixels a point)
    size_t row = (size_t)(fb_h - 1 - 60) * (size_t)fb_w * 4;
    int r0 = px[row + 60 * 4], r1 = px[row + 160 * 4];
    CHECK(abs(r0 - 200) <= 2);
    CHECK(abs(r1 - 90) <= 2);
    free(px);
    CHECK(gl_atlas_pages(0, 1) == 1 && gl_atlas_have[0] == 1);
    glass_forget();
    reset_counters(test_fb);
    gl_present(scene, 1, 0.0, 0.0, 0.0);
    px = pixels_of_fb(test_fb);
    CHECK(abs((int)px[row + 60 * 4] - 200) <= 2);
    free(px);
    if (failed) fprintf(stderr, "gles_render_test: atlas pages r0=%d r1=%d\n", r0, r1);
  }

  glass_free();
  glDeleteFramebuffers(1, &test_fb);
  glDeleteTextures(1, &test_tex);
  if (test_under_white_tex) glDeleteTextures(1, &test_under_white_tex);
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(dpy, ctx);
  eglTerminate(dpy);

  if (!failed) printf("gles_render_test: OK\n");
  return failed;
}
