// Invar Android host: C/JNI + GLES3 bridge for android/ (dev.invar.*).
// Java owns the Activity, GLSurfaceView, input, IME, text rasterization, and
// image decoding; this file owns the GLES3 renderer (gles_render.h) and
// bridges Java with the Pion host callbacks (packages/host/src/host_native.pion).
// All Pion app logic runs on MBView's logic thread; GL drawing runs on the GL
// thread with double-buffered frame handoff (frame_mail.h) and synchronous
// readback/offscreen-layer dispatch (gl_sync).

#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <time.h>
#include <zlib.h>

#include "moonbit.h"
#include "post.h"
#include "term.h"
#include "app_calls.h"
#include "done_watch.h"
#include "corner_sl.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "invar", __VA_ARGS__)

typedef void (*mb_frame_fn)(double, double, double, double);
typedef void (*mb_pointer_fn)(int32_t, double, double, int32_t);
typedef void (*mb_wheel_fn)(double, double, double, double, int32_t);
typedef void (*mb_key_fn)(int32_t, int32_t);
typedef void (*mb_text_fn)(int32_t, moonbit_string_t, int32_t, int32_t);
typedef void (*mb_back_fn)(int32_t, double, double, int32_t);

static struct {
  mb_frame_fn frame;
  mb_pointer_fn pointer;
  mb_wheel_fn wheel;
  mb_key_fn key;
  mb_text_fn text;
} cb;

static JavaVM *vm;
static jclass host_cls, text_cls, images_cls, mbview_cls;
static jmethodID m_request_frame, m_posted, m_wake_after, m_frames_paced, m_set_ime, m_clipboard;
static jmethodID m_clipboard_secret, m_clipboard_secret_off, m_text_menu, m_text_menu_own;
static jmethodID m_log, m_bench, m_reduce_motion, m_exit, m_back_enabled, m_drive;
static jmethodID m_system_dark, m_font_scale, m_native_pointing, m_keyboard, m_a11y_tree;
static jmethodID m_set_dark, m_set_sideways, m_set_turnable, m_word_at, m_open_url;
static jmethodID m_cs_known, m_cs_decode;
static jmethodID m_metrics, m_shape, m_glyph, m_glyph_put, m_generation, m_text_key;
static jmethodID m_face_add, m_face_add_bytes, m_face_clear, m_ahead, m_flush, m_text_scale;
static jmethodID m_ref_begin, m_ref_text, m_ref_end, m_ref_frame;
static jmethodID m_img_load, m_img_free, m_img_reload, m_img_hold, m_img_pump;
static jmethodID m_gl_sync;

static void device_init(JNIEnv *env);

static mb_back_fn back_cb;
static jdoubleArray shape_buf, small_buf;
static jintArray ahead_buf;
static int want_frame;
static int own_clock;
static double frame_now;
static int gl_ready;
static atomic_int g_a11y_enabled = 0;

// Weak entry point supplied by the linked Pion application binary.
extern int invar_main(int argc, char **argv) __attribute__((weak));

JavaVM *invar_android_vm(void) { return vm; }

static JNIEnv *env_(void) {
  if (!vm) return NULL;
  JNIEnv *env = NULL;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return NULL;
  return env;
}

static jstring jstr(JNIEnv *env, moonbit_string_t s) {
  if (!s) return (*env)->NewStringUTF(env, "");
  return (*env)->NewString(env, (const jchar *)s, Moonbit_array_length(s));
}

static jbyteArray jbytes(JNIEnv *env, moonbit_bytes_t data) {
  jsize n = data ? Moonbit_array_length(data) : 0;
  jbyteArray b = (*env)->NewByteArray(env, n);
  if (n) (*env)->SetByteArrayRegion(env, b, 0, n, (const jbyte *)data);
  return b;
}

static moonbit_string_t mstr(JNIEnv *env, jstring s) {
  if (!s) return moonbit_make_string_raw(0);
  jsize n = (*env)->GetStringLength(env, s);
  moonbit_string_t out = moonbit_make_string_raw(n);
  if (n > 0) (*env)->GetStringRegion(env, s, 0, n, (jchar *)out);
  return out;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *v, void *reserved) {
  (void)reserved;
  vm = v;
  JNIEnv *env = env_();
  if (!env) return JNI_ERR;

  host_cls = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "dev/invar/Host"));
  text_cls = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "dev/invar/MBText"));
  images_cls = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "dev/invar/Images"));

  m_request_frame = (*env)->GetStaticMethodID(env, host_cls, "requestFrame", "()V");
  m_posted = (*env)->GetStaticMethodID(env, host_cls, "posted", "()V");
  m_wake_after = (*env)->GetStaticMethodID(env, host_cls, "wakeAfter", "(D)V");
  m_frames_paced = (*env)->GetStaticMethodID(env, host_cls, "framesPaced", "(D)Z");
  m_set_ime = (*env)->GetStaticMethodID(env, host_cls, "setIme", "(ZDDDDLjava/lang/String;II)V");
  m_clipboard = (*env)->GetStaticMethodID(env, host_cls, "clipboardSet", "(Ljava/lang/String;)V");
  m_clipboard_secret = (*env)->GetStaticMethodID(env, host_cls, "clipboardSecret", "(Ljava/lang/String;D)V");
  m_clipboard_secret_off = (*env)->GetStaticMethodID(env, host_cls, "clipboardSecretOff", "()V");
  m_text_menu = (*env)->GetStaticMethodID(env, host_cls, "textMenu", "(ZDDDDI)V");
  m_text_menu_own = (*env)->GetStaticMethodID(env, host_cls, "textMenuOwn", "(Ljava/lang/String;)V");
  m_log = (*env)->GetStaticMethodID(env, host_cls, "log", "(Ljava/lang/String;)V");
  m_bench = (*env)->GetStaticMethodID(env, host_cls, "benchMode", "()I");
  m_reduce_motion = (*env)->GetStaticMethodID(env, host_cls, "reduceMotion", "()I");
  m_exit = (*env)->GetStaticMethodID(env, host_cls, "exit", "(I)V");
  m_back_enabled = (*env)->GetStaticMethodID(env, host_cls, "backEnabled", "(Z)V");
  m_drive = (*env)->GetStaticMethodID(env, host_cls, "drive", "(ILjava/lang/String;)I");
  m_system_dark = (*env)->GetStaticMethodID(env, host_cls, "systemDark", "()Z");
  m_font_scale = (*env)->GetStaticMethodID(env, host_cls, "fontScale", "()F");
  m_native_pointing = (*env)->GetStaticMethodID(env, host_cls, "nativePointing", "()I");
  m_keyboard = (*env)->GetStaticMethodID(env, host_cls, "keyboard", "()I");
  m_a11y_tree = (*env)->GetStaticMethodID(env, host_cls, "a11yTree", "(Ljava/lang/String;)V");
  m_set_dark = (*env)->GetStaticMethodID(env, host_cls, "setDark", "(Z)V");
  m_set_sideways = (*env)->GetStaticMethodID(env, host_cls, "setSideways", "(Z)V");
  m_set_turnable = (*env)->GetStaticMethodID(env, host_cls, "setTurnable", "(Z)Z");
  m_word_at = (*env)->GetStaticMethodID(env, host_cls, "wordAt", "(Ljava/lang/String;I)D");
  m_open_url = (*env)->GetStaticMethodID(env, host_cls, "openUrl", "(Ljava/lang/String;)Z");
  m_cs_known = (*env)->GetStaticMethodID(env, host_cls, "charsetKnown", "(Ljava/lang/String;)Z");
  m_cs_decode = (*env)->GetStaticMethodID(env, host_cls, "charsetDecode", "([BLjava/lang/String;)Ljava/lang/String;");

  const char *a11y_env = getenv("INVAR_A11Y");
  if (a11y_env && strcmp(a11y_env, "1") == 0) {
    atomic_store_explicit(&g_a11y_enabled, 1, memory_order_release);
  }

  m_metrics = (*env)->GetStaticMethodID(env, text_cls, "fontMetrics", "(DII[D)V");
  m_shape = (*env)->GetStaticMethodID(env, text_cls, "shape", "(Ljava/lang/String;DII[D)I");
  m_glyph = (*env)->GetStaticMethodID(env, text_cls, "glyph", "(I[D)V");
  m_glyph_put = (*env)->GetStaticMethodID(env, text_cls, "glyphPut", "(III)V");
  m_generation = (*env)->GetStaticMethodID(env, text_cls, "atlasGeneration", "()I");
  m_text_key = (*env)->GetStaticMethodID(env, text_cls, "textKey", "(Ljava/lang/String;DII)I");
  m_face_add = (*env)->GetStaticMethodID(env, text_cls, "faceAdd", "(ILjava/lang/String;I)Z");
  m_face_add_bytes = (*env)->GetStaticMethodID(env, text_cls, "faceAddBytes", "(I[BI)Z");
  m_face_clear = (*env)->GetStaticMethodID(env, text_cls, "faceClear", "(I)V");
  m_ahead = (*env)->GetStaticMethodID(env, text_cls, "ahead", "([II)V");
  m_flush = (*env)->GetStaticMethodID(env, text_cls, "flush", "()V");
  m_text_scale = (*env)->GetStaticMethodID(env, text_cls, "setScale", "(F)V");
  m_ref_begin = (*env)->GetStaticMethodID(env, text_cls, "refBegin", "(DDDI)V");
  m_ref_text = (*env)->GetStaticMethodID(env, text_cls, "refText", "(Ljava/lang/String;DIIDDI[D)V");
  m_ref_end = (*env)->GetStaticMethodID(env, text_cls, "refEnd", "(Ljava/lang/String;)Z");
  m_ref_frame = (*env)->GetStaticMethodID(env, text_cls, "refFrame", "(Ljava/lang/String;)Z");

  m_img_load = (*env)->GetStaticMethodID(env, images_cls, "load", "(ILjava/lang/String;[BI)V");
  m_img_free = (*env)->GetStaticMethodID(env, images_cls, "free", "(I)V");
  m_img_reload = (*env)->GetStaticMethodID(env, images_cls, "reload", "()V");
  m_img_hold = (*env)->GetStaticMethodID(env, images_cls, "hold", "()V");
  m_img_pump = (*env)->GetStaticMethodID(env, images_cls, "pump", "()V");

  shape_buf = (*env)->NewGlobalRef(env, (*env)->NewDoubleArray(env, 4 * 8192));
  small_buf = (*env)->NewGlobalRef(env, (*env)->NewDoubleArray(env, 16));
  ahead_buf = (*env)->NewGlobalRef(env, (*env)->NewIntArray(env, 4096));

  device_init(env);
  return JNI_VERSION_1_6;
}

// ---------------------------------------------------------------------------
// Images: dev.invar.Images decodes on background threads and uploads textures
// on the GL thread. App-thread metadata (imgs) is isolated from GL-thread
// texture IDs (img_texs).

typedef struct {
  int state;  // 0 decoding, 1 ready, -1 failed or freed
  int w, h, ow, oh;
  int told;
} invar_image;

static invar_image *imgs;
static int nimgs, img_cap;
static GLuint *img_texs;
static uint8_t *img_gone;
static int img_tex_cap;

static void img_texs_hold(int id) {
  if (id < img_tex_cap) return;
  int cap = img_tex_cap ? img_tex_cap : 256;
  while (cap <= id) cap *= 2;
  img_texs = realloc(img_texs, (size_t)cap * sizeof *img_texs);
  img_gone = realloc(img_gone, (size_t)cap);
  memset(img_texs + img_tex_cap, 0, (size_t)(cap - img_tex_cap) * sizeof *img_texs);
  memset(img_gone + img_tex_cap, 0, (size_t)(cap - img_tex_cap));
  img_tex_cap = cap;
}

#define MB_STREAM_TEX 4194304
#define MB_VIEW_TEX 8388608
#define MB_LAYER_TEX 12582912

static GLuint mb_layer_gl_tex(int id);

static GLuint image_tex(int id) {
  if (id > MB_LAYER_TEX) return mb_layer_gl_tex(id - MB_LAYER_TEX);
  return id > 0 && id < img_tex_cap ? img_texs[id] : 0;
}

static void view_parts_in(int tid, int32_t start, int32_t end) {
  (void)tid;
  (void)start;
  (void)end;
}

static int one_thread = 1;

static int images_lost(JNIEnv *env) {
  static int first = 1;
  if (first) {
    first = 0;
    return 0;
  }
  if (img_tex_cap) memset(img_texs, 0, (size_t)img_tex_cap * sizeof *img_texs);
  if (one_thread && env && images_cls) {
    (*env)->CallStaticVoidMethod(env, images_cls, m_img_reload);
  }
  return 1;
}

JNIEXPORT void JNICALL Java_dev_invar_Native_imagesUntold(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  for (int i = 0; i < nimgs; i++) imgs[i].told = 0;
}

static int image_new(void) {
  if (nimgs == 0) nimgs = 1;
  if (nimgs >= img_cap) {
    img_cap = img_cap ? img_cap * 2 : 256;
    imgs = realloc(imgs, (size_t)img_cap * sizeof *imgs);
  }
  imgs[nimgs] = (invar_image){0};
  return nimgs++;
}

// ---------------------------------------------------------------------------
// GLES 3 renderer (shared gles_render.h) + dual-thread frame handoff (frame_mail.h)

#define MB_HOST_THREADS 1
#include "gles_render.h"
#include "frame_mail.h"

JNIEXPORT jboolean JNICALL Java_dev_invar_Native_surfaceCreated(JNIEnv *env, jclass c) {
  (void)c;
  gl_setup();
  gl_ready = 1;
  return images_lost(env) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_dev_invar_Native_atlasTexture(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  return (jint)atlas;
}

JNIEXPORT jint JNICALL Java_dev_invar_Native_colourAtlasTexture(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  return (jint)catlas;
}

JNIEXPORT void JNICALL Java_dev_invar_Native_resize(JNIEnv *env, jclass c, jint w, jint h) {
  (void)env;
  (void)c;
  fb_w = w;
  fb_h = h;
}

JNIEXPORT void JNICALL Java_dev_invar_Native_overOther(JNIEnv *env, jclass c, jboolean on) {
  (void)env;
  (void)c;
  over_other = on ? 1 : 0;
}

static volatile int thermal_hot;

JNIEXPORT void JNICALL Java_dev_invar_Native_thermal(JNIEnv *env, jclass c, jboolean hot) {
  (void)env;
  (void)c;
  thermal_hot = hot ? 1 : 0;
}

int32_t mb_host_thermal_throttled(void) { return thermal_hot; }

JNIEXPORT void JNICALL Java_dev_invar_Native_twoThreads(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  one_thread = 0;
}

static double app_w, app_h, app_scale;

void mb_host_render_dirty(double x, double y, double w, double h) {
  if (one_thread) gl_dirty_add(x, y, w, h);
  else fm_said(x, y, w, h);
}

void mb_host_glass_light_angle(double rad) {
  gl_set_glass_light_angle(rad);
}

void mb_host_present(double *buf, int32_t count, double r, double g, double b) {
  if (one_thread) {
    JNIEnv *env = env_();
    if (env && text_cls) (*env)->CallStaticVoidMethod(env, text_cls, m_flush);
    gl_present(buf, count, r, g, b);
    return;
  }
  float *f = fm_present(gl_frame_floats(count), count, r, g, b, app_w, app_h, app_scale);
  mb_groups_floats(f, buf, count);
}

void mb_render_present(const double *inst, int32_t count, double r, double g, double b) {
  mb_host_present((double *)inst, count, r, g, b);
}

JNIEXPORT jboolean JNICALL Java_dev_invar_Native_publish(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  return fm_publish() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_dev_invar_Native_take(JNIEnv *env, jclass c, jboolean shown) {
  (void)env;
  (void)c;
  handed_frame got;
  if (!fm_take(&got)) return JNI_FALSE;
  fm_keep(&got, shown ? 1 : 0);
  return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_dev_invar_Native_dropPending(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  fm_drop_pending();
}

JNIEXPORT void JNICALL Java_dev_invar_Native_draw(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  gl_present_kept(kept_count, kept_r, kept_g, kept_b);
}

JNIEXPORT void JNICALL Java_dev_invar_Native_trim(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  mb_render_trim();
}

JNIEXPORT void JNICALL Java_dev_invar_Native_redraw(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  if (!fbuf) {
    glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    return;
  }
  draw_kept();
}

// Synchronous GL-thread dispatch for readbacks and offscreen layer ops.
static struct {
  int kind, id;
  const void *in;
  int32_t count, x, y, w, h;
  double r, g, b, scale;
  void *out;
  int32_t result;
} sync_ask;

static int32_t image_pixels_gl(int32_t id, int w, int h, moonbit_bytes_t out);

static int32_t gl_sync_run(void) {
  switch (sync_ask.kind) {
    case 1:
      return gles_drawn_pixels(sync_ask.x, sync_ask.y, sync_ask.w, sync_ask.h, sync_ask.out);
    case 2: {
      JNIEnv *env = env_();
      if (env && text_cls) (*env)->CallStaticVoidMethod(env, text_cls, m_flush);
      return gles_pixels_of(sync_ask.in, sync_ask.count, sync_ask.r, sync_ask.g, sync_ask.b, sync_ask.scale,
                            sync_ask.w, sync_ask.h, sync_ask.out);
    }
    case 3:
      return image_pixels_gl(sync_ask.id, sync_ask.w, sync_ask.h, sync_ask.out);
    case 4: {
      JNIEnv *env = env_();
      if (env && text_cls) (*env)->CallStaticVoidMethod(env, text_cls, m_flush);
      return gl_layer_render(sync_ask.id, sync_ask.w, sync_ask.h, sync_ask.scale, (const double *)sync_ask.in,
                             sync_ask.count);
    }
    case 5:
      gl_layer_destroy(sync_ask.id);
      return 1;
  }
  return 0;
}

JNIEXPORT void JNICALL Java_dev_invar_Native_glSync(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  sync_ask.result = gl_sync_run();
}

static int32_t gl_sync(int kind, int id, const void *in, int32_t count, double r, double g, double b, double scale,
                       int32_t x, int32_t y, int32_t w, int32_t h, void *out) {
  sync_ask.kind = kind;
  sync_ask.id = id;
  sync_ask.in = in;
  sync_ask.count = count;
  sync_ask.r = r;
  sync_ask.g = g;
  sync_ask.b = b;
  sync_ask.scale = scale;
  sync_ask.x = x;
  sync_ask.y = y;
  sync_ask.w = w;
  sync_ask.h = h;
  sync_ask.out = out;
  sync_ask.result = 0;
  if (one_thread) return gl_sync_run();
  JNIEnv *env = env_();
  if (!env) return 0;
  if (!mbview_cls) {
    jclass v = (*env)->FindClass(env, "dev/invar/MBView");
    mbview_cls = (*env)->NewGlobalRef(env, v);
    (*env)->DeleteLocalRef(env, v);
    m_gl_sync = (*env)->GetStaticMethodID(env, mbview_cls, "glSync", "()Z");
  }
  jboolean ran = (*env)->CallStaticBooleanMethod(env, mbview_cls, m_gl_sync);
  return ran ? sync_ask.result : 0;
}

int32_t mb_host_layer_supported(void) { return gl_ready ? 1 : 0; }

int32_t mb_host_layer_render(int32_t id, int32_t pw, int32_t ph, double scale, const double *inst, int32_t count) {
  if (!gl_ready || pw <= 0 || ph <= 0 || !(scale > 0.0)) return 0;
  return gl_sync(4, id, inst, count, 0, 0, 0, scale, 0, 0, pw, ph, NULL);
}

void mb_host_layer_destroy(int32_t id) {
  if (!gl_ready) return;
  (void)gl_sync(5, id, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
}

int32_t mb_render_drawn_pixels(int32_t x, int32_t y, int32_t w, int32_t h, moonbit_bytes_t out) {
  if (w <= 0 || h <= 0 || x < 0 || y < 0) return 0;
  if ((size_t)Moonbit_array_length(out) < (size_t)w * (size_t)h * 4) return 0;
  return gl_sync(1, 0, NULL, 0, 0, 0, 0, 0, x, y, w, h, out);
}

int32_t mb_render_pixels_of(const double *inst, int32_t count, double r, double g, double b, double scale, int32_t w,
                            int32_t h, moonbit_bytes_t out) {
  if (count <= 0 || !(scale > 0) || w <= 0 || h <= 0) return 0;
  if ((size_t)Moonbit_array_length(out) < (size_t)w * (size_t)h * 4) return 0;
  return gl_sync(2, 0, inst, count, r, g, b, scale, 0, 0, w, h, out);
}

double mb_host_now(void);

void mb_render_init_offscreen(double w, double h, double scale) {
  JNIEnv *env = env_();
  gl_offscreen(w, h, scale);
  if (env && text_cls) (*env)->CallStaticVoidMethod(env, text_cls, m_text_scale, (jfloat)scale);
  if (env && images_cls) (*env)->CallStaticVoidMethod(env, images_cls, m_img_hold);
}

void mb_images_upload(void) {
  JNIEnv *env = env_();
  if (env && images_cls) (*env)->CallStaticVoidMethod(env, images_cls, m_img_pump);
}

void mb_render_draw_timed(const double *inst, int32_t count, double r, double g, double b, double *out) {
  static int timer = -1;
  static GLuint q;
  if (timer < 0) {
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    timer = ext && strstr(ext, "GL_EXT_disjoint_timer_query") != NULL;
    if (timer) glGenQueries(1, &q);
  }
  double t0 = mb_host_now();
  if (timer) glBeginQuery(0x88BF, q);
  mb_host_present((double *)inst, count, r, g, b);
  if (timer) glEndQuery(0x88BF);
  glFinish();
  out[0] = mb_host_now() - t0;
  out[1] = -1;
  if (timer) {
    GLuint ns = 0;
    glGetQueryObjectuiv(q, GL_QUERY_RESULT, &ns);
    GLint disjoint = 0;
    glGetIntegerv(0x8FBB, &disjoint);
    if (!disjoint) out[1] = ns / 1e6;
  }
}

void mb_render_spin(double ms) {
  mb_images_upload();
  struct timespec t = {(time_t)(ms / 1000), (long)(fmod(ms, 1000) * 1e6)};
  nanosleep(&t, NULL);
}

int mb_render_snapshot(const char *path) { return target_fb ? gl_png(path, target_fb) : 0; }

// ---------------------------------------------------------------------------
// Java -> Pion callbacks

JNIEXPORT void JNICALL Java_dev_invar_Native_start(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  char *argv[] = {"invar", NULL};
  if (invar_main) invar_main(1, argv);
}

static double frame_late;
static atomic_int post_woken;

JNIEXPORT jboolean JNICALL Java_dev_invar_Native_frame(JNIEnv *env, jclass c, jfloat w, jfloat h,
                                                       jfloat scale, jdouble now, jdouble late) {
  (void)env;
  (void)c;
  frame_late = late;
  app_w = w;
  app_h = h;
  app_scale = scale;
  if (one_thread) {
    cur_w = w;
    cur_h = h;
    cur_scale = scale;
  }
  want_frame = 0;
  frame_now = now;
  atomic_store_explicit(&post_woken, 0, memory_order_release);
  if (cb.frame) cb.frame(w, h, scale, now);
  return want_frame ? JNI_TRUE : JNI_FALSE;
}

static double event_time = -1;

JNIEXPORT void JNICALL Java_dev_invar_Native_pointer(JNIEnv *env, jclass c, jint kind, jfloat x,
                                                     jfloat y, jdouble t) {
  (void)env;
  (void)c;
  event_time = t;
  if (cb.pointer) cb.pointer(kind, x, y, 0);
  event_time = -1;
}

JNIEXPORT void JNICALL Java_dev_invar_Native_touch(JNIEnv *env, jclass c, jint kind, jint id, jfloat x, jfloat y,
                                                   jdouble t) {
  (void)env;
  (void)c;
  event_time = t;
  if (cb.pointer) cb.pointer(kind, x, y, id);
  event_time = -1;
}

JNIEXPORT void JNICALL Java_dev_invar_Native_wheel(JNIEnv *env, jclass c, jfloat x, jfloat y, jfloat dx, jfloat dy,
                                                   jint phase) {
  (void)env;
  (void)c;
  if (cb.wheel) cb.wheel(x, y, dx, dy, phase);
}

double mb_host_event_time(void) { return event_time; }
int32_t mb_host_event_finger(void) { return -1; }
double mb_host_frame_late(void) { return frame_late; }
int32_t mb_host_platform(void) { return 2; }

JNIEXPORT void JNICALL Java_dev_invar_Native_key(JNIEnv *env, jclass c, jint code, jint mods) {
  (void)env;
  (void)c;
  if (cb.key) cb.key(code, mods);
}

JNIEXPORT void JNICALL Java_dev_invar_Native_text(JNIEnv *env, jclass c, jint kind, jstring s,
                                                  jint a, jint b) {
  (void)c;
  if (cb.text) cb.text(kind, mstr(env, s), a, b);
}

static void (*state_cb)(int32_t);
void mb_host_set_app_state(void (*f)(int32_t)) { state_cb = f; }

JNIEXPORT void JNICALL Java_dev_invar_Native_appState(JNIEnv *env, jclass c, jint state) {
  (void)env;
  (void)c;
  if (state_cb) state_cb(state);
}

JNIEXPORT void JNICALL Java_dev_invar_Native_postBytes(JNIEnv *env, jclass c, jstring channel, jbyteArray json) {
  (void)c;
  const char *ch = (*env)->GetStringUTFChars(env, channel, NULL);
  jsize n = (*env)->GetArrayLength(env, json);
  jbyte *j = (*env)->GetByteArrayElements(env, json, NULL);
  mb_post(ch, (const char *)j, (size_t)n, NULL, 0);
  (*env)->ReleaseByteArrayElements(env, json, j, JNI_ABORT);
  (*env)->ReleaseStringUTFChars(env, channel, ch);
}

JNIEXPORT void JNICALL Java_dev_invar_Native_back(JNIEnv *env, jclass c, jint kind, jfloat x, jfloat progress,
                                                  jint edge) {
  (void)env;
  (void)c;
  if (back_cb) back_cb(kind, x, progress, edge);
}

// ---------------------------------------------------------------------------
// Core host API called by Pion

void mb_host_run(moonbit_string_t title, mb_frame_fn frame, mb_pointer_fn pointer,
                 mb_wheel_fn wheel, mb_key_fn key, mb_text_fn text) {
  (void)title;
  cb.frame = frame;
  cb.pointer = pointer;
  cb.wheel = wheel;
  cb.key = key;
  cb.text = text;
}

JNIEXPORT void JNICALL Java_dev_invar_Native_ownClock(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  own_clock = 1;
}

double mb_host_now(void) {
  if (own_clock) return frame_now;
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

double mb_host_wall_ms(void) {
  if (own_clock) return 1790000000000.0 + frame_now;
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int32_t mb_host_utc_offset_min(double at_ms) {
  if (own_clock) return 0;
  time_t s = (time_t)(at_ms / 1000.0);
  struct tm local;
  if (!localtime_r(&s, &local)) return 0;
  return (int32_t)(local.tm_gmtoff / 60);
}

void mb_host_request_frame(void) {
  want_frame = 1;
  JNIEnv *env = env_();
  if (env && host_cls) (*env)->CallStaticVoidMethod(env, host_cls, m_request_frame);
}

// Cross-thread wake: attach calling OS thread once (auto-detached on thread exit)
// and notify Host.posted() / Host.requestFrame().
static pthread_key_t post_attached;
static pthread_once_t post_attached_once = PTHREAD_ONCE_INIT;

static void post_detach(void *unused) {
  (void)unused;
  if (vm) (*vm)->DetachCurrentThread(vm);
}

static void post_attached_key(void) { pthread_key_create(&post_attached, post_detach); }

static JNIEnv *post_env(void) {
  if (!vm) return NULL;
  JNIEnv *env = NULL;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) return env;
  JavaVMAttachArgs args = {JNI_VERSION_1_6, "invar-post", NULL};
  if ((*vm)->AttachCurrentThread(vm, &env, &args) != JNI_OK) return NULL;
  pthread_once(&post_attached_once, post_attached_key);
  pthread_setspecific(post_attached, (void *)1);
  return env;
}

static void mb_post_wake(void) {
  if (!vm || !host_cls) return;
  if (atomic_exchange_explicit(&post_woken, 1, memory_order_acq_rel)) return;
  JNIEnv *env = post_env();
  if (!env) {
    atomic_store_explicit(&post_woken, 0, memory_order_release);
    return;
  }
  (*env)->CallStaticVoidMethod(env, host_cls, m_posted);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

void mb_host_wake_now(void) {
  if (!vm || !host_cls) return;
  JNIEnv *env = post_env();
  if (!env) return;
  (*env)->CallStaticVoidMethod(env, host_cls, m_request_frame);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

// ---------------------------------------------------------------------------
// System theme, accessibility, display & text

int32_t mb_host_system_dark(void) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return 0;
  return (*env)->CallStaticBooleanMethod(env, host_cls, m_system_dark) ? 1 : 0;
}

JNIEXPORT void JNICALL Java_dev_invar_Native_a11yEnabled(JNIEnv *env, jclass c, jboolean enabled) {
  (void)env;
  (void)c;
  atomic_store_explicit(&g_a11y_enabled, enabled ? 1 : 0, memory_order_release);
}

int32_t mb_host_a11y_on(void) {
  return atomic_load_explicit(&g_a11y_enabled, memory_order_acquire);
}

void mb_host_a11y_tree(moonbit_string_t s) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  jstring j = jstr(env, s);
  (*env)->CallStaticVoidMethod(env, host_cls, m_a11y_tree, j);
  (*env)->DeleteLocalRef(env, j);
}

double mb_host_font_scale(void) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return 1.0;
  return (double)(*env)->CallStaticFloatMethod(env, host_cls, m_font_scale);
}

int32_t mb_host_native_pointing(void) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return 1;
  return (int32_t)(*env)->CallStaticIntMethod(env, host_cls, m_native_pointing);
}

int32_t mb_host_keyboard(void) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return 0;
  return (int32_t)(*env)->CallStaticIntMethod(env, host_cls, m_keyboard);
}

void mb_host_set_dark(int32_t dark) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  (*env)->CallStaticVoidMethod(env, host_cls, m_set_dark, (jboolean)(dark != 0));
}

void mb_host_set_sideways(int32_t on) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  (*env)->CallStaticVoidMethod(env, host_cls, m_set_sideways, (jboolean)(on != 0));
}

int32_t mb_host_set_turnable(int32_t on) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return 0;
  return (*env)->CallStaticBooleanMethod(env, host_cls, m_set_turnable, (jboolean)(on != 0)) ? 1 : 0;
}

double mb_host_word_at(moonbit_string_t text, int32_t off) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return -1.0;
  jstring t = jstr(env, text);
  jdouble v = (*env)->CallStaticDoubleMethod(env, host_cls, m_word_at, t, (jint)off);
  (*env)->DeleteLocalRef(env, t);
  return v;
}

int32_t mb_host_open_url(moonbit_string_t url) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return 0;
  jstring u = jstr(env, url);
  jboolean ok = (*env)->CallStaticBooleanMethod(env, host_cls, m_open_url, u);
  (*env)->DeleteLocalRef(env, u);
  return ok ? 1 : 0;
}

int32_t mb_host_open_tab(moonbit_string_t url) {
  return mb_host_open_url(url);
}

void mb_host_over_done(int32_t open_app) { (void)open_app; }

void mb_host_hold_words(moonbit_string_t channel, moonbit_string_t title, moonbit_string_t body) {
  (void)channel;
  (void)title;
  (void)body;
}

void mb_host_wake_after(double ms) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  (*env)->CallStaticVoidMethod(env, host_cls, m_wake_after, ms);
}

void mb_host_frames_paced(double hz) {
  JNIEnv *env = env_();
  if (own_clock || want_frame || !env || !host_cls || !(*env)->CallStaticBooleanMethod(env, host_cls, m_frames_paced, hz))
    mb_host_request_frame();
}

void mb_host_font_metrics(double size, int32_t weight, int32_t style, double *out) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return;
  (*env)->CallStaticVoidMethod(env, text_cls, m_metrics, size, weight, style, small_buf);
  (*env)->GetDoubleArrayRegion(env, small_buf, 0, 6, out);
}

int32_t mb_host_shape(moonbit_string_t text, double size, int32_t weight, int32_t style, double *out) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return 0;
  jstring s = jstr(env, text);
  jint n = (*env)->CallStaticIntMethod(env, text_cls, m_shape, s, size, weight, style, shape_buf);
  (*env)->DeleteLocalRef(env, s);
  int32_t cap = Moonbit_array_length(out) / 4;
  if (n > cap) n = cap;
  if (n > 0) (*env)->GetDoubleArrayRegion(env, shape_buf, 0, n * 4, out);
  return n;
}

int32_t mb_host_text_key(moonbit_string_t text, double size, int32_t weight, int32_t style) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return -1;
  jstring s = jstr(env, text);
  jint k = (*env)->CallStaticIntMethod(env, text_cls, m_text_key, s, size, weight, style);
  (*env)->DeleteLocalRef(env, s);
  return k;
}

int32_t mb_host_face_add(int32_t face, moonbit_string_t path, int32_t weight) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return 0;
  jstring s = jstr(env, path);
  jboolean ok = (*env)->CallStaticBooleanMethod(env, text_cls, m_face_add, face, s, weight);
  (*env)->DeleteLocalRef(env, s);
  return ok ? 1 : 0;
}

int32_t mb_host_face_add_bytes(int32_t face, moonbit_bytes_t data, int32_t weight) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return 0;
  jbyteArray b = jbytes(env, data);
  jboolean ok = (*env)->CallStaticBooleanMethod(env, text_cls, m_face_add_bytes, face, b, weight);
  (*env)->DeleteLocalRef(env, b);
  return ok ? 1 : 0;
}

void mb_host_face_clear(int32_t face) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return;
  (*env)->CallStaticVoidMethod(env, text_cls, m_face_clear, face);
}

void mb_host_glyph(int32_t key, double *out) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return;
  (*env)->CallStaticVoidMethod(env, text_cls, m_glyph, key, small_buf);
  (*env)->GetDoubleArrayRegion(env, small_buf, 0, 7, out);
}

void mb_host_glyph_put(int32_t key, int32_t x, int32_t y) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return;
  (*env)->CallStaticVoidMethod(env, text_cls, m_glyph_put, key, x, y);
}

int32_t mb_host_glyph_picture(int32_t key, moonbit_bytes_t out) {
  (void)key;
  (void)out;
  return 0;
}

int32_t mb_host_atlas_put(int32_t x, int32_t y, int32_t w, int32_t h, moonbit_bytes_t px) {
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)px;
  return 0;
}

int32_t mb_host_atlas_size(int32_t colour) { return colour ? MB_ATLAS_COLOR : MB_ATLAS; }

void mb_host_glyphs_ahead(int32_t *keys, int32_t n) {
  JNIEnv *env = env_();
  if (!env || !text_cls || n <= 0) return;
  if (n > 4096) n = 4096;
  (*env)->SetIntArrayRegion(env, ahead_buf, 0, n, (const jint *)keys);
  (*env)->CallStaticVoidMethod(env, text_cls, m_ahead, ahead_buf, n);
}

int32_t mb_host_atlas_generation(void) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return 0;
  return (*env)->CallStaticIntMethod(env, text_cls, m_generation);
}

void mb_host_ref_begin(double w, double h, double scale, int32_t bg) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return;
  (*env)->CallStaticVoidMethod(env, text_cls, m_ref_begin, w, h, scale, bg);
}

void mb_host_ref_text(moonbit_string_t text, double size, int32_t weight, int32_t style, double x, double base,
                      int32_t color, double *out) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return;
  jstring s = jstr(env, text);
  (*env)->CallStaticVoidMethod(env, text_cls, m_ref_text, s, size, weight, style, x, base, color, small_buf);
  (*env)->DeleteLocalRef(env, s);
  (*env)->GetDoubleArrayRegion(env, small_buf, 0, 5, out);
}

int32_t mb_host_ref_end(moonbit_string_t name) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return 0;
  jstring s = jstr(env, name);
  jboolean ok = (*env)->CallStaticBooleanMethod(env, text_cls, m_ref_end, s);
  (*env)->DeleteLocalRef(env, s);
  return ok ? 1 : 0;
}

int32_t mb_host_ref_frame(moonbit_string_t name) {
  JNIEnv *env = env_();
  if (!env || !text_cls) return 0;
  jstring s = jstr(env, name);
  jboolean ok = (*env)->CallStaticBooleanMethod(env, text_cls, m_ref_frame, s);
  (*env)->DeleteLocalRef(env, s);
  return ok ? 1 : 0;
}

// ---------------------------------------------------------------------------
// IME, text menu, clipboard, logging & outside notifications

void mb_host_set_ime(int32_t active, double x, double y, double w, double h, moonbit_string_t text,
                     int32_t a, int32_t b) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  jstring s = jstr(env, text);
  (*env)->CallStaticVoidMethod(env, host_cls, m_set_ime, (jboolean)(active != 0), x, y, w, h, s, a, b);
  (*env)->DeleteLocalRef(env, s);
}

void mb_host_edit_can(int32_t can) { (void)can; }

void mb_host_text_menu(int32_t shown, double x, double y, double w, double h, int32_t can) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  (*env)->CallStaticVoidMethod(env, host_cls, m_text_menu, shown ? JNI_TRUE : JNI_FALSE, x, y, w, h, can);
}

void mb_host_text_menu_own(moonbit_string_t words) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  jstring j = jstr(env, words);
  (*env)->CallStaticVoidMethod(env, host_cls, m_text_menu_own, j);
  (*env)->DeleteLocalRef(env, j);
}

void mb_host_clipboard_image(moonbit_bytes_t data) { (void)data; }

void mb_host_clipboard_set(moonbit_string_t s) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  jstring j = jstr(env, s);
  (*env)->CallStaticVoidMethod(env, host_cls, m_clipboard, j);
  (*env)->DeleteLocalRef(env, j);
}

void mb_host_clipboard_secret(moonbit_string_t s, double clear_ms) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  jstring j = jstr(env, s);
  (*env)->CallStaticVoidMethod(env, host_cls, m_clipboard_secret, j, clear_ms);
  (*env)->DeleteLocalRef(env, j);
}

void mb_host_clipboard_secret_off(void) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  (*env)->CallStaticVoidMethod(env, host_cls, m_clipboard_secret_off);
}

void mb_host_log(moonbit_string_t s) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  jstring j = jstr(env, s);
  (*env)->CallStaticVoidMethod(env, host_cls, m_log, j);
  (*env)->DeleteLocalRef(env, j);
}

int32_t mb_host_own_clock(void) { return own_clock; }

int32_t mb_host_reduce_motion(void) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return 0;
  return (*env)->CallStaticIntMethod(env, host_cls, m_reduce_motion);
}

enum { INVAR_OUTSIDE_IMAGE = 1, INVAR_OUTSIDE_POSTED = 11 };
static void (*outside_cb)(int32_t, int32_t, int32_t);

int32_t mb_host_set_outside(void (*f)(int32_t, int32_t, int32_t)) {
  outside_cb = f;
  return 1;
}

JNIEXPORT void JNICALL Java_dev_invar_Native_outside(JNIEnv *env, jclass c, jint kind, jint id, jint state) {
  (void)env;
  (void)c;
  if (outside_cb) outside_cb(kind, id, state);
}

JNIEXPORT void JNICALL Java_dev_invar_Native_posted(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  atomic_store_explicit(&post_woken, 0, memory_order_release);
  if (outside_cb) outside_cb(INVAR_OUTSIDE_POSTED, 0, 0);
  else mb_host_request_frame();
}

static int awaiting_n;

void mb_host_frame_awaiting(int32_t awaiting, moonbit_string_t what) {
  (void)what;
  awaiting_n = awaiting;
}

JNIEXPORT jint JNICALL Java_dev_invar_Native_awaiting(JNIEnv *env, jclass c) {
  (void)env;
  (void)c;
  return awaiting_n;
}

int32_t mb_host_bench_mode(void) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return 0;
  return (*env)->CallStaticIntMethod(env, host_cls, m_bench);
}

void mb_host_exit(int32_t code) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  (*env)->CallStaticVoidMethod(env, host_cls, m_exit, code);
}

void mb_host_set_back(mb_back_fn f) { back_cb = f; }

void mb_host_back_enabled(int32_t on) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return;
  (*env)->CallStaticVoidMethod(env, host_cls, m_back_enabled, (jboolean)(on != 0));
}

int32_t mb_host_drive(int32_t kind, moonbit_string_t text) {
  if (kind == 9) return mb_done_watch(text);
  JNIEnv *env = env_();
  if (!env || !host_cls) return 0;
  jstring s = jstr(env, text);
  jint r = (*env)->CallStaticIntMethod(env, host_cls, m_drive, (jint)kind, s);
  (*env)->DeleteLocalRef(env, s);
  return r;
}

moonbit_string_t mb_host_env(moonbit_string_t name) {
  int n = Moonbit_array_length(name);
  char k[256];
  if (n >= (int)sizeof k) n = sizeof k - 1;
  for (int i = 0; i < n; i++) k[i] = name[i] < 128 ? (char)name[i] : '?';
  k[n] = 0;
  const unsigned char *v = (const unsigned char *)getenv(k);
  char prop[PROP_VALUE_MAX];
  if (!v && strncmp(k, "INVAR_PROBE", 11) == 0) {
    char pn[64] = "debug.invar.";
    size_t at = strlen(pn);
    for (const char *c = k + 6; *c && at + 1 < sizeof pn; c++)
      pn[at++] = (*c >= 'A' && *c <= 'Z') ? (char)(*c - 'A' + 'a') : *c;
    pn[at] = 0;
    if (__system_property_get(pn, prop) > 0) v = (const unsigned char *)prop;
  }
  if (!v) return moonbit_make_string_raw(0);
  size_t len = strlen((const char *)v);
  return pion_mb_str_from_utf8((const char *)v, (int64_t)len);
}

// ---------------------------------------------------------------------------
// Performance counters & memory

int32_t mb_host_reduce_transparency(void) { return 0; }
int32_t mb_host_increase_contrast(void) { return 0; }
int32_t mb_host_perf_state(void) { return -1; }
void mb_host_set_perf(int32_t on) { (void)on; }
void mb_host_perf_line(moonbit_string_t line) { (void)line; }
moonbit_string_t mb_host_perf_where(void) { return moonbit_make_string_raw(0); }
int32_t mb_host_perf_open(int32_t go) { (void)go; return 0; }
int32_t mb_host_reveal_file(moonbit_string_t s, int32_t go) { (void)s; (void)go; return 0; }
double mb_host_perf_size(void) { return 0; }
moonbit_bytes_t mb_host_perf_read(void) { return moonbit_make_bytes(0, 0); }

static int64_t gl_drawn_us = -1;
static int32_t gl_dropped;

JNIEXPORT void JNICALL Java_dev_invar_Native_glDrawn(JNIEnv *env, jclass c, jdouble ms, jint dropped) {
  (void)env;
  (void)c;
  __atomic_store_n(&gl_drawn_us, (int64_t)(ms * 1000.0), __ATOMIC_RELAXED);
  if (dropped > 0) __atomic_add_fetch(&gl_dropped, (int32_t)dropped, __ATOMIC_RELAXED);
}

double mb_host_gpu_ms(void) {
  int64_t us = __atomic_load_n(&gl_drawn_us, __ATOMIC_RELAXED);
  return us < 0 ? -1.0 : (double)us / 1000.0;
}

int32_t mb_host_frames_dropped(void) { return __atomic_load_n(&gl_dropped, __ATOMIC_RELAXED); }

double mb_host_cpu_ms(void) {
  struct rusage u;
  if (getrusage(RUSAGE_SELF, &u) != 0) return -1;
  return (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000.0 + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1000.0;
}

double mb_host_thread_cpu_ms(void) {
  if (own_clock) return -1.0;
  struct timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
  return -1.0;
}

double mb_host_mem_bytes(void) {
  FILE *f = fopen("/proc/self/statm", "r");
  if (!f) return 0;
  long pages = 0, rss = 0;
  if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
  fclose(f);
  return (double)rss * 4096.0;
}

JNIEXPORT jboolean JNICALL Java_dev_invar_Native_snapshot(JNIEnv *env, jclass c, jstring path) {
  (void)env;
  (void)c;
  (void)path;
  return JNI_FALSE;
}

// ---------------------------------------------------------------------------
// Images

static int32_t image_unmade(JNIEnv *env, int id, const char *what) {
  if (env && (*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
  imgs[id] = (invar_image){-1, 0, 0, 0, 0, 1};
  LOG("image %d: allocation failed for %s", id, what);
  return id;
}

int32_t mb_host_image_load(moonbit_string_t src, int32_t max_px) {
  JNIEnv *env = env_();
  int id = image_new();
  if (!env || !images_cls) return image_unmade(env, id, "no env");
  jstring s = jstr(env, src);
  if (!s) return image_unmade(env, id, "source");
  (*env)->CallStaticVoidMethod(env, images_cls, m_img_load, id, s, NULL, max_px);
  (*env)->DeleteLocalRef(env, s);
  if ((*env)->ExceptionCheck(env)) return image_unmade(env, id, "load");
  return id;
}

int32_t mb_host_image_load_bytes(moonbit_bytes_t data, int32_t max_px) {
  JNIEnv *env = env_();
  int id = image_new();
  if (!env || !images_cls) return image_unmade(env, id, "no env");
  jsize n = Moonbit_array_length(data);
  jbyteArray b = (*env)->NewByteArray(env, n);
  if (!b) return image_unmade(env, id, "bytes");
  if (n > 0) (*env)->SetByteArrayRegion(env, b, 0, n, (const jbyte *)data);
  jstring s = (*env)->NewStringUTF(env, "(bytes)");
  if (!s) {
    (*env)->DeleteLocalRef(env, b);
    return image_unmade(env, id, "name");
  }
  (*env)->CallStaticVoidMethod(env, images_cls, m_img_load, id, s, b, max_px);
  (*env)->DeleteLocalRef(env, s);
  (*env)->DeleteLocalRef(env, b);
  if ((*env)->ExceptionCheck(env)) return image_unmade(env, id, "load");
  return id;
}

int32_t mb_host_image_info(int32_t id, double *out) {
  if (id <= 0 || id >= nimgs) return -1;
  invar_image *im = &imgs[id];
  if (im->state == 1) {
    out[0] = im->w;
    out[1] = im->h;
    out[2] = im->ow;
    out[3] = im->oh;
  }
  return im->state;
}

void mb_host_image_free(int32_t id) {
  if (id <= 0 || id >= nimgs) return;
  imgs[id] = (invar_image){-1, 0, 0, 0, 0, 1};
  JNIEnv *env = env_();
  if (env && images_cls) (*env)->CallStaticVoidMethod(env, images_cls, m_img_free, id);
}

JNIEXPORT void JNICALL Java_dev_invar_Native_imageGone(JNIEnv *env, jclass c, jint id) {
  (void)env;
  (void)c;
  if (id <= 0) return;
  img_texs_hold(id);
  if (img_texs[id]) glDeleteTextures(1, &img_texs[id]);
  img_texs[id] = 0;
  img_gone[id] = 1;
}

static int32_t image_pixels_gl(int32_t id, int w, int h, moonbit_bytes_t out) {
  GLuint tex = id > 0 && id < img_tex_cap ? img_texs[id] : 0;
  if (!tex) return 0;
  GLint was = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was);
  GLuint fb = 0;
  glGenFramebuffers(1, &fb);
  glBindFramebuffer(GL_FRAMEBUFFER, fb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (ok) {
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)was);
  glDeleteFramebuffers(1, &fb);
  return ok;
}

int32_t mb_host_image_pixels(int32_t id, moonbit_bytes_t out) {
  if (id <= 0 || id >= nimgs) return 0;
  invar_image *im = &imgs[id];
  if (im->state != 1) return 0;
  if ((size_t)Moonbit_array_length(out) < (size_t)im->w * (size_t)im->h * 4) return 0;
  return gl_sync(3, id, NULL, 0, 0, 0, 0, 0, 0, 0, im->w, im->h, out);
}

JNIEXPORT jboolean JNICALL Java_dev_invar_Native_imageDone(JNIEnv *env, jclass c, jint id, jint tex) {
  (void)env;
  (void)c;
  GLuint t = (GLuint)tex;
  if (id <= 0) {
    if (t) glDeleteTextures(1, &t);
    return JNI_FALSE;
  }
  img_texs_hold(id);
  if (img_gone[id]) {
    if (t) glDeleteTextures(1, &t);
    return JNI_FALSE;
  }
  if (img_texs[id] && img_texs[id] != t) glDeleteTextures(1, &img_texs[id]);
  img_texs[id] = t;
  return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_dev_invar_Native_imageTold(JNIEnv *env, jclass c, jint id, jboolean ok, jint w, jint h,
                                                       jint ow, jint oh) {
  (void)env;
  (void)c;
  if (id <= 0 || id >= nimgs || imgs[id].state == -1) return;
  int told = imgs[id].told;
  imgs[id] = (invar_image){ok ? 1 : -1, w, h, ow, oh, 1};
  if (told) return;
  if (outside_cb) outside_cb(INVAR_OUTSIDE_IMAGE, id, ok ? 1 : -1);
}

// ---------------------------------------------------------------------------
// Charset decoding (java.nio.charset.Charset via Host)

int32_t mb_host_charset_known(moonbit_string_t charset) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return 0;
  jstring c = jstr(env, charset);
  jboolean ok = (*env)->CallStaticBooleanMethod(env, host_cls, m_cs_known, c);
  (*env)->DeleteLocalRef(env, c);
  return ok ? 1 : 0;
}

moonbit_string_t mb_host_charset_decode(moonbit_bytes_t data, moonbit_string_t charset) {
  JNIEnv *env = env_();
  if (!env || !host_cls) return moonbit_make_string_raw(0);
  jstring c = jstr(env, charset);
  jbyteArray b = jbytes(env, data);
  jstring s = (jstring)(*env)->CallStaticObjectMethod(env, host_cls, m_cs_decode, b, c);
  moonbit_string_t out = s ? mstr(env, s) : moonbit_make_string_raw(0);
  if (s) (*env)->DeleteLocalRef(env, s);
  (*env)->DeleteLocalRef(env, b);
  (*env)->DeleteLocalRef(env, c);
  return out;
}

// ---------------------------------------------------------------------------
// Desktop window & pointer APIs (no-ops on Android full-screen Activity)

void mb_host_set_window_zoom(double zoom) { (void)zoom; }
void mb_host_set_window_key(int32_t (*f)(int32_t, int32_t)) { (void)f; }
void mb_host_set_window_close(int32_t (*f)(void)) { (void)f; }
void mb_host_window_hold(void) {}
void mb_host_window_show(void) {}
void mb_host_window_center(void) {}
void mb_host_window_hide(void) {}
void mb_host_window_setup(double min_w, double min_h, double w, double h, int32_t inset) {
  (void)min_w; (void)min_h; (void)w; (void)h; (void)inset;
}
int32_t mb_host_window_inset(void) { return 0; }
int32_t mb_host_full_screen(void) { return 0; }
void mb_host_set_full_screen(int32_t on) { (void)on; }
void mb_host_window_buttons_down(double by, double ms) { (void)by; (void)ms; }
double mb_host_window_corner(void) { return 0; }
double mb_host_window_corner_at(int k) { (void)k; return 0; }
void mb_host_window_drag(void) {}
void mb_host_window_act(int32_t what) { (void)what; }
int32_t mb_host_window_zoomed(void) { return 0; }
void mb_host_window_snap(double x, double y, double w, double h) { (void)x; (void)y; (void)w; (void)h; }
int32_t mb_host_window_snap_state(void) { return 0; }
int32_t mb_host_window_opens(void) { return 0; }
int32_t mb_host_window_open(moonbit_string_t what) { (void)what; return 0; }
int32_t mb_host_window_front(moonbit_string_t what) { (void)what; return 0; }
int32_t mb_host_windows(int32_t op, int32_t wid, moonbit_string_t what) {
  (void)op; (void)wid; (void)what;
  return 0;
}
void mb_host_set_cursor(int32_t kind) { (void)kind; }
void mb_host_set_badge(int32_t n) { (void)n; }
void mb_host_menu_setup(moonbit_string_t json) { (void)json; }
void mb_host_set_site_name(moonbit_string_t name) { (void)name; }
int32_t mb_host_auto_launch(void) { return 0; }
int32_t mb_host_set_auto_launch(int32_t enabled) { (void)enabled; return 0; }
int32_t mb_host_hotkey_set(moonbit_string_t id, moonbit_string_t accel) { (void)id; (void)accel; return 0; }
void mb_host_hotkey_clear(moonbit_string_t id) { (void)id; }
int32_t mb_host_face_add_family(int32_t face, moonbit_string_t family) { (void)face; (void)family; return 0; }
moonbit_string_t mb_host_local_font_families(void) { return moonbit_make_string_raw(0); }
int32_t mb_host_mods_held(void) { return 0; }
void mb_host_pointer_lock(int32_t on) { (void)on; }
int32_t mb_host_pointer_locked(void) { return 0; }
void mb_host_pointer_delta(double *out) { out[0] = 0; out[1] = 0; }
int32_t mb_host_gamepads(double *out) { (void)out; return 0; }

// ---------------------------------------------------------------------------
// Optional platform extensions (default no-op stubs in core framework host)

int32_t mb_host_share(moonbit_string_t text, moonbit_string_t name, moonbit_string_t mime, moonbit_bytes_t data) {
  (void)text; (void)name; (void)mime; (void)data;
  return 0;
}
moonbit_string_t mb_host_shared_in(void) { return moonbit_make_string_raw(0); }
moonbit_bytes_t mb_host_shared_in_file(int32_t i) { (void)i; return moonbit_make_bytes(0, 0); }

int32_t mb_host_app_version(void) { return 0; }
void mb_host_update_fetch(moonbit_string_t url, moonbit_string_t sha256, double size, moonbit_string_t name,
                          int32_t now, moonbit_string_t delta) {
  (void)url; (void)sha256; (void)size; (void)name; (void)now; (void)delta;
}
void mb_host_update_apply(moonbit_string_t said) { (void)said; }
void mb_host_install_package(moonbit_string_t name, moonbit_bytes_t data) { (void)name; (void)data; }
moonbit_string_t mb_host_update_state(void) { return moonbit_make_string_raw(0); }

int32_t mb_host_browser_login_start(moonbit_string_t path, moonbit_string_t done) { (void)path; (void)done; return 0; }
int32_t mb_host_browser_login_port(int32_t id) { (void)id; return 0; }
int32_t mb_host_browser_login_open(int32_t id, moonbit_string_t url) { (void)id; (void)url; return 0; }
moonbit_string_t mb_host_browser_login_back(int32_t id) { (void)id; return moonbit_make_string_raw(0); }
void mb_host_browser_login_end(int32_t id) { (void)id; }

int32_t mb_host_passwords_able(void) { return 0; }
int32_t mb_host_passwords_run(moonbit_string_t args) { (void)args; return 0; }
moonbit_string_t mb_host_passwords_out(int32_t id) { (void)id; return moonbit_make_string_raw(0); }
void mb_host_passwords_end(int32_t id) { (void)id; }

int32_t mb_host_autofill_able(void) { return 0; }
int32_t mb_host_autofill_ask(moonbit_string_t host) { (void)host; return 0; }
moonbit_string_t mb_host_autofill_take(void) { return moonbit_make_string_raw(0); }
void mb_host_autofill_end(void) {}

int32_t mb_host_passkeys_able(void) { return 0; }
int32_t mb_host_passkeys_get(moonbit_string_t ask) { (void)ask; return 0; }
moonbit_string_t mb_host_passkeys_out(int32_t id) { (void)id; return moonbit_make_string_raw(0); }
void mb_host_passkeys_end(int32_t id) { (void)id; }

int32_t mb_host_pick_start(int32_t kind) { (void)kind; return 0; }
int32_t mb_host_pick_state(int32_t id) { (void)id; return -1; }
int32_t mb_host_pick_count(int32_t id) { (void)id; return 0; }
moonbit_string_t mb_host_pick_name(int32_t id, int32_t i) { (void)id; (void)i; return moonbit_make_string_raw(0); }
moonbit_string_t mb_host_pick_info(int32_t id, int32_t i) { (void)id; (void)i; return moonbit_make_string_raw(0); }
moonbit_bytes_t mb_host_pick_bytes(int32_t id, int32_t i) { (void)id; (void)i; return moonbit_make_bytes(0, 0); }
void mb_host_pick_end(int32_t id) { (void)id; }
int32_t mb_host_pick_photo(moonbit_string_t src) { (void)src; return 0; }

int32_t mb_host_keep_begin(moonbit_string_t name, moonbit_string_t mime, int32_t way) {
  (void)name; (void)mime; (void)way;
  return 0;
}
int32_t mb_host_keep_append(int32_t id, moonbit_bytes_t data) { (void)id; (void)data; return 0; }
void mb_host_keep_finish(int32_t id) { (void)id; }
int32_t mb_host_keep_state(int32_t id) { (void)id; return -1; }
moonbit_string_t mb_host_keep_where(int32_t id) { (void)id; return moonbit_make_string_raw(0); }
void mb_host_keep_end(int32_t id) { (void)id; }

int32_t mb_host_gallery_access(void) { return 0; }
void mb_host_gallery_ask(void) {}
int32_t mb_host_gallery_answers(void) { return 0; }
int32_t mb_host_gallery_read(void) { return 0; }
int32_t mb_host_gallery_read_state(int32_t id) { (void)id; return -1; }
moonbit_string_t mb_host_gallery_read_rows(int32_t id) { (void)id; return moonbit_make_string_raw(0); }
void mb_host_gallery_read_end(int32_t id) { (void)id; }

int32_t mb_host_marks_start(moonbit_bytes_t image, moonbit_string_t marks) { (void)image; (void)marks; return 0; }
int32_t mb_host_marks_state(int32_t id) { (void)id; return -1; }
moonbit_bytes_t mb_host_marks_png(int32_t id) { (void)id; return moonbit_make_bytes(0, 0); }

void mb_host_notice_show(moonbit_string_t channel, moonbit_string_t tag, moonbit_string_t title,
                         moonbit_string_t body, moonbit_string_t opens) {
  (void)channel; (void)tag; (void)title; (void)body; (void)opens;
}
void mb_host_notice_cancel(moonbit_string_t tag) { (void)tag; }
int32_t mb_host_notice_state(int32_t what) { (void)what; return 0; }
moonbit_string_t mb_host_notice_text(int32_t what) { (void)what; return moonbit_make_string_raw(0); }

int32_t mb_host_nav_bar(moonbit_string_t spec) { (void)spec; return 0; }
int32_t mb_host_nav_able(void) { return 0; }

int32_t mb_host_view_open(int32_t kind, moonbit_string_t arg) { (void)kind; (void)arg; return 0; }
void mb_host_view_place(int32_t id, double x, double y, double w, double h, double cx0, double cy0, double cx1,
                        double cy1, double r0, double r1, double r2, double r3, int32_t shown) {
  (void)id; (void)x; (void)y; (void)w; (void)h;
  (void)cx0; (void)cy0; (void)cx1; (void)cy1;
  (void)r0; (void)r1; (void)r2; (void)r3; (void)shown;
}
void mb_host_view_close(int32_t id) { (void)id; }
void mb_host_view_control(int32_t id, int32_t op, double value) { (void)id; (void)op; (void)value; }
int32_t mb_host_view_info(int32_t id, double *out) { (void)id; (void)out; return -1; }
int32_t mb_host_view_read_next(int32_t id, double *out) { (void)id; (void)out; return 0; }
void mb_host_view_read_answer(int32_t id, int32_t req, moonbit_bytes_t data, double total, int32_t more) {
  (void)id; (void)req; (void)data; (void)total; (void)more;
}
moonbit_string_t mb_host_view_site_next(int32_t id) { (void)id; return moonbit_make_string_raw(0); }
moonbit_bytes_t mb_host_view_site_body(int32_t id, int32_t req) { (void)id; (void)req; return moonbit_make_bytes(0, 0); }
void mb_host_view_site_head(int32_t id, int32_t req, int32_t status, moonbit_string_t headers) {
  (void)id; (void)req; (void)status; (void)headers;
}
void mb_host_view_site_chunk(int32_t id, int32_t req, moonbit_bytes_t data) { (void)id; (void)req; (void)data; }
void mb_host_view_site_end(int32_t id, int32_t req, moonbit_string_t error) { (void)id; (void)req; (void)error; }
moonbit_string_t mb_host_view_site_state(int32_t id) { (void)id; return moonbit_make_string_raw(0); }
void mb_host_view_site_load(int32_t id, moonbit_string_t path) { (void)id; (void)path; }
void mb_host_view_tell(int32_t id, int32_t what, moonbit_string_t text) { (void)id; (void)what; (void)text; }
moonbit_string_t mb_host_view_text(int32_t id, int32_t what) { (void)id; (void)what; return moonbit_make_string_raw(0); }
int32_t mb_host_view_text_known(int32_t id, int32_t what) { (void)id; (void)what; return 0; }
moonbit_bytes_t mb_host_view_still(int32_t id) { (void)id; return moonbit_make_bytes(0, 0); }
int32_t mb_host_save_file(moonbit_string_t name, moonbit_bytes_t data) { (void)name; (void)data; return 0; }

int32_t mb_host_pdf_open(moonbit_bytes_t data) { (void)data; return 0; }
int32_t mb_host_pdf_pages(int32_t doc) { (void)doc; return 0; }
void mb_host_pdf_page_size(int32_t doc, int32_t page, double *out) { (void)doc; (void)page; out[0] = 0; out[1] = 0; }
int32_t mb_host_pdf_page_load(int32_t doc, int32_t page, int32_t max_px) { (void)doc; (void)page; (void)max_px; return 0; }
void mb_host_pdf_close(int32_t doc) { (void)doc; }

int32_t mb_host_stream_open(void) { return 0; }
void mb_host_stream_h264(int32_t id, moonbit_bytes_t avcc, int32_t w, int32_t h) { (void)id; (void)avcc; (void)w; (void)h; }
void mb_host_stream_frame(int32_t id, moonbit_bytes_t data, int32_t key, double ts) { (void)id; (void)data; (void)key; (void)ts; }
void mb_host_stream_jpeg(int32_t id, moonbit_bytes_t data, double ts) { (void)id; (void)data; (void)ts; }
moonbit_string_t mb_host_stream_next(int32_t id) { (void)id; return moonbit_make_string_raw(0); }
void mb_host_stream_show(int32_t id, double ts) { (void)id; (void)ts; }
void mb_host_stream_close(int32_t id) { (void)id; }
int32_t mb_host_stream_feeds(void) { return 0; }
void mb_host_stream_codec(int32_t id, moonbit_string_t cmd, moonbit_bytes_t data) { (void)id; (void)cmd; (void)data; }
int32_t mb_host_h264_here(void) { return 0; }
int32_t mb_host_stream_audio_here(void) { return 0; }
void mb_host_stream_audio(int32_t id, moonbit_bytes_t data) { (void)id; (void)data; }
int32_t mb_host_stream_layers(void) { return 0; }
void mb_host_stream_pointer(int32_t id, moonbit_bytes_t png, double w, double h, double hx, double hy) {
  (void)id; (void)png; (void)w; (void)h; (void)hx; (void)hy;
}
void mb_host_stream_pointer_at(int32_t id, double x, double y, int32_t shown) { (void)id; (void)x; (void)y; (void)shown; }
int32_t mb_host_h265_here(void) { return 0; }
void mb_host_stream_hevc(int32_t id, moonbit_bytes_t hvcc, int32_t w, int32_t h) { (void)id; (void)hvcc; (void)w; (void)h; }
int32_t mb_host_av1_here(void) { return 0; }
void mb_host_stream_av1(int32_t id, moonbit_bytes_t av1c, int32_t w, int32_t h) { (void)id; (void)av1c; (void)w; (void)h; }
void mb_host_stream_direct(int32_t sid, double route, double after, int32_t what) { (void)sid; (void)route; (void)after; (void)what; }
int32_t mb_host_stream_frame_shown(int32_t id, moonbit_bytes_t data, int32_t key, double ts, int32_t show) {
  (void)id; (void)data; (void)key; (void)ts; (void)show;
  return 0;
}
void mb_host_stream_flush(int32_t id, int32_t drain) { (void)id; (void)drain; }
double mb_host_stream_audio_played(int32_t id) { (void)id; return 0.0; }
double mb_host_stream_audio_queued(int32_t id) { (void)id; return 0.0; }
double mb_host_stream_audio_wanted(int32_t id) { (void)id; return 0.0; }
void mb_host_stream_audio_volume(int32_t id, double v) { (void)id; (void)v; }

#include "android_device.h"
