// Metal renderer: one instanced quad per scene instance (some drawn in strips: bands.h), by one shader made a pipeline
// for each kind it draws most (kind_pipe).
#import "apple.h"
#include "corner_sl.h"
#include "contained.h"
#include "bands.h"
#include "groups.h"
#include "atlas_pages.h"
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <zlib.h>

// the numbers of an instance (src/ui/scene.mbt)
#define MB_INST 60
#include "glass_damage.h"

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> mpipe;
static id<MTLRenderPipelineState> rpipe_off;
// fast path for simple axis-aligned, sharp-cornered, borderless rectangles (frect)
static id<MTLRenderPipelineState> rpipe_rect;
static id<MTLRenderPipelineState> rpipe_rect_off;
// holes for native views (kind 9): written, not blended
static id<MTLRenderPipelineState> hpipe;
// a ground's flat middle (bands.h mb_pieces)
static id<MTLRenderPipelineState> fpipe;
// offscreen physical-pixel tile layers (MB_LAYER_TEX + id)
static NSMutableDictionary<NSNumber *, id<MTLTexture>> *layer_texs;
// the words' glyphs (R8) and colour glyphs (RGBA), arrays of square pages (apple.h, atlas_pages.h); what stands in a
// texture2d slot nothing is bound to (an instance with no picture): 1 texel, clear
static id<MTLTexture> atlas, catlas, stand_tex;
static id<MTLBuffer> bufs[3];
static size_t buf_cap[3];
static int buf_i;
static dispatch_semaphore_t inflight;
// What of a frame is said to differ from the frame before (partial redraw, below): told at all, all of it, or its parts.
#define MB_PARTS 8
// the captures of glass over native views a frame has at most (glass_through)
#define MB_THROUGH 16
typedef struct {
  int told, whole, n, glass_closure;
  double r[MB_PARTS][4];
} MBDirty;

// A frame handed from the main thread to the presenting queue (present_q, below): its instances as floats (a buffer of
// pool_take's), its ground, the size it was made at and the drawable's then, what of it differs from the frame before,
// whether it has glass and which of its captures frost a native view (glass_through), the atlas's puts it needs
// (atlas_seq), the textures of its pictures and streams as they were when it was made (mb_image_texture and
// mb_stream_texture are the main thread's alone), and its frame of a latency run (mac_latency.h).
@interface MBFrame : NSObject {
 @public
  float *inst;
  size_t cap;
  int32_t count;
  double bg[3], w, h, scale;
  int tw, th, glass, lat, clear;
  MBDirty parts;
  int through_n;
  float through_tid[MB_THROUGH];
  uint64_t atlas_seq;
  NSMutableDictionary<NSNumber *, id> *texs;
}
@end

// What is a window's own (mac_app.m: the windows of one run, each its view and its layer; the device, the queue, the
// pipelines, the atlases and the ring of instance buffers are all the windows'). The main thread's: its layer and its
// size (mb_render_set_size's); the last frame, kept for snapshots and compared with the next (its instances and ground:
// keep_last's); the frame last handed to be shown: its size, and whether it shows a stream (whose pictures change under
// the same instances); what was said to differ since (`said`). The presenting queue's (present_q): the canvas its frames
// are drawn into, whether it holds the last drawn; the frame waiting to be drawn (`pending`, under present_mu) and
// whether a drawing of it is queued; whether a frame found no drawable (`missed`: the next is not skipped as shown). Put
// in place for a window by mb_render_use.
#define MB_ADAPTIVE_MAX 16
#define MB_AD_TAU 0.070
#define MB_AD_SETTLE_SEC 0.42
#define MB_AD_MATCH_DIST 64.0

typedef struct {
  int active;
  float cx, cy, hx, hy;
  int is_dark;
  int is_inverting;
  int through;
  float tint_a;
  double settle_left;
  uint64_t updated_serial;
  uint64_t seen_serial;
} MBAdSlot;

typedef struct {
  CAMetalLayer *layer;
  double w, h, scale;
  float *inst;
  int32_t count;
  size_t cap;
  double bg[3];
  double shown_w, shown_h, shown_scale;
  int shown_ok, shown_streams;
  MBDirty said;
  id<MTLTexture> cv;
  int cv_ok;
  MBFrame *pending;
  int queued;
  atomic_int missed;
  int clear;  // window drawn on transparent ground (mb_render_set_clear)
  id<MTLTexture> ad_tex[2];
  int ad_cur;
  MBAdSlot ad_slots[MB_ADAPTIVE_MAX];
  double ad_last_time;
  uint64_t ad_frame_serial;
  atomic_int ad_settling;
  atomic_int ad_drawing;
  atomic_int ad_has_adaptive;
  MBDirty ad_settle_dirty;
  int last_through_n;
  float last_through_tid[MB_THROUGH];
} MBRenderWin;
static MBRenderWin render_wins[MB_WINS] = {[0 ... MB_WINS - 1] = {.w = 800, .h = 600, .scale = 2}};
static MBRenderWin *rw = &render_wins[0];
#define mlayer (rw->layer)
#define vw (rw->w)
#define vh (rw->h)
#define vscale (rw->scale)
#define last_inst (rw->inst)
#define last_count (rw->count)
#define last_cap (rw->cap)
#define last_bg (rw->bg)
#define shown_w (rw->shown_w)
#define shown_h (rw->shown_h)
#define shown_scale (rw->shown_scale)
#define shown_ok (rw->shown_ok)
#define shown_streams (rw->shown_streams)
#define canvas (rw->cv)
#define canvas_ok (rw->cv_ok)
#define dirty (rw->said.r)
#define dirty_n (rw->said.n)
#define dirty_whole (rw->said.whole)
#define dirty_told (rw->said.told)

// The window of the run drawn now (mac_app.m's win_use): its layer, its size, its last frame and its canvas in place.
void mb_render_use(int wid) {
  if (wid >= 0 && wid < MB_WINS) rw = &render_wins[wid];
}

// Window `wid` drawn on transparent ground (its layer not opaque): drawn whole each frame.
void mb_render_set_clear(int wid, int on) {
  if (wid >= 0 && wid < MB_WINS) render_wins[wid].clear = on != 0;
}

// ---------------------------------------------------------------------------
// Presenting off the main thread. Waiting for a buffer of the ring (the GPU three frames behind), for a drawable (the
// display's swapchain full: a GPU slowed by its heat, a busy window server) and encoding a frame are done on a serial
// queue of their own (present_q), not on the main thread that takes input and builds the next frame: there such a wait
// held the touches, a glide and the next frame's layout for 16-50 ms. The main thread makes a frame's floats, compares
// it with the last, keeps it as the last and hands it over (MBFrame) as the window's one frame waiting (`pending`): a
// frame made while the one before still waits takes its place, what each said to differ joined (dirty_join), so a slow
// GPU draws the newest frame, never a queue of old ones behind the hand. What the main thread alone may touch stays
// there: the pictures' and streams' textures are looked up as the frame is made (frame_texs), the glyphs it puts in the
// atlases are kept in order and put in by the queue before the frame that needs them (atlas_puts). A frame presented
// with Core Animation's transaction (a window resized, shown first or turned; native views moved with it; the layer's
// opacity changed) or checked (MBUI_PARTIAL_CHECK) is drawn on the main thread as before, once the queue has drawn what
// it holds (present_drain); so is all that reads back or lets go of what drawing holds.
static dispatch_queue_t present_q;
static os_unfair_lock present_mu = OS_UNFAIR_LOCK_INIT;

// Everything handed to present_q drawn, its state the caller's (the main thread's) till it hands more.
static void present_drain(void) {
  if (present_q) dispatch_sync(present_q, ^{
                 });
}

// The buffers frames' floats are made into, given back as a frame is drawn or replaced: a few kept, the rest freed.
#define MB_POOL 3
static float *pool_bufs[MB_POOL];
static size_t pool_caps[MB_POOL];

static float *pool_take(size_t need, size_t *cap) {
  float *got = NULL;
  size_t got_cap = 0;
  os_unfair_lock_lock(&present_mu);
  for (int i = 0; i < MB_POOL; i++)
    if (pool_bufs[i] && (!got || pool_caps[i] >= need)) {
      float *b = pool_bufs[i];
      size_t c = pool_caps[i];
      pool_bufs[i] = got;
      pool_caps[i] = got_cap;
      got = b;
      got_cap = c;
      if (got_cap >= need) break;
    }
  os_unfair_lock_unlock(&present_mu);
  if (got_cap < need) {
    free(got);
    got_cap = need + need / 4;
    got = malloc(got_cap);
  }
  *cap = got_cap;
  return got;
}

static void pool_give(float *b, size_t cap) {
  if (!b) return;
  os_unfair_lock_lock(&present_mu);
  for (int i = 0; i < MB_POOL; i++)
    if (!pool_bufs[i]) {
      pool_bufs[i] = b;
      pool_caps[i] = cap;
      b = NULL;
      break;
    }
  os_unfair_lock_unlock(&present_mu);
  free(b);
}

@implementation MBFrame
- (void)dealloc {
  pool_give(inst, cap);
}
@end

// What window `wid` kept let go of, as it closes: the next window given its number starts as none was drawn.
void mb_render_forget(int wid) {
  if (wid <= 0 || wid >= MB_WINS) return;
  present_drain();
  MBRenderWin *w = &render_wins[wid];
  free(w->inst);
  *w = (MBRenderWin){.w = 800, .h = 600, .scale = 2};
}

// A part said to differ added to `d`: past as many as it keeps, the last grown round it.
static void dirty_add(MBDirty *d, double x, double y, double w, double h) {
  d->told = 1;
  if (w < 0 || h < 0) {
    d->whole = 1;
  } else if (w > 0 && h > 0) {
    if (d->n == MB_PARTS) {
      double *e = d->r[MB_PARTS - 1];
      double x1 = fmax(e[0] + e[2], x + w), y1 = fmax(e[1] + e[3], y + h);
      e[0] = fmin(e[0], x);
      e[1] = fmin(e[1], y);
      e[2] = x1 - e[0];
      e[3] = y1 - e[1];
    } else {
      double *e = d->r[d->n++];
      e[0] = x; e[1] = y; e[2] = w; e[3] = h;
    }
  }
}

// What a frame said to differ from the one before joined with what the frame it replaces (`was`, never drawn) said:
// all of it where either was drawn whole, else both's parts (what differs from the frame drawn last is in one or the
// other).
static void dirty_join(MBDirty *d, const MBDirty *was) {
  d->glass_closure |= was->glass_closure;
  if (!d->told || d->whole) return;
  if (!was->told || was->whole) {
    d->whole = 1;
    return;
  }
  for (int i = 0; i < was->n; i++) dirty_add(d, was->r[i][0], was->r[i][1], was->r[i][2], was->r[i][3]);
}

// The glyphs put in the atlases (mb_atlas_upload, the main thread's), kept in order till the queue puts them in, each
// numbered: a frame is drawn after the puts made before it and none after (a glyph put for the next frame where an
// atlas started over could be where one of this frame's was).
typedef struct {
  uint64_t seq;
  int color, x, y, w, h;
  uint8_t *px;
  // (a PAGES op, px NULL: the atlas, `pages` pages from here on, `tex` made for it (CFBridgingRetain'd))
  int pages;
  void *tex;
} AtlasPut;
// the PAGES ops kept and not yet done (under present_mu)
static int atlas_ops_owed;
static void atlas_ops_kick(void);
static AtlasPut *atlas_puts;
static size_t atlas_puts_n, atlas_puts_cap, atlas_puts_head;
static uint64_t atlas_seq;
// glass (src/ui/glass.mbt): the passes that blur what is under it (into the copy, 8 bits a channel, and into the rest,
// half floats), the copies they work in (level j is 1/2^j of the largest capture yet, the glass and the blur's margin
// round it, not of the target: a composer's strip, not the window; tmp for a Gaussian's first half), the GPU's time of
// the last frame done
static id<MTLRenderPipelineState> ppipe, ppipe_deep, cpipe, adpipe;
static id<MTLTexture> lv[4], ltmp;
static int lv_w, lv_h;
static double last_gpu = -1;
static double ad_fixed_dt = 0.0;
// The size, in points, and the pixels a point of the frame being drawn (encode and what it draws by): its MBFrame's on
// present_q, the window's or a picture's own (read_of) on the main thread.
static double at_w = 800, at_h = 600, at_scale = 2;
// The textures of its pictures and streams as its MBFrame found them (frame_texs; NSNull none yet), or nil: looked up
// as it is drawn (the main thread's own drawing).
static NSDictionary<NSNumber *, id> *at_texs;

static NSString *shader_src = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"// the kind of what this pipeline draws, fixed (kind_pipe); -1 any, as the instance says\n"
"constant int one_kind [[function_constant(0)]];\n"
"// scene.mbt's 60 numbers: clip, rc and rs the rounded rect it is cut to (its rect x0 y0 x1 y1, its corners' radii,\n"
"// how smooth they are); rot how it is turned (radians, about rot.yz; its clip where it is drawn when rot.w is 1); rr\n"
"// a box's corners' radii (top left, top right, bottom right, bottom left); oclip, orc, ors the rounded clip outside\n"
"// its own that its own cuts through, as clip, rc, rs (none where orc is 0); fade where it fades out towards a cut\n"
"// (from its top: 0 at x, whole y below it; from its foot: 0 at z, whole w above it; none where y, w are 0), fadex\n"
"// the same across (from its left, from its right)\n"
"struct Inst { float4 rect; float4 p; float4 color; float4 c2; float4 clip; float4 pad; float4 rc; float4 rs; float4 rot; float4 rr; float4 oclip; float4 orc; float4 ors; float4 fade; float4 fadex; };\n"
"struct U { float2 vp; float scale; float pad; float2 origin; float2 _pad2; };\n"
"// glass: where a point's backdrop is in the blurred copy (uv = p * map.xy + map.zw, kept in uvc), a liquid group's\n"
"// bounds, shapes and corners, how many (misc.x); misc.y 1 where what it copied of a native view's hole is let through\n"
"// (glass_through)\n"
"struct G { float4 map; float4 uvc; float4 gb; float4 shape[8]; float4 rad[8]; float4 misc; float4 ad_box[16]; float4 ad_meta; };\n"
"struct VOut { float4 pos [[position]]; float2 rot_cs [[flat]]; uint iid [[flat]]; };\n"
"vertex VOut vmain(uint vid [[vertex_id]], uint iid [[instance_id]],\n"
"    const device Inst* inst [[buffer(0)]], constant U& u [[buffer(1)]], constant G& g [[buffer(2)]]) {\n"
"  Inst I = inst[iid];\n"
"  float2 corner = float2((vid == 1 || vid == 4 || vid == 5) ? 1.0 : 0.0,\n"
"                         (vid == 2 || vid == 3 || vid == 5) ? 1.0 : 0.0);\n"
"  int kind = int(I.p.y + 0.5);\n"
"  bool merged_liquid = kind == 6 && I.pad.w > 0.0;\n"
"  float grow = kind == 2 ? I.p.w * 3.0 + 2.0 : (kind == 6 && I.pad.w <= 0.0) ? 2.0 : ((kind == 0 || kind == 4 || kind == 5 || kind == 8) ? 1.0 : 0.0);\n"
"  float2 pos = I.rect.xy - grow + corner * (I.rect.zw + 2.0 * grow);\n"
"  if (merged_liquid) pos = mix(g.gb.xy, g.gb.zw, corner);\n"
"  // the pieces of shapes (kind 12): nothing drawn\n"
"  if (kind == 12) pos = float2(0.0, 0.0);\n"
"  // turned (scene.mbt 32-35): drawn turned about its point; fragment recovers exact p/cp from in.pos.xy\n"
"  float2 at = pos;\n"
"  float2 rot_cs = float2(1.0, 0.0);\n"
"  if (I.rot.x != 0.0) {\n"
"    float c = cos(I.rot.x), s = sin(I.rot.x);\n"
"    float2 d = pos - I.rot.yz;\n"
"    at = I.rot.yz + float2(c * d.x - s * d.y, s * d.x + c * d.y);\n"
"    rot_cs = float2(c, s);\n"
"  }\n"
"  VOut o;\n"
"  float2 rel = at - u.origin;\n"
"  o.pos = float4(rel.x / u.vp.x * 2.0 - 1.0, 1.0 - rel.y / u.vp.y * 2.0, 0.0, 1.0);\n"
"  o.rot_cs = rot_cs;\n"
"  o.iid = iid;\n"
"  return o;\n"
"}\n"
MB_CORNER_SL
"static float sd_rr(float2 p, float4 r, float4 rad) {\n"
"  float2 hs = r.zw * 0.5;\n"
"  return mb_sd_box(p - r.xy - hs, hs, mb_corner_of(p - r.xy - hs, rad), 1e9);\n"
"}\n"
"// a notch cut into the top edge (scene.mbt 21-23): a circle about the top's middle (pad.y below it, pad.z its radius),\n"
"// met by the edge through fillets (pad.w): the rounded intersection with what is outside the circle\n"
"static float notch_cut(float d, float2 p, float4 r, float4 pad) {\n"
"  if (pad.z <= 0.0) return d;\n"
"  float b = pad.z - length(p - float2(r.x + r.z * 0.5, r.y + pad.y));\n"
"  float f = max(pad.w, 0.0);\n"
"  float2 u = max(float2(f + d, f + b), 0.0);\n"
"  return min(-f, max(d, b)) + length(u);\n"
"}\n"
"static float smin_k(float a, float b, float k) {\n"
"  if (k <= 0.0) return min(a, b);\n"
"  float h = max(k - abs(a - b), 0.0) / k;\n"
"  return min(a, b) - h * h * k * 0.25;\n"
"}\n"
"static float sd_group(float2 p, constant G& g, float k) {\n"
"  float d = 1e9;\n"
"  int n = int(g.misc.x + 0.5);\n"
"  for (int i = 0; i < 8; i++) {\n"
"    if (i >= n) break;\n"
"    d = smin_k(d, sd_rr(p, g.shape[i], g.rad[i]), k);\n"
"  }\n"
"  return d;\n"
"}\n"
"// Integral of linear ramp from 0 to w (contourOffset = 0, width w in device px)\n"
"static float mb_contour_int(float t, float w) {\n"
"  float u = clamp(t, 0.0, w);\n"
"  return u - u * u / (2.0 * w);\n"
"}\n"
"static float mb_para_int(float t, float w) {\n"
"  float u = clamp(t / max(w, 0.0001), 0.0, 1.0);\n"
"  return w * (2.0 * u * u - (4.0 / 3.0) * u * u * u);\n"
"}\n"
"static float mb_bevel_F(float t, float w0, float L) {\n"
"  if (t <= w0) return t;\n"
"  return w0 + L * (1.0 - exp(-(t - w0) / max(L, 0.0001)));\n"
"}\n"
"static float mb_bevel_cov(float sd_px, float sc, float w0_pt, float L_pt) {\n"
"  float invS = 1.0 / max(sc, 1.0);\n"
"  float s0 = max(sd_px - 0.5, 0.0) * invS;\n"
"  float s1 = max(sd_px + 0.5, 0.0) * invS;\n"
"  if (s1 <= s0) return 0.0;\n"
"  return max(sc * (mb_bevel_F(s1, w0_pt, L_pt) - mb_bevel_F(s0, w0_pt, L_pt)), 0.0);\n"
"}\n"
"static float mb_snell_intersect(float s, float ior, float H_opt) {\n"
"  float invLen = rsqrt(1.0 + s * s);\n"
"  float cosI = invLen;\n"
"  float sinI = abs(s) * invLen;\n"
"  float eta = 1.0 / ior;\n"
"  float sinT = sinI * eta;\n"
"  float cosT = sqrt(max(1.0 - sinT * sinT, 0.0001));\n"
"  float k = eta * cosI - cosT;\n"
"  float Tr = k * (s * invLen);\n"
"  float Tz = -eta + k * invLen;\n"
"  return H_opt * (Tr / max(-Tz, 0.0001));\n"
"}\n"
"static float2 mb_droplet_snell(float d, float rawAmount, float halfMinor, int role, float ior) {\n"
"  float hs = clamp(halfMinor / 19.2, 0.72, 1.25);\n"
"  float refScale = clamp(rawAmount / 9.0, 0.0, 2.0);\n"
"  float medialTaper = 1.0 - smoothstep(0.68 * halfMinor, 0.96 * halfMinor, d);\n"
"  float s_lip = 0.0;\n"
"  float s_cap = 0.0;\n"
"  float H_opt = 0.0;\n"
"  if (role == 1) {\n"
"    float r_lip = 5.0 * hs;\n"
"    float L_lip = 1.15 * hs;\n"
"    s_lip = 10.5 * exp(-d / L_lip) * (1.0 - smoothstep(0.55 * r_lip, r_lip, d)) * medialTaper;\n"
"    H_opt = 9.4 * hs * refScale;\n"
"  } else if (role == 2) {\n"
"    float r_lip = 3.8 * hs;\n"
"    float L_lip = 0.52 * hs;\n"
"    s_lip = 6.3 * exp(-d / L_lip) * (1.0 - smoothstep(0.55 * r_lip, r_lip, d)) * medialTaper;\n"
"    float u = max(d - 1.20 * hs, 0.0) / (2.80 * hs);\n"
"    s_cap = 1.28 * 1.847264 * u * u * exp(-u) * medialTaper;\n"
"    H_opt = 10.6 * hs * refScale;\n"
"  } else if (role == 3) {\n"
"    float r_lip = 2.15 * hs;\n"
"    float L_lip = 0.42 * hs;\n"
"    s_lip = 14.5 * exp(-d / L_lip) * (1.0 - smoothstep(0.45 * r_lip, r_lip, d)) * medialTaper;\n"
"    float u = max(d - 1.95 * hs, 0.0) / (1.80 * hs);\n"
"    float capCut = 1.0 - smoothstep(7.5 * hs, 10.5 * hs, d);\n"
"    s_cap = 1.16 * 1.847264 * u * u * exp(-u) * capCut * medialTaper;\n"
"    H_opt = 10.6 * hs * refScale;\n"
"  } else {\n"
"    float r_lip = 4.4 * hs;\n"
"    float L_lip = 1.75 * hs;\n"
"    s_lip = 1.45 * exp(-d / L_lip) * (1.0 - smoothstep(0.45 * r_lip, r_lip, d)) * medialTaper;\n"
"    float u = max(d - 2.6 * hs, 0.0) / (2.4 * hs);\n"
"    float capCut = 1.0 - smoothstep(8.5 * hs, 12.0 * hs, d);\n"
"    s_cap = 0.55 * 1.847264 * u * u * exp(-u) * capCut * medialTaper;\n"
"    H_opt = 3.2 * hs * min(refScale, 1.5);\n"
"  }\n"
"  return float2(mb_snell_intersect(s_lip - s_cap, ior, H_opt), s_lip);\n"
"}\n"
"static float mb_sd_quad_bezier(float2 pos, float2 A, float2 B, float2 C) {\n"
"  float2 a = B - A;\n"
"  float2 b = A - 2.0 * B + C;\n"
"  float2 c = a * 2.0;\n"
"  float2 d = A - pos;\n"
"  float2 ac = C - A;\n"
"  float bb = dot(b, b);\n"
"  float t_lin = clamp(dot(pos - A, ac) / max(dot(ac, ac), 1e-12), 0.0, 1.0);\n"
"  // Collinear fallback for normalized [0,1] coords (residual <= ~1.25e-6 * chord length)\n"
"  if (bb <= 1e-10 * max(dot(a, a), 1e-8)) {\n"
"    return length(pos - A - ac * t_lin);\n"
"  }\n"
"  float2 vl = d + (c + b * t_lin) * t_lin;\n"
"  float bestT = t_lin;\n"
"  float res = dot(vl, vl);\n"
"  float kk = 1.0 / bb;\n"
"  float kx = kk * dot(a, b);\n"
"  float ky = kk * (2.0 * dot(a, a) + dot(d, b)) / 3.0;\n"
"  float kz = kk * dot(d, a);\n"
"  float p = ky - kx * kx;\n"
"  float p3 = p * p * p;\n"
"  float q = kx * (2.0 * kx * kx - 3.0 * ky) + kz;\n"
"  float h = q * q + 4.0 * p3;\n"
"  if (h >= 0.0) {\n"
"    float sh = sqrt(h);\n"
"    float2 x = (float2(sh, -sh) - q) * 0.5;\n"
"    float2 uv = sign(x) * pow(max(abs(x), float2(1e-20, 1e-20)), float2(1.0 / 3.0, 1.0 / 3.0));\n"
"    float t = clamp(uv.x + uv.y - kx, 0.0, 1.0);\n"
"    float2 v = d + (c + b * t) * t;\n"
"    float dd = dot(v, v);\n"
"    if (dd < res) { res = dd; bestT = t; }\n"
"  } else {\n"
"    float z = sqrt(-p);\n"
"    float denom = min(p * z * 2.0, -1e-12);\n"
"    float ang = acos(clamp(q / denom, -1.0, 1.0)) / 3.0;\n"
"    float m = cos(ang);\n"
"    float n = sin(ang) * 1.732050808;\n"
"    float t0 = clamp((m + m) * z - kx, 0.0, 1.0);\n"
"    float t1 = clamp((-n - m) * z - kx, 0.0, 1.0);\n"
"    float t2 = clamp((n - m) * z - kx, 0.0, 1.0);\n"
"    float2 v0 = d + (c + b * t0) * t0;\n"
"    float2 v1 = d + (c + b * t1) * t1;\n"
"    float2 v2 = d + (c + b * t2) * t2;\n"
"    float d0 = dot(v0, v0), d1 = dot(v1, v1), d2 = dot(v2, v2);\n"
"    if (d0 < res) { res = d0; bestT = t0; }\n"
"    if (d1 < res) { res = d1; bestT = t1; }\n"
"    if (d2 < res) { res = d2; bestT = t2; }\n"
"  }\n"
"  // 2 safeguarded Newton-Raphson steps eliminate float32 Cardano cancellation on shallow curves\n"
"  for (int ni = 0; ni < 2; ni++) {\n"
"    float2 vn = d + (c + b * bestT) * bestT;\n"
"    float2 dp = c + 2.0 * b * bestT;\n"
"    float fp = dot(dp, dp) + 2.0 * dot(vn, b);\n"
"    if (fp > 1e-12) {\n"
"      float nt = clamp(bestT - dot(vn, dp) / fp, 0.0, 1.0);\n"
"      float2 nv = d + (c + b * nt) * nt;\n"
"      float nd = dot(nv, nv);\n"
"      if (nd < res) { res = nd; bestT = nt; } else { break; }\n"
"    } else { break; }\n"
"  }\n"
"  return sqrt(res);\n"
"}\n"
"static float4 backdrop4(texture2d<float> bd, constant G& g, float2 p) {\n"
"  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
"  float2 uv = clamp(p * g.map.xy + g.map.zw, g.uvc.xy, g.uvc.zw);\n"
"  return bd.sample(s, uv);\n"
"}\n"
"static float3 backdrop(texture2d<float> bd, constant G& g, float2 p) { return backdrop4(bd, g, p).rgb; }\n"
"static float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }\n"
"static float mb_ink_luma(float3 c) { return dot(c * c, float3(0.2126, 0.7152, 0.0722)); }\n"
"static float mb_backdrop_luma_rect(texture2d<float> bd, constant G& g, float2 c, float2 h, float hole_y) {\n"
"  float sum = 0.0;\n"
"  float mx1 = -1.0, mx2 = -1.0;\n"
"  float mn1 = 2.0, mn2 = 2.0;\n"
"  for (int iy = -1; iy <= 1; iy++) {\n"
"    float py = c.y + float(iy) * (h.y * 0.60);\n"
"    for (int ix = -2; ix <= 2; ix++) {\n"
"      float px = c.x + float(ix) * (h.x * 0.38);\n"
"      float4 smp = backdrop4(bd, g, float2(px, py));\n"
"      float a = (g.misc.y > 0.5) ? clamp(smp.a, 0.0, 1.0) : 1.0;\n"
"      float y = mb_ink_luma(clamp(smp.rgb + float3((1.0 - a) * hole_y), 0.0, 1.0));\n"
"      sum += y;\n"
"      if (y > mx1) { mx2 = mx1; mx1 = y; } else if (y > mx2) { mx2 = y; }\n"
"      if (y < mn1) { mn2 = mn1; mn1 = y; } else if (y < mn2) { mn2 = y; }\n"
"    }\n"
"  }\n"
"  return (sum - mx1 - mx2 - mn1 - mn2) / 11.0;\n"
"}\n"
"static float mb_unpack16(float2 rg) {\n"
"  float hi = floor(clamp(rg.x, 0.0, 1.0) * 255.0 + 0.5);\n"
"  float lo = floor(clamp(rg.y, 0.0, 1.0) * 255.0 + 0.5);\n"
"  return (hi * 256.0 + lo) / 65535.0;\n"
"}\n"
"static float2 mb_pack16(float v) {\n"
"  float q = floor(clamp(v, 0.0, 1.0) * 65535.0 + 0.5);\n"
"  float hi = floor(q / 256.0);\n"
"  float lo = q - hi * 256.0;\n"
"  return float2(hi / 255.0, lo / 255.0);\n"
"}\n"
"static float2 ad_sample(texture2d<float> bd, texture2d<float> ad_tex, constant G& g, float2 ac, float fallback_r, bool is_dark_glass, float tint_a) {\n"
"  int ad_n = int(g.ad_meta.x + 0.5);\n"
"  if (ad_n > 0) {\n"
"    int best = -1;\n"
"    float best_d = 1e9;\n"
"    for (int i = 0; i < 16; i++) {\n"
"      if (i >= ad_n) break;\n"
"      float d = length(ac - g.ad_box[i].xy);\n"
"      if (d <= 2.0 && d < best_d) {\n"
"        float4 px = ad_tex.read(uint2(uint(i), 0));\n"
"        int code = int(floor(clamp(px.b, 0.0, 1.0) * 255.0 + 0.5));\n"
"        bool slot_dark = (code >= 48 && code <= 95);\n"
"        if (slot_dark != is_dark_glass) continue;\n"
"        best = i; best_d = d;\n"
"      }\n"
"    }\n"
"    if (best < 0) {\n"
"      best_d = 1e9;\n"
"      for (int i = 0; i < 16; i++) {\n"
"        if (i >= ad_n) break;\n"
"        float2 diff = abs(ac - g.ad_box[i].xy);\n"
"        if (diff.x <= g.ad_box[i].z + 2.0 && diff.y <= g.ad_box[i].w + 2.0) {\n"
"          float d = length(diff);\n"
"          if (d < best_d) {\n"
"            float4 px = ad_tex.read(uint2(uint(i), 0));\n"
"            int code = int(floor(clamp(px.b, 0.0, 1.0) * 255.0 + 0.5));\n"
"            bool slot_dark = (code >= 48 && code <= 95);\n"
"            if (slot_dark != is_dark_glass) continue;\n"
"            best = i; best_d = d;\n"
"          }\n"
"        }\n"
"      }\n"
"    }\n"
"    if (best >= 0) {\n"
"      float4 px = ad_tex.read(uint2(uint(best), 0));\n"
"      return float2(mb_unpack16(px.rg), 1.0);\n"
"    }\n"
"  }\n"
"  float bgL = mb_backdrop_luma_rect(bd, g, ac, float2(max(fallback_r, 1.0)), is_dark_glass ? 0.0 : 1.0);\n"
"  float baseDens = mix(0.404, tint_a, smoothstep(0.405, 0.46, tint_a));\n"
"  float extra = clamp((baseDens - 0.404) / (1.0 - 0.404), 0.0, 1.0);\n"
"  if (is_dark_glass) return float2(smoothstep(0.08, 0.42, bgL) * (1.0 - extra), 0.0);\n"
"  float c_eq = mix(sqrt(max(bgL, 0.0)), 0.992, pow(extra, 1.35));\n"
"  float effBgL = c_eq * c_eq;\n"
"  return float2((1.0 - smoothstep(0.06, 0.34, effBgL)) * (1.0 - extra), 0.0);\n"
"}\n"
"static float3 ad_sample_ink(texture2d<float> bd, texture2d<float> ad_tex, constant G& g, float2 ac, float fallback_r) {\n"
"  int ad_n = int(g.ad_meta.x + 0.5);\n"
"  if (ad_n > 0) {\n"
"    int best = -1;\n"
"    float best_d = 1e9;\n"
"    for (int i = 0; i < 16; i++) {\n"
"      if (i >= ad_n) break;\n"
"      float d = length(ac - g.ad_box[i].xy);\n"
"      if (d <= 2.0 && d < best_d) { best = i; best_d = d; }\n"
"    }\n"
"    if (best < 0) {\n"
"      best_d = 1e9;\n"
"      for (int i = 0; i < 16; i++) {\n"
"        if (i >= ad_n) break;\n"
"        float2 diff = abs(ac - g.ad_box[i].xy);\n"
"        if (diff.x <= g.ad_box[i].z + 2.0 && diff.y <= g.ad_box[i].w + 2.0) {\n"
"          float d = length(diff);\n"
"          if (d < best_d) { best = i; best_d = d; }\n"
"        }\n"
"      }\n"
"    }\n"
"    if (best >= 0) {\n"
"      float4 px = ad_tex.read(uint2(uint(best), 0));\n"
"      return float3(mb_unpack16(px.rg), px.a, px.b);\n"
"    }\n"
"  }\n"
"  float bgL = mb_backdrop_luma_rect(bd, g, ac, float2(max(fallback_r, 1.0)), 1.0);\n"
"  return float3(1.0 - smoothstep(0.06, 0.34, bgL), bgL, 0.0);\n"
"}\n"
"static float3 mb_adaptive_ink_at(texture2d<float> bd, texture2d<float> ad_tex, constant G& g, float3 rgb, float2 ac, float fallback_r, float4 rect) {\n"
"  float ch = max(max(rgb.r, rgb.g), rgb.b) - min(min(rgb.r, rgb.g), rgb.b);\n"
"  float origL = luma(rgb);\n"
"  if (ch > 0.08 || origL >= 0.5) return rgb;\n"
"  float3 cap = ad_sample_ink(bd, ad_tex, g, ac, fallback_r);\n"
"  float capsuleDarkT = cap.x;\n"
"  int code = int(floor(clamp(cap.z, 0.0, 1.0) * 255.0 + 0.5));\n"
"  if (code >= 160) {\n"
"    float inkT = smoothstep(0.0, 1.0, capsuleDarkT);\n"
"    float lightL = clamp(1.06 - origL, 0.68, 0.98);\n"
"    return mix(rgb, float3(lightL), inkT);\n"
"  }\n"
"  if (origL > 0.16) {\n"
"    float darkT = smoothstep(0.0, 1.0, capsuleDarkT);\n"
"    return mix(rgb, min(rgb, float3(0.20)), darkT);\n"
"  }\n"
"  return rgb;\n"
"}\n"
"// how much of the pixel at p its rounded clip keeps (r: the corners' radii, top left, then clockwise; s: how smooth\n"
"// each is), its edge covered as a box's own (0.5 - d * scale), so what it cuts meets the ground of the box that cuts it\n"
"static float clip_cover(float2 p, float4 c, float4 r, float4 s, float scale) {\n"
"  float d = mb_sd_clip(p, c, r, s, 1.0 + 2.0 / scale);\n"
"  return clamp(0.5 - d * scale, 0.0, 1.0);\n"
"}\n"
"static float mb_cr(float x) {\n"
"  x = abs(x);\n"
"  if (x >= 2.0) return 0.0;\n"
"  if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;\n"
"  return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;\n"
"}\n"
"static void mb_cr5(float tc, float inv_sz, float s, thread float *u, thread float *w) {\n"
"  float c = floor(tc), f = tc - c, is = 1.0 / s;\n"
"  float k0 = mb_cr((-3.0 - f) * is), k1 = mb_cr((-2.0 - f) * is);\n"
"  float k2 = mb_cr((-1.0 - f) * is), k3 = mb_cr(( 0.0 - f) * is);\n"
"  float k4 = mb_cr(( 1.0 - f) * is), k5 = mb_cr(( 2.0 - f) * is);\n"
"  float k6 = mb_cr(( 3.0 - f) * is), k7 = mb_cr(( 4.0 - f) * is);\n"
"  float wA = k0 + k1, wC = k3 + k4, wE = k6 + k7;\n"
"  float sum = wA + k2 + wC + k5 + wE;\n"
"  float norm = sum > 1e-6 ? 1.0 / sum : 1.0;\n"
"  u[0] = (c - 2.5 + (wA < -1e-6 ? k1 / wA : 0.0)) * inv_sz;\n"
"  u[1] = (c - 0.5) * inv_sz;\n"
"  u[2] = (c + 0.5 + (wC >  1e-6 ? k4 / wC : 0.0)) * inv_sz;\n"
"  u[3] = (c + 2.5) * inv_sz;\n"
"  u[4] = (c + 3.5 + (wE < -1e-6 ? k7 / wE : 0.0)) * inv_sz;\n"
"  w[0] = wA * norm; w[1] = k2 * norm; w[2] = wC * norm; w[3] = k5 * norm; w[4] = wE * norm;\n"
"}\n"
"static float4 mb_sample_pic(texture2d<float> img, sampler si, float2 uv, float2 dudx, float2 dudy) {\n"
"  uint tw = img.get_width(0), th = img.get_height(0), mips = img.get_num_mip_levels();\n"
"  float2 sz = float2(tw, th);\n"
"  float2 rate = float2(length(dudx * sz), length(dudy * sz));\n"
"  if (max(abs(rate.x - 1.0), abs(rate.y - 1.0)) < 0.005)\n"
"    return img.sample(si, uv, level(0.0));\n"
"  if (max(rate.x, rate.y) <= 1.005) {\n"
"    float2 pos = uv * sz - 0.5;\n"
"    float2 tc = floor(pos), f = pos - tc;\n"
"    float2 f2 = f * f, f3 = f2 * f;\n"
"    float2 w0 = -0.5 * f3 + f2 - 0.5 * f;\n"
"    float2 w1 =  1.5 * f3 - 2.5 * f2 + 1.0;\n"
"    float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;\n"
"    float2 w3 =  0.5 * f3 - 0.5 * f2;\n"
"    float2 w12 = w1 + w2;\n"
"    float2 t0 = (tc - 0.5) / sz;\n"
"    float2 t12 = (tc + 0.5 + w2 / w12) / sz;\n"
"    float2 t3 = (tc + 2.5) / sz;\n"
"    float4 c =\n"
"      (img.sample(si, float2(t0.x,  t0.y),  level(0.0)) * w0.x  +\n"
"       img.sample(si, float2(t12.x, t0.y),  level(0.0)) * w12.x +\n"
"       img.sample(si, float2(t3.x,  t0.y),  level(0.0)) * w3.x)  * w0.y +\n"
"      (img.sample(si, float2(t0.x,  t12.y), level(0.0)) * w0.x  +\n"
"       img.sample(si, float2(t12.x, t12.y), level(0.0)) * w12.x +\n"
"       img.sample(si, float2(t3.x,  t12.y), level(0.0)) * w3.x)  * w12.y +\n"
"      (img.sample(si, float2(t0.x,  t3.y),  level(0.0)) * w0.x  +\n"
"       img.sample(si, float2(t12.x, t3.y),  level(0.0)) * w12.x +\n"
"       img.sample(si, float2(t3.x,  t3.y),  level(0.0)) * w3.x)  * w3.y;\n"
"    c.a = saturate(c.a);\n"
"    c.rgb = clamp(c.rgb, float3(0.0), float3(c.a));\n"
"    return c;\n"
"  }\n"
"  float rmin = min(rate.x, rate.y);\n"
"  uint L = (mips > 1u && rmin > 2.0) ? min((uint)max(0.0, ceil(log2(rmin)) - 1.0), mips - 1u) : 0u;\n"
"  float2 msz = float2(img.get_width(L), img.get_height(L));\n"
"  float2 s = float2(length(dudx * msz), length(dudy * msz));\n"
"  if (max(s.x, s.y) > 2.05) return img.sample(si, uv, gradient2d(dudx, dudy));\n"
"  float ux[5], wx[5], uy[5], wy[5];\n"
"  mb_cr5(uv.x * msz.x - 0.5, 1.0 / msz.x, max(s.x, 1.0), ux, wx);\n"
"  mb_cr5(uv.y * msz.y - 0.5, 1.0 / msz.y, max(s.y, 1.0), uy, wy);\n"
"  float4 c = float4(0.0);\n"
"  float lv = float(L);\n"
"  for (int j = 0; j < 5; j++) {\n"
"    float4 row = float4(0.0);\n"
"    for (int i = 0; i < 5; i++)\n"
"      row += img.sample(si, float2(ux[i], uy[j]), level(lv)) * wx[i];\n"
"    c += row * wy[j];\n"
"  }\n"
"  c.a = saturate(c.a);\n"
"  c.rgb = clamp(c.rgb, float3(0.0), float3(c.a));\n"
"  return c;\n"
"}\n"
"fragment float4 fmain(VOut in [[stage_in]], const device Inst* inst [[buffer(0)]],\n"
"    constant U& u [[buffer(1)]], constant G& g [[buffer(2)]], texture2d_array<float> atlas [[texture(0)]],\n"
"    texture2d<float> img [[texture(1)]], texture2d<float> bd [[texture(2)]], texture2d_array<float> catlas [[texture(3)]],\n"
"    texture2d<float> ad_tex [[texture(4)]]) {\n"
"  Inst I = inst[in.iid];\n"
"  float2 at_p = in.pos.xy / u.scale + u.origin;\n"
"  float2 p = at_p, cp = at_p;\n"
"  if (I.rot.x != 0.0) {\n"
"    float2 d = at_p - I.rot.yz;\n"
"    p = I.rot.yz + float2(in.rot_cs.x * d.x + in.rot_cs.y * d.y, -in.rot_cs.y * d.x + in.rot_cs.x * d.y);\n"
"    cp = I.rot.w > 0.5 ? at_p : p;\n"
"  }\n"
"  // cut to its clip: a rect's, hard; a rounded one's, covered as edges are\n"
"  float cover = 1.0;\n"
"  if (max(max(I.rc.x, I.rc.y), max(I.rc.z, I.rc.w)) > 0.0) {\n"
"    cover = clip_cover(cp, I.clip, I.rc, I.rs, u.scale);\n"
"    if (cover <= 0.0) discard_fragment();\n"
"  } else if (cp.x < I.clip.x || cp.y < I.clip.y || cp.x > I.clip.z || cp.y > I.clip.w) discard_fragment();\n"
"  // and by the clip outside it, where it has one (a pane round an image's frame in its corner)\n"
"  if (max(max(I.orc.x, I.orc.y), max(I.orc.z, I.orc.w)) > 0.0) {\n"
"    cover *= clip_cover(cp, I.oclip, I.orc, I.ors, u.scale);\n"
"    if (cover <= 0.0) discard_fragment();\n"
"  }\n"
"  int kind = one_kind == 13 ? 6 : (one_kind >= 0 ? one_kind : int(I.p.y + 0.5));\n"
"  bool prog_glass = (kind == 5 && I.pad.z < -0.5);\n"
"  // and faded out towards a cut, each pixel by where it is (scene.mbt 52-59)\n"
"  float fade_r = 1.0;\n"
"  if (I.fade.y > 0.0) fade_r *= clamp((cp.y - I.fade.x) / I.fade.y, 0.0, 1.0);\n"
"  if (I.fade.w > 0.0) fade_r *= clamp((I.fade.z - cp.y) / I.fade.w, 0.0, 1.0);\n"
"  if (I.fadex.y > 0.0) fade_r *= clamp((cp.x - I.fadex.x) / I.fadex.y, 0.0, 1.0);\n"
"  if (I.fadex.w > 0.0) fade_r *= clamp((I.fadex.z - cp.x) / I.fadex.w, 0.0, 1.0);\n"
"  if (fade_r <= 0.0) discard_fragment();\n"
"  if (!prog_glass) {\n"
"    cover *= fade_r;\n"
"    if (cover <= 0.0) discard_fragment();\n"
"  }\n"
"  float4 col = I.color;\n"
"  if (kind == 1 || kind == 3 || kind == 7) {\n"
"    constexpr sampler s(filter::linear);\n"
"    // within its own cell (c2: where in the atlas, in texels; a colour glyph's MB_ATLAS_COLOR_U across past its place\n"
"    // in theirs), however its rect is stretched or turned: a sample past the cell's edge texels would blend in what\n"
"    // lies beside it (another glyph, one left from before the atlas started over). The words' atlas has a glyph's\n"
"    // cover alone: white, premultiplied, as the colour one would have it.\n"
"    bool colour = I.c2.x >= 16384.0;\n"
"    float2 at = float2(I.c2.x - (colour ? 16384.0 : 0.0), I.c2.y);\n"
"    // (its page, a page's side of its atlas down for each: atlas_pages.h)\n"
"    float side = colour ? float(catlas.get_height()) : float(atlas.get_height());\n"
"    float layer = max(floor(at.y / side), 0.0);\n"
"    at.y -= layer * side;\n"
"    // One drawn as big as its picture, not turned, its corner on a pixel: each pixel its texel's centre, as\n"
"    // win_render.cpp takes it (worked out of where it is across the rect, a float's division took a texel's\n"
"    // neighbour in where its rounding fell, at 3, 2.625 and 1.5 pixels a point and far across)\n"
"    float2 off = abs(I.rect.zw * u.scale - I.c2.zw);\n"
"    float2 r0 = I.rect.xy * u.scale, r0i = floor(r0 + 0.5);\n"
"    bool exact = I.rot.x == 0.0 && max(off.x, off.y) <= 0.05 && max(abs(r0.x - r0i.x), abs(r0.y - r0i.y)) <= 0.05;\n"
"    float2 tex = exact ? floor(p * u.scale - r0i) + 0.5 : (p - I.rect.xy) / I.rect.zw * I.c2.zw;\n"
"    float2 uv = clamp(at + tex, at + 0.5, at + I.c2.zw - 0.5);\n"
"    // (a texel's centre, a power of two over: the same texel as its place in texels)\n"
"    float4 t = colour ? catlas.sample(s, uv / side, uint(layer)) : float4(atlas.sample(s, uv / side, uint(layer)).r);\n"
"    if (kind == 1) return (float4(col.rgb, 1.0) * (t.a * col.a)) * cover;\n"
"    if (kind == 7) {\n"
"      // inked by how light what is under its glass is, looked at round the glass's middle and locally\n"
"      float3 ink = mb_adaptive_ink_at(bd, ad_tex, g, col.rgb, I.pad.yz, I.pad.w * 0.5, I.rect);\n"
"      return (float4(ink, 1.0) * (t.a * col.a)) * cover;\n"
"    }\n"
"    return (t * col.a) * cover;\n"
"  }\n"
"  if (kind == 6) {\n"
"    // Copyright 2025, Tim Lehmann for whynotmake.it. Adapted from flutter_liquid_glass\n"
"    // liquid_glass_final_render_core.glsl, Apache-2.0.\n"
"    float k = I.pad.w;\n"
"    float4 s_rr = max(max(I.rr.x, I.rr.y), max(I.rr.z, I.rr.w)) > 0.0 ? I.rr : float4(I.p.x);\n"
"    float d;\n"
"    float halfMinor = 1e9;\n"
"    float2 extSpineVec = float2(0.0, 0.0);\n"
"    bool single_liquid = (one_kind == 13) || (k <= 0.0);\n"
"    if (single_liquid) {\n"
"      d = sd_rr(p, I.rect, s_rr);\n"
"      float2 hs = I.rect.zw * 0.5;\n"
"      halfMinor = min(hs.x, hs.y);\n"
"      float2 ct = I.rect.xy + hs;\n"
"      float2 ch = max(hs - float2(halfMinor, halfMinor), float2(0.0, 0.0));\n"
"      extSpineVec = p - clamp(p, ct - ch, ct + ch);\n"
"    } else {\n"
"      d = 1e9;\n"
"      int n_sh = int(g.misc.x + 0.5);\n"
"      for (int i = 0; i < 8; i++) {\n"
"        if (i >= n_sh) break;\n"
"        float di = sd_rr(p, g.shape[i], g.rad[i]);\n"
"        float2 hsi = g.shape[i].zw * 0.5;\n"
"        float hi = min(hsi.x, hsi.y);\n"
"        float2 cti = g.shape[i].xy + hsi;\n"
"        float2 chi = max(hsi - float2(hi, hi), float2(0.0, 0.0));\n"
"        float2 vi = p - clamp(p, cti - chi, cti + chi);\n"
"        if (i == 0) {\n"
"          d = di;\n"
"          halfMinor = hi;\n"
"          extSpineVec = vi;\n"
"        } else {\n"
"          float h = max(k - abs(d - di), 0.0) / k;\n"
"          float w = d < di ? 0.5 * h : 1.0 - 0.5 * h;\n"
"          d = min(d, di) - h * h * k * 0.25;\n"
"          halfMinor = mix(halfMinor, hi, w);\n"
"          extSpineVec = mix(extSpineVec, vi, w);\n"
"        }\n"
"      }\n"
"    }\n"
"    halfMinor = max(halfMinor, 1.0);\n"
"    float sd_px = -d * u.scale;\n"
"    float materialAlpha = clamp(sd_px + 0.5, 0.0, 1.0);\n"
"    float edgeW = max(0.90 * u.scale, 2.6);\n"
"    float outward = -sd_px;\n"
"    float ext_cov = mb_contour_int(max(outward + 0.5, 0.0), edgeW)\n"
"                  - mb_contour_int(max(outward - 0.5, 0.0), edgeW);\n"
"    if (materialAlpha < 0.01 && ext_cov < 0.01) discard_fragment();\n"
"    float regMix = smoothstep(0.105, 0.305, col.a);\n"
"    bool is_clear = (regMix <= 0.0);\n"
"    bool is_adaptive = I.c2.z < -0.5;\n"
"    float spot_raw = is_adaptive ? (-I.c2.z - 1.0) : I.c2.z;\n"
"    bool is_dark = (!is_clear && luma(col.rgb) < 0.35);\n"
"    float clearVeil = is_clear ? smoothstep(0.005, 0.102, col.a) : 0.0;\n"
"    float droplet = is_clear ? (1.0 - clearVeil) : 0.0;\n"
"    bool is_drop = (droplet > 0.001) && single_liquid;\n"
"    float inward = max(-d, 0.0);\n"
"    float rawBevel = max(I.c2.y, 0.0);\n"
"    float rawAmount = max(I.pad.y, 0.0);\n"
"    float colChroma = max(max(col.r, col.g), col.b) - min(min(col.r, col.g), col.b);\n"
"    float solidFade = 1.0 - smoothstep(0.88, 0.99, col.a);\n"
"    float lensScale = min(1.0, halfMinor / max(rawBevel, 0.001));\n"
"    float clearBevel = min(rawBevel * lensScale, halfMinor * 0.72);\n"
"    float clearAmount = min(rawAmount * lensScale, halfMinor * 0.58);\n"
"    float regBevel = min(rawBevel * 0.90, 0.58 * halfMinor);\n"
"    float regAmount = min(rawAmount * 0.96, 0.88 * halfMinor);\n"
"    float bevel = mix(clearBevel, regBevel, regMix);\n"
"    float amount = mix(clearAmount, regAmount, regMix);\n"
"    float2 surfaceNormal = float2(0.0, 0.0);\n"
"    float shortening = 0.0;\n"
"    float2 disp = float2(0.0, 0.0);\n"
"    bool need_grad = (inward < max(bevel, 23.0)) || (ext_cov > 0.0) || is_drop;\n"
"    if (need_grad) {\n"
"      float e = 0.5;\n"
"      float2 grad = single_liquid\n"
"        ? float2(\n"
"            sd_rr(p + float2(e, 0.0), I.rect, s_rr) - sd_rr(p - float2(e, 0.0), I.rect, s_rr),\n"
"            sd_rr(p + float2(0.0, e), I.rect, s_rr) - sd_rr(p - float2(0.0, e), I.rect, s_rr)\n"
"          ) / (2.0 * e)\n"
"        : float2(\n"
"            sd_group(p + float2(e, 0.0), g, k) - sd_group(p - float2(e, 0.0), g, k),\n"
"            sd_group(p + float2(0.0, e), g, k) - sd_group(p - float2(0.0, e), g, k)\n"
"          ) / (2.0 * e);\n"
"      float gradLen = length(grad);\n"
"      surfaceNormal = gradLen > 0.0001 ? grad / gradLen : float2(0.0, 0.0);\n"
"      shortening = sqrt(clamp(gradLen, 0.0, 1.0));\n"
"      if (inward < bevel && bevel > 0.001 && amount > 0.0) {\n"
"        float bevelX = 1.0 - clamp(inward / bevel, 0.0, 1.0);\n"
"        // iOS 27.0 Glass::clear() inverted-refraction caustic spans ~73% of 14pt bevel (2.20x rawAmount).\n"
"        float clearVeilR = smoothstep(0.005, 0.102, col.a) * (1.0 - smoothstep(0.001, 0.008, abs(I.p.w))) * (1.0 - regMix);\n"
"        float clearBevelX = pow(bevelX, mix(1.0, 0.78, clearVeilR));\n"
"        float clearAmt = amount * mix(1.0, 2.20, clearVeilR);\n"
"        float clearMag = clearAmt * (1.0 - sqrt(max(1.0 - clearBevelX * clearBevelX, 0.0))) * shortening;\n"
"        float regMag = amount * (1.0 - sqrt(max(1.0 - bevelX * bevelX, 0.0))) * shortening;\n"
"        float mag = mix(clearMag, regMag, regMix);\n"
"        disp = surfaceNormal * (-mag);\n"
"      }\n"
"    }\n"
"    int dropRole = (I.p.w <= -8.0) ? int(floor((-I.p.w + 4.0) / 16.0)) : 0;\n"
"    float rawZoomDelta = (I.p.w <= -8.0) ? ((-I.p.w) - float(dropRole) * 16.0) : ((I.p.w < 0.0) ? (-I.p.w) : 0.0);\n"
"    float lensZoom = clamp(1.0 + rawZoomDelta, 1.0, 3.0);\n"
"    bool isRoleSlid = (dropRole == 1);\n"
"    bool isRoleSw = (dropRole == 2);\n"
"    bool isRoleSeg = (dropRole == 3);\n"
"    bool isRoleTab = (dropRole == 4);\n"
"    bool isModernDrop = is_drop && (dropRole >= 1) && (dropRole <= 4);\n"
"    float snellSLip = 0.0;\n"
"    float snellDmag = 0.0;\n"
"    float2 sampleBase = p;\n"
"    if (lensZoom > 1.0001) {\n"
"      float2 zoomC = single_liquid ? (I.rect.xy + I.rect.zw * 0.5) : ((g.gb.xy + g.gb.zw) * 0.5);\n"
"      float zoomMask = (dropRole == 4) ? smoothstep(0.22, 0.52, inward / halfMinor) : 1.0;\n"
"      float invZoom = mix(1.0, 1.0 / lensZoom, zoomMask);\n"
"      sampleBase = zoomC + (p - zoomC) * invZoom;\n"
"    }\n"
"    float dropHairline = 0.0;\n"
"    float pressAmt = 0.0;\n"
"    float menAA = 0.0;\n"
"    if (is_drop) {\n"
"      pressAmt = clamp(I.c2.x, 0.0, 1.0);\n"
"      dropHairline = (1.0 - smoothstep(0.18, 0.62, inward)) * pressAmt;\n"
"      if (isModernDrop) {\n"
"        float2 sn = mb_droplet_snell(inward, rawAmount, halfMinor, dropRole, 1.52);\n"
"        snellSLip = sn.y;\n"
"        snellDmag = clamp(sn.x * shortening, -halfMinor, halfMinor);\n"
"        float2 dropDisp = surfaceNormal * snellDmag;\n"
"        if (isRoleSlid && snellSLip > 0.6 && pressAmt > 0.001) {\n"
"          menAA = 0.35 * smoothstep(0.6, 2.0, snellSLip) * pressAmt * droplet;\n"
"        }\n"
"        disp = mix(disp, dropDisp, droplet);\n"
"      } else {\n"
"        float bevelX = (bevel > 0.001) ? (1.0 - clamp(inward / bevel, 0.0, 1.0)) : 0.0;\n"
"        float2 paraxialDisp = -surfaceNormal * (bevelX * bevelX * 3.6 * pressAmt);\n"
"        float2 dropDisp = mix(paraxialDisp, surfaceNormal * 0.85, dropHairline);\n"
"        float dLen = length(dropDisp);\n"
"        if (dLen > halfMinor) dropDisp *= (halfMinor / dLen);\n"
"        disp = mix(disp, dropDisp, droplet);\n"
"      }\n"
"    }\n"
"    float3 refractColor;\n"
"    if (isModernDrop) {\n"
"      if (abs(I.pad.z) > 0.001 && snellSLip > 0.15 && pressAmt > 0.001) {\n"
"        float dispR = mb_droplet_snell(inward, rawAmount, halfMinor, dropRole, 1.47).x * shortening;\n"
"        float dispB = mb_droplet_snell(inward, rawAmount, halfMinor, dropRole, 1.58).x * shortening;\n"
"        float caW = smoothstep(0.15, 1.20, snellSLip) * clamp(abs(I.pad.z) / 0.04, 0.0, 1.0) * droplet;\n"
"        float dR = clamp(mix(snellDmag, dispR, caW), -halfMinor, halfMinor);\n"
"        float dB = clamp(mix(snellDmag, dispB, caW), -halfMinor, halfMinor);\n"
"        float2 dispR2 = mix(disp, surfaceNormal * dR, droplet);\n"
"        float2 dispB2 = mix(disp, surfaceNormal * dB, droplet);\n"
"        refractColor = float3(\n"
"          backdrop(bd, g, sampleBase + dispR2).r,\n"
"          backdrop(bd, g, sampleBase + disp).g,\n"
"          backdrop(bd, g, sampleBase + dispB2).b\n"
"        );\n"
"      } else {\n"
"        refractColor = backdrop(bd, g, sampleBase + disp);\n"
"      }\n"
"    } else if (inward >= bevel || abs(I.pad.z) * (1.0 - regMix) * amount * u.scale <= 0.25) {\n"
"      refractColor = backdrop(bd, g, sampleBase + disp);\n"
"    } else {\n"
"      float ds = I.pad.z * 0.5 * (1.0 - regMix);\n"
"      refractColor = float3(\n"
"        backdrop(bd, g, sampleBase + disp * (1.0 + ds)).r,\n"
"        backdrop(bd, g, sampleBase + disp).g,\n"
"        backdrop(bd, g, sampleBase + disp * (1.0 - ds)).b\n"
"      );\n"
"    }\n"
"    if (menAA > 0.01) {\n"
"      float aaOff = isModernDrop ? 0.25 : 0.35;\n"
"      float2 nOff = surfaceNormal * aaOff;\n"
"      float2 tOff = float2(-surfaceNormal.y, surfaceNormal.x) * aaOff;\n"
"      float2 mc = sampleBase + disp;\n"
"      float3 menAvg = (backdrop(bd, g, mc + nOff + tOff) + backdrop(bd, g, mc + nOff - tOff)\n"
"                     + backdrop(bd, g, mc - nOff + tOff) + backdrop(bd, g, mc - nOff - tOff)) * 0.25;\n"
"      refractColor = mix(refractColor, menAvg, menAA);\n"
"    }\n"
"    float bgA = (g.misc.y > 0.5) ? clamp(backdrop4(bd, g, sampleBase + disp).a, 0.0, 1.0) : 1.0;\n"
"    float3 emission;\n"
"    float transmittance;\n"
"    float lift;\n"
"    float chromaGain;\n"
"    float contourStr;\n"
"    float contourDirAmt;\n"
"    float bevelShadowStr;\n"
"    float glintLum;\n"
"    float glintFaceGain;\n"
"    float glintVib;\n"
"    float fogT = 0.0;\n"
"    float darkAdGlareW = 0.0;\n"
"    float darkMixSaved = 0.0;\n"
"    float brightTSaved = 0.0;\n"
"    float regExtra = 0.0;\n"
"    if (is_clear) {\n"
"      // iOS 27 clear liquid glass: col.a == 0.0 keeps crystal-clear zero-veil optical lens\n"
"      // (lifted tab-bar / toggle / slider thumb & loupe), while Glass::clear() (col.a ~ 0.102)\n"
"      // matches iOS 27.0 (+32/255 emission, 0.95 transmittance, 1.10 chroma, channel clamp).\n"
"      emission = float3(1.0, 1.0, 1.0) * (0.1255 * clearVeil);\n"
"      transmittance = mix(1.0, 0.95, clearVeil);\n"
"      lift = 0.0;\n"
"      chromaGain = mix(1.08, 1.06, clearVeil);\n"
"      contourStr = 0.0;\n"
"      contourDirAmt = mix(0.32, 0.77, clearVeil);\n"
"      bevelShadowStr = 0.0;\n"
"      glintLum = mix(1.85, 1.35, clearVeil);\n"
"      glintFaceGain = mix(0.14, 0.25, clearVeil);\n"
"      glintVib = mix(2.4, 1.5, clearVeil);\n"
"    } else if (is_adaptive) {\n"
"      bool is_inverting = (I.p.z < 0.0);\n"
"      float2 ac = single_liquid ? (I.rect.xy + I.rect.zw * 0.5) : ((g.gb.xy + g.gb.zw) * 0.5);\n"
"      float ar = (single_liquid ? max(min(I.rect.z, I.rect.w) * 0.5, 1.0) : max(min(g.gb.z - g.gb.x, g.gb.w - g.gb.y) * 0.5, 1.0)) * 0.5;\n"
"      float2 ad = ad_sample(bd, ad_tex, g, ac, ar, is_dark, col.a);\n"
"      float val = ad.x;\n"
"      bool ad_slotted = (ad.y > 0.5);\n"
"      if (g.misc.y > 0.5) val *= bgA;\n"
"      float baseDens = mix(0.404, col.a, smoothstep(0.405, 0.46, col.a));\n"
"      if (is_dark) {\n"
"        float extra = clamp((baseDens - 0.404) / (1.0 - 0.404), 0.0, 1.0);\n"
"        float denseT = val * solidFade;\n"
"        fogT = denseT * 0.65;\n"
"        brightTSaved = 0.0;\n"
"        regExtra = extra;\n"
"        float darkPow = pow(extra, 1.00);\n"
"        float darkCPow = pow(extra, 1.05);\n"
"        float darkBump = 3.5 * smoothstep(0.0, 0.16, extra) * (1.0 - smoothstep(0.22, 0.38, extra));\n"
"        float3 darkEm = mix(float3((34.0 + darkBump) / 255.0), col.rgb, pow(extra, 1.5));\n"
"        float darkTrans = mix(0.59, 0.0, darkPow);\n"
"        float darkLift = mix(0.55, 0.0, extra);\n"
"        float darkChroma = mix(1.05, 0.0, darkCPow);\n"
"        emission = darkEm;\n"
"        transmittance = mix(darkTrans, min(darkTrans, 0.24), denseT);\n"
"        lift = mix(darkLift, 0.10, denseT);\n"
"        chromaGain = mix(darkChroma, 0.55, denseT);\n"
"        darkAdGlareW = (1.0 - extra);\n"
"        contourStr = 0.88; contourDirAmt = 1.0; bevelShadowStr = 0.036;\n"
"        glintLum = 1.6; glintFaceGain = 0.0; glintVib = 2.85;\n"
"      } else if (is_inverting && ad_slotted) {\n"
"        float darkMix = val * solidFade;\n"
"        fogT = 0.0;\n"
"        darkMixSaved = darkMix;\n"
"        float extra = clamp((baseDens - 0.404) / (1.0 - 0.404), 0.0, 1.0);\n"
"        float lightPow = pow(extra, 1.35);\n"
"        float lightBaseDens = mix(0.525, 1.0, lightPow);\n"
"        float darkDens = baseDens;\n"
"        float darkExtra = clamp((darkDens - 0.404) / (1.0 - 0.404), 0.0, 1.0);\n"
"        regExtra = mix(extra, darkExtra, darkMix);\n"
"        float darkPow = pow(darkExtra, 1.00);\n"
"        float darkCPow = pow(darkExtra, 1.05);\n"
"        float darkBump = 3.5 * smoothstep(0.0, 0.16, darkExtra) * (1.0 - smoothstep(0.22, 0.38, darkExtra));\n"
"        float3 lightEm = mix(float3(253.0 / 255.0, 252.0 / 255.0, 253.0 / 255.0), col.rgb, extra) * lightBaseDens;\n"
"        float3 darkEm = mix(float3((34.0 + darkBump) / 255.0), float3(32.0 / 255.0), pow(darkExtra, 1.5));\n"
"        float lightLift = mix(0.04, 0.14, smoothstep(0.0, 0.16, extra)) * (1.0 - smoothstep(0.40, 0.95, extra));\n"
"        emission = mix(lightEm, darkEm, darkMix);\n"
"        transmittance = mix(1.0 - lightBaseDens, mix(0.59, 0.0, darkPow), darkMix);\n"
"        lift = mix(lightLift, mix(0.55, 0.0, darkExtra), darkMix);\n"
"        chromaGain = mix(mix(1.00, 0.0, lightPow), mix(1.05, 0.0, darkCPow), darkMix);\n"
"        contourStr = mix(0.43, 0.88, darkMix);\n"
"        contourDirAmt = mix(0.77, 1.0, darkMix);\n"
"        bevelShadowStr = 0.036;\n"
"        glintLum = 1.6; glintFaceGain = 0.0; glintVib = 2.85;\n"
"      } else {\n"
"        float denseT = val * solidFade;\n"
"        fogT = denseT * 0.60;\n"
"        darkMixSaved = 0.0;\n"
"        float extra = clamp((baseDens - 0.404) / (1.0 - 0.404), 0.0, 1.0);\n"
"        regExtra = extra;\n"
"        float lightPow = pow(extra, 1.35);\n"
"        float lightBaseDens = mix(0.525, 1.0, lightPow);\n"
"        float lightDens = mix(lightBaseDens, max(lightBaseDens, 0.74), denseT);\n"
"        float lightLift = mix(0.04, 0.14, smoothstep(0.0, 0.16, extra)) * (1.0 - smoothstep(0.40, 0.95, extra));\n"
"        emission = mix(float3(253.0 / 255.0, 252.0 / 255.0, 253.0 / 255.0), col.rgb, extra) * lightDens;\n"
"        transmittance = 1.0 - lightDens;\n"
"        lift = lightLift * (1.0 - 0.5 * denseT);\n"
"        chromaGain = mix(mix(1.00, 0.0, lightPow), 0.55, denseT);\n"
"        contourStr = 0.43;\n"
"        contourDirAmt = 0.77;\n"
"        bevelShadowStr = 0.036;\n"
"        glintLum = 1.6; glintFaceGain = 0.0; glintVib = 2.85;\n"
"      }\n"
"    } else if (is_dark) {\n"
"      float darkDens = mix(0.404, col.a, smoothstep(0.405, 0.46, col.a));\n"
"      float extra = clamp((darkDens - 0.404) / (1.0 - 0.404), 0.0, 1.0);\n"
"      regExtra = extra;\n"
"      float darkPow = pow(extra, 1.00);\n"
"      float darkCPow = pow(extra, 1.05);\n"
"      float darkBump = 3.5 * smoothstep(0.0, 0.16, extra) * (1.0 - smoothstep(0.22, 0.38, extra));\n"
"      emission = mix(float3((34.0 + darkBump) / 255.0), col.rgb, pow(extra, 1.5));\n"
"      transmittance = mix(0.59, 0.0, darkPow);\n"
"      lift = mix(0.55, 0.0, extra);\n"
"      chromaGain = mix(1.05, 0.0, darkCPow);\n"
"      contourStr = 0.88; contourDirAmt = 1.0; bevelShadowStr = 0.036;\n"
"      glintLum = 1.6; glintFaceGain = 0.0; glintVib = 2.85;\n"
"    } else {\n"
"      float baseDens = mix(0.404, col.a, smoothstep(0.405, 0.46, col.a));\n"
"      float extra = clamp((baseDens - 0.404) / (1.0 - 0.404), 0.0, 1.0);\n"
"      regExtra = extra;\n"
"      float lightPow = pow(extra, 1.35);\n"
"      float lightDens = mix(0.525, 1.0, lightPow);\n"
"      emission = mix(float3(253.0 / 255.0, 252.0 / 255.0, 253.0 / 255.0), col.rgb, extra) * lightDens;\n"
"      transmittance = 1.0 - lightDens;\n"
"      lift = mix(0.04, 0.14, smoothstep(0.0, 0.16, extra)) * (1.0 - smoothstep(0.40, 0.95, extra));\n"
"      chromaGain = mix(1.00, 0.0, lightPow);\n"
"      contourStr = 0.43; contourDirAmt = 0.77; bevelShadowStr = 0.036;\n"
"      glintLum = 1.6; glintFaceGain = 0.0; glintVib = 2.85;\n"
"    }\n"
"    if (regMix > 0.001 && fogT > 0.001) {\n"
"      float2 fc = sampleBase + disp;\n"
"      float rD = mix(3.2, 4.2, fogT);\n"
"      float3 s1 = backdrop(bd, g, fc + float2( rD,  rD));\n"
"      float3 s2 = backdrop(bd, g, fc + float2(-rD,  rD));\n"
"      float3 s3 = backdrop(bd, g, fc + float2( rD, -rD));\n"
"      float3 s4 = backdrop(bd, g, fc + float2(-rD, -rD));\n"
"      float3 lowC = (refractColor * 2.0 + s1 + s2 + s3 + s4) * (1.0 / 6.0);\n"
"      float interior = (bevel > 0.001) ? smoothstep(0.30 * bevel, 0.85 * bevel, inward) : 1.0;\n"
"      float dampW = interior * (fogT * 0.72) * regMix;\n"
"      refractColor = mix(refractColor, lowC, dampW);\n"
"    }\n"
"    if (regMix > 0.0 && regMix < 1.0) {\n"
"      emission = mix(float3(0.1255, 0.1255, 0.1255), emission, regMix);\n"
"      transmittance = mix(0.95, transmittance, regMix);\n"
"      lift = mix(0.0, lift, regMix);\n"
"      chromaGain = mix(1.06, chromaGain, regMix);\n"
"      contourStr = mix(0.0, contourStr, regMix);\n"
"      contourDirAmt = mix(0.77, contourDirAmt, regMix);\n"
"      bevelShadowStr = mix(0.0, bevelShadowStr, regMix);\n"
"      glintLum = mix(1.35, glintLum, regMix);\n"
"      glintFaceGain = mix(0.25, glintFaceGain, regMix);\n"
"      glintVib = mix(1.5, glintVib, regMix);\n"
"    }\n"
"    float3 rawRefractColor = refractColor;\n"
"    float3 extRimEnv = refractColor;\n"
"    float extRimValid = 0.0;\n"
"    float extBlurT = 0.0;\n"
"    float extDensFade = 1.0 - smoothstep(0.35, 0.85, regExtra);\n"
"    float extRadLen = length(extSpineVec);\n"
"    float2 extRadDir = extRadLen > 0.001 ? extSpineVec / extRadLen : float2(0.0, 1.0);\n"
"    if (regMix > 0.001 && solidFade > 0.001 && extDensFade > 0.001 && colChroma < 0.16 && inward < 76.0) {\n"
"      float extBlur = max(I.p.w, 0.0);\n"
"      float extRefrSigned = min(I.pad.y, I.pad.y * (1.0 + abs(I.pad.z)));\n"
"      float extRefrGate = smoothstep(-1.0, 0.0, I.pad.y);\n"
"      float extMaxReach = max(3.0 * extBlur + extRefrSigned - 2.0, 0.0) * extRefrGate;\n"
"      if (extMaxReach > 1.0) {\n"
"        extBlurT = smoothstep(2.0, 10.0, extBlur);\n"
"        float extBaseValid = smoothstep(1.0, 4.0, extMaxReach) * extDensFade * extRefrGate;\n"
"        float extTintGate = 1.0 - smoothstep(0.08, 0.16, colChroma);\n"
"        float diffTail = exp(-max(inward - 2.0, 0.0) / 30.0) * (1.0 - smoothstep(52.0, 76.0, inward));\n"
"        float radCenterGate = smoothstep(0.18, 0.55, extRadLen / max(halfMinor, 1.0));\n"
"        float wideGate = smoothstep(4.0, 10.0, extBlur) * smoothstep(24.0, 48.0, halfMinor) * diffTail * radCenterGate;\n"
"        if (wideGate > 0.001) {\n"
"          float2 rDir = extRadDir;\n"
"          float2 tDir = float2(-rDir.y, rDir.x);\n"
"          float dOutNear = inward + min(mix(10.0, 22.0, extBlurT), extMaxReach * 0.60);\n"
"          float dOutFar  = inward + min(mix(18.0, 40.0, extBlurT), extMaxReach * 0.88);\n"
"          float spread   = min(14.0 + 0.36 * inward, extMaxReach * 0.45);\n"
"          float dBack    = min(mix(10.0, 22.0, extBlurT), extMaxReach * 0.45);\n"
"          float3 wNear = backdrop(bd, g, p + rDir * dOutNear);\n"
"          float3 wFarL = backdrop(bd, g, p + rDir * dOutFar - tDir * spread);\n"
"          float3 wFarR = backdrop(bd, g, p + rDir * dOutFar + tDir * spread);\n"
"          float3 wBack = backdrop(bd, g, p - rDir * dBack);\n"
"          float3 wideAmb = wNear * 0.24 + (wFarL + wFarR) * 0.32 + wBack * 0.12;\n"
"          float ambDelta = smoothstep(0.008, 0.052, length(wideAmb - refractColor));\n"
"          float ambY = luma(wideAmb);\n"
"          float3 ambCh = wideAmb - float3(ambY);\n"
"          float ambChLen = length(ambCh);\n"
"          float refY = luma(refractColor);\n"
"          float refChLen = length(refractColor - float3(refY));\n"
"          float neutralRefGate = 1.0 - smoothstep(0.06, 0.16, refChLen);\n"
"          float regDarkAmb = is_dark ? (1.0 - brightTSaved) : darkMixSaved;\n"
"          float lightHeadroom = min(refY + 0.036 * diffTail * smoothstep(0.01, 0.08, ambChLen) * neutralRefGate, mix(0.995, 0.945, smoothstep(0.015, 0.08, ambChLen)));\n"
"          float lightAmbY = clamp(lightHeadroom, 0.0, 0.995);\n"
"          float3 lightAmbD = ambCh * mix(1.00, 1.48, diffTail * neutralRefGate);\n"
"          float sLA = 1.0;\n"
"          if (lightAmbD.r > 0.0001) sLA = min(sLA, (1.0 - lightAmbY) / lightAmbD.r); else if (lightAmbD.r < -0.0001) sLA = min(sLA, lightAmbY / (-lightAmbD.r));\n"
"          if (lightAmbD.g > 0.0001) sLA = min(sLA, (1.0 - lightAmbY) / lightAmbD.g); else if (lightAmbD.g < -0.0001) sLA = min(sLA, lightAmbY / (-lightAmbD.g));\n"
"          if (lightAmbD.b > 0.0001) sLA = min(sLA, (1.0 - lightAmbY) / lightAmbD.b); else if (lightAmbD.b < -0.0001) sLA = min(sLA, lightAmbY / (-lightAmbD.b));\n"
"          float3 lightAmbTarget = clamp(float3(lightAmbY) + lightAmbD * clamp(sLA, 0.0, 1.0), 0.0, 1.0);\n"
"          float darkAmbY = clamp(mix(refY, ambY, 0.65) + 0.024 * diffTail * smoothstep(0.01, 0.08, ambChLen), 0.0, 0.995);\n"
"          float3 darkAmbD = ambCh * mix(0.90, 1.12, diffTail);\n"
"          float sDA = 1.0;\n"
"          if (darkAmbD.r > 0.0001) sDA = min(sDA, (1.0 - darkAmbY) / darkAmbD.r); else if (darkAmbD.r < -0.0001) sDA = min(sDA, darkAmbY / (-darkAmbD.r));\n"
"          if (darkAmbD.g > 0.0001) sDA = min(sDA, (1.0 - darkAmbY) / darkAmbD.g); else if (darkAmbD.g < -0.0001) sDA = min(sDA, darkAmbY / (-darkAmbD.g));\n"
"          if (darkAmbD.b > 0.0001) sDA = min(sDA, (1.0 - darkAmbY) / darkAmbD.b); else if (darkAmbD.b < -0.0001) sDA = min(sDA, darkAmbY / (-darkAmbD.b));\n"
"          float3 darkAmbTarget = clamp(float3(darkAmbY) + darkAmbD * clamp(sDA, 0.0, 1.0), 0.0, 1.0);\n"
"          float3 ambTarget = mix(lightAmbTarget, darkAmbTarget, regDarkAmb);\n"
"          float ambStr = mix(0.48, 0.30, regDarkAmb) * neutralRefGate;\n"
"          float ambW = ambStr * wideGate * extBaseValid * extTintGate * solidFade * regMix * ambDelta * bgA;\n"
"          refractColor = mix(refractColor, ambTarget, ambW);\n"
"        }\n"
"        if (need_grad && inward < 1.3) {\n"
"          float2 rimDirRaw = mix(surfaceNormal, extRadDir, 0.36 * extBlurT);\n"
"          float rimDirLen = length(rimDirRaw);\n"
"          float2 rimDir = rimDirLen > 0.001 ? rimDirRaw / rimDirLen : surfaceNormal;\n"
"          float2 pEdge = p + surfaceNormal * inward;\n"
"          float dRimNear = min(mix(6.5, 18.0, extBlurT), extMaxReach);\n"
"          float dRimFar  = min(mix(10.0, 38.0, extBlurT), extMaxReach);\n"
"          float3 rNear = backdrop(bd, g, pEdge + rimDir * dRimNear);\n"
"          float3 rFar  = backdrop(bd, g, pEdge + rimDir * dRimFar);\n"
"          extRimEnv = mix(rNear, rFar, mix(0.42, 0.72, extBlurT));\n"
"          extRimValid = extBaseValid * shortening * bgA;\n"
"        }\n"
"      }\n"
"    }\n"
"    float tintDarkEnv = (1.0 - smoothstep(0.18, 0.55, luma(refractColor))) * bgA;\n"
"    float regDarkContour = tintDarkEnv * max(is_dark ? 1.0 : darkMixSaved, smoothstep(0.08, 0.16, colChroma));\n"
"    float regExtContour = mix(0.34, 0.85, regDarkContour) * solidFade;\n"
"    contourStr = mix(contourStr, regExtContour, regMix);\n"
"    float Y = luma(refractColor);\n"
"    float regDarkOpt = is_dark ? 1.0 : darkMixSaved;\n"
"    float transLum = clamp(Y * (1.0 + lift * (1.0 - Y)), 0.0, 1.0);\n"
"    float Lt = clamp(transLum * transmittance, 0.0, 1.0);\n"
"    // Symmetric monotonic p=4 gentle-knee for non-adaptive Regular overlays (extra > 0.09, e.g. 0x80 Menu/Alert, 0xCC Sheet/Drawer):\n"
"    // - Dark overlays (regDarkOpt=1): caps Lt at Lcap=0.295*(1-regExtra) (0x80 Lcap=0.247 -> pure white ~99/255, gray97 ~89/255 vs native 86) while softer chroma knee preserves vivid iOS 27.0 color bleed.\n"
"    // - Light overlays (regDarkOpt=0): floors darkness D=max(transmittance-Lt,0) at Dcap=0.205 -> pure black ~202/255 with continuous grayscale translucency.\n"
"    // Branchless C1 blend: strictly monotonic in Y and col.a, zero change to 0x67 controls (at frost=0, normal contrast) or adaptive glass.\n"
"    float overlayKnee = smoothstep(0.09, 0.16, regExtra) * regMix * (is_adaptive ? 0.0 : 1.0);\n"
"    float darkKneeW = overlayKnee * regDarkOpt;\n"
"    float lightKneeW = overlayKnee * (1.0 - regDarkOpt) * bgA;\n"
"    float kt = transLum * 2.0;\n"
"    float kt2 = kt * kt;\n"
"    float kDenom = 1.0 + kt2 * kt2;\n"
"    float kInv = pow(kDenom, -0.25);\n"
"    float Lc = Lt * kInv;\n"
"    float D = max(transmittance - Lt, 0.0);\n"
"    float dt = D / 0.205;\n"
"    float dt2 = dt * dt;\n"
"    float dInv = pow(1.0 + dt2 * dt2, -0.25);\n"
"    float Dc = D * dInv;\n"
"    float baseLum = Lt + (Lc - Lt) * darkKneeW + (max(transmittance - Dc, 0.0) - Lt) * lightKneeW;\n"
"    float darkKS = pow(kDenom, -0.075);\n"
"    float kneeScale = 1.0 + (darkKS - 1.0) * darkKneeW + (dInv - 1.0) * lightKneeW;\n"
"    float3 chromaVec = (refractColor - float3(Y)) * (chromaGain * abs(I.p.z) * kneeScale);\n"
"    if (darkAdGlareW > 0.001) {\n"
"      if (baseLum > 0.18) {\n"
"        float over = baseLum - 0.18;\n"
"        float capped = 0.18 + 0.115 * (over / (over + 0.115));\n"
"        float newLum = mix(baseLum, capped, darkAdGlareW);\n"
"        chromaVec *= (newLum / max(baseLum, 0.0001));\n"
"        baseLum = newLum;\n"
"      }\n"
"      float3 testRaw = emission + float3(baseLum) + chromaVec;\n"
"      float peak = max(testRaw.r, max(testRaw.g, testRaw.b));\n"
"      if (peak > 0.44) {\n"
"        float lum0 = luma(emission) + baseLum;\n"
"        float s = clamp((0.44 - lum0) / max(peak - lum0, 0.0001), 0.0, 1.0);\n"
"        chromaVec *= mix(1.0, s, darkAdGlareW);\n"
"      }\n"
"    }\n"
"    float3 transmittedColor = float3(baseLum) + chromaVec;\n"
"    float3 rawBase = emission + transmittedColor;\n"
"    // Exact Wave 3 legacy mapping for zero-veil lenses (col.a == 0); channel-clamped saturation for Clear glass\n"
"    float maxC = max(rawBase.r, max(rawBase.g, rawBase.b));\n"
"    float3 legacyBase = rawBase;\n"
"    if (maxC > 1.0) {\n"
"      float lumL = clamp(luma(emission) + baseLum, 0.0, 0.999);\n"
"      float sL = clamp((1.0 - lumL) / max(maxC - lumL, 0.0001), 0.0, 1.0);\n"
"      legacyBase = float3(lumL) + (rawBase - float3(lumL)) * sL;\n"
"    }\n"
"    legacyBase = clamp(legacyBase, 0.0, 1.0);\n"
"    float clearClampW = smoothstep(0.005, 0.102, col.a);\n"
"    float3 baseColor = mix(legacyBase, clamp(rawBase, float3(0.0), float3(1.0)), clearClampW);\n"
"    if (regMix > 0.001) {\n"
"      float lumR = clamp(luma(emission) + baseLum, 0.001, 0.999);\n"
"      float3 dC = rawBase - float3(lumR);\n"
"      float s = 1.0;\n"
"      if (dC.r > 0.0001) s = min(s, (1.0 - lumR) / dC.r);\n"
"      else if (dC.r < -0.0001) s = min(s, lumR / (-dC.r));\n"
"      if (dC.g > 0.0001) s = min(s, (1.0 - lumR) / dC.g);\n"
"      else if (dC.g < -0.0001) s = min(s, lumR / (-dC.g));\n"
"      if (dC.b > 0.0001) s = min(s, (1.0 - lumR) / dC.b);\n"
"      else if (dC.b < -0.0001) s = min(s, lumR / (-dC.b));\n"
"      float3 regProjected = clamp(float3(lumR) + dC * clamp(s, 0.0, 1.0), 0.0, 1.0);\n"
"      float projMix = mix(0.25, mix(0.08, 0.35, regDarkOpt), smoothstep(0.08, 0.16, regExtra));\n"
"      float3 regMapped = mix(clamp(rawBase, float3(0.0), float3(1.0)), regProjected, projMix);\n"
"      baseColor = mix(baseColor, regMapped, regMix);\n"
"    }\n"
"    float holeTrans = is_dark ? min(transmittance, mix(0.295, 0.247, darkKneeW)) : transmittance;\n"
"    float ga = (g.misc.y > 0.5) ? (1.0 - (1.0 - bgA) * clamp(holeTrans, 0.0, 1.0)) : 1.0;\n"
"    // Honor explicit chromatic tints (non-neutral) if a caller requests colored liquid glass\n"
"    if (colChroma > 0.08 && col.a > 0.001) {\n"
"      float tintW = col.a <= 0.40\n"
"        ? col.a * 1.15\n"
"        : mix(0.46, 0.98, smoothstep(0.40, 0.58, col.a));\n"
"      baseColor = mix(baseColor, col.rgb, tintW);\n"
"      ga = mix(ga, 1.0, tintW);\n"
"    }\n"
"    float spot = 0.0;\n"
"    if (spot_raw > 0.5) {\n"
"      float q = spot_raw - 1.0;\n"
"      float qx = floor(q * 0.000244140625);\n"
"      float qy = q - qx * 4096.0;\n"
"      float2 gp = I.rect.xy + float2(qx * 0.5 - 256.0, qy * 0.5 - 256.0);\n"
"      float gr = max(min(I.rect.z, I.rect.w) * 1.5, 64.0);\n"
"      spot = 1.0 - smoothstep(gr * 0.1, gr, length(p - gp));\n"
"      baseColor = clamp(baseColor + float3(0.08 * spot), 0.0, 1.0);\n"
"    }\n"
"    float2 rn = float2(0.0, 0.0);\n"
"    float tangency = 0.0;\n"
"    if (need_grad) {\n"
"      float2 la = g.misc.zw;\n"
"      rn = float2(\n"
"        surfaceNormal.x * la.x + surfaceNormal.y * la.y,\n"
"        -surfaceNormal.x * la.y + surfaceNormal.y * la.x\n"
"      );\n"
"      tangency = abs(rn.x);\n"
"    }\n"
"    float3 result = baseColor;\n"
"    // (a) Inner bevel shadow (subtracts only transmittedColor; lightFacing = -rn.y)\n"
"    if (bevelShadowStr >= 0.001 && inward < 23.0) {\n"
"      float lightFacing = -rn.y;\n"
"      float shadowShift = 6.0 * lightFacing;\n"
"      float penumbra = 2.0 * shadowShift;\n"
"      float bevelLeading = penumbra > 0.001 ? smoothstep(0.0, penumbra, inward) : 1.0;\n"
"      float bevelFalloff = 1.0 - smoothstep(0.0, 16.0, inward - shadowShift);\n"
"      float wrappedFacing = smoothstep(0.0, 1.0, lightFacing * 0.5 + 0.5);\n"
"      float bevelDir = mix(1.0, wrappedFacing, 0.5);\n"
"      float bevelShadow = clamp(bevelLeading * bevelFalloff * bevelDir * bevelShadowStr, 0.0, 1.0);\n"
"      result = max(result - transmittedColor * bevelShadow, float3(0.0));\n"
"    }\n"
"    // (b) Glint (clear glass top/bottom specular rim + unified bevel wall for Glass::clear)\n"
"    float clearVeilC = smoothstep(0.005, 0.102, col.a);\n"
"    if (I.c2.x > 0.01 && inward < 6.0 && regMix < 0.999) {\n"
"      float glintProfile = mix(\n"
"                         clamp(1.0 - inward / 1.2, 0.0, 1.0) + 0.21 * clamp(1.0 - inward / 4.8, 0.0, 1.0),\n"
"                         clamp(1.0 - inward / 1.35, 0.0, 1.0) + 0.24 * clamp(1.0 - inward / 5.5, 0.0, 1.0),\n"
"                         clearVeilC);\n"
"      float dirLobe = pow(max(1.0 - tangency, 0.0), mix(0.85, 1.35, clearVeilC));\n"
"      float lobe = mix(mix(0.30, 0.08, clearVeilC), 1.0, dirLobe);\n"
"      float glintGain = 0.58 * clearVeilC;\n"
"      float glint = clamp(I.c2.x * glintGain * lobe * glintProfile + spot * 0.08 * glintProfile, 0.0, 1.0) * (1.0 - regMix);\n"
"      float resLum = luma(result);\n"
"      float3 glintTarget = float3(glintLum) + result * glintFaceGain + (result - float3(resLum)) * glintVib;\n"
"      result = clamp(mix(result, glintTarget, glint), 0.0, 1.0);\n"
"      ga = mix(ga, 1.0, glint);\n"
"    }\n"
"    float clrWallW = clearVeilC * (1.0 - regMix);\n"
"    if (clrWallW > 0.001) {\n"
"      float clrAcross = pow(max(1.0 - rn.y * rn.y, 0.0), 1.8);\n"
"      float clrInvS = 1.0 / max(u.scale, 1.0);\n"
"      float clrW0 = mix(max(0.20, 0.50 * clrInvS), max(0.28, 0.75 * clrInvS), clrAcross);\n"
"      float clrL  = mix(0.06, 0.12, clrAcross);\n"
"      float clrCov = mb_bevel_cov(sd_px, u.scale, clrW0, clrL);\n"
"      float clrHair = clamp(clrCov / max(materialAlpha, 0.001), 0.0, 1.0) * clrAcross * clrWallW;\n"
"      if (clrHair > 0.001) {\n"
"        float3 clrEnv = backdrop(bd, g, p + surfaceNormal * 1.2);\n"
"        float clrBrightGate = smoothstep(0.55, 0.90, luma(clrEnv)) * 0.55;\n"
"        float3 clrWall = pow(max(clrEnv, float3(1e-6)), float3(1.75)) * 0.695;\n"
"        result = mix(result, clrWall, clrHair * clrBrightGate);\n"
"      }\n"
"    }\n"
"    if (regMix > 0.001) {\n"
"      float regDark = is_dark ? 1.0 : darkMixSaved;\n"
"      float regDarkRim = regDark * tintDarkEnv;\n"
"      float tintChroma = smoothstep(0.08, 0.16, colChroma);\n"
"      if (I.c2.x > 0.01 && inward < 12.0 && solidFade > 0.001) {\n"
"        float along = -rn.y;\n"
"        float lit = max(along, 0.0);\n"
"        float opp = max(-along, 0.0);\n"
"        float lit2 = lit * lit;\n"
"        float opp2 = opp * opp;\n"
"        float diagPos = max(rn.x * rn.y, 0.0) * 2.0;\n"
"        float diagNeg = max(-rn.x * rn.y, 0.0) * 2.0;\n"
"        float regLobe = 1.0 * (lit2 * lit2) + mix(0.96, 0.94, regDark) * (opp2 * opp2) + mix(0.22, 0.30, regDark) * (diagPos * diagPos) + mix(0.18, 0.24, regDark) * (diagNeg * diagNeg) + 0.06 * regDark;\n"
"        float regCore = exp(-max(0.0, inward - 0.15) / 0.55);\n"
"        float regTailLight = exp(-inward / 2.2);\n"
"        float regTailDark  = exp(-inward / 3.8);\n"
"        float regTail = mix(regTailLight, regTailDark, regDark);\n"
"        float resY = luma(result);\n"
"        float regEaseLight = 1.0 / max(1.0 - 0.75 * resY, 0.25);\n"
"        float regEase = mix(regEaseLight, pow(max(1.0 - resY, 0.0), 0.25), regDarkRim);\n"
"        float regRimGain = 1.0 * clamp(I.c2.x / 0.6, 0.0, 1.5);\n"
"        float regCoreW = mix(0.21, 0.24, regDark) * (1.0 - tintChroma);\n"
"        float regTailW = mix(0.030, 0.055, regDark) * (1.0 - tintChroma);\n"
"        float lipY = resY;\n"
"        if (inward < 3.0 && regDarkRim < 0.999 && tintChroma < 0.999) {\n"
"          float2 tang = float2(-surfaceNormal.y, surfaceNormal.x) * 3.0;\n"
"          float3 lt1 = backdrop(bd, g, sampleBase + disp + tang);\n"
"          float3 lt2 = backdrop(bd, g, sampleBase + disp - tang);\n"
"          float avgBgY = (Y + luma(lt1) + luma(lt2)) * (1.0 / 3.0);\n"
"          lipY = clamp(luma(emission) + clamp(avgBgY * (1.0 + lift * (1.0 - avgBgY)) * transmittance, 0.0, 1.0), 0.0, 1.0);\n"
"        }\n"
"        float brightLip = smoothstep(0.77, 0.92, lipY) * (1.0 - regDarkRim) * (1.0 - tintChroma);\n"
"        float lipProfile = exp(-max(0.0, inward - 0.70) / 0.55);\n"
"        float regRim = (regRimGain * regLobe * (regEase * (regCoreW * regCore + regTailW * regTail) + 0.72 * brightLip * lipProfile) + spot * 0.10 * regCore) * solidFade * regMix;\n"
"        float regDarkEff = is_dark ? (1.0 - brightTSaved) : darkMixSaved;\n"
"        float extRimCoreMask = 1.0 - smoothstep(0.55, 0.95, inward);\n"
"        float3 rimSrc = mix(refractColor, extRimEnv, extRimValid * extRimCoreMask * (1.0 - tintChroma));\n"
"        float rimSrcY = luma(rimSrc);\n"
"        float3 rimSrcChroma = rimSrc - float3(rimSrcY);\n"
"        float rimPeak = max(0.0, max(rimSrcChroma.r, max(rimSrcChroma.g, rimSrcChroma.b)));\n"
"        float extRimW = extRimValid * extRimCoreMask * (1.0 - tintChroma);\n"
"        float3 rimTarget = clamp(float3(1.0 - rimPeak * 0.25 * extRimW) + rimSrcChroma * mix(1.0, mix(1.12, 1.40, regDarkEff), extRimW), 0.0, 1.0);\n"
"        float3 neutralRimRes = mix(result, rimTarget, clamp(regRim, 0.0, 1.0));\n"
"        float gaRim = mix(ga, 1.0, clamp(regRim, 0.0, 1.0));\n"
"        if (extRimValid > 0.001 && inward < 1.2 && tintChroma < 0.999) {\n"
"          float extRimGate = smoothstep(0.0, 0.6, I.c2.x);\n"
"          float extDiff = smoothstep(0.02, 0.18, length(extRimEnv - rawRefractColor));\n"
"          float extLip = exp(-inward / 0.28) * (1.0 - smoothstep(0.65, 1.05, inward));\n"
"          float extDir = mix(0.62, 1.0, min(abs(rn.y) * 1.15, 1.0));\n"
"          float extY = luma(extRimEnv);\n"
"          float3 extCh = extRimEnv - float3(extY);\n"
"          float lightLum = clamp(mix(0.985, extY, mix(0.24, 0.14, extBlurT)), 0.84, 0.985);\n"
"          float3 lightD = extCh * mix(0.95, 0.55, extBlurT);\n"
"          float sL = 1.0;\n"
"          if (lightD.r > 0.0001) sL = min(sL, (1.0 - lightLum) / lightD.r); else if (lightD.r < -0.0001) sL = min(sL, lightLum / (-lightD.r));\n"
"          if (lightD.g > 0.0001) sL = min(sL, (1.0 - lightLum) / lightD.g); else if (lightD.g < -0.0001) sL = min(sL, lightLum / (-lightD.g));\n"
"          if (lightD.b > 0.0001) sL = min(sL, (1.0 - lightLum) / lightD.b); else if (lightD.b < -0.0001) sL = min(sL, lightLum / (-lightD.b));\n"
"          float3 extReflLight = clamp(float3(lightLum) + lightD * clamp(sL, 0.0, 1.0), 0.0, 1.0);\n"
"          float darkLum = clamp(extY * 1.28 + 0.16, 0.0, 0.90);\n"
"          float3 darkD = extCh * mix(2.15, 2.75, extBlurT);\n"
"          float sD = 1.0;\n"
"          if (darkD.r > 0.0001) sD = min(sD, (1.0 - darkLum) / darkD.r); else if (darkD.r < -0.0001) sD = min(sD, darkLum / (-darkD.r));\n"
"          if (darkD.g > 0.0001) sD = min(sD, (1.0 - darkLum) / darkD.g); else if (darkD.g < -0.0001) sD = min(sD, darkLum / (-darkD.g));\n"
"          if (darkD.b > 0.0001) sD = min(sD, (1.0 - darkLum) / darkD.b); else if (darkD.b < -0.0001) sD = min(sD, darkLum / (-darkD.b));\n"
"          float3 extReflDark = clamp(float3(darkLum) + darkD * clamp(sD, 0.0, 1.0), 0.0, 1.0);\n"
"          float3 extRefl = mix(extReflLight, extReflDark, regDarkEff);\n"
"          float extMixW = clamp(mix(mix(0.65, 0.52, extBlurT), mix(0.88, 0.86, extBlurT), regDarkEff) * extRimGate * extDir * extLip * extDiff * extRimValid * solidFade * regMix * (1.0 - tintChroma), 0.0, 0.90);\n"
"          neutralRimRes = mix(neutralRimRes, extRefl, extMixW);\n"
"          gaRim = mix(gaRim, 1.0, extMixW);\n"
"        }\n"
"        if (tintChroma > 0.001) {\n"
"          float tintCore = exp(-inward / 0.80);\n"
"          float tintTail = exp(-inward / 3.2);\n"
"          float tintRimAmt = clamp(regRimGain * regLobe * (0.95 * tintCore + 0.12 * tintTail) * solidFade * regMix, 0.0, 1.0);\n"
"          float3 rLin = pow(max(result, float3(0.0)), float3(2.2));\n"
"          float brightTint = smoothstep(0.40, 0.70, resY);\n"
"          float3 tintLit = pow(clamp(rLin * (1.0 + mix(1.05, 0.65, brightTint) * tintRimAmt) + float3(mix(0.007, 0.002, brightTint) * tintRimAmt), float3(0.0), float3(1.0)), float3(1.0 / 2.2));\n"
"          result = mix(neutralRimRes, tintLit, tintChroma);\n"
"          ga = mix(gaRim, mix(ga, 1.0, tintRimAmt), tintChroma);\n"
"        } else {\n"
"          result = neutralRimRes;\n"
"          ga = gaRim;\n"
"        }\n"
"      }\n"
"    }\n"
"    if (isModernDrop) {\n"
"      float backdropLuma = clamp(Y, 0.0, 1.0);\n"
"      float brightBg = smoothstep(0.35, 0.75, backdropLuma);\n"
"      float liftGain = isRoleTab ? 0.025 : (isRoleSeg ? 0.014 : 0.098);\n"
"      float3 dropRes = clamp(refractColor + float3(liftGain * pressAmt * (1.0 - backdropLuma)), 0.0, 1.0);\n"
"      float domeScale = clamp(I.rect.w / 38.4, 0.80, 1.35);\n"
"      float sizeGate = smoothstep(28.0, 36.0, I.rect.w);\n"
"      float roleVol = isRoleSeg ? 0.65 : (isRoleTab ? mix(0.35, 0.90, brightBg) : 1.0);\n"
"      float upperFace = smoothstep(-0.15, 0.85, -surfaceNormal.y);\n"
"      float qTop = (inward - 8.5 * domeScale) / (6.2 * domeScale);\n"
"      float topEnv = exp(-qTop * qTop) * upperFace * smoothstep(0.55, 1.80, inward);\n"
"      dropRes *= (1.0 - mix(0.20, 0.082, brightBg) * roleVol * sizeGate * pressAmt * topEnv);\n"
"      float sideArc = pow(max(abs(surfaceNormal.x), 0.0), 1.6);\n"
"      float sideEnv = exp(-max(inward - 1.5, 0.0) / 4.2) * smoothstep(0.25, 1.20, inward);\n"
"      dropRes *= (1.0 - mix(0.16, 0.065, brightBg) * (isRoleSeg ? 0.55 : (isRoleTab ? mix(0.25, 0.70, brightBg) : 1.0)) * sizeGate * pressAmt * sideArc * sideEnv);\n"
"      float lowerFace = smoothstep(0.10, 0.85, surfaceNormal.y);\n"
"      float lowEnv = exp(-max(inward - 2.0, 0.0) / 5.5) * smoothstep(0.50, 1.60, inward);\n"
"      dropRes = clamp(dropRes + float3(mix(0.032, 0.025, brightBg) * lowerFace * lowEnv * (isRoleSeg ? 0.45 : (isRoleTab ? mix(0.30, 0.60, brightBg) : 1.0)) * sizeGate * pressAmt), 0.0, 1.0);\n"
"      // Stage A: Razor-sharp inner 3D specular rim\n"
"      if (pressAmt > 0.001 && inward < 12.0) {\n"
"        float litTop = max(-surfaceNormal.y, 0.0);\n"
"        float litBot = max(surfaceNormal.y, 0.0);\n"
"        float lobeFloor = isRoleTab ? 0.10 : 0.05;\n"
"        float dirLobe = 0.96 * (litTop * litTop * litTop) + 1.00 * (litBot * litBot * litBot) + lobeFloor;\n"
"        float rimStart = isRoleSw ? 0.60 : 0.45;\n"
"        float rimPeak  = isRoleSw ? 0.95 : 0.75;\n"
"        float rimRise = smoothstep(rimStart, rimPeak, inward);\n"
"        float rimInset = max(inward - rimPeak, 0.0);\n"
"        float rimEnv = rimRise * (0.90 * exp(-rimInset / 0.38) + 0.14 * exp(-rimInset / 1.50));\n"
"        float rimStr = (isRoleTab ? mix(0.28, 0.34, brightBg) : mix(0.24, 0.14, brightBg)) * dirLobe * rimEnv * pressAmt;\n"
"        dropRes = mix(dropRes, float3(1.0), clamp(rimStr, 0.0, 1.0));\n"
"      }\n"
"      // Stage B: Slim Apple-exact closed-form analytic 3D bevel wall\n"
"      float across = pow(max(1.0 - surfaceNormal.y * surfaceNormal.y, 0.0), 1.35);\n"
"      float invS = 1.0 / max(u.scale, 1.0);\n"
"      float hsWall = clamp(halfMinor / 19.2, 0.72, 1.25);\n"
"      float w0_top = (isRoleSw ? 0.30 : (isRoleTab ? mix(0.18, 0.26, brightBg) : 0.24)) * hsWall;\n"
"      float w0_side = isRoleTab ? max(0.30 * hsWall, 1.10 * invS) : max(0.52 * hsWall, 1.55 * invS);\n"
"      float w0_eff = mix(w0_top, w0_side, across);\n"
"      float L_eff  = (isRoleTab ? mix(mix(0.08, 0.16, across), mix(0.13, 0.16, across), brightBg) : mix(0.12, 0.42, across)) * hsWall;\n"
"      float wallCov = mb_bevel_cov(sd_px, u.scale, w0_eff, L_eff);\n"
"      float wallRaw = clamp(wallCov / max(materialAlpha, 0.001), 0.0, 1.0);\n"
"      float wallBlend = wallRaw * (isRoleTab ? 0.82 : 0.94) * pressAmt;\n"
"      if (wallBlend > 0.001) {\n"
"        float3 envBg = backdrop(bd, g, p + surfaceNormal * 1.2);\n"
"        float envY = luma(envBg);\n"
"        float envChroma = max(envBg.r, max(envBg.g, envBg.b)) - min(envBg.r, min(envBg.g, envBg.b));\n"
"        float brightEnv = smoothstep(0.35, 0.75, envY);\n"
"        float satEnv = smoothstep(0.10, 0.35, envChroma);\n"
"        float wallMix = isRoleTab ? (wallRaw * mix(0.62, 0.92, brightEnv) * pressAmt) : wallBlend;\n"
"        float neutralMul = isRoleTab ? mix(0.50, mix(0.79, 0.72, across), brightEnv) : mix(0.26, mix(0.80, 0.72, across), brightEnv);\n"
"        float3 neutralTint = envBg * neutralMul;\n"
"        float3 satTint = pow(max(envBg, float3(0.0001)), float3(1.25)) * 0.72;\n"
"        dropRes = mix(dropRes, mix(neutralTint, satTint, satEnv), wallMix);\n"
"      }\n"
"      // Stage C: Segmented prismatic edge dispersion AFTER Stage B\n"
"      if (pressAmt > 0.001 && inward < 12.0 && abs(I.pad.z) > 0.005) {\n"
"        float chromaGate = smoothstep(0.005, 0.04, abs(I.pad.z));\n"
"        float pBand = smoothstep(0.45, 0.75, inward) * (1.0 - smoothstep(1.35, 2.20, inward)) * pressAmt * chromaGate;\n"
"        float hw = smoothstep(0.35, 0.85, abs(surfaceNormal.x));\n"
"        float3 pDark = mix(float3(-0.025, 0.015, 0.045), float3(0.048, 0.012, -0.032), hw);\n"
"        float3 pLight = mix(float3(-0.085, 0.0, 0.0), float3(-0.015, -0.075, -0.135), hw);\n"
"        float3 pAdd = mix(pDark, pLight, brightBg);\n"
"        dropRes = clamp(dropRes + pAdd * pBand, 0.0, 1.0);\n"
"      }\n"
"      result = mix(result, dropRes, droplet);\n"
"    } else if (is_drop) {\n"
"      float backdropLuma = clamp(Y, 0.0, 1.0);\n"
"      float brightBg = smoothstep(0.35, 0.75, backdropLuma);\n"
"      float liftGain = isRoleTab ? 0.025 : (isRoleSeg ? 0.008 : 0.101);\n"
"      float lensLift = liftGain * pressAmt * (1.0 - backdropLuma) * (1.0 - dropHairline);\n"
"      result = clamp(result + float3(lensLift), 0.0, 1.0);\n"
"      float domeScale = clamp(I.rect.w / 38.0, 0.85, 1.35);\n"
"      float sizeGate = smoothstep(28.0, 36.0, I.rect.w);\n"
"      float upperFace = smoothstep(0.0, 0.75, -surfaceNormal.y);\n"
"      float qDome = (inward - 9.2 * domeScale) / (5.6 * domeScale);\n"
"      float domeEnv = exp(-qDome * qDome) * upperFace * smoothstep(0.65, 1.85, inward);\n"
"      float domeStr = mix(0.196, 0.086, brightBg) * sizeGate;\n"
"      result *= (1.0 - domeStr * domeEnv * pressAmt);\n"
"      float lightSh = smoothstep(0.75, 1.80, inward) * (1.0 - smoothstep(4.5, 8.5, inward))\n"
"                    * upperFace * brightBg * sizeGate;\n"
"      result *= (1.0 - 0.032 * lightSh * pressAmt);\n"
"      if (pressAmt > 0.001 && inward < 12.0) {\n"
"        float along = surfaceNormal.y;\n"
"        float lit = max(along, 0.0);\n"
"        float opp = max(-along, 0.0);\n"
"        float lobe = 1.0 * (lit * lit * lit) + 1.0 * (opp * opp * opp) + 0.14;\n"
"        float insetPt = max(0.0, inward - 0.60);\n"
"        float core = exp(-insetPt / 0.48);\n"
"        float shoulder = exp(-insetPt / 1.8);\n"
"        float ease = pow(max(1.0 - clamp(luma(result), 0.0, 1.0), 0.0), 0.30);\n"
"        float rimGain = isRoleSeg ? 0.85 : (isRoleTab ? 0.75 : 1.0);\n"
"        float rimLight = pressAmt * rimGain * lobe * ease * (0.205 * core + 0.025 * shoulder) * (1.0 - dropHairline);\n"
"        result = mix(result, float3(1.0), clamp(rimLight, 0.0, 1.0));\n"
"      }\n"
"      if (isRoleSeg && pressAmt > 0.01) {\n"
"        float prismBand = smoothstep(0.40, 0.80, inward) * (1.0 - smoothstep(1.15, 1.85, inward))\n"
"                        * (1.0 - dropHairline) * pressAmt;\n"
"        if (prismBand > 0.001) {\n"
"          float hW = smoothstep(0.35, 0.85, abs(surfaceNormal.x));\n"
"          float3 rawD = mix(float3(-0.045, 0.015, 0.030), float3(0.075, -0.015, -0.060), hW);\n"
"          float3 darkPrism = (rawD - float3(luma(rawD))) * 0.60 * prismBand;\n"
"          float3 lightPrism = mix(float3(-0.068, 0.0, 0.0), float3(-0.055, -0.148, -0.188), hW) * prismBand;\n"
"          float3 pD = mix(darkPrism, lightPrism, brightBg);\n"
"          float sP = 1.0;\n"
"          if (pD.r > 0.0001) sP = min(sP, (1.0 - result.r) / pD.r); else if (pD.r < -0.0001) sP = min(sP, result.r / (-pD.r));\n"
"          if (pD.g > 0.0001) sP = min(sP, (1.0 - result.g) / pD.g); else if (pD.g < -0.0001) sP = min(sP, result.g / (-pD.g));\n"
"          if (pD.b > 0.0001) sP = min(sP, (1.0 - result.b) / pD.b); else if (pD.b < -0.0001) sP = min(sP, result.b / (-pD.b));\n"
"          result = clamp(result + pD * clamp(sP, 0.0, 1.0), 0.0, 1.0);\n"
"        }\n"
"      }\n"
"      if (dropHairline > 0.001) {\n"
"        float across = pow(max(1.0 - surfaceNormal.y * surfaceNormal.y, 0.0), 1.75);\n"
"        float shadeTB = mix(0.047, 0.161, brightBg);\n"
"        float shadeEnds = mix(0.063, 0.239, brightBg);\n"
"        float shade = mix(shadeTB, shadeEnds, across) * pressAmt;\n"
"        result = max(result - float3(shade * dropHairline), float3(0.0));\n"
"      }\n"
"    }\n"
"    result = clamp(result + mb_dither(p * u.scale) / 255.0, 0.0, 1.0);\n"
"    // (c) External dark contour (iOS 27.0: Regular controls place their bevel contour strictly outside the silhouette so circles keep full geometric width)\n"
"    float extUseCov = ext_cov;\n"
"    float contourDir = mix(1.0, tangency, contourDirAmt);\n"
"    if (regMix > 0.001 && solidFade > 0.001) {\n"
"      float extAcross = pow(max(1.0 - rn.y * rn.y, 0.0), mix(1.6, 2.2, regDarkContour));\n"
"      float extRegDir = mix(mix(0.30, 1.0, extAcross), extAcross, regDarkContour);\n"
"      float extW0 = mix(0.28, 0.36, extAcross);\n"
"      float extL  = mix(0.08, 0.14, extAcross);\n"
"      float regExtCov = mb_bevel_cov(outward, u.scale, extW0, extL);\n"
"      extUseCov = mix(ext_cov, regExtCov, regMix);\n"
"      contourDir = mix(contourDir, extRegDir, regMix);\n"
"    }\n"
"    float extA = extUseCov * contourStr * contourDir * I.c2.w;\n"
"    float visA = materialAlpha * I.c2.w;\n"
"    if (g.misc.y > 0.5) ga = max(ga, max(result.r, max(result.g, result.b)));\n"
"    return float4(result * visA, clamp(visA * ga + extA, 0.0, 1.0)) * cover;\n"
"  }\n"
"  if (kind == 8) {\n"
"    // Foreground optical lens (View::content_lens): offscreen blurred child subtree refracted through spherical bevel & SDF-clipped\n"
"    float4 s_rr = max(max(I.rr.x, I.rr.y), max(I.rr.z, I.rr.w)) > 0.0 ? I.rr : float4(I.p.x);\n"
"    float d = sd_rr(p, I.rect, s_rr);\n"
"    float materialAlpha = clamp(0.5 - d * u.scale, 0.0, 1.0);\n"
"    if (materialAlpha < 0.005) discard_fragment();\n"
"    float halfMinor = max(min(I.rect.z, I.rect.w) * 0.5, 1.0);\n"
"    float inward = max(-d, 0.0);\n"
"    float rawBevel = max(I.c2.y, 0.0);\n"
"    float rawAmount = max(I.c2.z, 0.0);\n"
"    float lensScale = min(1.0, halfMinor / max(rawBevel, 0.001));\n"
"    float bevel = rawBevel * lensScale;\n"
"    float amount = rawAmount * lensScale;\n"
"    float2 disp = float2(0.0, 0.0);\n"
"    if (inward < bevel && bevel > 0.001 && amount > 0.001) {\n"
"      float e = 0.5;\n"
"      float2 grad = float2(\n"
"        sd_rr(p + float2(e, 0.0), I.rect, s_rr) - sd_rr(p - float2(e, 0.0), I.rect, s_rr),\n"
"        sd_rr(p + float2(0.0, e), I.rect, s_rr) - sd_rr(p - float2(0.0, e), I.rect, s_rr)\n"
"      ) / (2.0 * e);\n"
"      float gradLen = length(grad);\n"
"      float2 surfaceNormal = gradLen > 0.0001 ? grad / gradLen : float2(0.0, 0.0);\n"
"      float shortening = sqrt(clamp(gradLen, 0.0, 1.0));\n"
"      float bevelX = 1.0 - clamp(inward / bevel, 0.0, 1.0);\n"
"      float mag = amount * (1.0 - sqrt(max(1.0 - bevelX * bevelX, 0.0))) * shortening;\n"
"      disp = surfaceNormal * (-mag);\n"
"    }\n"
"    float2 uv = (p + disp) * g.map.xy + g.map.zw;\n"
"    float4 fg = float4(0.0);\n"
"    if (uv.x >= g.uvc.x && uv.x <= g.uvc.z && uv.y >= g.uvc.y && uv.y <= g.uvc.w) {\n"
"      constexpr sampler sb(filter::linear, address::clamp_to_edge);\n"
"      fg = bd.sample(sb, uv);\n"
"    }\n"
"    return fg * (materialAlpha * col.a) * cover;\n"
"  }\n"
"  if (kind == 10) {\n"
"    // a line segment (p.z == 0) or analytic quadratic Bezier stroke (p.z > 0.5, control point in rr.xy)\n"
"    float2 lp = p - I.rect.xy;\n"
"    float d;\n"
"    if (I.p.z > 0.5) {\n"
"      float s = max(max(I.rect.z, I.rect.w), 1.0);\n"
"      float2 sz = I.rect.zw / s;\n"
"      d = mb_sd_quad_bezier(lp / s, I.c2.xy * sz, I.rr.xy * sz, I.c2.zw * sz) * s - I.p.x;\n"
"    } else {\n"
"      float2 a = I.c2.xy * I.rect.zw, c = I.c2.zw * I.rect.zw;\n"
"      float2 ab = c - a;\n"
"      float h = clamp(dot(lp - a, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);\n"
"      d = length(lp - a - ab * h) - I.p.x;\n"
"    }\n"
"    if (I.pad.w > 0.0) col.rgb = mb_adaptive_ink_at(bd, ad_tex, g, col.rgb, I.pad.yz, I.pad.w * 0.5, I.rect);\n"
"    return (float4(col.rgb, 1.0) * col.a * clamp(0.5 - d * u.scale, 0.0, 1.0)) * cover;\n"
"  }\n"
"  if (kind == 11) {\n"
"    // a band of a shape (src/ui/shape.mbt): c2.y of its pieces from c2.x, among those after every instance drawn\n"
"    // (kind 12, from instance u.pad on); p.x half its stroke's width (0: a fill, c2.z 1 by the even-odd rule)\n"
"    const device float4* sg = (const device float4*)inst;\n"
"    int first = int(I.c2.x + 0.5), n = int(I.c2.y + 0.5), base = int(u.pad + 0.5);\n"
"    float a;\n"
"    if (I.p.x > 0.0) {\n"
"      float d = 1e9;\n"
"      for (int i = first; i < first + n; i++) d = min(d, mb_seg_dist(sg[(base + i / 6) * 15 + mb_seg_slot(i)], I.rect, p));\n"
"      a = clamp(0.5 - (d - I.p.x) * u.scale, 0.0, 1.0);\n"
"    } else {\n"
"      float4 w = float4(0.0);\n"
"      for (int i = first; i < first + n; i++) w += mb_path_rows(sg[(base + i / 6) * 15 + mb_seg_slot(i)], I.rect, p, u.scale);\n"
"      a = mb_path_cover(w, I.c2.z);\n"
"    }\n"
"    if (I.pad.w > 0.0) col.rgb = mb_adaptive_ink_at(bd, ad_tex, g, col.rgb, I.pad.yz, I.pad.w * 0.5, I.rect);\n"
"    return (float4(col.rgb, 1.0) * col.a * a) * cover;\n"
"  }\n"
"  float2 hs = I.rect.zw * 0.5;\n"
"  float2 c = I.rect.xy + hs;\n"
"  // worked out as near the curve as its cover, its edge or its blur reaches\n"
"  float band = 1.0 + 2.0 / u.scale + (kind == 2 ? max(I.p.w, 0.5) : max(I.p.z, 0.0));\n"
"  // (its corner by p: rr, each its own)\n"
"  float rad = mb_corner_of(p - c, I.rr);\n"
"  float d = mb_sd_box(p - c, hs, rad, band);\n"
"  if ((kind == 0 && I.pad.w >= 0.0) || kind == 5) d = notch_cut(d, p, I.rect, I.pad);\n"
"  if (kind == 9) { float rr = min(rad, min(hs.x, hs.y)); float2 q = abs(p - c) - hs + rr; if (length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rr > 0.0) discard_fragment(); return (float4(0.0)) * cover; }\n"
"  if (kind == 4) {\n"
"    constexpr sampler si(filter::linear, mip_filter::linear, address::clamp_to_edge);\n"
"    float2 uv = I.c2.xy + clamp((p - I.rect.xy) / I.rect.zw, 0.0, 1.0) * I.c2.zw;\n"
"    float2 dudx = dfdx(p) / max(I.rect.zw, float2(1e-6)) * I.c2.zw;\n"
"    float2 dudy = dfdy(p) / max(I.rect.zw, float2(1e-6)) * I.c2.zw;\n"
"    float4 pic = (one_kind == 4)\n"
"      ? mb_sample_pic(img, si, clamp(uv, 0.0, 1.0), dudx, dudy)\n"
"      : img.sample(si, clamp(uv, 0.0, 1.0), gradient2d(dudx, dudy));\n"
"    float edge = clamp(0.5 - d * u.scale, 0.0, 1.0);\n"
"    if (I.pad.z > 0.5) {\n"
"      // Only picture edges inside the frame cut it; the frame's clip covers shared edges once.\n"
"      float2 size = I.rect.zw / I.c2.zw, lo = I.rect.xy - I.c2.xy * size, hi = lo + size;\n"
"      float2 a = lo - p, b = p - hi;\n"
"      if (I.c2.x >= 0.0) a.x = -1e9; if (I.c2.y >= 0.0) a.y = -1e9;\n"
"      if (I.c2.x + I.c2.z <= 1.0) b.x = -1e9; if (I.c2.y + I.c2.w <= 1.0) b.y = -1e9;\n"
"      float2 q = max(a, b);\n"
"      float pd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);\n"
"      pic *= I.pad.w * clamp(0.5 - pd * u.scale, 0.0, 1.0);\n"
"      pic += float4(col.rgb * I.pad.y, I.pad.y) * (1.0 - pic.a);\n"
"      if (I.rot.w < 0.5 && max(max(I.rr.x, I.rr.y), max(I.rr.z, I.rr.w)) <= 0.0 &&\n"
"          max(max(I.rc.x, I.rc.y), max(I.rc.z, I.rc.w)) > 0.0 &&\n"
"          all(I.clip.xy >= I.rect.xy) && all(I.clip.zw <= I.rect.xy + I.rect.zw)) edge = 1.0;\n"
"    }\n"
"    return (pic * (col.a * edge)) * cover;\n"
"  }\n"
"  if (kind == 2) {\n"
"    // as Core Animation's: the shape blurred, sigma its radius (corner.sl mb_shadow)\n"
"    float a = mb_shadow(p - c, hs, rad, max(I.p.w, 0.25));\n"
"    // glass's: only outside its box (c2.y above it)\n"
"    if (I.c2.x > 0.5) {\n"
"      float2 q = p - c + float2(0.0, I.c2.y);\n"
"      float db = mb_sd_box(q, hs, mb_corner_of(q, I.rr), band);\n"
"      a *= clamp(0.5 + db * u.scale, 0.0, 1.0);\n"
"    }\n"
"    return (float4(col.rgb, 1.0) * col.a * a) * cover;\n"
"  }\n"
"  float a = clamp(0.5 - d * u.scale, 0.0, 1.0);\n"
"  if (kind == 5) {\n"
"    // frosted: the blurred copy, saturated, the tint over it; the rim (a hairline inside the edge, the top left lit,\n"
"    // the foot and right less), 0.5 wide. Through (misc.y): as much of it as was a native view's hole left out (its\n"
"    // alpha), for the view's own frost under the canvas to show there\n"
"    float4 b4 = backdrop4(bd, g, p);\n"
"    float u_soft = smoothstep(0.0, 1.0, fade_r);\n"
"    float soft_w = pow(u_soft, 1.75);\n"
"    float ps = prog_glass ? soft_w : 1.0;\n"
"    float sat = prog_glass ? mix(1.0, I.p.z, ps) : I.p.z;\n"
"    float3 g3 = max(mix(float3(luma(b4.rgb)), b4.rgb, sat), 0.0);\n"
"    float ta = prog_glass ? (col.a * soft_w) : col.a;\n"
"    float ga = g.misc.y > 0.5 ? b4.a : 1.0;\n"
"    g3 = g3 * (1.0 - ta) + col.rgb * ta;\n"
"    ga = ga * (1.0 - ta) + ta;\n"
"    if (I.c2.x + I.c2.y + I.c2.z > 0.0) {\n"
"      float edge = clamp(0.5 + (d + 0.5) * u.scale, 0.0, 1.0);\n"
"      float lit = clamp(0.5 + notch_cut(sd_rr(p - 0.5, I.rect, I.rr), p - 0.5, I.rect, I.pad) * u.scale, 0.0, 1.0);\n"
"      float back = clamp(0.5 + notch_cut(sd_rr(p + 0.5, I.rect, I.rr), p + 0.5, I.rect, I.pad) * u.scale, 0.0, 1.0);\n"
"      float4 w4 = mix(float4(g3, ga), float4(1.0), I.c2.x * edge);\n"
"      w4 = mix(w4, float4(1.0), I.c2.y * lit);\n"
"      w4 = mix(w4, float4(1.0), I.c2.z * back);\n"
"      g3 = w4.rgb; ga = w4.a;\n"
"    }\n"
"    // dithered as it is written to 8 bits, so its slopes do not band (corner.sl mb_dither)\n"
"    g3 += mb_dither(p * u.scale) * ps / 255.0;\n"
"    float edge_a = prog_glass ? smoothstep(0.0, 0.04, fade_r) : 1.0;\n"
"    return (float4(g3, ga) * (a * I.c2.w * edge_a)) * cover;\n"
"  }\n"
"  if (kind == 0 && I.pad.w < 0.0) {\n"
"    float ch = max(max(col.r, col.g), col.b) - min(min(col.r, col.g), col.b);\n"
"    float3 cap = ad_sample_ink(bd, ad_tex, g, I.pad.yz, -I.pad.w * 0.5);\n"
"    int code = int(floor(clamp(cap.z, 0.0, 1.0) * 255.0 + 0.5));\n"
"    float darkT = (code >= 160) ? cap.x : 0.0;\n"
"    if (ch <= 0.08 && luma(col.rgb) < 0.5) {\n"
"      col.rgb = mix(col.rgb, float3(1.0), darkT);\n"
"      col.a = mix(col.a, (col.a >= 0.75) ? col.a : min(col.a * 1.45, 0.28), darkT);\n"
"    } else if (ch > 0.08 && luma(col.rgb) < 0.35) {\n"
"      col.rgb = mix(col.rgb, min(col.rgb * 1.65 + float3(0.28, 0.20, 0.18), float3(1.0)), darkT);\n"
"      col.a = mix(col.a, (col.a >= 0.75) ? col.a : min(col.a * 1.45, 0.36), darkT);\n"
"    }\n"
"  }\n"
"  float4 fill = float4(col.rgb, 1.0) * col.a;\n"
"  float bw = I.p.z;\n"
"  if (bw > 0.0) {\n"
"    float inner = clamp(0.5 - (d + bw) * u.scale, 0.0, 1.0);\n"
"    float4 bc = float4(I.c2.rgb, 1.0) * I.c2.a;\n"
"    fill = mix(bc, fill, inner);\n"
"  }\n"
"  return (fill * a) * cover;\n"
"}\n"
"// the flat pipeline: a ground's colour where it is covered whole, cut by straight clips alone (bands.h mb_pieces): what\n"
"// fmain writes there, nothing worked out\n"
"fragment float4 fflat(VOut in [[stage_in]], const device Inst* inst [[buffer(0)]]) {\n"
"  float4 col = inst[in.iid].color;\n"
"  return float4(col.rgb, 1.0) * col.a;\n"
"}\n"
"// fast path for simple axis-aligned, sharp-cornered, borderless rectangles (same AA as fmain)\n"
"fragment float4 frect(VOut in [[stage_in]], const device Inst* inst [[buffer(0)]], constant U& u [[buffer(1)]]) {\n"
"  Inst I = inst[in.iid];\n"
"  float2 p = in.pos.xy / u.scale;\n"
"  if (p.x < I.clip.x || p.y < I.clip.y || p.x > I.clip.z || p.y > I.clip.w) discard_fragment();\n"
"  float2 hs = I.rect.zw * 0.5;\n"
"  float2 q = abs(p - (I.rect.xy + hs)) - hs;\n"
"  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);\n"
"  return (float4(I.color.rgb, 1.0) * I.color.a) * clamp(0.5 - d * u.scale, 0.0, 1.0);\n"
"}\n"
"// The backdrop's passes: a.xy the source's texel, a.z 1 halving (dual Kawase's down), 2 a Gaussian along b.zw of\n"
"// deviation a.w (texels); b.xy the last texel's centre of what the source holds\n"
"struct PU { float4 a; float4 b; float4 map, box, rr, clip, rc, rs, outer, orc, ors; };\n"
"vertex float4 vpost(uint vid [[vertex_id]]) {\n"
"  float2 c = float2(vid == 1 ? 3.0 : -1.0, vid == 2 ? 3.0 : -1.0);\n"
"  return float4(c, 0.0, 1.0);\n"
"}\n"
"fragment float4 fcopy(float4 pos [[position]], texture2d<float, access::read> src [[texture(0)]]) {\n"
"  return src.read(uint2(pos.xy));\n"
"}\n"
"fragment float4 fpost(float4 pos [[position]], texture2d<float> src [[texture(0)]],\n"
"    texture2d<float> lv1 [[texture(1)]], texture2d<float> lv2 [[texture(2)]], texture2d<float> lv3 [[texture(3)]],\n"
"    constant PU& u [[buffer(0)]]) {\n"
"  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
"  float2 ts = u.a.xy;\n"
"  float2 lo = ts * 0.5, hi = u.b.xy;\n"
"  if (u.a.z > 5.5) {\n"
"    float2 uv = pos.xy * ts;\n"
"    float2 dir = u.b.zw * ts;\n"
"    float sg = u.a.w;\n"
"    float k = -0.5 / (sg * sg);\n"
"    float4 c0 = src.sample(s, clamp(uv, lo, hi));\n"
"    float4 oS = c0;\n"
"    float3 oL = pow(max(c0.rgb, float3(0.0)), float3(2.2));\n"
"    float wsum = 1.0;\n"
"    int n = int(ceil(sg * 3.0));\n"
"    for (int i = 1; i <= 64; i += 2) {\n"
"      if (i > n) break;\n"
"      float w1 = exp(float(i * i) * k), w2 = exp(float((i + 1) * (i + 1)) * k);\n"
"      float w = w1 + w2;\n"
"      float off = (float(i) * w1 + float(i + 1) * w2) / w;\n"
"      float4 cp = src.sample(s, clamp(uv + dir * off, lo, hi));\n"
"      float4 cm = src.sample(s, clamp(uv - dir * off, lo, hi));\n"
"      oS += (cp + cm) * w;\n"
"      oL += (pow(max(cp.rgb, float3(0.0)), float3(2.2)) + pow(max(cm.rgb, float3(0.0)), float3(2.2))) * w;\n"
"      wsum += 2.0 * w;\n"
"    }\n"
"    float4 outS = oS / wsum;\n"
"    // Separable H+V bright-gated linear-light wide blur: exact for axis-aligned features, mildly directional on diagonals.\n"
"    float3 outL = pow(max(oL / wsum, float3(0.0)), float3(1.0 / 2.2));\n"
"    float bw = smoothstep(0.55, 0.82, 0.2126 * outS.r + 0.7152 * outS.g + 0.0722 * outS.b);\n"
"    return float4(mix(outS.rgb, outL, bw), outS.a);\n"
"  }\n"
"  if (u.a.z > 4.5) {\n"
"    float4 cSharp;\n"
"    float sg = u.a.w;\n"
"    if (sg > 0.3) {\n"
"      float2 uv = pos.xy * ts;\n"
"      float2 dir = u.b.zw * ts;\n"
"      float k = -0.5 / (sg * sg);\n"
"      float4 o = src.sample(s, clamp(uv, lo, hi));\n"
"      float wsum = 1.0;\n"
"      int n = int(ceil(sg * 3.0));\n"
"      for (int i = 1; i <= 64; i += 2) {\n"
"        if (i > n) break;\n"
"        float w1 = exp(float(i * i) * k), w2 = exp(float((i + 1) * (i + 1)) * k);\n"
"        float w = w1 + w2;\n"
"        float off = (float(i) * w1 + float(i + 1) * w2) / w;\n"
"        o += (src.sample(s, clamp(uv + dir * off, lo, hi)) + src.sample(s, clamp(uv - dir * off, lo, hi))) * w;\n"
"        wsum += 2.0 * w;\n"
"      }\n"
"      cSharp = o / wsum;\n"
"    } else if (u.rc.w > 0.001) {\n"
"      float2 uv = pos.xy * ts;\n"
"      float2 ox = float2(u.rc.w, 0.0) * ts;\n"
"      float2 oy = float2(0.0, u.rc.w) * ts;\n"
"      cSharp = (src.sample(s, clamp(uv - ox - oy, lo, hi)) +\n"
"                src.sample(s, clamp(uv + ox - oy, lo, hi)) +\n"
"                src.sample(s, clamp(uv - ox + oy, lo, hi)) +\n"
"                src.sample(s, clamp(uv + ox + oy, lo, hi))) * 0.25;\n"
"    } else {\n"
"      cSharp = src.sample(s, clamp(pos.xy * ts, lo, hi));\n"
"    }\n"
"    float4 cWide = lv3.sample(s, clamp(pos.xy * u.rs.xy, u.rs.zw * 0.5, u.rc.xy));\n"
"    return mix(cWide, cSharp, u.rc.z);\n"
"  }\n"
"  if (u.a.z > 3.5) {\n"
"    float2 p = u.map.xy + pos.xy * u.map.zw;\n"
"    float ramp = 1.0;\n"
"    if (u.box.y > 0.0) ramp *= clamp((p.y - u.box.x) / u.box.y, 0.0, 1.0);\n"
"    if (u.box.w > 0.0) ramp *= clamp((u.box.z - p.y) / u.box.w, 0.0, 1.0);\n"
"    if (u.rr.y > 0.0) ramp *= clamp((p.x - u.rr.x) / u.rr.y, 0.0, 1.0);\n"
"    if (u.rr.w > 0.0) ramp *= clamp((u.rr.z - p.x) / u.rr.w, 0.0, 1.0);\n"
"    float sp = max(u.clip.x, 1.0);\n"
"    float kf = u.clip.y;\n"
"    float4 m0 = u.rc, m1 = u.rs, m2 = u.outer, m3 = u.orc;\n"
"    if (kf < 0.5) return src.sample(s, clamp(pos.xy * m0.xy, m0.xy * 0.5, m0.zw));\n"
"    float r = clamp(ramp, 0.0, 1.0);\n"
"    if (r <= 0.0) return src.sample(s, clamp(pos.xy * m0.xy, m0.xy * 0.5, m0.zw));\n"
"    if (r >= 1.0) {\n"
"      int ki = int(kf + 0.5);\n"
"      if (ki <= 1) return lv1.sample(s, clamp(pos.xy * m1.xy, m1.xy * 1.0, m1.zw));\n"
"      if (ki == 2) return lv2.sample(s, clamp(pos.xy * m2.xy, m2.xy * 2.0, m2.zw));\n"
"      return lv3.sample(s, clamp(pos.xy * m3.xy, m3.xy * 4.0, m3.zw));\n"
"    }\n"
"    float sig1 = pow(sp, 1.0 / kf);\n"
"    float u_b = smoothstep(0.06, 1.0, r);\n"
"    float rs = u_b * u_b;\n"
"    float target_px = sp * rs;\n"
"    if (target_px <= sig1 || kf <= 1.05) {\n"
"      float2 uv0_min = m0.xy * 0.5, uv0_max = m0.zw;\n"
"      float2 c_uv = pos.xy * m0.xy;\n"
"      float4 g_small;\n"
"      if (target_px <= 0.01) {\n"
"        g_small = src.sample(s, clamp(c_uv, uv0_min, uv0_max));\n"
"      } else if (target_px <= 0.55) {\n"
"        // Exact separable 3x3 Gaussian via 2x2 bilinear taps (center weight 0.5 split across +-o1)\n"
"        float inv2 = -0.5 / (target_px * target_px);\n"
"        float w1 = exp(inv2);\n"
"        float o1 = w1 / (0.5 + w1);\n"
"        float2 ox = float2(o1, 0.0) * m0.xy;\n"
"        float2 oy = float2(0.0, o1) * m0.xy;\n"
"        g_small = (src.sample(s, clamp(c_uv - ox - oy, uv0_min, uv0_max)) +\n"
"                   src.sample(s, clamp(c_uv + ox - oy, uv0_min, uv0_max)) +\n"
"                   src.sample(s, clamp(c_uv - ox + oy, uv0_min, uv0_max)) +\n"
"                   src.sample(s, clamp(c_uv + ox + oy, uv0_min, uv0_max))) * 0.25;\n"
"      } else {\n"
"        // Exact separable 7x7 Gaussian via 4x4 bilinear taps (zero gaps, zero comb ghosting)\n"
"        float inv2 = -0.5 / (target_px * target_px);\n"
"        float w1 = exp(inv2);\n"
"        float w2 = exp(4.0 * inv2);\n"
"        float w3 = exp(9.0 * inv2);\n"
"        float W1 = 0.5 + w1;\n"
"        float W2 = w2 + w3;\n"
"        float o1 = w1 / W1;\n"
"        float o2 = (2.0 * w2 + 3.0 * w3) / max(W2, 1e-6);\n"
"        float inv_sum = 0.5 / (W1 + W2);\n"
"        float p1 = W1 * inv_sum;\n"
"        float p2 = W2 * inv_sum;\n"
"        float ox[4] = {-o2, -o1, o1, o2};\n"
"        float pw[4] = {p2, p1, p1, p2};\n"
"        g_small = float4(0.0);\n"
"        for (int iy = 0; iy < 4; iy++) {\n"
"          float4 row = float4(0.0);\n"
"          float uy = clamp(c_uv.y + ox[iy] * m0.y, uv0_min.y, uv0_max.y);\n"
"          for (int ix = 0; ix < 4; ix++) {\n"
"            float ux = clamp(c_uv.x + ox[ix] * m0.x, uv0_min.x, uv0_max.x);\n"
"            row += src.sample(s, float2(ux, uy)) * pw[ix];\n"
"          }\n"
"          g_small += row * pw[iy];\n"
"        }\n"
"      }\n"
"      float blend_lo = min(1.1, sig1 * 0.45);\n"
"      float bt = clamp((target_px - blend_lo) / max(sig1 - blend_lo, 1e-4), 0.0, 1.0);\n"
"      float blend1 = bt > 0.0 ? pow(bt, 1.8) : 0.0;\n"
"      if (blend1 > 0.0) {\n"
"        float4 c_lv1 = lv1.sample(s, clamp(pos.xy * m1.xy, m1.xy * 1.0, m1.zw));\n"
"        return mix(g_small, c_lv1, blend1);\n"
"      }\n"
"      return g_small;\n"
"    }\n"
"    float lv_u = clamp(1.0 + (kf - 1.0) * log(target_px / sig1) / max(log(sp / sig1), 1e-4), 1.0, kf);\n"
"    int idx = min(int(floor(lv_u)), int(kf + 0.5) - 1);\n"
"    if (idx < 0) idx = 0;\n"
"    float f = lv_u - float(idx);\n"
"    float4 c0, c1;\n"
"    if (idx == 0) {\n"
"      c0 = src.sample(s, clamp(pos.xy * m0.xy, m0.xy * 0.5, m0.zw));\n"
"      c1 = lv1.sample(s, clamp(pos.xy * m1.xy, m1.xy * 1.0, m1.zw));\n"
"    } else if (idx == 1) {\n"
"      c0 = lv1.sample(s, clamp(pos.xy * m1.xy, m1.xy * 1.0, m1.zw));\n"
"      c1 = lv2.sample(s, clamp(pos.xy * m2.xy, m2.xy * 2.0, m2.zw));\n"
"    } else {\n"
"      c0 = lv2.sample(s, clamp(pos.xy * m2.xy, m2.xy * 2.0, m2.zw));\n"
"      c1 = lv3.sample(s, clamp(pos.xy * m3.xy, m3.xy * 4.0, m3.zw));\n"
"    }\n"
"    return mix(c0, c1, f);\n"
"  }\n"
"  if (u.a.z > 2.5) {\n"
"    float2 p = u.map.xy + pos.xy * u.map.zw;\n"
"    p = mb_contained_point(p, u.box, u.rr, u.clip, u.rc, u.rs, u.outer, u.orc, u.ors, u.map.z);\n"
"    return src.sample(s, clamp((p - u.map.xy) / u.map.zw * ts, lo, hi));\n"
"  }\n"
"  if (u.a.z < 1.5) {\n"
"    float2 uv = pos.xy * 2.0 * ts;\n"
"    float4 o = src.sample(s, clamp(uv, lo, hi)) * 4.0;\n"
"    o += src.sample(s, clamp(uv - ts, lo, hi));\n"
"    o += src.sample(s, clamp(uv + ts, lo, hi));\n"
"    o += src.sample(s, clamp(uv + float2(ts.x, -ts.y), lo, hi));\n"
"    o += src.sample(s, clamp(uv + float2(-ts.x, ts.y), lo, hi));\n"
"    return o / 8.0;\n"
"  }\n"
"  float2 uv = pos.xy * ts;\n"
"  float2 dir = u.b.zw * ts;\n"
"  float sg = u.a.w;\n"
"  float k = -0.5 / (sg * sg);\n"
"  float4 o = src.sample(s, clamp(uv, lo, hi));\n"
"  float wsum = 1.0;\n"
"  int n = int(ceil(sg * 3.0));\n"
"  for (int i = 1; i <= 64; i += 2) {\n"
"    if (i > n) break;\n"
"    float w1 = exp(float(i * i) * k), w2 = exp(float((i + 1) * (i + 1)) * k);\n"
"    float w = w1 + w2;\n"
"    float off = (float(i) * w1 + float(i + 1) * w2) / w;\n"
"    o += (src.sample(s, clamp(uv + dir * off, lo, hi)) + src.sample(s, clamp(uv - dir * off, lo, hi))) * w;\n"
"    wsum += 2.0 * w;\n"
"  }\n"
"  return o / wsum;\n"
"}\n"
"struct AdU { float4 map; float4 uvc; float4 box; float4 param; };\n"
"fragment float4 fad(float4 pos [[position]], texture2d<float> bd [[texture(0)]],\n"
"    texture2d<float> prev_ad [[texture(1)]], constant AdU& u [[buffer(0)]]) {\n"
"  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
"  float is_dark = u.param.x;\n"
"  bool is_inverting = u.param.y > 3.5;\n"
"  float p1 = u.param.y - (is_inverting ? 4.0 : 0.0);\n"
"  bool is_through = p1 > 1.5;\n"
"  float is_new = p1 - (is_through ? 2.0 : 0.0);\n"
"  float dt = u.param.z;\n"
"  float hole_y = (is_dark > 0.5) ? 0.0 : 1.0;\n"
"  float2 c = u.box.xy, h = u.box.zw;\n"
"  float sum = 0.0, mx1 = -1.0, mx2 = -1.0, mn1 = 2.0, mn2 = 2.0;\n"
"  for (int iy = -1; iy <= 1; iy++) {\n"
"    float py = c.y + float(iy) * (h.y * 0.60);\n"
"    for (int ix = -2; ix <= 2; ix++) {\n"
"      float px = c.x + float(ix) * (h.x * 0.38);\n"
"      float2 uv = clamp(float2(px, py) * u.map.xy + u.map.zw, u.uvc.xy, u.uvc.zw);\n"
"      float4 smp = bd.sample(s, uv);\n"
"      float a = is_through ? clamp(smp.a, 0.0, 1.0) : 1.0;\n"
"      float y = mb_ink_luma(clamp(smp.rgb + float3((1.0 - a) * hole_y), 0.0, 1.0));\n"
"      sum += y;\n"
"      if (y > mx1) { mx2 = mx1; mx1 = y; } else if (y > mx2) { mx2 = y; }\n"
"      if (y < mn1) { mn2 = mn1; mn1 = y; } else if (y < mn2) { mn2 = y; }\n"
"    }\n"
"  }\n"
"  float bgL = (sum - mx1 - mx2 - mn1 - mn2) / 11.0;\n"
"  uint sid = uint(u.param.w + 0.5);\n"
"  float4 prev = prev_ad.read(uint2(sid, 0));\n"
"  float prevT = mb_unpack16(prev.rg);\n"
"  float latch, target;\n"
"  float slot_tint_a = (is_dark > 0.5) ? max(is_dark - (is_dark > 1.5 ? 2.0 : 1.0), 0.0) : max(-is_dark, 0.0);\n"
"  if (is_dark > 0.5) {\n"
"    float baseDens = mix(0.404, slot_tint_a, smoothstep(0.405, 0.46, slot_tint_a));\n"
"    float extra = clamp((baseDens - 0.404) / (1.0 - 0.404), 0.0, 1.0);\n"
"    target = smoothstep(0.08, 0.42, bgL) * (1.0 - extra);\n"
"    latch = 64.0 / 255.0;\n"
"  } else {\n"
"    float baseDens = mix(0.404, slot_tint_a, smoothstep(0.405, 0.46, slot_tint_a));\n"
"    float extra = clamp((baseDens - 0.404) / (1.0 - 0.404), 0.0, 1.0);\n"
"    float c_eq = mix(sqrt(max(bgL, 0.0)), 0.992, pow(extra, 1.35));\n"
"    float effBgL = c_eq * c_eq;\n"
"    float q = floor(extra * 46.0 + 0.5);\n"
"    if (is_inverting) {\n"
"      int pcode = int(floor(clamp(prev.b, 0.0, 1.0) * 255.0 + 0.5));\n"
"      bool prev_dark = (is_new <= 0.5) && (pcode > 208);\n"
"      bool on = prev_dark ? (effBgL <= 0.21) : (effBgL < 0.14);\n"
"      target = on ? 1.0 : 0.0;\n"
"      latch = on ? ((255.0 - q) / 255.0) : ((160.0 + q) / 255.0);\n"
"    } else {\n"
"      target = (1.0 - smoothstep(0.06, 0.34, effBgL)) * (1.0 - extra);\n"
"      latch = q / 255.0;\n"
"    }\n"
"  }\n"
"  float nextT;\n"
"  if (is_new > 0.5 || dt >= 1.0 || slot_tint_a >= 0.88) nextT = target;\n"
"  else if (dt <= 0.0) nextT = prevT;\n"
"  else {\n"
"    nextT = mix(prevT, target, 1.0 - exp(-dt / 0.070));\n"
"    if (abs(nextT - target) < 0.003) nextT = target;\n"
"  }\n"
"  return float4(mb_pack16(nextT), latch, clamp(bgL, 0.0, 1.0));\n"
"}\n";

// What draws by `frag` only instances of one kind (`kind`; -1 any).
static id<MTLRenderPipelineState> make_pipe(id<MTLLibrary> lib, MTLPixelFormat fmt, BOOL blend, NSString *frag,
                                            int kind) {
  MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
  d.vertexFunction = [lib newFunctionWithName:@"vmain"];
  MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
  [cv setConstantValue:&kind type:MTLDataTypeInt atIndex:0];
  NSError *ferr = nil;
  d.fragmentFunction = [lib newFunctionWithName:frag constantValues:cv error:&ferr];
  if (!d.fragmentFunction) NSLog(@"%@ %d: %@", frag, kind, ferr);
  d.colorAttachments[0].pixelFormat = fmt;
  d.colorAttachments[0].blendingEnabled = blend;
  d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
  d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
  d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  NSError *err = nil;
  id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&err];
  if (!p) NSLog(@"pipeline: %@", err);
  return p;
}

// The kinds drawn by a pipeline of their own: the shader with its kind fixed (one_kind), so each is compiled to its
// kind's branch alone and a pixel of it is shaded with the registers that branch needs, not the most any kind's does.
// So a frame's GPU time on an M2 Max fell (2026-11-09, mbui-render's bench, after bands.h and what is covered left
// out: Home on Small 0.56 ms to 0.50, a chat on Large 0.43 to 0.37, a chat on Small 0.435 to 0.424), what it draws the
// same to the byte; the draws one for each run of a kind instead of each run of an image, which cost next to nothing
// (a hundred more, 0.01 ms). Rounded rects, glyphs,
// shadows, colour glyphs, images, lines, shapes' bands: what else there is (glass, a native view's hole) the pipeline
// for any kind draws. Made after the rest, on their own (kinds_made): until they are, that one draws every kind, the
// same pixels.
#define MB_KIND_LIQUID_SINGLE 13
#define MB_KINDS 14
static int has_own_pipe(int kind) {
  return (kind >= 0 && kind <= 8) || kind == 10 || kind == 11 || kind == MB_KIND_LIQUID_SINGLE;
}
static id<MTLRenderPipelineState> kpipe[MB_KINDS];
static dispatch_group_t kinds_made;
static int kinds_ready;

// The pipeline that draws a run of instances of `kind` where `p` would draw them all.
static id<MTLRenderPipelineState> kind_pipe(id<MTLRenderPipelineState> p, int kind) {
  if (p != mpipe || !has_own_pipe(kind)) return p;
  if (!kinds_ready) kinds_ready = dispatch_group_wait(kinds_made, DISPATCH_TIME_NOW) == 0;
  return kinds_ready && kpipe[kind] ? kpipe[kind] : p;
}

// An atlas of `pages` pages `side` texels square: the words' a byte a texel, colour glyphs' four. An array however
// many pages, as the shader takes it.
static id<MTLTexture> atlas_texture(int color, int side, int pages) {
  MTLTextureDescriptor *td = [MTLTextureDescriptor new];
  td.textureType = MTLTextureType2DArray;
  td.pixelFormat = color ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatR8Unorm;
  td.width = td.height = (NSUInteger)side;
  td.arrayLength = (NSUInteger)(pages > 0 ? pages : 1);
  td.mipmapLevelCount = 1;
  td.usage = MTLTextureUsageShaderRead;
  return [dev newTextureWithDescriptor:td];
}

// The device, the shaders compiled and the pipelines made: what drawing needs whatever it draws into, on a queue of its
// own from as the program loads (prepare, below), so that compiling the shaders (most of a start's time before its
// window, as AppKit waited on Metal's compiler: 2026-10-27) goes on while the program starts and AppKit makes the app
// and its window; what is drawn into waits for it only as it is made (mb_render_init).
static dispatch_group_t prepared;

static void prepare_gpu(void) {
  dev = MTLCreateSystemDefaultDevice();
  queue = [dev newCommandQueue];
  NSError *err = nil;
  id<MTLLibrary> lib = [dev newLibraryWithSource:shader_src options:nil error:&err];
  if (!lib) NSLog(@"shader: %@", err);
  mpipe = make_pipe(lib, MTLPixelFormatBGRA8Unorm, YES, @"fmain", -1);
  hpipe = make_pipe(lib, MTLPixelFormatBGRA8Unorm, NO, @"fmain", -1);
  fpipe = make_pipe(lib, MTLPixelFormatBGRA8Unorm, YES, @"fflat", -1);
  rpipe_rect = make_pipe(lib, MTLPixelFormatBGRA8Unorm, YES, @"frect", -1);
  rpipe_off = mpipe;
  rpipe_rect_off = rpipe_rect;
  MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
  pd.vertexFunction = [lib newFunctionWithName:@"vpost"];
  pd.fragmentFunction = [lib newFunctionWithName:@"fpost"];
  pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  ppipe = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
  if (!ppipe) NSLog(@"post pipeline: %@", err);
  pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
  ppipe_deep = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
  if (!ppipe_deep) NSLog(@"post pipeline: %@", err);
  MTLRenderPipelineDescriptor *cd = [MTLRenderPipelineDescriptor new];
  cd.vertexFunction = [lib newFunctionWithName:@"vpost"];
  cd.fragmentFunction = [lib newFunctionWithName:@"fcopy"];
  cd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  cpipe = [dev newRenderPipelineStateWithDescriptor:cd error:&err];
  if (!cpipe) NSLog(@"copy pipeline: %@", err);
  MTLRenderPipelineDescriptor *ad = [MTLRenderPipelineDescriptor new];
  ad.vertexFunction = [lib newFunctionWithName:@"vpost"];
  ad.fragmentFunction = [lib newFunctionWithName:@"fad"];
  ad.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  adpipe = [dev newRenderPipelineStateWithDescriptor:ad error:&err];
  if (!adpipe) NSLog(@"adaptive pipeline: %@", err);
  atlas = atlas_texture(0, MB_ATLAS_SIDE, 1);
  catlas = atlas_texture(1, MB_ATLAS_COLOR_SIDE, 1);
  MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                width:1
                                                                               height:1
                                                                            mipmapped:NO];
  sd.usage = MTLTextureUsageShaderRead;
  stand_tex = [dev newTextureWithDescriptor:sd];
  static const uint32_t clear_texel = 0;
  [stand_tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&clear_texel bytesPerRow:4];
  inflight = dispatch_semaphore_create(3);
  dispatch_group_async(kinds_made, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    for (int k = 0; k < MB_KINDS; k++)
      if (has_own_pipe(k)) kpipe[k] = make_pipe(lib, MTLPixelFormatBGRA8Unorm, YES, @"fmain", k);
  });
}

__attribute__((constructor)) static void prepare(void) {
  present_q = dispatch_queue_create("mbui.present", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                                                                           QOS_CLASS_USER_INTERACTIVE, 0));
  prepared = dispatch_group_create();
  kinds_made = dispatch_group_create();
  dispatch_group_async(prepared, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ prepare_gpu(); });
}

void mb_render_init(CAMetalLayer *layer) {
  dispatch_group_wait(prepared, DISPATCH_TIME_FOREVER);
  mlayer = layer;
  layer.device = dev;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  // frames are drawn into canvas (where glass copies out what is drawn so far) and copied into the drawable by a
  // render pass, so the drawable needs no shader read or blit and keeps lossless framebuffer compression
  layer.framebufferOnly = YES;
  // opaque while no native view's hole shows through the canvas (glass_through); cleared only on frames with holes
  layer.opaque = YES;
}

id<MTLDevice> mb_render_device(void) {
  dispatch_group_wait(prepared, DISPATCH_TIME_FOREVER);
  return dev;
}

void mb_render_set_size(double w, double h, double scale) {
  vw = w;
  vh = h;
  vscale = scale;
  CGSize want = CGSizeMake(round(w * scale), round(h * scale));
  if (!CGSizeEqualToSize(mlayer.drawableSize, want)) {
    // (not while the queue may be asking the layer for a drawable)
    present_drain();
    mlayer.drawableSize = want;
    shown_ok = 0;
  }
}

// A glyph's pixels for an atlas, kept to be put in before the next frame drawn (atlas_puts, above).
void mb_atlas_upload(int color, int x, int y, int w, int h, const void *px) {
  if (w <= 0 || h <= 0) return;
  size_t n = (size_t)w * h * (color ? 4 : 1);
  uint8_t *copy = malloc(n);
  memcpy(copy, px, n);
  os_unfair_lock_lock(&present_mu);
  if (atlas_puts_head > 0 && atlas_puts_head == atlas_puts_n) atlas_puts_head = atlas_puts_n = 0;
  if (atlas_puts_n == atlas_puts_cap) {
    if (atlas_puts_head > 0) {
      memmove(atlas_puts, atlas_puts + atlas_puts_head, (atlas_puts_n - atlas_puts_head) * sizeof *atlas_puts);
      atlas_puts_n -= atlas_puts_head;
      atlas_puts_head = 0;
    }
    if (atlas_puts_n == atlas_puts_cap) {
      atlas_puts_cap = atlas_puts_cap ? atlas_puts_cap * 2 : 64;
      atlas_puts = realloc(atlas_puts, atlas_puts_cap * sizeof *atlas_puts);
    }
  }
  atlas_puts[atlas_puts_n++] = (AtlasPut){++atlas_seq, color, x, y, w, h, copy, 0, NULL};
  os_unfair_lock_unlock(&present_mu);
}

// The atlas `color` made texture `t`'s pages (a PAGES op, on the thread that draws): the pages both have copied into it
// on the GPU (the queue's, after what it holds: done before a glyph is put in it on the CPU), put in the old one's
// place. What the GPU still draws with the old one keeps it till done.
static void atlas_pages_put(int color, id<MTLTexture> t) {
  id<MTLTexture> old = color ? catlas : atlas;
  NSUInteger keep = old ? MIN(old.arrayLength, t.arrayLength) : 0;
  if (keep > 0) {
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:old sourceSlice:0 sourceLevel:0 toTexture:t destinationSlice:0 destinationLevel:0
             sliceCount:keep levelCount:1];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
  }
  if (color) catlas = t;
  else atlas = t;
}

// The glyphs kept put into the atlases, in order, those numbered up to `upto`.
static void atlas_flush(uint64_t upto) {
  for (;;) {
    os_unfair_lock_lock(&present_mu);
    AtlasPut p = {0};
    int has = atlas_puts_head < atlas_puts_n && atlas_puts[atlas_puts_head].seq <= upto;
    if (has) p = atlas_puts[atlas_puts_head++];
    os_unfair_lock_unlock(&present_mu);
    if (!has) return;
    if (p.tex) {
      atlas_pages_put(p.color, (__bridge_transfer id<MTLTexture>)p.tex);
      os_unfair_lock_lock(&present_mu);
      atlas_ops_owed--;
      os_unfair_lock_unlock(&present_mu);
      continue;
    }
    id<MTLTexture> t = p.color ? catlas : atlas;
    int side = (int)t.height, page = side > 0 ? p.y / side : 0;
    if (page < (int)t.arrayLength && p.x + p.w <= (int)t.width && p.y - page * side + p.h <= side)
      [t replaceRegion:MTLRegionMake2D(p.x, p.y - page * side, p.w, p.h) mipmapLevel:0 slice:(NSUInteger)page
             withBytes:p.px bytesPerRow:(NSUInteger)p.w * (p.color ? 4 : 1) bytesPerImage:0];
    free(p.px);
  }
}

// ---------------------------------------------------------------------------
// Glass: when the instances reach a run of glass (slot 20 negative: the capture it shares), what is drawn so far
// under it (its rects with room for the blur round them) is copied out, halved down to where the blur is a few texels
// wide (dual Kawase's downsample, at most 1/8), blurred there by a Gaussian of what deviation is left, and drawn
// through. Level 0, the copy, has 8 bits a channel as the target has; what the passes write keeps more (half floats,
// its alpha too: how much of it was a native view's hole, glass_through): at 8 each pass rounds the frost's slow
// slopes into flat steps, a texel of the last level 8 pixels wide on the screen (a blur under 4 pixels, too narrow to
// halve, is written back into the copy: it leaves no slope that wide).

typedef struct { float map[4]; float uvc[4]; float gb[4]; float shape[8][4]; float rad[8][4]; float misc[4]; float ad_box[MB_ADAPTIVE_MAX][4]; float ad_meta[4]; } GU;
static GU gu;
static id<MTLTexture> gtex;  // the blurred copy the glass in view shows

static id<MTLTexture> level_tex(MTLPixelFormat format, int w, int h) {
  MTLTextureDescriptor *td = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:format width:(NSUInteger)(w > 0 ? w : 1)
                                  height:(NSUInteger)(h > 0 ? h : 1) mipmapped:NO];
  td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModePrivate;
  return [dev newTextureWithDescriptor:td];
}

// the last capture's blur is kept to be shown again (encode, below)
static int kept_ok;
static void slots_invalidate(void);
static void glass_forget(void);

static void ensure_levels(int w, int h) {
  if (lv_w >= w && lv_h >= h && lv[0]) return;
  int nw = (w + 255) & ~255;
  int nh = (h + 255) & ~255;
  int rt_w = (int)ceil(at_w * at_scale);
  int rt_h = (int)ceil(at_h * at_scale);
  if (rt_w > 0 && nw > rt_w) nw = rt_w;
  if (rt_h > 0 && nh > rt_h) nh = rt_h;
  if (nw < w) nw = w;
  if (nh < h) nh = h;
  lv_w = nw > lv_w ? nw : lv_w;
  lv_h = nh > lv_h ? nh : lv_h;
  for (int j = 0; j < 4; j++)
    lv[j] = level_tex(j == 0 ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA16Float, (lv_w + (1 << j) - 1) >> j,
                      (lv_h + (1 << j) - 1) >> j);
  ltmp = level_tex(MTLPixelFormatRGBA16Float, lv_w, lv_h);
  // Each cached slot owns its own texture (`slot_bt[i]`) and `show_blur` reads `gtex.width`/`height` directly,
  // so growing the shared scratch pyramid must not invalidate cached glass slots.
}

// Offscreen blur layers (kind == 14 PUSH_BLUR_LAYER / kind == 15 POP_BLUR_LAYER): lazily allocated on first use.
#define MB_BLUR_LAYERS 4
typedef struct { int px0, py0, w, h, active; float sigma; } BlurFrame;
static id<MTLTexture> bl_tex[MB_BLUR_LAYERS];
static int bl_w, bl_h;

static void ensure_blur_layer(int depth, int tw, int th) {
  if (bl_w != tw || bl_h != th) {
    for (int j = 0; j < MB_BLUR_LAYERS; j++) bl_tex[j] = nil;
    bl_w = tw;
    bl_h = th;
  }
  if (!bl_tex[depth]) bl_tex[depth] = level_tex(MTLPixelFormatBGRA8Unorm, tw, th);
}

static void clear_tex(id<MTLCommandBuffer> cb, id<MTLTexture> tex) {
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = tex;
  rp.colorAttachments[0].loadAction = MTLLoadActionClear;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
  id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
  [e endEncoding];
}

// one pass of the blur: src into dst's top left (w × h); mode 1 halving, 2 a Gaussian of deviation sg along (dx, dy)
static void post_pass_ex(id<MTLCommandBuffer> cb, id<MTLTexture> src, int sw, int sh, id<MTLTexture> dst, int w, int h,
                         int mode, float sg, float dx, float dy, const MBContained *edge, int clear,
                         const MTLScissorRect *sc_rect) {
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = dst;
  rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  if (clear) rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
  id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
  [e setRenderPipelineState:dst.pixelFormat == MTLPixelFormatRGBA16Float ? ppipe_deep : ppipe];
  [e setViewport:(MTLViewport){0, 0, (double)w, (double)h, 0, 1}];
  [e setScissorRect:sc_rect ? *sc_rect : (MTLScissorRect){0, 0, (NSUInteger)w, (NSUInteger)h}];
  float tw = 1.0f / (float)src.width, th = 1.0f / (float)src.height;
  struct { float a[4]; float b[4]; MBContained edge; } u = {{tw, th, (float)mode, sg}, {((float)sw - 0.5f) * tw, ((float)sh - 0.5f) * th, dx, dy}, {0}};
  if (edge) u.edge = *edge;
  [e setFragmentBytes:&u length:sizeof u atIndex:0];
  [e setFragmentTexture:src atIndex:0];
  [e setFragmentTexture:(mode == 4 ? lv[1] : src) atIndex:1];
  [e setFragmentTexture:(mode == 4 ? lv[2] : src) atIndex:2];
  [e setFragmentTexture:((mode == 4 || mode == 5) ? lv[3] : src) atIndex:3];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [e endEncoding];
}

static void post_pass(id<MTLCommandBuffer> cb, id<MTLTexture> src, int sw, int sh, id<MTLTexture> dst, int w, int h,
                      int mode, float sg, float dx, float dy, const MBContained *edge) {
  post_pass_ex(cb, src, sw, sh, dst, w, h, mode, sg, dx, dy, edge, 0, NULL);
}

static void post_pass_clear(id<MTLCommandBuffer> cb, id<MTLTexture> src, int sw, int sh, id<MTLTexture> dst, int w, int h,
                            int mode, float sg, float dx, float dy, const MBContained *edge) {
  post_pass_ex(cb, src, sw, sh, dst, w, h, mode, sg, dx, dy, edge, 1, NULL);
}

// Glass over a native view (a page, a video: under the canvas, seen through its hole) frosts what the view shows too.
// The canvas holds none of it (a hole is nothing in it), so that part of the frost is Core Animation's: a backdrop
// over the views, under the canvas (apple_views.m mb_views_frost), each the shape of a glass instance, blurred and
// saturated as the glass is; the canvas's glass leaves out as much of its frost as was the hole (its copy's alpha,
// gu.misc.y), for that to show there. The captures that do (their slot 20, through_tid), found as a frame is
// presented; none in a frame drawn otherwise (offscreen, read back, a snapshot: no views under it).
#define MB_HOLES 32
static int through_n;
static float through_tid[MB_THROUGH];
// (those of the frame being drawn: its MBFrame's, or the main thread's as it draws one itself; none read back)
static int at_through_n;
static float at_through_tid[MB_THROUGH];

static int in_through(const float *tids, int n, float tid) {
  for (int i = 0; i < n; i++)
    if (tids[i] == tid) return 1;
  return 0;
}

static int is_through(float tid) { return in_through(through_tid, through_n, tid); }

// what of instance b shows: its rect cut by its clip's (x0 y0 x1 y1); 0 when nothing does
static int shows_of(const float *b, float r[4]) {
  r[0] = fmaxf(b[0], b[16]); r[1] = fmaxf(b[1], b[17]);
  r[2] = fminf(b[0] + b[2], b[18]); r[3] = fminf(b[1] + b[3], b[19]);
  return r[2] > r[0] && r[3] > r[1];
}

// Effective Gaussian blur radius (in points) for a glass instance (kind 5 or kind 6).
static float glass_blur_of(const float *b) {
  int kind = (int)(b[5] + 0.5f);
  float raw_blur = (kind == 6 && b[7] < 0.0f) ? 0.0f : fabsf(b[7]);
  int is_clear_liq = (kind == 6 && b[7] >= -0.001f && b[7] <= 0.1f && b[11] < 0.12f);
  return (is_clear_liq && b[11] > 0.01f && raw_blur < 0.1f) ? 1.2f : raw_blur;
}

// Effective backdrop sampling reach (in points) beyond a glass instance's bounds.
static float glass_reach_of(const float *b) {
  int kind = (int)(b[5] + 0.5f);
  float blur = glass_blur_of(b);
  if (kind != 6) return 3.0f * blur;
  float clear_refract_scale = (fabsf(b[7]) < 0.01f && b[11] > 0.005f && b[11] < 0.31f) ? 2.20f : 1.0f;
  return 3.0f * blur + b[21] * clear_refract_scale * (1.0f + fabsf(b[22])) + fmaxf(b[23], 0.0f);
}

// The frame's frosted and liquid glass over holes (kind 9) painted before it: its captures through, and every instance
// of them a backdrop over the views whose holes were painted before it (the last of them: `under`), at that view's place
// among them. Told to the views (which say whether they can frost: else none is through, the glass as before).
// Returns whether any hole shows in this frame.
static int glass_through(const float *f, int32_t count) {
  // Instances are in content points; the system views underneath are placed in window points.
  double zoom = mlayer && mlayer.contentsScale > 0 ? vscale / mlayer.contentsScale : 1.0;
  float holes[MB_HOLES][4];
  int nh = 0, has_hole = 0;
  through_n = 0;
  for (int32_t i = 0; i < count; i++) {
    const float *b = f + i * MB_INST;
    int kind = (int)(b[5] + 0.5f);
    float r[4];
    if (kind == 9) {
      if (shows_of(b, r)) {
        has_hole = 1;
        if (nh < MB_HOLES) memcpy(holes[nh++], r, sizeof r);
      }
      continue;
    }
    if ((kind != 5 && kind != 6) || (kind == 5 && b[22] < -0.5f) || b[20] >= 0 || !nh || through_n >= MB_THROUGH ||
        is_through(b[20]) || !shows_of(b, r))
      continue;
    // (its blur and liquid refraction reach a hole near it too)
    float m = glass_reach_of(b);
    for (int h = 0; h < nh; h++)
      if (holes[h][0] < r[2] + m && r[0] - m < holes[h][2] && holes[h][1] < r[3] + m && r[1] - m < holes[h][3]) {
        through_tid[through_n++] = b[20];
        break;
      }
  }
  double frosts[MB_FROSTS * MB_FROST_N];
  float frost_tid[MB_FROSTS];
  int n = 0;
  int32_t under = 0;
  int overflowed = 0;
  for (int32_t i = 0; i < count && through_n; i++) {
    const float *b = f + i * MB_INST;
    int kind = (int)(b[5] + 0.5f);
    float r[4];
    if (kind == 9) {
      // b[20] >= MB_VIEW_TEX (2^23) has float32 ULP == 1.0, so adding 0.5f would round odd view IDs up to even!
      if (shows_of(b, r)) under = (int32_t)b[20] - MB_VIEW_TEX;
      continue;
    }
    if ((kind != 5 && kind != 6) || (kind == 5 && b[22] < -0.5f) || !is_through(b[20]) || under <= 0 ||
        !shows_of(b, r))
      continue;
    if (n >= MB_FROSTS) {
      // Drop any tid that could not fit all its instances within MB_FROSTS so it never renders transparent without a CABackdropLayer.
      for (int t = 0; t < through_n; t++) {
        if (through_tid[t] == b[20]) {
          through_tid[t] = through_tid[--through_n];
          overflowed = 1;
          break;
        }
      }
      continue;
    }
    frost_tid[n] = b[20];
    double *e = frosts + n++ * MB_FROST_N;
    int has_rr = fmaxf(fmaxf(b[36], b[37]), fmaxf(b[38], b[39])) > 0.0f;
    for (int k = 0; k < 4; k++) {
      e[k] = r[k] * zoom;
      e[4 + k] = b[k] * zoom;
      e[8 + k] = (has_rr ? b[36 + k] : b[4]) * zoom;
    }
    e[12] = glass_blur_of(b) * zoom;
    e[13] = b[6];
    e[14] = b[15];
    e[15] = under;
  }
  if (overflowed) {
    int kept = 0;
    for (int i = 0; i < n; i++) {
      if (is_through(frost_tid[i])) {
        if (kept != i) memcpy(frosts + kept * MB_FROST_N, frosts + i * MB_FROST_N, MB_FROST_N * sizeof(double));
        kept++;
      }
    }
    n = kept;
  }
  if (!mb_views_frost(n, frosts)) through_n = 0;
  return has_hole;
}

// What a capture of glass is of, worked out before any of the GPU's work: its rectangle in the target's pixels, how
// many times it is halved (k, at most 3) and the Gaussian left after (sg), and the glass's own numbers (gu's but map
// and uvc, which follow from these and the level's size). Zeroed whole first, so two of the same compare the same.
typedef struct {
  int px0, py0, w, h, k, contained, progressive, dual_scale;
  int vis_x, vis_y, vis_w, vis_h;
  float sg, sp, dual_w, dual_wsp;
  float fade[4], fadex[4];
  GU g;
  MBContained edge;
} Grab;

// The capture the run [start, end) of glass makes, of a target tw × th pixels; 0 when none of it shows.
// Frosted glass of the same capture further on (glass.mbt shares_capture) is under it too.
static int grab_of(Grab *m, const float *f, int32_t start, int32_t end, int32_t count, int tw, int th) {
  memset(m, 0, sizeof *m);
  const float *a = f + start * MB_INST;
  // For frosted glass (kind 5), negative slot 7 is contained; for liquid glass (kind 6), negative slot 7 encodes clear-lens zoom.
  int kind = (int)(a[5] + 0.5f);
  int is_contained = (kind == 5 && a[7] < 0.0f);
  int is_progressive = (kind == 5 && a[22] < -0.5f);
  int is_clear_liq = (kind == 6 && a[7] >= -0.001f && a[7] <= 0.1f && a[11] < 0.12f);
  float col_ch = fmaxf(fmaxf(a[8], a[9]), a[10]) - fminf(fminf(a[8], a[9]), a[10]);
  // iOS 27 Clear glass (col.a ~ 0.102, non-droplet a[7] >= 0) uses a crisp 1.2pt Gaussian blur;
  // iOS 27 Regular controls use C1-continuous dual-scale blur (34% sharp optical refraction + 66% 14pt wide ambient frost).
  float blur = glass_blur_of(a);
  float g_lo = fminf(fmaxf((a[11] - 0.02f) / 0.085f, 0.0f), 1.0f);
  g_lo = g_lo * g_lo * (3.0f - 2.0f * g_lo);
  float g_neu = fminf(fmaxf((a[11] - 0.405f) / 0.095f, 0.0f), 1.0f);
  g_neu = 1.0f - g_neu * g_neu * (3.0f - 2.0f * g_neu);
  float g_ch = fminf(fmaxf((col_ch - 0.08f) / 0.08f, 0.0f), 1.0f);
  g_ch = g_ch * g_ch * (3.0f - 2.0f * g_ch);
  float g_ta = fminf(fmaxf((a[11] - 0.72f) / 0.08f, 0.0f), 1.0f);
  g_ta = 1.0f - g_ta * g_ta * (3.0f - 2.0f * g_ta);
  float dual_b = fminf(fmaxf((blur - 0.2f) / 0.7f, 0.0f), 1.0f);
  dual_b = dual_b * dual_b * (3.0f - 2.0f * dual_b);
  float dual_hi = fminf(fmaxf((blur - 3.5f) / 1.0f, 0.0f), 1.0f);
  dual_hi = 1.0f - dual_hi * dual_hi * (3.0f - 2.0f * dual_hi);
  float dual_sz = fminf(fmaxf((fminf(a[2], a[3]) - 16.0f) / 12.0f, 0.0f), 1.0f);
  dual_sz = dual_sz * dual_sz * (3.0f - 2.0f * dual_sz);
  float min_dim = fminf(a[2], a[3]);
  float tall_t = fminf(fmaxf((min_dim - 44.0f) / 18.0f, 0.0f), 1.0f);
  tall_t = tall_t * tall_t * (3.0f - 2.0f * tall_t);
  float dual_base = 0.68f + 0.18f * tall_t;
  float dual_w = (kind == 6 && !is_clear_liq) ? (dual_base * dual_b * dual_hi * dual_sz * g_lo * fmaxf(g_neu, g_ch * g_ta)) : 0.0f;
  int is_dual = (dual_w > 0.005f);
  int merged_liquid = (kind == 6 && a[23] > 0.0f);
  int unmerged_liquid = (kind == 6 && !merged_liquid);
  float clear_refract_scale = (kind == 6 && fabsf(a[7]) < 0.01f && a[11] > 0.005f && a[11] < 0.31f) ? 2.20f : 1.0f;
  float margin = (is_contained || is_progressive) ? 0.0f : glass_reach_of(a) + 2.0f;
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  int n = 0;
  for (int32_t i = start; i < end; i++) {
    const float *b = f + i * MB_INST;
    // what of it shows: its clip
    float bx0 = fmaxf(b[0], b[16]), by0 = fmaxf(b[1], b[17]);
    float bx1 = fminf(b[0] + b[2], b[18]), by1 = fminf(b[1] + b[3], b[19]);
    if (kind == 6) {
      float m23 = fmaxf(unmerged_liquid ? b[23] : a[23], 0.0f);
      bx0 = b[0] - m23; by0 = b[1] - m23; bx1 = b[0] + b[2] + m23; by1 = b[1] + b[3] + m23;
      if (merged_liquid && n < 8) {
        for (int k = 0; k < 4; k++) m->g.shape[n][k] = b[k];
        if (fmaxf(fmaxf(b[36], b[37]), fmaxf(b[38], b[39])) > 0.0f) {
          for (int k = 0; k < 4; k++) m->g.rad[n][k] = b[36 + k];
        } else {
          for (int k = 0; k < 4; k++) m->g.rad[n][k] = b[4];
        }
        n++;
      }
      if (unmerged_liquid) {
        float bm = 3.0f * blur + b[21] * clear_refract_scale * (1.0f + fabsf(b[22])) + m23 + 2.0f;
        if (bm > margin) margin = bm;
      }
    }
    x0 = fminf(x0, bx0); y0 = fminf(y0, by0); x1 = fmaxf(x1, bx1); y1 = fmaxf(y1, by1);
  }
  if (kind == 5 || unmerged_liquid) {
    for (int32_t i = end; i < count; i++) {
      const float *b = f + i * MB_INST;
      if (b[20] != a[20]) continue;
      if (unmerged_liquid) {
        float m23 = fmaxf(b[23], 0.0f);
        x0 = fminf(x0, b[0] - m23); y0 = fminf(y0, b[1] - m23);
        x1 = fmaxf(x1, b[0] + b[2] + m23); y1 = fmaxf(y1, b[1] + b[3] + m23);
        {
          float bm = 3.0f * blur + b[21] * clear_refract_scale * (1.0f + fabsf(b[22])) + m23 + 2.0f;
          if (bm > margin) margin = bm;
        }
      } else {
        x0 = fminf(x0, fmaxf(b[0], b[16])); y0 = fminf(y0, fmaxf(b[1], b[17]));
        x1 = fmaxf(x1, fminf(b[0] + b[2], b[18])); y1 = fmaxf(y1, fminf(b[1] + b[3], b[19]));
      }
    }
  }
  m->g.misc[0] = (float)n;
  m->g.misc[1] = in_through(at_through_tid, at_through_n, a[20]) ? 1.0f : 0.0f;
  m->g.gb[0] = x0 - 2; m->g.gb[1] = y0 - 2; m->g.gb[2] = x1 + 2; m->g.gb[3] = y1 + 2;
  float sc = (float)at_scale;
  int px0 = (int)floorf((x0 - margin) * sc), py0 = (int)floorf((y0 - margin) * sc);
  int px1 = (int)ceilf((x1 + margin) * sc), py1 = (int)ceilf((y1 + margin) * sc);
  if (px0 < 0) px0 = 0;
  if (py0 < 0) py0 = 0;
  if (px1 > tw) px1 = tw;
  if (py1 > th) py1 = th;
  if (px1 <= px0 || py1 <= py0 || x1 <= x0 || y1 <= y0) return 0;
  m->px0 = px0; m->py0 = py0; m->w = px1 - px0; m->h = py1 - py0;
  m->contained = is_contained ? mb_contained_make(&m->edge, a, px0 / sc, py0 / sc, 1.0f / sc, 1.0f / sc) : 0;
  m->progressive = is_progressive;
  m->dual_scale = is_dual;
  m->dual_w = dual_w;
  m->dual_wsp = is_dual ? 14.0f : 0.0f;
  if (is_progressive) {
    for (int i = 0; i < 4; i++) {
      m->fade[i] = a[52 + i];
      m->fadex[i] = a[56 + i];
    }
    int vx0 = (int)floorf(x0 * sc) - px0 - 2;
    int vy0 = (int)floorf(y0 * sc) - py0 - 2;
    int vx1 = (int)ceilf(x1 * sc) - px0 + 2;
    int vy1 = (int)ceilf(y1 * sc) - py0 + 2;
    if (vx0 < 0) vx0 = 0;
    if (vy0 < 0) vy0 = 0;
    if (vx1 > m->w) vx1 = m->w;
    if (vy1 > m->h) vy1 = m->h;
    if (vx1 <= vx0) { vx0 = 0; vx1 = m->w; }
    if (vy1 <= vy0) { vy0 = 0; vy1 = m->h; }
    m->vis_x = vx0; m->vis_y = vy0; m->vis_w = vx1 - vx0; m->vis_h = vy1 - vy0;
  }
  // down to where the deviation is 2 texels or more, at most 1/8 (and >= 2.4px uses half-res k=1 for 4x lower fill/cache cost with exact Gaussian variance).
  // iOS 27.0 Regular controls (blur <= 1.5pt) use crisp zero-frost sharp sampling (half-res lv[1] at >=2x, full-res at 1x) + 14pt wide ambient glass color, smoothly restoring sharp blur across 1.5..3.0pt during menu morphs.
  float sharp_fade = fminf(fmaxf((blur - 1.5f) / 1.5f, 0.0f), 1.0f);
  sharp_fade = sharp_fade * sharp_fade * (3.0f - 2.0f * sharp_fade);
  float crisp = fminf(dual_w / 0.10f, 1.0f);
  float sp = blur * (1.0f - crisp * (1.0f - sharp_fade)) * sc;
  int k = 0;
  if (sp >= 2.4f) k = 1;
  while (k < 3 && sp / (float)(1 << (k + 1)) >= 2.0f) k++;
  if (is_dual && k == 0 && sc >= 2.0f) k = 1;
  m->sp = sp;
  if (is_progressive && k == 0 && sp > 0.5f) k = 1;
  if (is_progressive && sp >= 2.5f && k < 2) k = 2;
  // what is left of the deviation once the halvings' and the last look's (bilinear) spread are taken out
  float sk = sp / (float)(1 << k);
  float pre = k > 0 ? 0.75f * (1.0f - powf(0.25f, (float)k)) / 3.0f + 1.0f / 6.0f : 0.0f;
  m->k = k;
  if (k >= 3) { m->dual_scale = 0; m->dual_w = 0.0f; m->dual_wsp = 0.0f; }
  m->sg = sqrtf(fmaxf(sk * sk - pre, 0.0f));
  return 1;
}

// The size of a grab's level k (its passes write each level's top left)
static void grab_levels(const Grab *m, int lw[4], int lh[4]) {
  lw[0] = m->w; lh[0] = m->h;
  for (int j = 1; j <= 3; j++) {
    lw[j] = (lw[j - 1] + 1) / 2;
    lh[j] = (lh[j - 1] + 1) / 2;
  }
}

static float g_light_angle = 0.0f;

static void grab_map_uvc(const Grab *m, id<MTLTexture> bt, float map[4], float uvc[4]) {
  int lw[4], lh[4];
  grab_levels(m, lw, lh);
  int sk = m->progressive ? 0 : m->k;
  float sc = (float)at_scale;
  float W = (float)bt.width, H = (float)bt.height, d = (float)(1 << sk);
  map[0] = sc / d / W; map[1] = sc / d / H;
  map[2] = -(float)m->px0 / d / W; map[3] = -(float)m->py0 / d / H;
  if (m->progressive) {
    uvc[0] = ((float)m->vis_x + 0.5f) / W;
    uvc[1] = ((float)m->vis_y + 0.5f) / H;
    uvc[2] = ((float)(m->vis_x + m->vis_w) - 0.5f) / W;
    uvc[3] = ((float)(m->vis_y + m->vis_h) - 0.5f) / H;
  } else {
    uvc[0] = 0.5f / W; uvc[1] = 0.5f / H;
    uvc[2] = ((float)lw[sk] - 0.5f) / W; uvc[3] = ((float)lh[sk] - 0.5f) / H;
  }
}

// What the glass shows of a grab's blur in texture `bt` (level k's place in it): gtex and gu, its numbers with where
// in it it is.
static void show_blur(const Grab *m, id<MTLTexture> bt) {
  gu = m->g;
  gu.misc[2] = cosf(g_light_angle);
  gu.misc[3] = sinf(g_light_angle);
  gtex = bt;
  grab_map_uvc(m, gtex, gu.map, gu.uvc);
}

// The level a grab's blur is left in (single source of truth for blur(), grab_blurred() and slot_fill()).
static id<MTLTexture> __strong *grab_blurred_ref(const Grab *m) {
  float dual_cut = m->k > 0 ? 0.55f : 0.3f;
  return (m->progressive || (m->dual_scale && m->sg <= dual_cut) || (m->contained && m->k == 0)) ? &ltmp : &lv[m->k];
}
static id<MTLTexture> grab_blurred(const Grab *m) { return *grab_blurred_ref(m); }

// A grab of target blurred into the levels: what is drawn so far in its rectangle copied out, halved k times, the
// Gaussian left (passes of their own, the target's pass ended before; grab_blurred). The levels are shared by every
// capture: what is shown is the copy a slot keeps (below).
// (how many: scripts/apple_render_test.m, a blur shown again as kept is none)
static int blurs_made;

static void blur(id<MTLCommandBuffer> cb, id<MTLTexture> target, const Grab *m) {
  blurs_made++;
  ensure_levels(m->w, m->h);
  id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
  [bl copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(m->px0, m->py0, 0)
           sourceSize:MTLSizeMake(m->w, m->h, 1) toTexture:lv[0] destinationSlice:0 destinationLevel:0
    destinationOrigin:MTLOriginMake(0, 0, 0)];
  [bl endEncoding];
  int lw[4], lh[4];
  grab_levels(m, lw, lh);
  int k = m->k;
  if (m->dual_scale) {
    for (int j = 1; j <= 3; j++) {
      post_pass(cb, lv[j - 1], lw[j - 1], lh[j - 1], lv[j], lw[j], lh[j], 1, 0, 0, 0, NULL);
    }
    float wsp = (m->dual_wsp > 0.0f ? m->dual_wsp : 14.0f) * (float)at_scale;
    float wsk = wsp / 8.0f;
    float wpre = 0.75f * (1.0f - 1.0f / 64.0f) / 3.0f + 1.0f / 6.0f;
    float wsg = sqrtf(fmaxf(wsk * wsk - wpre, 0.0f));
    if (wsg > 0.3f) {
      post_pass(cb, lv[3], lw[3], lh[3], ltmp, lw[3], lh[3], 6, wsg, 1, 0, NULL);
      post_pass(cb, ltmp, lw[3], lh[3], lv[3], lw[3], lh[3], 6, wsg, 0, 1, NULL);
    }
    MBContained dual;
    memset(&dual, 0, sizeof dual);
    float d3 = (float)(1 << (3 - k));
    float tw3 = (float)lv[3].width, th3 = (float)lv[3].height;
    dual.rs[0] = 1.0f / (d3 * tw3); dual.rs[1] = 1.0f / (d3 * th3);
    dual.rs[2] = 1.0f / tw3;        dual.rs[3] = 1.0f / th3;
    dual.rc[0] = ((float)lw[3] - 0.5f) / tw3;
    dual.rc[1] = ((float)lh[3] - 0.5f) / th3;
    dual.rc[2] = 1.0f - m->dual_w;
    dual.rc[3] = k > 0 ? 0.277f : 0.0f;
    float dual_cut = k > 0 ? 0.55f : 0.3f;
    if (m->sg > dual_cut) {
      post_pass(cb, lv[k], lw[k], lh[k], ltmp, lw[k], lh[k], 2, m->sg, 1, 0, NULL);
      post_pass(cb, ltmp, lw[k], lh[k], lv[k], lw[k], lh[k], 5, m->sg, 0, 1, &dual);
    } else {
      post_pass(cb, lv[k], lw[k], lh[k], ltmp, lw[k], lh[k], 5, 0.0f, 0, 0, &dual);
    }
    return;
  }
  if (m->progressive) {
    float prev_var = 0.0f;
    for (int j = 1; j <= k; j++) {
      post_pass(cb, lv[j - 1], lw[j - 1], lh[j - 1], lv[j], lw[j], lh[j], 1, 0, 0, 0, NULL);
      float target_px = powf(fmaxf(m->sp, 1.0f), (float)j / (float)k);
      float have_var = prev_var + 0.75f * (float)(1 << (2 * j - 2));
      float need_var = fmaxf(target_px * target_px - have_var, 0.0f);
      float sg_j = sqrtf(need_var) / (float)(1 << j);
      if (sg_j > 0.3f) {
        post_pass(cb, lv[j], lw[j], lh[j], ltmp, lw[j], lh[j], 2, sg_j, 1, 0, NULL);
        post_pass(cb, ltmp, lw[j], lh[j], lv[j], lw[j], lh[j], 2, sg_j, 0, 1, NULL);
      }
      prev_var = fmaxf(have_var, target_px * target_px);
    }
    float sc = (float)at_scale;
    MBContained prog;
    memset(&prog, 0, sizeof prog);
    prog.map[0] = (float)m->px0 / sc; prog.map[1] = (float)m->py0 / sc;
    prog.map[2] = 1.0f / sc; prog.map[3] = 1.0f / sc;
    for (int i = 0; i < 4; i++) { prog.box[i] = m->fade[i]; prog.rr[i] = m->fadex[i]; }
    prog.clip[0] = m->sp; prog.clip[1] = (float)k;
    float *ms[4] = {prog.rc, prog.rs, prog.outer, prog.orc};
    for (int j = 0; j < 4; j++) {
      float dj = (float)(1 << j), tw = (float)lv[j].width, th = (float)lv[j].height;
      ms[j][0] = 1.0f / (dj * tw); ms[j][1] = 1.0f / (dj * th);
      ms[j][2] = ((float)lw[j] - 0.5f) / tw; ms[j][3] = ((float)lh[j] - 0.5f) / th;
    }
    MTLScissorRect sc_vis = {(NSUInteger)m->vis_x, (NSUInteger)m->vis_y, (NSUInteger)m->vis_w, (NSUInteger)m->vis_h};
    post_pass_ex(cb, lv[0], lw[0], lh[0], ltmp, lw[0], lh[0], 4, 0, 0, 0, &prog, 0, &sc_vis);
    return;
  }
  if (m->contained) post_pass(cb, lv[0], lw[0], lh[0], ltmp, lw[0], lh[0], 3, 0, 0, 0, &m->edge);
  for (int j = 1; j <= k; j++) post_pass(cb, j == 1 && m->contained ? ltmp : lv[j - 1], lw[j - 1], lh[j - 1], lv[j], lw[j], lh[j], 1, 0, 0, 0, NULL);
  if (m->sg > 0.3f) {
    id<MTLTexture> from = grab_blurred(m);
    id<MTLTexture> tmp = m->contained && k == 0 ? lv[0] : ltmp;
    post_pass(cb, from, lw[k], lh[k], tmp, lw[k], lh[k], 2, m->sg, 1, 0, NULL);
    post_pass(cb, tmp, lw[k], lh[k], from, lw[k], lh[k], 2, m->sg, 0, 1, NULL);
  }
}

// The blurs kept, a slot each, as many as a window has groups of glass at once (a bar at the top, a composer and its
// ground at the foot, a sheet's, a page sliding over another: 2 to 6), the one least lately shown taken for a new one
// (gles_render.h's, GLES's). A slot holds a capture's blur copied out of the levels,
// as big as the levels (so it is shown by the same numbers as the level it came from, to the byte), and what it was
// of. What a capture copies is what the frame drew before it in the grab's rectangle (the instances up to the glass
// that reach into it, the pieces of the frame's shapes they are drawn by, the ground the frame cleared to) of a target
// of that size: a frame that draws all that the same under the same glass (a caret blinking in a composer over a chat
// at rest) shows the blur as it is, with no copy, no passes, and its pass not broken in two; one where something moved
// under one group of glass makes that group's blur again, the others' kept. A stream or a native view's hole drawn
// into it (its pictures change under the same instances) is blurred anew each frame, as is a frame an image under it
// had no texture for yet.
#define MB_GLASS_SLOTS 32
typedef struct {
  float *under;
  size_t under_cap, under_n;
  int32_t whole, picked;
  Grab grab;
  double key[7];
  int valid;
  unsigned used;
  unsigned gen;
} GlassSlot;
static GlassSlot slots[MB_GLASS_SLOTS];
static id<MTLTexture> slot_bt[MB_GLASS_SLOTS];
static unsigned slots_clock;
static unsigned slots_gen;
// what the capture now is of (under_take), as a slot keeps it
static float *under_now;
static size_t under_now_cap, under_now_n;
static int32_t under_whole, under_picked;

static MBRenderWin *at_win;

static void ad_forget(MBRenderWin *w) {
  if (!w) return;
  memset(w->ad_slots, 0, sizeof w->ad_slots);
  w->ad_last_time = 0.0;
  atomic_store(&w->ad_settling, 0);
  atomic_store(&w->ad_drawing, 0);
  atomic_store(&w->ad_has_adaptive, 0);
  os_unfair_lock_lock(&present_mu);
  w->ad_settle_dirty = (MBDirty){0};
  os_unfair_lock_unlock(&present_mu);
}

static void ensure_ad_tex(MBRenderWin *w) {
  if (!w || w->ad_tex[0]) return;
  for (int i = 0; i < 2; i++) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:MB_ADAPTIVE_MAX
                                                                                 height:1
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    w->ad_tex[i] = [dev newTextureWithDescriptor:td];
  }
  w->ad_cur = 0;
}

static void ad_fill_gu(MBRenderWin *w) {
  if (!w) {
    gu.ad_meta[0] = 0.0f;
    return;
  }
  int max_active = 0;
  for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
    if (w->ad_slots[i].active) {
      gu.ad_box[i][0] = w->ad_slots[i].cx;
      gu.ad_box[i][1] = w->ad_slots[i].cy;
      gu.ad_box[i][2] = w->ad_slots[i].hx;
      gu.ad_box[i][3] = w->ad_slots[i].hy;
      max_active = i + 1;
    } else {
      gu.ad_box[i][0] = -1e9f;
      gu.ad_box[i][1] = -1e9f;
      gu.ad_box[i][2] = -1e9f;
      gu.ad_box[i][3] = -1e9f;
    }
  }
  gu.ad_meta[0] = (float)max_active;
}

typedef struct {
  int sid;
  int is_new;
  int is_dark;
  int is_inverting;
  int through;
  float cx, cy, hx, hy;
  float tint_a;
} AdPending;

static int ad_collect_and_match(MBRenderWin *w, const float *f, int32_t start, int32_t end, int32_t count,
                                const Grab *m, int backdrop_changed, double dt, uint64_t serial,
                                AdPending out[MB_ADAPTIVE_MAX]) {
  (void)end;
  if (!w || dt < 0.0) return 0;
  const float *a = f + (size_t)start * MB_INST;
  int kind = (int)(a[5] + 0.5f);
  if (kind != 6) return 0;
  int merged_liquid = a[23] > 0.0f;
  struct { float cx, cy, hx, hy; int is_dark; int is_inverting; float tint_a; } items[MB_ADAPTIVE_MAX];
  int ni = 0;
  if (merged_liquid) {
    int is_clear = (a[7] <= 0.1f && a[11] < 0.12f);
    int is_adaptive = a[14] < -0.5f;
    if (is_adaptive && !is_clear) {
      float lum = 0.2126f * a[8] + 0.7152f * a[9] + 0.0722f * a[10];
      items[ni].cx = (m->g.gb[0] + m->g.gb[2]) * 0.5f;
      items[ni].cy = (m->g.gb[1] + m->g.gb[3]) * 0.5f;
      items[ni].hx = fmaxf((m->g.gb[2] - m->g.gb[0]) * 0.5f, 1.0f);
      items[ni].hy = fmaxf((m->g.gb[3] - m->g.gb[1]) * 0.5f, 1.0f);
      items[ni].is_dark = (a[11] >= 0.05f && lum < 0.35f) ? (a[11] > 0.42f ? 2 : 1) : 0;
      items[ni].is_inverting = (a[6] < 0.0f) ? 1 : 0;
      items[ni].tint_a = fminf(fmaxf(a[11], 0.0f), 1.0f);
      ni++;
    }
  } else {
    float tid = a[20];
    for (int32_t i = start; i < count && ni < MB_ADAPTIVE_MAX; i++) {
      const float *b = f + (size_t)i * MB_INST;
      if (b[20] != tid) continue;
      if ((int)(b[5] + 0.5f) != 6 || b[23] > 0.0f) continue;
      int is_clear = (b[7] <= 0.1f && b[11] < 0.12f);
      int is_adaptive = b[14] < -0.5f;
      if (!is_adaptive || is_clear) continue;
      float lum = 0.2126f * b[8] + 0.7152f * b[9] + 0.0722f * b[10];
      items[ni].cx = b[0] + b[2] * 0.5f;
      items[ni].cy = b[1] + b[3] * 0.5f;
      items[ni].hx = fmaxf(b[2] * 0.5f, 1.0f);
      items[ni].hy = fmaxf(b[3] * 0.5f, 1.0f);
      items[ni].is_dark = (b[11] >= 0.05f && lum < 0.35f) ? (b[11] > 0.42f ? 2 : 1) : 0;
      items[ni].is_inverting = (b[6] < 0.0f) ? 1 : 0;
      items[ni].tint_a = fminf(fmaxf(b[11], 0.0f), 1.0f);
      ni++;
    }
  }
  int cur_through = (m && m->g.misc[1] > 0.5f) ? 1 : 0;
  int n_upd = 0;
  for (int k = 0; k < ni; k++) {
    int sid = -1;
    int is_new = 0;
    int mode_changed = 0;
    // Check if already seen in an earlier dirty part of this same frame
    for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
      MBAdSlot *s = &w->ad_slots[i];
      if (s->active && s->seen_serial == serial && ((s->is_dark > 0) == (items[k].is_dark > 0)) &&
          hypotf(items[k].cx - s->cx, items[k].cy - s->cy) <= 2.0f) {
        sid = i;
        if (s->is_dark != items[k].is_dark || s->is_inverting != items[k].is_inverting ||
            fabsf(s->tint_a - items[k].tint_a) > 0.002f || s->through != cur_through) {
          s->is_dark = items[k].is_dark;
          s->is_inverting = items[k].is_inverting;
          s->tint_a = items[k].tint_a;
          s->through = cur_through;
          mode_changed = 1;
        }
        break;
      }
    }
    if (sid < 0) {
      float best_d = (float)MB_AD_MATCH_DIST + 1e-3f;
      for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
        MBAdSlot *s = &w->ad_slots[i];
        if (!s->active || s->seen_serial == serial || ((s->is_dark > 0) != (items[k].is_dark > 0))) continue;
        float d = hypotf(items[k].cx - s->cx, items[k].cy - s->cy);
        if (d <= best_d) { best_d = d; sid = i; }
      }
      if (sid < 0) {
        for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
          if (!w->ad_slots[i].active) { sid = i; break; }
        }
        if (sid < 0) {
          uint64_t oldest = UINT64_MAX;
          for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
            if (w->ad_slots[i].seen_serial != serial && w->ad_slots[i].seen_serial < oldest) {
              oldest = w->ad_slots[i].seen_serial;
              sid = i;
            }
          }
          if (sid < 0) sid = 0;
        }
        is_new = 1;
        w->ad_slots[sid] = (MBAdSlot){0};
        w->ad_slots[sid].active = 1;
        w->ad_slots[sid].is_dark = items[k].is_dark;
        w->ad_slots[sid].is_inverting = items[k].is_inverting;
        w->ad_slots[sid].through = cur_through;
        w->ad_slots[sid].tint_a = items[k].tint_a;
      } else if (w->ad_slots[sid].is_dark != items[k].is_dark ||
                 w->ad_slots[sid].is_inverting != items[k].is_inverting ||
                 fabsf(w->ad_slots[sid].tint_a - items[k].tint_a) > 0.002f ||
                 w->ad_slots[sid].through != cur_through) {
        w->ad_slots[sid].is_dark = items[k].is_dark;
        w->ad_slots[sid].is_inverting = items[k].is_inverting;
        w->ad_slots[sid].through = cur_through;
        w->ad_slots[sid].tint_a = items[k].tint_a;
        mode_changed = 1;
      }
      w->ad_slots[sid].cx = items[k].cx;
      w->ad_slots[sid].cy = items[k].cy;
      w->ad_slots[sid].hx = items[k].hx;
      w->ad_slots[sid].hy = items[k].hy;
      w->ad_slots[sid].seen_serial = serial;
    }
    MBAdSlot *s = &w->ad_slots[sid];
    if (!is_new && !mode_changed && s->updated_serial == serial) continue;
    int slot_changed = backdrop_changed || mode_changed;
    int need = is_new || slot_changed || s->settle_left > 0.0;
    if (need) {
      if (is_new || s->tint_a >= 0.88f) {
        s->settle_left = 0.0;
      } else if (slot_changed) {
        s->settle_left = MB_AD_SETTLE_SEC;
      } else {
        s->settle_left -= dt;
        if (s->settle_left <= 0.0) s->settle_left = 0.0;
      }
      s->updated_serial = serial;
      out[n_upd++] = (AdPending){sid, is_new, s->is_dark, s->is_inverting, s->through, s->cx, s->cy, s->hx, s->hy, s->tint_a};
    }
  }
  return n_upd;
}

typedef struct {
  AdPending p;
  __unsafe_unretained id<MTLTexture> tex;
  float map[4];
  float uvc[4];
} AdBatchItem;

static void ad_run_updates_batch(id<MTLCommandBuffer> cb, MBRenderWin *w, const AdBatchItem *items, int n_upd, double dt) {
  if (!w || n_upd <= 0) return;
  ensure_ad_tex(w);
  int cur = w->ad_cur, nxt = 1 - cur;
  id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
  [bl copyFromTexture:w->ad_tex[cur] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
           sourceSize:MTLSizeMake(MB_ADAPTIVE_MAX, 1, 1) toTexture:w->ad_tex[nxt] destinationSlice:0 destinationLevel:0
    destinationOrigin:MTLOriginMake(0, 0, 0)];
  [bl endEncoding];
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = w->ad_tex[nxt];
  rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
  [e setRenderPipelineState:adpipe];
  [e setViewport:(MTLViewport){0, 0, (double)MB_ADAPTIVE_MAX, 1.0, 0, 1}];
  [e setFragmentTexture:w->ad_tex[cur] atIndex:1];
  for (int i = 0; i < n_upd; i++) {
    if (!items[i].tex) continue;
    [e setFragmentTexture:items[i].tex atIndex:0];
    [e setScissorRect:(MTLScissorRect){(NSUInteger)items[i].p.sid, 0, 1, 1}];
    struct { float map[4]; float uvc[4]; float box[4]; float param[4]; } u;
    memcpy(u.map, items[i].map, sizeof u.map);
    memcpy(u.uvc, items[i].uvc, sizeof u.uvc);
    u.box[0] = items[i].p.cx; u.box[1] = items[i].p.cy; u.box[2] = items[i].p.hx; u.box[3] = items[i].p.hy;
    u.param[0] = (items[i].p.is_dark > 0) ? ((float)items[i].p.is_dark + items[i].p.tint_a) : -items[i].p.tint_a;
    u.param[1] = (float)items[i].p.is_new + (items[i].p.through ? 2.0f : 0.0f) + (items[i].p.is_inverting ? 4.0f : 0.0f);
    u.param[2] = (float)dt;
    u.param[3] = (float)items[i].p.sid;
    [e setFragmentBytes:&u length:sizeof u atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }
  [e endEncoding];
  w->ad_cur = nxt;
}

static void ad_run_updates(id<MTLCommandBuffer> cb, MBRenderWin *w, const AdPending *pend, int n_upd, double dt) {
  if (!w || n_upd <= 0 || !gtex) return;
  AdBatchItem items[MB_ADAPTIVE_MAX];
  int m = n_upd > MB_ADAPTIVE_MAX ? MB_ADAPTIVE_MAX : n_upd;
  for (int i = 0; i < m; i++) {
    items[i].p = pend[i];
    items[i].tex = gtex;
    memcpy(items[i].map, gu.map, sizeof items[i].map);
    memcpy(items[i].uvc, gu.uvc, sizeof items[i].uvc);
  }
  ad_run_updates_batch(cb, w, items, m, dt);
}

static double ad_next_dt(MBRenderWin *w) {
  if (ad_fixed_dt > 0.0) return ad_fixed_dt;
  double now = CACurrentMediaTime();
  double dt = (w && w->ad_last_time > 0.0) ? (now - w->ad_last_time) : (1.0 / 60.0);
  if (w) w->ad_last_time = now;
  if (dt < 0.0) dt = 0.0;
  if (dt > 0.1) dt = 0.1;
  return dt;
}

static void ad_finalize_frame(MBRenderWin *w, int whole_frame, uint64_t serial) {
  if (!w) return;
  int settling = 0, has_ad = 0;
  MBDirty sd = {0};
  for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
    MBAdSlot *s = &w->ad_slots[i];
    if (!s->active) continue;
    if (whole_frame && s->seen_serial != serial) {
      s->active = 0;
      s->settle_left = 0.0;
      continue;
    }
    has_ad = 1;
    if (s->settle_left > 0.0) {
      settling = 1;
      dirty_add(&sd, s->cx - s->hx - 4.0, s->cy - s->hy - 4.0, s->hx * 2.0 + 8.0, s->hy * 2.0 + 8.0);
    }
  }
  sd.glass_closure = settling;
  os_unfair_lock_lock(&present_mu);
  w->ad_settle_dirty = sd;
  os_unfair_lock_unlock(&present_mu);
  atomic_store(&w->ad_settling, settling);
  atomic_store(&w->ad_has_adaptive, has_ad);
}

static void slots_invalidate(void) {
  for (int i = 0; i < MB_GLASS_SLOTS; i++) slots[i].valid = 0;
  kept_ok = 0;
}

static void glass_free(void) {
  for (int j = 0; j < 4; j++) lv[j] = nil;
  ltmp = nil;
  gtex = nil;
  for (int j = 0; j < MB_BLUR_LAYERS; j++) bl_tex[j] = nil;
  bl_w = bl_h = 0;
  for (int i = 0; i < MB_GLASS_SLOTS; i++) {
    slot_bt[i] = nil;
    free(slots[i].under);
    slots[i].under = NULL;
    slots[i].under_cap = slots[i].under_n = 0;
  }
  lv_w = lv_h = 0;
  for (int i = 0; i < MB_WINS; i++) {
    render_wins[i].ad_tex[0] = nil;
    render_wins[i].ad_tex[1] = nil;
    ad_forget(&render_wins[i]);
  }
  glass_forget();
}

// Glass's textures (its levels as big as the largest capture yet, a copy a slot) let go of once this many frames in a
// row drew no glass (a sheet closed, the composer gone): held on, they are tens of MB at a window's size.
#define MB_GLASS_IDLE 180
static int glass_idle_frames;

static void glass_idle_tick(int glass) {
  if (glass) glass_idle_frames = 0;
  else if (glass_idle_frames < MB_GLASS_IDLE && ++glass_idle_frames == MB_GLASS_IDLE && lv_w > 0) glass_free();
}

static void glass_forget(void) {
  slots_invalidate();
  ad_forget(rw);
  if (at_win && at_win != rw) ad_forget(at_win);
}

// Whether instance i (its run of glass starting at `run`) draws anything in g (x0, y0, x1, y1, points): its quad as
// the vertex shader makes it (vmain: grown round its rect, liquid glass over its group's bounds, rotated when turned).
static int reaches(const float *f, int32_t i, int32_t run, const float g[4]) {
  const float *b = f + (size_t)i * MB_INST;
  int kind = (int)(b[5] + 0.5f);
  if (b[32] != 0.0f && kind == 6 && (b[23] > 0.0f || f[(size_t)run * MB_INST + 23] > 0.0f)) return 1;
  float e = (kind == 2 || kind == 14 || kind == 15) ? fabsf(b[7]) * 3.0f + 2.0f : 1.0f;
  // (liquid: its group's bounds, each shape and the first's reach round it, 2 more: grab_of)
  if (kind == 6) e = fmaxf(fmaxf(b[23], f[(size_t)run * MB_INST + 23]), 0.0f) + 2.0f;
  e += 1.0f;
  float rx0 = fminf(b[0], b[0] + b[2]) - e, rx1 = fmaxf(b[0], b[0] + b[2]) + e;
  float ry0 = fminf(b[1], b[1] + b[3]) - e, ry1 = fmaxf(b[1], b[1] + b[3]) + e;
  float x0 = rx0, x1 = rx1, y0 = ry0, y1 = ry1;
  if (b[32] != 0.0f) {
    float c = cosf(b[32]), s = sinf(b[32]);
    float ox = b[33], oy = b[34];
    float px[4] = {rx0, rx1, rx0, rx1};
    float py[4] = {ry0, ry0, ry1, ry1};
    x0 = x1 = ox + c * (px[0] - ox) - s * (py[0] - oy);
    y0 = y1 = oy + s * (px[0] - ox) + c * (py[0] - oy);
    for (int k = 1; k < 4; k++) {
      float ax = ox + c * (px[k] - ox) - s * (py[k] - oy);
      float ay = oy + s * (px[k] - ox) + c * (py[k] - oy);
      if (ax < x0) x0 = ax;
      if (ax > x1) x1 = ax;
      if (ay < y0) y0 = ay;
      if (ay > y1) y1 = ay;
    }
    x0 -= 1.0f; x1 += 1.0f;
    y0 -= 1.0f; y1 += 1.0f;
  }
  return x1 > g[0] && x0 < g[2] && y1 > g[1] && y0 < g[3];
}

// What the capture of the run at start is of (under_now): exact spatial dependency closure of every instance before
// start whose pixels (directly, or transitively through an earlier glass capture or adaptive ink sample) reach into
// the capture's rectangle, plus the frame's referenced shapes' pieces (from `pieces` on). 0 when a stream or a native
// view's hole is among them.
static int under_take(const float *f, int32_t start, int32_t count, int32_t pieces, const Grab *m) {
  float sc = (float)at_scale;
  float dep[4] = {(float)m->px0 / sc - 1.0f, (float)m->py0 / sc - 1.0f, (float)(m->px0 + m->w) / sc + 1.0f,
                  (float)(m->py0 + m->h) / sc + 1.0f};
  float seen_tids[32];
  int seen_n = 0;
  for (int pass = 0; pass < 4; pass++) {
    int grew = 0;
    for (int32_t i = start - 1; i >= 0; i--) {
      const float *bi = f + (size_t)i * MB_INST;
      float tid = bi[20];
      int k = (int)(bi[5] + 0.5f);
      int is_glass = tid < 0.0f;
      int is_ad = k == 7 || ((k == 10 || k == 11) && bi[23] > 0.0f);
      if (!is_glass && !is_ad) continue;
      int32_t run = i;
      if (is_glass) {
        while (run > 0 && f[(size_t)(run - 1) * MB_INST + 20] == tid) run--;
      }
      if (!reaches(f, i, run, dep)) continue;
      if (is_glass) {
        int already = 0;
        for (int s = 0; s < seen_n; s++) {
          if (seen_tids[s] == tid) { already = 1; break; }
        }
        if (already) continue;
        if (seen_n < 32) seen_tids[seen_n++] = tid;
        int32_t first_i = run;
        for (int32_t j = 0; j < run; j++) {
          if (f[(size_t)j * MB_INST + 20] == tid) { first_i = j; break; }
        }
        const float *a = f + (size_t)first_i * MB_INST;
        int gkind = (int)(a[5] + 0.5f);
        int is_contained = (gkind == 5 && a[7] < 0.0f);
        float raw_blur_r = (gkind == 6 && a[7] < 0.0f) ? 0.0f : fabsf(a[7]);
        int is_clear_r = (gkind == 6 && a[7] >= -0.001f && a[7] <= 0.1f && a[11] < 0.12f);
        float blur_r = (is_clear_r && a[11] > 0.01f && raw_blur_r < 0.1f) ? 1.2f : raw_blur_r;
        float clear_rs = (gkind == 6 && fabsf(a[7]) < 0.01f && a[11] > 0.005f && a[11] < 0.31f) ? 2.20f : 1.0f;
        int merged_liquid = (gkind == 6 && a[23] > 0.0f);
        int unmerged_liquid = (gkind == 6 && !merged_liquid);
        float margin = (is_contained || (gkind == 5 && a[22] < -0.5f)) ? 0.0f : 3.0f * blur_r + (gkind == 6 ? a[21] * clear_rs * (1.0f + fabsf(a[22])) + fmaxf(a[23], 0.0f) : 0.0f) + 2.0f;
        float gx0 = 1e9f, gy0 = 1e9f, gx1 = -1e9f, gy1 = -1e9f;
        for (int32_t j = first_i; j < count; j++) {
          const float *b = f + (size_t)j * MB_INST;
          if (b[20] != tid) continue;
          float bx0, by0, bx1, by1;
          if (gkind == 6) {
            float m23 = fmaxf(unmerged_liquid ? b[23] : a[23], 0.0f);
            bx0 = b[0] - m23; by0 = b[1] - m23; bx1 = b[0] + b[2] + m23; by1 = b[1] + b[3] + m23;
            if (unmerged_liquid) {
              float bm = 3.0f * blur_r + b[21] * clear_rs * (1.0f + fabsf(b[22])) + m23 + 2.0f;
              if (bm > margin) margin = bm;
            }
          } else {
            bx0 = fmaxf(b[0], b[16]); by0 = fmaxf(b[1], b[17]);
            bx1 = fminf(b[0] + b[2], b[18]); by1 = fminf(b[1] + b[3], b[19]);
          }
          gx0 = fminf(gx0, bx0); gy0 = fminf(gy0, by0);
          gx1 = fmaxf(gx1, bx1); gy1 = fmaxf(gy1, by1);
        }
        if (gx1 > gx0 && gy1 > gy0) {
          float ex0 = gx0 - margin - 2.0f, ey0 = gy0 - margin - 2.0f;
          float ex1 = gx1 + margin + 2.0f, ey1 = gy1 + margin + 2.0f;
          if (ex0 < dep[0]) { dep[0] = ex0; grew = 1; }
          if (ey0 < dep[1]) { dep[1] = ey0; grew = 1; }
          if (ex1 > dep[2]) { dep[2] = ex1; grew = 1; }
          if (ey1 > dep[3]) { dep[3] = ey1; grew = 1; }
        }
      } else {
        float rad = bi[23] + 4.0f;
        float ex0 = bi[21] - rad, ey0 = bi[22] - rad;
        float ex1 = bi[21] + rad, ey1 = bi[22] + rad;
        if (ex0 < dep[0]) { dep[0] = ex0; grew = 1; }
        if (ey0 < dep[1]) { dep[1] = ey0; grew = 1; }
        if (ex1 > dep[2]) { dep[2] = ex1; grew = 1; }
        if (ey1 > dep[3]) { dep[3] = ey1; grew = 1; }
      }
    }
    if (!grew) break;
  }
  size_t base_need = (size_t)start * MB_INST;
  if (under_now_cap < base_need) {
    under_now_cap = base_need;
    under_now = realloc(under_now, under_now_cap * sizeof(float));
  }
  int in_blur = 0, blur_hit = 0;
  int has_vec = 0;
  int32_t seg_min = INT32_MAX, seg_max = 0;
  size_t off = 0;
  int32_t picked = 0, run = 0;
  for (int32_t i = 0; i < start; i++) {
    float tid = f[(size_t)i * MB_INST + 20];
    if (i > 0 && tid != f[(size_t)(i - 1) * MB_INST + 20]) run = i;
    int k = (int)(f[(size_t)i * MB_INST + 5] + 0.5f);
    if (k == 14) {
      if (in_blur == 0) blur_hit = reaches(f, i, run, dep);
      in_blur++;
    }
    int hit = blur_hit || reaches(f, i, run, dep);
    if (k == 15 && in_blur > 0) {
      if (--in_blur == 0) blur_hit = 0;
    }
    if (!hit) continue;
    if (tid > (float)MB_STREAM_TEX) return 0;
    if (k == 11) {
      int32_t f0 = (int32_t)lroundf(f[(size_t)i * MB_INST + 12]);
      int32_t fn = (int32_t)lroundf(f[(size_t)i * MB_INST + 13]);
      if (fn > 0 && f0 >= 0) {
        has_vec = 1;
        if (f0 < seg_min) seg_min = f0;
        if (f0 + fn > seg_max) seg_max = f0 + fn;
      }
    }
    memcpy(under_now + off, f + (size_t)i * MB_INST, MB_INST * sizeof(float));
    off += MB_INST;
    picked++;
  }
  if (has_vec && count > pieces) {
    size_t total_seg = (size_t)(count - pieces) * MB_INST;
    size_t s0 = (size_t)(seg_min / 6) * MB_INST;
    size_t s1 = (size_t)((seg_max + 5) / 6) * MB_INST;
    if (s0 > total_seg) s0 = total_seg;
    if (s1 > total_seg) s1 = total_seg;
    if (s1 > s0) {
      size_t plen = s1 - s0;
      if (under_now_cap < off + plen) {
        under_now_cap = off + plen;
        under_now = realloc(under_now, under_now_cap * sizeof(float));
      }
      memcpy(under_now + off, f + (size_t)pieces * MB_INST + s0, plen * sizeof(float));
      off += plen;
    }
  }
  under_now_n = off;
  under_whole = 0;
  under_picked = picked;
  return 1;
}

// The slot whose blur is of what the capture now is (under_now), or -1.
static int slot_same(const Grab *m, const double key[7]) {
  for (int i = 0; i < MB_GLASS_SLOTS; i++) {
    GlassSlot *s = &slots[i];
    if (s->valid && s->under_n == under_now_n && s->whole == under_whole && s->picked == under_picked &&
        memcmp(&s->grab, m, sizeof *m) == 0 && memcmp(s->key, key, sizeof s->key) == 0 &&
        memcmp(s->under, under_now, under_now_n * sizeof(float)) == 0)
      return i;
  }
  return -1;
}

// The blur just made (blur) kept in the slot least lately shown by swapping texture handles with the level
// (zero GPU blit). Not yet said to be of anything (valid).
static int slot_fill(const Grab *m, unsigned frame_clock) {
  id<MTLTexture> __strong *Lp = grab_blurred_ref(m);
  id<MTLTexture> L = *Lp;
  int t0 = -1, t1 = -1, t2 = -1, t3 = -1, t4 = -1, fallback = 0;
  for (int j = 0; j < MB_GLASS_SLOTS; j++) {
    if (slots[j].used < slots[fallback].used) fallback = j;
    if (slots[j].used > frame_clock) continue;
    int has_bt = slot_bt[j] != nil;
    int same_tex = has_bt && (int)slot_bt[j].width == (int)L.width && (int)slot_bt[j].height == (int)L.height &&
                   slot_bt[j].pixelFormat == L.pixelFormat;
    if (!slots[j].valid) {
      if (same_tex) {
        if (t0 < 0) t0 = j;
      } else if (!has_bt) {
        if (t1 < 0) t1 = j;
      } else {
        if (t3 < 0) t3 = j;
      }
    } else if (same_tex) {
      if (t2 < 0 || slots[j].used < slots[t2].used) t2 = j;
    }
    if (t4 < 0 || slots[j].used < slots[t4].used) t4 = j;
  }
  int i = t0 >= 0 ? t0 : t1 >= 0 ? t1 : t2 >= 0 ? t2 : t3 >= 0 ? t3 : t4 >= 0 ? t4 : fallback;
  slots[i].valid = 0;
  slots[i].gen = ++slots_gen;
  if (slot_bt[i] != nil && slot_bt[i].width == L.width && slot_bt[i].height == L.height &&
      slot_bt[i].pixelFormat == L.pixelFormat) {
    id<MTLTexture> old = slot_bt[i];
    slot_bt[i] = L;
    *Lp = old;
  } else {
    slot_bt[i] = L;
    *Lp = level_tex(L.pixelFormat, (int)L.width, (int)L.height);
  }
  return i;
}

static void slot_show(const Grab *m, int i) {
  slots[i].used = ++slots_clock;
  show_blur(m, slot_bt[i]);
}

static void slot_keep(int i, const Grab *m, const double key[7]) {
  GlassSlot *s = &slots[i];
  if (s->under_cap < under_now_n) {
    s->under = realloc(s->under, under_now_n * sizeof(float));
    s->under_cap = under_now_n;
  }
  memcpy(s->under, under_now, under_now_n * sizeof(float));
  s->under_n = under_now_n;
  s->whole = under_whole;
  s->picked = under_picked;
  s->grab = *m;
  memcpy(s->key, key, sizeof s->key);
  s->valid = 1;
}

// where the pieces of the frame's shapes start (src/ui/shape.mbt: the instances of kind 12 after every one drawn)
static int32_t seg_base;

// Partial redraw: a window's frame is drawn into a canvas of its own, kept across frames, and
// copied into the drawable; where the app says only some parts of it differ from the frame before
// (@hostapi.render_dirty), only those parts of the canvas are drawn again, a pass each: it loads what the canvas holds,
// cut to the part (the pass's scissor, `sc`, while `sc_on`), the frame's ground drawn over the part first (the
// instance after the frame's, upload's), then every instance (what falls outside is cut away).
// a picture the frame drawn last had none of yet (its texture not made: drawn without it): the next drawn whole, as
// what it shows changes under the same numbers
static int pictures_missing, encode_missing;
static int sc_on;
static MTLScissorRect sc;

void mb_host_render_dirty(double x, double y, double w, double h) { dirty_add(&rw->said, x, y, w, h); }
void mb_host_glass_light_angle(double rad) {
  float a = (float)rad;
  if (a != g_light_angle) { g_light_angle = a; if (rw) shown_ok = 0; }
}

// The parts of a target tw × th pixels a frame draws, from what was said of them (`d`, a frame nothing was said for
// drawn whole), into `r`: how many (0 none), -1 all of it. `kept`: the target holds the frame before.
static int take_parts(const MBDirty *d, int tw, int th, int kept, MTLScissorRect r[MB_PARTS]) {
  if (!d->told || d->whole || !kept || pictures_missing) return -1;
  int k = 0;
  for (int i = 0; i < d->n; i++) {
    // (whole pixels round it, and one more: the app's part reaches its pixels' antialiasing, not their rounding)
    const double *e = d->r[i];
    int x0 = (int)floor(e[0] * at_scale) - 1, y0 = (int)floor(e[1] * at_scale) - 1;
    int x1 = (int)ceil((e[0] + e[2]) * at_scale) + 1, y1 = (int)ceil((e[1] + e[3]) * at_scale) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw) x1 = tw;
    if (y1 > th) y1 = th;
    if (x1 > x0 && y1 > y0) r[k++] = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
  }
  return k;
}

// Whether a grab's rectangle meets the part drawn (all of it where the frame is drawn whole).
static int grab_in_part(const Grab *m) {
  if (!sc_on) return 1;
  return m->px0 < (int)(sc.x + sc.width) && (int)sc.x < m->px0 + m->w && m->py0 < (int)(sc.y + sc.height) &&
         (int)sc.y < m->py0 + m->h;
}

static id<MTLRenderCommandEncoder> begin_pass_ex(id<MTLCommandBuffer> cb, id<MTLTexture> target, id<MTLRenderPipelineState> p,
                                                 id<MTLBuffer> buf, int clear, double r, double g, double b, double alpha,
                                                 float ox, float oy, float vp_w, float vp_h, int use_sc, int vpw, int vph) {
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = target;
  rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  rp.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, alpha);
  id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
  if (vpw > 0 && vph > 0) {
    [enc setViewport:(MTLViewport){0, 0, (double)vpw, (double)vph, 0, 1}];
    [enc setScissorRect:(MTLScissorRect){0, 0, (NSUInteger)vpw, (NSUInteger)vph}];
  }
  struct { float vp[2]; float scale; float pad; float origin[2]; float pad2[2]; } u = {
    {vp_w, vp_h}, (float)at_scale, (float)seg_base, {ox, oy}, {0, 0}};
  [enc setRenderPipelineState:p];
  [enc setVertexBuffer:buf offset:0 atIndex:0];
  [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
  [enc setVertexBytes:&gu length:sizeof(gu) atIndex:2];
  [enc setFragmentBuffer:buf offset:0 atIndex:0];
  [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
  [enc setFragmentBytes:&gu length:sizeof(gu) atIndex:2];
  [enc setFragmentTexture:atlas atIndex:0];
  [enc setFragmentTexture:catlas atIndex:3];
  [enc setFragmentTexture:(gtex ?: stand_tex) atIndex:2];
  id<MTLTexture> adt = (at_win && at_win->ad_tex[at_win->ad_cur]) ? at_win->ad_tex[at_win->ad_cur] : stand_tex;
  [enc setFragmentTexture:adt atIndex:4];
  if (use_sc && sc_on) [enc setScissorRect:sc];
  return enc;
}

static id<MTLRenderCommandEncoder> begin_pass(id<MTLCommandBuffer> cb, id<MTLTexture> target, id<MTLRenderPipelineState> p,
                                              id<MTLBuffer> buf, int clear, double r, double g, double b, double alpha) {
  return begin_pass_ex(cb, target, p, buf, clear, r, g, b, alpha, 0.0f, 0.0f, (float)at_w, (float)at_h, 1, 0, 0);
}

// Instances start to end drawn, each that pays for it in strips (bands.h), the rest as they come.
static void draw_banded(id<MTLRenderCommandEncoder> enc, id<MTLRenderPipelineState> p, const float *f, int32_t start,
                        int32_t end, int32_t count, int tw, int th) {
  MBStrip to = sc_on ? mb_strip((int)sc.x, (int)sc.y, (int)(sc.x + sc.width), (int)(sc.y + sc.height))
                     : mb_strip(0, 0, tw, th);
  MBStrip st[MB_STRIPS];
  int32_t s = start;
  for (int32_t i = start; i < end; i++) {
    if (!mb_band_kind(f, i)) continue;
    int n = mb_bands(f, i, count, (float)at_scale, to, st);
    if (n < 0) continue;
    if (i > s) [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:i - s baseInstance:s];
    for (int k = 0; k < n; k++) {
      if (k == 0 || st[k].flat != st[k - 1].flat) [enc setRenderPipelineState:st[k].flat ? fpipe : p];
      [enc setScissorRect:(MTLScissorRect){(NSUInteger)st[k].x0, (NSUInteger)st[k].y0, (NSUInteger)(st[k].x1 - st[k].x0),
                                           (NSUInteger)(st[k].y1 - st[k].y0)}];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:1 baseInstance:i];
    }
    if (n > 0 && st[n - 1].flat) [enc setRenderPipelineState:p];
    [enc setScissorRect:sc_on ? sc : (MTLScissorRect){0, 0, (NSUInteger)tw, (NSUInteger)th}];
    s = i + 1;
  }
  if (end > s) [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:end - s baseInstance:s];
}

static int fast_rect_enabled = -1;

static int fast_rect_on(void) {
  if (fast_rect_enabled < 0) {
    const char *v = getenv("MBUI_FAST_RECT");
    fast_rect_enabled = !(v && v[0] == '0');
  }
  return fast_rect_enabled;
}

static int is_simple_rect(const float *f, int32_t i) {
  if (!fast_rect_on()) return 0;
  const float *b = f + (size_t)i * MB_INST;
  return (int)(b[5] + 0.5f) == 0 && b[6] <= 0.0f &&
         b[20] == 0.0f && b[21] == 0.0f && b[22] == 0.0f && b[23] == 0.0f &&
         b[24] <= 0.0f && b[25] <= 0.0f && b[26] <= 0.0f && b[27] <= 0.0f &&
         b[32] == 0.0f &&
         b[36] <= 0.0f && b[37] <= 0.0f && b[38] <= 0.0f && b[39] <= 0.0f &&
         b[44] <= 0.0f && b[45] <= 0.0f && b[46] <= 0.0f && b[47] <= 0.0f &&
         b[53] <= 0.0f && b[55] <= 0.0f && b[57] <= 0.0f && b[59] <= 0.0f;
}

static id<MTLTexture> layer_tex_of(int lid) {
  if (lid <= 0 || !layer_texs) return nil;
  return layer_texs[@(lid)];
}

// The texture of picture, stream, or offscreen layer `tid` (> 0) for the frame being drawn (at_texs), nil where it has none yet.
static id<MTLTexture> tex_of(int tid) {
  if (at_texs) {
    id t = at_texs[@(tid)];
    return t == (id)NSNull.null ? nil : t;
  }
  return tid > MB_LAYER_TEX ? layer_tex_of(tid - MB_LAYER_TEX)
       : tid > MB_STREAM_TEX ? mb_stream_texture(tid - MB_STREAM_TEX)
       : tid > 0 ? mb_image_texture(tid) : nil;
}

// The textures of the pictures, streams, and layers `f` draws (`count` instances), looked up now (the main thread), for the
// queue to draw them by: a picture's not decoded yet NSNull (drawn without it, encode_missing).
static NSMutableDictionary<NSNumber *, id> *frame_texs(const float *f, int32_t count) {
  NSMutableDictionary<NSNumber *, id> *d = [NSMutableDictionary new];
  int last = 0;
  for (int32_t i = 0; i < count; i++) {
    const float *b = f + (size_t)i * MB_INST;
    int tid = (int)b[20];
    if (tid <= 0 || tid == last || (int)(b[5] + 0.5f) == 9) continue;
    last = tid;
    NSNumber *k = @(tid);
    if (d[k]) continue;
    id<MTLTexture> t = tid > MB_LAYER_TEX ? layer_tex_of(tid - MB_LAYER_TEX)
                     : tid > MB_STREAM_TEX ? mb_stream_texture(tid - MB_STREAM_TEX)
                     : mb_image_texture(tid);
    d[k] = t ?: (id)NSNull.null;
  }
  return d;
}

static void size_levels_once(const float *f, int32_t from, int32_t count, int tw, int th) {
  int max_w = 0, max_h = 0, scan_bound = 0;
  float sc_f = (float)at_scale;
  for (int32_t s0 = from; s0 < count;) {
    int stid = (int)f[s0 * MB_INST + 20];
    int skind = (int)(f[s0 * MB_INST + 5] + 0.5f);
    int32_t e0 = s0 + 1;
    if (stid < 0) {
      while (e0 < count && (int)f[e0 * MB_INST + 20] == stid) e0++;
      if (stid != scan_bound) {
        scan_bound = stid;
        Grab gm;
        if (grab_of(&gm, f, s0, e0, count, tw, th) && grab_in_part(&gm)) {
          if (gm.w > max_w) max_w = gm.w;
          if (gm.h > max_h) max_h = gm.h;
        }
      }
    } else if (skind == 14) {
      const float *bl = f + (size_t)s0 * MB_INST;
      float sigma = fmaxf(bl[7], 0.0f);
      if (sigma > 0.0f) {
        float margin = ceilf(3.0f * sigma) + 2.0f;
        int px0 = (int)floorf((bl[0] - margin) * sc_f), py0 = (int)floorf((bl[1] - margin) * sc_f);
        int px1 = (int)ceilf((bl[0] + bl[2] + margin) * sc_f), py1 = (int)ceilf((bl[1] + bl[3] + margin) * sc_f);
        if (px0 < 0) px0 = 0;
        if (py0 < 0) py0 = 0;
        if (px1 > tw) px1 = tw;
        if (py1 > th) py1 = th;
        int bw = px1 - px0, bh = py1 - py0;
        if (bw > max_w) max_w = bw;
        if (bh > max_h) max_h = bh;
      }
    }
    s0 = e0;
  }
  if (max_w > lv_w || max_h > lv_h) ensure_levels(max_w, max_h);
}

// `part`: only that part of the target drawn again, over what it holds (the frame's ground first: the instance after
// the frame's, ring_put's); NULL, all of it, cleared to the ground.
static void encode(id<MTLCommandBuffer> cb, id<MTLTexture> target, id<MTLRenderPipelineState> p,
                   id<MTLBuffer> buf, int32_t count, double r, double g, double b, double alpha,
                   const MTLScissorRect *part, double ad_dt, uint64_t ad_serial) {
  gtex = nil;
  memset(&gu, 0, sizeof gu);
  ad_fill_gu(at_win);
  encode_missing = 0;
  const float *f = (const float *)buf.contents;
  seg_base = count;
  while (seg_base > 0 && (int)(f[(seg_base - 1) * MB_INST + 5] + 0.5f) == 12) seg_base--;
  const MTLScissorRect *orig_part = part;
  sc_on = orig_part != NULL;
  if (orig_part) sc = *orig_part;
  id<MTLTexture> cur_target = target;
  int tw = (int)target.width, th = (int)target.height;
  float sc_f = (float)at_scale;
  BlurFrame bstack[MB_BLUR_LAYERS];
  int bdepth = 0, bover = 0;
  // Pre-scan top-level glass captures before opening the main render pass: size blur levels once,
  // cache slot hits (kind 5 & 6), and batch adaptive updates for hit kind-6 groups before begin_pass.
  int levels_sized = 0;
  struct {
    int tid;
    int slot;
    unsigned gen;
    Grab grab;
  } pre_hits[16];
  int pre_hits_n = 0;
  {
    int any_glass = 0, pre_bd = 0;
    for (int32_t s0 = 0; s0 < count; s0++) {
      int sk = (int)(f[s0 * MB_INST + 5] + 0.5f);
      if (sk == 14) { pre_bd++; continue; }
      if (sk == 15) { if (pre_bd > 0) pre_bd--; continue; }
      if (pre_bd == 0 && (int)f[s0 * MB_INST + 20] < 0) { any_glass = 1; break; }
    }
    if (any_glass) {
      levels_sized = 1;
      size_levels_once(f, 0, count, tw, th);
      AdBatchItem pre_batch[MB_ADAPTIVE_MAX];
      int pre_n = 0;
      double key[7] = {target.width, target.height, at_scale, r, g, b, alpha};
      int pre_bound = 0, scan_bd = 0;
      for (int32_t s0 = 0; s0 < count;) {
        int skind = (int)(f[s0 * MB_INST + 5] + 0.5f);
        if (skind == 14) { scan_bd++; s0++; continue; }
        if (skind == 15) { if (scan_bd > 0) scan_bd--; s0++; continue; }
        int stid = (int)f[s0 * MB_INST + 20];
        int32_t e0 = s0 + 1;
        if (scan_bd == 0 && stid < 0) {
          while (e0 < count && (int)f[e0 * MB_INST + 20] == stid) {
            int ek = (int)(f[e0 * MB_INST + 5] + 0.5f);
            if (ek == 14 || ek == 15) break;
            e0++;
          }
          if (stid != pre_bound) {
            pre_bound = stid;
            if (skind == 5 || skind == 6) {
              Grab gm;
              if (grab_of(&gm, f, s0, e0, count, tw, th) && grab_in_part(&gm) &&
                  under_take(f, s0, count, seg_base, &gm)) {
                int sl = slot_same(&gm, key);
                if (sl >= 0 && slot_bt[sl]) {
                  if (pre_hits_n < 16) {
                    pre_hits[pre_hits_n].tid = stid;
                    pre_hits[pre_hits_n].slot = sl;
                    pre_hits[pre_hits_n].gen = slots[sl].gen;
                    pre_hits[pre_hits_n].grab = gm;
                    pre_hits_n++;
                  }
                  if (at_win && ad_dt >= 0.0 && skind == 6 && pre_n < MB_ADAPTIVE_MAX) {
                    AdPending tmp[MB_ADAPTIVE_MAX];
                    int nu = ad_collect_and_match(at_win, f, s0, e0, count, &gm, 0, ad_dt, ad_serial, tmp);
                    if (nu > 0) {
                      float map[4], uvc[4];
                      grab_map_uvc(&gm, slot_bt[sl], map, uvc);
                      for (int i = 0; i < nu && pre_n < MB_ADAPTIVE_MAX; i++) {
                        pre_batch[pre_n].p = tmp[i];
                        pre_batch[pre_n].tex = slot_bt[sl];
                        memcpy(pre_batch[pre_n].map, map, sizeof map);
                        memcpy(pre_batch[pre_n].uvc, uvc, sizeof uvc);
                        pre_n++;
                      }
                    }
                  }
                }
              }
            }
          }
        }
        s0 = e0;
      }
      if (pre_n > 0) {
        ad_run_updates_batch(cb, at_win, pre_batch, pre_n, ad_dt);
        ad_fill_gu(at_win);
      }
    }
  }
  id<MTLRenderCommandEncoder> enc = begin_pass(cb, cur_target, p, buf, orig_part ? 0 : 1, r, g, b, alpha);
  if (orig_part) {
    [enc setRenderPipelineState:hpipe];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:1 baseInstance:count];
  }
  // one draw per run of instances showing the same image (slot 20; 0:
  // none, the atlas stands in) of one kind (its pipeline: kind_pipe), or sharing one capture of glass (negative),
  // and holes for native views (kind 9) apart, unblended. Glass over a hole
  // sees the hole (the canvas only, not the view under it).
  int32_t start = 0;
  int bound = 0, shows = 0;
  unsigned frame_clock = slots_clock;
  while (start < count) {
    int tid = (int)f[start * MB_INST + 20];
    int kind = (int)(f[start * MB_INST + 5] + 0.5f), hole = kind == 9;
    if (kind == 14) {
      if (!levels_sized) {
        levels_sized = 1;
        size_levels_once(f, start, count, tw, th);
      }
      if (bdepth >= MB_BLUR_LAYERS) {
        bover++;
        start++;
        continue;
      }
      const float *bl = f + (size_t)start * MB_INST;
      float sigma = fmaxf(bl[7], 0.0f);
      float margin = sigma > 0.0f ? ceilf(3.0f * sigma) + 2.0f : 0.0f;
      int px0 = (int)floorf((bl[0] - margin) * sc_f), py0 = (int)floorf((bl[1] - margin) * sc_f);
      int px1 = (int)ceilf((bl[0] + bl[2] + margin) * sc_f), py1 = (int)ceilf((bl[1] + bl[3] + margin) * sc_f);
      if (px0 < 0) px0 = 0;
      if (py0 < 0) py0 = 0;
      if (px1 > tw) px1 = tw;
      if (py1 > th) py1 = th;
      int bw = px1 - px0, bh = py1 - py0;
      if (bw <= 0 || bh <= 0 || sigma <= 0.0f) {
        bstack[bdepth++] = (BlurFrame){px0, py0, bw, bh, 0, sigma};
        start++;
        continue;
      }
      [enc endEncoding];
      ensure_blur_layer(bdepth, tw, th);
      bstack[bdepth] = (BlurFrame){px0, py0, bw, bh, 1, sigma};
      cur_target = bl_tex[bdepth];
      bdepth++;
      sc_on = 1;
      sc = (MTLScissorRect){(NSUInteger)px0, (NSUInteger)py0, (NSUInteger)bw, (NSUInteger)bh};
      enc = begin_pass(cb, cur_target, p, buf, 1, 0, 0, 0, 0);
      bound = 0;
      start++;
      continue;
    }
    if (kind == 15) {
      if (bover > 0) {
        bover--;
        start++;
        continue;
      }
      if (bdepth <= 0) {
        start++;
        continue;
      }
      BlurFrame L = bstack[--bdepth];
      if (!L.active) {
        start++;
        continue;
      }
      [enc endEncoding];
      ensure_levels(L.w, L.h);
      float sp = L.sigma * sc_f;
      int k = 0;
      while (k < 3 && sp / (float)(1 << (k + 1)) >= 2.0f) k++;
      float sk = sp / (float)(1 << k);
      float pre = k > 0 ? 0.75f * (1.0f - powf(0.25f, (float)k)) / 3.0f + 1.0f / 6.0f : 0.0f;
      float sg = sqrtf(fmaxf(sk * sk - pre, 0.0f));
      int lw[4], lh[4];
      lw[0] = L.w; lh[0] = L.h;
      for (int j = 1; j <= 3; j++) {
        lw[j] = (lw[j - 1] + 1) / 2;
        lh[j] = (lh[j - 1] + 1) / 2;
      }
      clear_tex(cb, lv[0]);
      id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
      [bl copyFromTexture:bl_tex[bdepth] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(L.px0, L.py0, 0)
               sourceSize:MTLSizeMake(L.w, L.h, 1) toTexture:lv[0] destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
      [bl endEncoding];
      for (int j = 1; j <= k; j++)
        post_pass_clear(cb, lv[j - 1], lw[j - 1], lh[j - 1], lv[j], lw[j], lh[j], 1, 0, 0, 0, NULL);
      if (sg > 0.3f) {
        post_pass_clear(cb, lv[k], lw[k], lh[k], ltmp, lw[k], lh[k], 2, sg, 1, 0, NULL);
        post_pass_clear(cb, ltmp, lw[k], lh[k], lv[k], lw[k], lh[k], 2, sg, 0, 1, NULL);
      }
      int parent = bdepth - 1;
      while (parent >= 0 && !bstack[parent].active) parent--;
      cur_target = parent >= 0 ? bl_tex[parent] : target;
      if (parent >= 0) {
        BlurFrame P = bstack[parent];
        sc_on = 1;
        sc = (MTLScissorRect){(NSUInteger)P.px0, (NSUInteger)P.py0, (NSUInteger)P.w, (NSUInteger)P.h};
      } else {
        sc_on = orig_part != NULL;
        if (orig_part) sc = *orig_part;
      }
      enc = begin_pass(cb, cur_target, p, buf, 0, r, g, b, alpha);
      bound = 0;
      float comp[MB_INST];
      memcpy(comp, f + (size_t)start * MB_INST, sizeof comp);
      comp[0] = (float)L.px0 / sc_f;
      comp[1] = (float)L.py0 / sc_f;
      comp[2] = (float)L.w / sc_f;
      comp[3] = (float)L.h / sc_f;
      comp[4] = 0.0f;
      comp[5] = 4.0f;
      comp[6] = 0.0f;
      comp[7] = 0.0f;
      comp[8] = 1.0f;
      comp[9] = 1.0f;
      comp[10] = 1.0f;
      float d = (float)(1 << k), W = (float)lv[k].width, H = (float)lv[k].height;
      comp[12] = 0.0f;
      comp[13] = 0.0f;
      comp[14] = (float)L.w / (d * W);
      comp[15] = (float)L.h / (d * H);
      comp[20] = comp[21] = comp[22] = comp[23] = 0.0f;
      memset(comp + 32, 0, 8 * sizeof(float));
      [enc setRenderPipelineState:kind_pipe(p, 4)];
      [enc setFragmentTexture:lv[k] atIndex:1];
      [enc setVertexBytes:comp length:sizeof comp atIndex:0];
      [enc setFragmentBytes:comp length:sizeof comp atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:1 baseInstance:0];
      [enc setVertexBuffer:buf offset:0 atIndex:0];
      [enc setFragmentBuffer:buf offset:0 atIndex:0];
      start++;
      continue;
    }
    if (kind == 8) {
      int32_t child_n = (int32_t)(f[start * MB_INST + 12] + 0.5f);
      if (child_n < 0) child_n = 0;
      if (start + 1 + child_n > count) child_n = count - start - 1;
      const float *hb = f + (size_t)start * MB_INST;
      if (child_n <= 0 || hb[11] <= 0.001f || hb[2] <= 0.0f || hb[3] <= 0.0f) {
        start += 1 + child_n;
        continue;
      }
      float blur_pt = fmaxf(hb[7], 0.0f);
      float refr_pt = fmaxf(hb[14], 0.0f);
      float margin = blur_pt * 3.0f + refr_pt + 4.0f;
      float x0 = hb[0], y0 = hb[1], x1 = hb[0] + hb[2], y1 = hb[1] + hb[3];
      for (int32_t ci = start + 1; ci < start + 1 + child_n; ci++) {
        const float *cb_f = f + (size_t)ci * MB_INST;
        int ck = (int)(cb_f[5] + 0.5f);
        if (ck == 12 || cb_f[2] <= 0.0f || cb_f[3] <= 0.0f) continue;
        if (cb_f[0] < x0) x0 = cb_f[0];
        if (cb_f[1] < y0) y0 = cb_f[1];
        if (cb_f[0] + cb_f[2] > x1) x1 = cb_f[0] + cb_f[2];
        if (cb_f[1] + cb_f[3] > y1) y1 = cb_f[1] + cb_f[3];
      }
      float ox = floorf((x0 - margin) * sc_f) / sc_f;
      float oy = floorf((y0 - margin) * sc_f) / sc_f;
      int pw = (int)ceilf(((x1 + margin) - ox) * sc_f);
      int ph = (int)ceilf(((y1 + margin) - oy) * sc_f);
      int max_dim_w = tw > 2048 ? tw : 2048;
      int max_dim_h = th > 2048 ? th : 2048;
      if (pw < 1) pw = 1;
      if (ph < 1) ph = 1;
      if (pw > max_dim_w) pw = max_dim_w;
      if (ph > max_dim_h) ph = max_dim_h;
      [enc endEncoding];
      ensure_levels(pw, ph);
      // Pass 1: render child subtree into lv[0] cleared to transparent
      id<MTLRenderCommandEncoder> lenc = begin_pass_ex(cb, lv[0], p, buf, 1, 0, 0, 0, 0,
                                                       ox, oy, (float)pw / sc_f, (float)ph / sc_f, 0, pw, ph);
      int32_t cs = start + 1, cend_all = start + 1 + child_n;
      while (cs < cend_all) {
        int ctid = (int)f[cs * MB_INST + 20];
        int ck = (int)(f[cs * MB_INST + 5] + 0.5f);
        if (ck == 5 || ck == 6 || ck == 8 || ck == 9 || ck == 12 || ck == 14 || ck == 15 || ctid < 0) {
          cs++;
          continue;
        }
        int cown = has_own_pipe(ck);
        int32_t ce = cs + 1;
        while (ce < cend_all && (int)f[ce * MB_INST + 20] == ctid) {
          int kk = (int)(f[ce * MB_INST + 5] + 0.5f);
          if (kk == 5 || kk == 6 || kk == 8 || kk == 9 || kk == 12 || kk == 14 || kk == 15) break;
          if (cown ? kk != ck : has_own_pipe(kk)) break;
          ce++;
        }
        id<MTLTexture> ct = tex_of(ctid);
        if (ctid > MB_STREAM_TEX && !ct) { cs = ce; continue; }
        if (ctid > 0 && ctid <= MB_STREAM_TEX && !ct) encode_missing = 1;
        [lenc setRenderPipelineState:(cown ? kind_pipe(p, ck) : p)];
        [lenc setFragmentTexture:(ct ?: stand_tex) atIndex:1];
        [lenc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:ce - cs baseInstance:cs];
        cs = ce;
      }
      [lenc endEncoding];
      // Pass 2: optional downsample + separable Gaussian blur
      int lw[4], lh[4];
      lw[0] = pw; lh[0] = ph;
      for (int j = 1; j <= 3; j++) { lw[j] = (lw[j - 1] + 1) / 2; lh[j] = (lh[j - 1] + 1) / 2; }
      float sp = blur_pt * sc_f;
      int lk = 0;
      while (lk < 2 && sp / (float)(1 << (lk + 1)) >= 2.0f) lk++;
      float sk_f = sp / (float)(1 << lk);
      float pre = lk > 0 ? 0.75f * (1.0f - powf(0.25f, (float)lk)) / 3.0f + 1.0f / 6.0f : 0.0f;
      float sg = sqrtf(fmaxf(sk_f * sk_f - pre, 0.0f));
      if (sp > 0.25f) {
        for (int j = 1; j <= lk; j++) {
          post_pass_ex(cb, lv[j - 1], lw[j - 1], lh[j - 1], lv[j], lw[j], lh[j], 1, 0, 0, 0, NULL, 0, NULL);
        }
        if (sg > 0.3f) {
          post_pass_ex(cb, lv[lk], lw[lk], lh[lk], ltmp, lw[lk], lh[lk], 2, sg, 1, 0, NULL, 0, NULL);
          post_pass_ex(cb, ltmp, lw[lk], lh[lk], lv[lk], lw[lk], lh[lk], 2, sg, 0, 1, NULL, 0, NULL);
        }
      } else {
        lk = 0;
      }
      // Pass 3: composite refracted & SDF-clipped foreground lens onto cur_target
      id<MTLTexture> ltex = lv[lk];
      gtex = ltex;
      memset(&gu, 0, sizeof gu);
      float W = (float)ltex.width, H = (float)ltex.height, d_lv = (float)(1 << lk);
      gu.map[0] = sc_f / d_lv / W; gu.map[1] = sc_f / d_lv / H;
      gu.map[2] = -(ox * sc_f) / d_lv / W; gu.map[3] = -(oy * sc_f) / d_lv / H;
      gu.uvc[0] = 0.5f / W; gu.uvc[1] = 0.5f / H;
      gu.uvc[2] = ((float)lw[lk] - 0.5f) / W; gu.uvc[3] = ((float)lh[lk] - 0.5f) / H;
      enc = begin_pass(cb, cur_target, p, buf, 0, r, g, b, alpha);
      [enc setRenderPipelineState:kind_pipe(p, 8)];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:1 baseInstance:start];
      bound = 0;
      start += 1 + child_n;
      continue;
    }
    // (kinds without their own pipeline one run, as before)
    int own = tid >= 0 && has_own_pipe(kind);
    id<MTLRenderPipelineState> rect_pipe = (p == rpipe_off) ? rpipe_rect_off : rpipe_rect;
    int simple = (tid == 0 && kind == 0 && rect_pipe) ? is_simple_rect(f, start) : 0;
    int32_t end = start + 1;
    while (end < count && (int)f[end * MB_INST + 20] == tid) {
      int k = (int)(f[end * MB_INST + 5] + 0.5f);
      if (k == 8 || k == 14 || k == 15) break;
      if ((k == 9) != hole || (own ? k != kind : tid >= 0 && has_own_pipe(k))) break;
      if (tid == 0 && kind == 0 && rect_pipe && is_simple_rect(f, end) != simple) break;
      end++;
    }
    if (tid < 0) {
      if (!levels_sized) {
        levels_sized = 1;
        size_levels_once(f, start, count, tw, th);
      }
      if (tid != bound) {
        Grab m;
        double key[7] = {target.width, target.height, at_scale, r, g, b, alpha};
        int known = 0, slot = -1;
        if (cur_target == target) {
          for (int h = 0; h < pre_hits_n; h++) {
            if (pre_hits[h].tid == tid) {
              int hs = pre_hits[h].slot;
              if (slots[hs].valid && slot_bt[hs] && slots[hs].gen == pre_hits[h].gen) {
                m = pre_hits[h].grab;
                shows = 1;
                known = 1;
                slot = hs;
              }
              break;
            }
          }
        }
        if (slot < 0) {
          shows = grab_of(&m, f, start, end, count, tw, th);
          // (a capture outside the part drawn: neither copied nor drawn, the blur kept as it was. One that meets it is
          // in it whole: the app's part takes in all a capture it reaches copies, @damage)
          shows = shows && grab_in_part(&m);
          known = shows && cur_target == target && under_take(f, start, count, seg_base, &m);
          slot = known ? slot_same(&m, key) : -1;
        }
        AdPending ad_pend[MB_ADAPTIVE_MAX];
        int ad_n_upd = 0;
        double eff_ad_dt = (cur_target == target) ? ad_dt : -1.0;
        if (slot >= 0) {
          // what it sees is what a slot holds: shown on in this pass unless an adaptive slot needs a settle step
          kept_ok = 1;
          slot_show(&m, slot);
          ad_n_upd = ad_collect_and_match(at_win, f, start, end, count, &m, 0, eff_ad_dt, ad_serial, ad_pend);
          if (ad_n_upd > 0) {
            [enc endEncoding];
            ad_run_updates(cb, at_win, ad_pend, ad_n_upd, eff_ad_dt);
            ad_fill_gu(at_win);
            enc = begin_pass(cb, cur_target, p, buf, 0, r, g, b, alpha);
          } else {
            ad_fill_gu(at_win);
            [enc setVertexBytes:&gu length:sizeof gu atIndex:2];
            [enc setFragmentBytes:&gu length:sizeof gu atIndex:2];
            [enc setFragmentTexture:gtex atIndex:2];
            id<MTLTexture> adt = (at_win && at_win->ad_tex[at_win->ad_cur]) ? at_win->ad_tex[at_win->ad_cur] : stand_tex;
            [enc setFragmentTexture:adt atIndex:4];
          }
        } else if (shows) {
          [enc endEncoding];
          blur(cb, cur_target, &m);
          slot = slot_fill(&m, frame_clock);
          // (an image drawn under it that had no texture yet: it comes under the same instances, so this blur is not
          // kept to be shown again as theirs)
          if (known && !encode_missing) slot_keep(slot, &m, key);
          kept_ok = slots[slot].valid;
          slot_show(&m, slot);
          ad_n_upd = ad_collect_and_match(at_win, f, start, end, count, &m, 1, eff_ad_dt, ad_serial, ad_pend);
          if (ad_n_upd > 0) ad_run_updates(cb, at_win, ad_pend, ad_n_upd, eff_ad_dt);
          ad_fill_gu(at_win);
          enc = begin_pass(cb, cur_target, p, buf, 0, r, g, b, alpha);
        }
        bound = tid;
      }
      // merged liquid (merge > 0): the group as one shape, drawn once; unmerged liquid and frosted: each instance
      if (shows) {
        int merged_liquid = kind == 6 && f[start * MB_INST + 23] > 0.0f;
        int gkind = kind == 5 ? 5 : (kind == 6 ? (merged_liquid ? 6 : MB_KIND_LIQUID_SINGLE) : -1);
        [enc setRenderPipelineState:kind_pipe(p, gkind)];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6
                instanceCount:merged_liquid ? 1 : end - start baseInstance:start];
      }
      start = end;
      continue;
    }
    id<MTLRenderPipelineState> kp = simple ? rect_pipe : (own ? kind_pipe(p, kind) : p);
    [enc setRenderPipelineState:(hole ? hpipe : kp)];
    // (a hole's image is its view's, src/hostapi/host.mbt view_tex_base + its id: the view is under the canvas here)
    id<MTLTexture> t = hole ? nil : tex_of(tid);
    // (a stream with no frame of its own yet, or lost: nothing, not the atlas)
    if (!hole && tid > MB_STREAM_TEX && !t) {
      start = end;
      continue;
    }
    if (!hole && tid > 0 && tid <= MB_STREAM_TEX && !t) encode_missing = 1;
    [enc setFragmentTexture:(t ?: stand_tex) atIndex:1];
    if (hole) [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6
                      instanceCount:end - start baseInstance:start];
    else draw_banded(enc, kp, f, start, end, count, tw, th);
    start = end;
  }
  [enc endEncoding];
  sc_on = 0;
}

// The frame in `buf` drawn into `target`, only its parts said to differ (`d`, take_parts) where it holds the frame
// before (`kept`), a pass each, else whole: how many parts (-1 whole, 0 none).
static int draw_parts(id<MTLCommandBuffer> cb, id<MTLTexture> target, id<MTLBuffer> buf, int32_t count, double r,
                      double g, double b, int kept, const MBDirty *d, double ad_dt, uint64_t ad_serial) {
  MBDirty expanded = *d;
  if (expanded.glass_closure && expanded.told && !expanded.whole && kept && !pictures_missing &&
      !mb_glass_damage_expand((const float *)buf.contents, count, expanded.r, &expanded.n,
                              target.width / at_scale, target.height / at_scale, at_scale)) {
    expanded.whole = 1;
  }
  MTLScissorRect parts[MB_PARTS];
  int n = take_parts(&expanded, (int)target.width, (int)target.height, kept, parts);
  int missing = 0;
  if (n < 0) {
    encode(cb, target, mpipe, buf, count, r, g, b, 1, NULL, ad_dt, ad_serial);
    missing = encode_missing;
  }
  for (int i = 0; i < n; i++) {
    encode(cb, target, mpipe, buf, count, r, g, b, 1, &parts[i], ad_dt, ad_serial);
    missing |= encode_missing;
  }
  if (n != 0) pictures_missing = missing;
  return n;
}

// The frame as floats as it comes (mb_groups_floats: a window's; as it is: one drawn offscreen), before it is the
// window's last (upload): each frame made here once, compared, put in the ring and kept from here.
static float *next_inst;
static size_t next_cap;

// Room in next_inst for `count` instances.
static float *next_room(int32_t count) {
  size_t need = (size_t)(count > 0 ? count : 1) * MB_INST * sizeof(float);
  if (next_cap < need) {
    next_inst = realloc(next_inst, need);
    next_cap = need;
  }
  return next_inst;
}

// Instances `inst` into next_inst as they are (no header of a group among them drawn out: a frame drawn offscreen, as
// it always was).
static void next_as_is(const double *inst, int32_t count) {
  float *f = next_room(count);
  size_t n = (size_t)(count > 0 ? count : 0) * MB_INST;
  for (size_t i = 0; i < n; i++) f[i] = (float)inst[i];
}

// The frame in next_inst (`count` instances) kept as the window's last (for snapshots, and what the next is compared
// with): next_inst and the last swapped, nothing copied.
static void keep_last(int32_t count, double r, double g, double b) {
  float *was = last_inst;
  size_t was_cap = last_cap;
  last_inst = next_inst;
  last_cap = next_cap;
  next_inst = was;
  next_cap = was ? was_cap : 0;
  last_count = count;
  last_bg[0] = r; last_bg[1] = g; last_bg[2] = b;
}

// Instances `f` (`count` of them) into the next buffer of the ring, and the frame's ground after them: a rect over all
// the frame (at_w × at_h), opaque, a part drawn again first.
static id<MTLBuffer> ring_put(const float *f, int32_t count, double r, double g, double b, float ga) {
  buf_i = (buf_i + 1) % 3;
  size_t need = (size_t)(count > 0 ? count : 1) * MB_INST * sizeof(float);
  size_t room = need + MB_INST * sizeof(float);
  if (buf_cap[buf_i] < room) {
    size_t cap = room * 2;
    bufs[buf_i] = [dev newBufferWithLength:cap options:MTLResourceStorageModeShared];
    buf_cap[buf_i] = cap;
  }
  float *d = (float *)bufs[buf_i].contents;
  int32_t n = count * MB_INST;
  if (n > 0) memcpy(d, f, (size_t)n * sizeof(float));
  float *gr = d + n;
  memset(gr, 0, MB_INST * sizeof(float));
  gr[0] = -16.0f; gr[1] = -16.0f; gr[2] = (float)at_w + 32.0f; gr[3] = (float)at_h + 32.0f;
  gr[8] = (float)r * ga; gr[9] = (float)g * ga; gr[10] = (float)b * ga; gr[11] = ga;
  gr[16] = -1e9f; gr[17] = -1e9f; gr[18] = 1e9f; gr[19] = 1e9f;
  return bufs[buf_i];
}

// Whether a frame is the one last handed to be shown to the last number: then it draws nothing and takes no drawable
// (the app builds a frame for what it waits on, as a clock's next second, that may show nothing new). Not one a layer
// presents with its transaction (a resize's: mac_app.m drawWithWindow), which the window's change waits for.
static int same_as_shown(const float *f, int32_t count, double r, double g, double b) {
  if (atomic_load(&rw->ad_settling)) return 0;
  if (!shown_ok || shown_streams || mlayer.presentsWithTransaction || count != last_count) return 0;
  if (vw != shown_w || vh != shown_h || vscale != shown_scale || r != last_bg[0] || g != last_bg[1] || b != last_bg[2]) return 0;
  size_t n = (size_t)(count > 0 ? count : 0) * MB_INST;
  return n == 0 || (last_inst && memcmp(f, last_inst, n * sizeof(float)) == 0);
}

// The canvas window `w`'s frames are drawn into (partial redraw, above), tw × th pixels: made again at another size,
// what it held lost.
static void ensure_canvas(MBRenderWin *w, int tw, int th) {
  if (w->cv && (int)w->cv.width == tw && (int)w->cv.height == th) return;
  w->cv = nil;
  w->cv_ok = 0;
  if (tw <= 0 || th <= 0) return;
  MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                width:(NSUInteger)tw
                                                                               height:(NSUInteger)th
                                                                            mipmapped:NO];
  td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModePrivate;
  w->cv = [dev newTextureWithDescriptor:td];
}

// MBUI_PARTIAL_CHECK=1 (a device's run, by hand: slow): each frame drawn in part compared with itself drawn whole,
// a line on the log where they differ, once its commands (`cb`, committed) are done. Its frames are drawn on the main
// thread (mb_render_present), the window's last frame then the one just drawn.
static int partial_check = -1, partial_frames;

static int partial_checking(void) {
  if (partial_check < 0) {
    const char *v = getenv("MBUI_PARTIAL_CHECK");
    partial_check = v && v[0] == '1';
  }
  return partial_check;
}

static uint8_t *read_texture(id<MTLTexture> src, int *pw, int *ph);
static uint8_t *read_last(int *pw, int *ph);

static void check_part(id<MTLCommandBuffer> cb, int parts, MBRenderWin *win) {
  if (!partial_checking()) return;
  [cb waitUntilCompleted];
  int w, h, fw, fh;
  uint8_t *drawn = read_texture(win->cv, &w, &h);
  uint8_t *whole = read_last(&fw, &fh);
  long apart = 0;
  int most = 0, ax = -1, ay = -1;
  if (fw == w && fh == h) {
    for (long i = 0; i < (long)w * h * 4; i++) {
      int d = abs((int)drawn[i] - (int)whole[i]);
      if (d) {
        if (!apart) { ax = (int)(i / 4 % w); ay = (int)(i / 4 / w); }
        apart++;
        if (d > most) most = d;
      }
    }
  }
  partial_frames++;
  if (apart || fw != w || fh != h || partial_frames % 120 == 1)
    fprintf(stderr, "PARTIAL check %d: %d parts of %dx%d: %ld bytes apart (at most %d, first at %d,%d)\n",
            partial_frames, parts, w, h, apart, most, ax, ay);
  free(drawn);
  free(whole);
}

// MBUI_PROBE=1 (src/ui/app.mbt's probe): where a present's time goes, said every 240 of them: waiting for a buffer of
// the ring (the GPU three frames behind), the instances into it, waiting for a drawable (the display's), and the
// drawing encoded and committed (ms, on average), on the queue that presents (present_q) or the main thread. On the
// log, and into the performance record while it is kept (ios_app.m's mb_perf_note, any thread: none on a Mac, these
// defaults).
static int present_probing = -1, present_n;
static double present_ms[4];

__attribute__((weak)) int mb_perf_on(void) { return 0; }
__attribute__((weak)) void mb_perf_note(const char *line) { (void)line; }

// A latency run's frames (mac_latency.h, a run measuring input to the screen: none elsewhere, these defaults): one
// made on the main thread (its number, -1 none: the events handled before it are its), one that took the place of
// another before it was drawn (the other's events are its), and its drawable about to be presented, on whatever thread
// draws it: when the frame asked for a buffer of the ring (p0), had one (p1), asked for the drawable (p2) and had it
// (p3), CACurrentMediaTime's seconds.
__attribute__((weak)) int mb_latency_built(void) { return -1; }
__attribute__((weak)) void mb_latency_merged(int from, int to) { (void)from, (void)to; }
__attribute__((weak)) void mb_latency_presenting(id<CAMetalDrawable> d, int k, double p0, double p1, double p2,
                                                 double p3) {
  (void)d, (void)k, (void)p0, (void)p1, (void)p2, (void)p3;
}

static void present_probe(double ring, double upload_s, double drawable, double encode_s) {
  if (present_probing < 0) {
    const char *v = getenv("MBUI_PROBE");
    present_probing = v && v[0] == '1';
  }
  if (!present_probing && !mb_perf_on()) return;
  present_ms[0] += ring * 1000.0;
  present_ms[1] += upload_s * 1000.0;
  present_ms[2] += drawable * 1000.0;
  present_ms[3] += encode_s * 1000.0;
  if (++present_n < 240) return;
  char line[160];
  snprintf(line, sizeof line, "present avg: ring wait %.2f, upload %.2f, drawable wait %.2f, encode %.2f ms",
           present_ms[0] / present_n, present_ms[1] / present_n, present_ms[2] / present_n, present_ms[3] / present_n);
  if (present_probing) fprintf(stderr, "PROBE %s\n", line);
  mb_perf_note(line);
  present_n = 0;
  memset(present_ms, 0, sizeof present_ms);
}

// the texture the last frame is drawn again into for the CPU to read (read_last), kept while its size is
static id<MTLTexture> read_tex;

// What drawing holds, given back as the window stops being shown (mac_app.m tell_state: the window covered,
// minimized, hidden or the Mac asleep): Core Animation keeps the layer's drawables (three or four, each
// the window's size in pixels: 22 MB at 3024 × 1890), and the glass its levels. The drawables shrink to a pixel, which
// frees all but the one shown (Core Animation shows it on), and the levels go; a frame after makes what it needs again
// at full size (mb_render_set_size). (The driver's own memory, 30 to 100 MB of the footprint's "owned unmapped
// (graphics)", it gives back itself a second or two after the last frame: same_as_shown and the caret at rest are what
// stop the frames.)
void mb_render_trim(void) {
  if (!mlayer || CGSizeEqualToSize(mlayer.drawableSize, CGSizeMake(1, 1))) return;
  // (what the queue holds drawn first, and nothing of the GPU's still in flight uses what goes)
  present_drain();
  for (int i = 0; i < 3; i++) dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
  for (int i = 0; i < 3; i++) dispatch_semaphore_signal(inflight);
  glass_free();
  glass_idle_frames = 0;
  // (and the read back's texture, the size of the last picture read: made again by the next read)
  read_tex = nil;
  // (and the canvas: the next frame drawn whole into one made again, not skipped as the same as before)
  canvas = nil;
  canvas_ok = 0;
  shown_ok = 0;
  dirty_told = dirty_whole = dirty_n = 0;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  mlayer.drawableSize = CGSizeMake(1, 1);
  [CATransaction commit];
}

// Copies `src` into `dst` (a drawable's texture) with a render pass so `mlayer.framebufferOnly` stays YES.
static void copy_to_drawable(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst) {
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = dst;
  rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
  [e setRenderPipelineState:cpipe];
  [e setViewport:(MTLViewport){0, 0, (double)dst.width, (double)dst.height, 0, 1}];
  [e setFragmentTexture:src atIndex:0];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [e endEncoding];
}

// How many frames were drawn and presented (scripts/apple_render_test.m: frames that took another's place are not).
static int present_drawn;
// How many frames handed to present_q were never drawn, each's place taken by a newer one as it waited
// (@hostapi.frames_dropped; the main thread's).
static int32_t present_dropped;

// Frame `fr` drawn for window `w` and presented: on present_q, or on the main thread once the queue is drained (`tx`:
// presented with the main thread's Core Animation transaction, once its commands are scheduled, as Core Animation
// asks; `check`: MBUI_PARTIAL_CHECK's comparison after). Returns whether it had a drawable.
static int draw_frame(MBRenderWin *w, MBFrame *fr, int tx, int check) {
  double p0 = CACurrentMediaTime();
  dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
  double p1 = CACurrentMediaTime();
  atlas_flush(fr->atlas_seq);
  at_w = fr->w;
  at_h = fr->h;
  at_scale = fr->scale;
  at_texs = fr->texs;
  at_through_n = fr->through_n;
  at_win = w;
  memcpy(at_through_tid, fr->through_tid, sizeof at_through_tid);
  double ad_dt = ad_next_dt(w);
  uint64_t ad_serial = ++w->ad_frame_serial;
  int32_t count = fr->count;
  double r = fr->bg[0], g = fr->bg[1], b = fr->bg[2];
  double ga = fr->clear ? 0 : 1;
  if (fr->clear) r = g = b = 0;
  id<MTLBuffer> buf = ring_put(fr->inst, count, r, g, b, (float)ga);
  double e0 = CACurrentMediaTime();
  // A frame drawn whole (nothing said of its parts, all of it said, a picture missing from the last) with no glass in
  // it is drawn straight into the drawable once it is had: the canvas would only be drawn whole and copied again, a
  // pass reading and writing every pixel twice more (30-45 MB a frame of a Retina window, as it scrolls), and the
  // drawable, framebufferOnly, is all such a frame needs. The canvas then holds no frame: the next drawn in part is
  // drawn whole into it first (canvas_ok). Glass copies what is under it out of what it is drawn into (blur), which a
  // drawable that is only a framebuffer cannot be read for: a frame with glass goes through the canvas, as do frames
  // drawn in part.
  int tw = fr->tw, th = fr->th;
  int direct = !fr->glass && tw > 0 && th > 0 && (fr->clear || !fr->parts.told || fr->parts.whole || pictures_missing);
  // drawn into the canvas and committed before asking for the drawable, so the GPU draws the canvas while nextDrawable
  // waits on the swapchain and the drawable is held only for the final copy; a drawable of another size than the
  // canvas (a resize between) redraws the canvas at that size before copying
  int checked = 0;
  id<MTLCommandBuffer> cb = nil;
  if (direct) {
    w->cv_ok = 0;
  } else if (tw > 0 && th > 0) {
    ensure_canvas(w, tw, th);
    cb = [queue commandBuffer];
    checked = draw_parts(cb, w->cv, buf, count, r, g, b, w->cv_ok, &fr->parts, ad_dt, ad_serial);
    w->cv_ok = 1;
    if (checked != 0) [cb commit];
  }
  double p2 = CACurrentMediaTime();
  id<CAMetalDrawable> drawable = [w->layer nextDrawable];
  double p3 = CACurrentMediaTime();
  id<MTLCommandBuffer> pcb = [queue commandBuffer];
  if (drawable && direct) {
    encode(pcb, drawable.texture, mpipe, buf, count, r, g, b, ga, NULL, ad_dt, ad_serial);
    pictures_missing = encode_missing;
  } else if (drawable) {
    int dw = (int)drawable.texture.width, dh = (int)drawable.texture.height;
    if (dw != tw || dh != th) {
      ensure_canvas(w, dw, dh);
      encode(pcb, w->cv, mpipe, buf, count, r, g, b, 1, NULL, ad_dt, ad_serial);
      w->cv_ok = 0;
      checked = 0;
    }
    copy_to_drawable(pcb, w->cv, drawable.texture);
  }
  ad_finalize_frame(w, direct || checked < 0, ad_serial);
  at_win = NULL;
  __block dispatch_semaphore_t sem = inflight;
  id<MTLCommandBuffer> ccb = checked != 0 ? cb : nil;
  [pcb addCompletedHandler:^(id<MTLCommandBuffer> done) {
    double g0 = ccb && ccb.GPUEndTime > ccb.GPUStartTime ? ccb.GPUEndTime - ccb.GPUStartTime : 0;
    double g1 = done.GPUEndTime > done.GPUStartTime ? done.GPUEndTime - done.GPUStartTime : 0;
    last_gpu = (g0 + g1) * 1000.0;
    dispatch_semaphore_signal(sem);
  }];
  if (drawable) mb_latency_presenting(drawable, fr->lat, p0, p1, p2, p3);
  if (drawable && tx) {
    [pcb commit];
    [pcb waitUntilScheduled];
    [drawable present];
  } else {
    if (drawable) [pcb presentDrawable:drawable];
    [pcb commit];
  }
  at_texs = nil;
  at_through_n = 0;
  glass_idle_tick(fr->glass);
  present_drawn++;
  present_probe(p1 - p0, e0 - p1, p3 - p2, (p2 - e0) + (CACurrentMediaTime() - p3));
  if (check && checked > 0) check_part(cb, checked, w);
  return drawable != nil;
}

// The frame waiting for window `w` drawn (present_q): the newest the main thread handed over.
static void present_job(MBRenderWin *w) {
  @autoreleasepool {
    os_unfair_lock_lock(&present_mu);
    MBFrame *fr = w->pending;
    w->pending = nil;
    w->queued = 0;
    if (fr && w->layer) atomic_store(&w->ad_drawing, 1);
    os_unfair_lock_unlock(&present_mu);
    // (none had: the window let go of first)
    if (!fr || !w->layer) return;
    if (!draw_frame(w, fr, 0, 0)) atomic_store(&w->missed, 1);
    atomic_store(&w->ad_drawing, 0);
  }
}

void mb_render_present(const double *inst, int32_t count, double r, double g, double b) {
  // a frame that put a native view elsewhere (apple_views.m: a page in a message, as the list scrolls) presented in
  // the transaction that puts it there (the frame's: mac_app.m drawFrame, ios_app.m tick), so the view and the hole it
  // shows through change on the same refresh. Presented by itself the drawable is shown once the GPU has drawn it,
  // apart from the view's move, which the frame's transaction takes to the screen: a refresh or two apart, the page
  // out of step with its hole as the list moved
  // (the views' frost of glass over them put where it shows now, with them: glass_through)
  // the frame as floats, its groups' headers drawn out (groups.h): made once, compared, handed over, kept
  float *f = next_room(count);
  mb_groups_floats(f, inst, count);
  int has_hole = glass_through(f, count);
  BOOL want_opaque = !has_hole && !rw->clear;
  int opaque_changed = mlayer && mlayer.opaque != want_opaque;
  int with_views = (mb_views_take_moved() || opaque_changed) && !mlayer.presentsWithTransaction;
  // (the last frame handed over had no drawable: not the one shown)
  if (atomic_exchange(&rw->missed, 0)) shown_ok = 0;
  if (!opaque_changed && same_as_shown(f, count, r, g, b)) {
    // (what was said of it, nothing changed, not left for the next)
    dirty_told = dirty_whole = dirty_n = 0;
    through_n = 0;
    mb_text_flush_ahead();
    // (pages given back with no frame drawn after: done on the queue, so their memory goes now)
    atlas_ops_kick();
    return;
  }
  MBFrame *fr = [MBFrame new];
  fr->count = count;
  fr->bg[0] = r; fr->bg[1] = g; fr->bg[2] = b;
  fr->w = vw; fr->h = vh; fr->scale = vscale;
  fr->clear = rw->clear;
  fr->tw = (int)mlayer.drawableSize.width;
  fr->th = (int)mlayer.drawableSize.height;
  // (whether it has glass, which copies what is drawn under it out of what it is drawn into: slot 20 negative)
  int has_ad = 0;
  for (int32_t i = 0; i < count; i++) {
    const float *b = f + (size_t)i * MB_INST;
    if (b[20] < 0.0f) fr->glass = 1;
    if (!has_ad && (int)(b[5] + 0.5f) == 6 && b[14] < -0.5f && !(b[7] <= 0.1f && b[11] < 0.12f)) has_ad = 1;
  }
  if (has_ad) atomic_store(&rw->ad_has_adaptive, 1);
  if (atomic_load(&rw->ad_settling) && rw->said.told && !rw->said.whole) {
    os_unfair_lock_lock(&present_mu);
    MBDirty sd = rw->ad_settle_dirty;
    os_unfair_lock_unlock(&present_mu);
    dirty_join(&rw->said, &sd);
  }
  fr->parts = rw->said;
  rw->said = (MBDirty){0};
  fr->through_n = through_n;
  memcpy(fr->through_tid, through_tid, sizeof through_tid);
  rw->last_through_n = through_n;
  memcpy(rw->last_through_tid, through_tid, sizeof through_tid);
  fr->atlas_seq = atlas_seq;
  fr->lat = mb_latency_built();
  through_n = 0;
  int streams = 0;
  for (int32_t i = 0; i < count && !streams; i++) {
    float t = f[i * MB_INST + 20];
    streams = t > (float)MB_STREAM_TEX && t <= (float)MB_LAYER_TEX;
  }
  if (mlayer.presentsWithTransaction || with_views || partial_checking()) {
    // drawn here, in the main thread's transaction, once the queue has drawn what it holds
    present_drain();
    if (opaque_changed) mlayer.opaque = want_opaque;
    if (with_views) mlayer.presentsWithTransaction = YES;
    fr->inst = f;
    int had = draw_frame(rw, fr, mlayer.presentsWithTransaction, 1);
    fr->inst = NULL;
    if (with_views) mlayer.presentsWithTransaction = NO;
    keep_last(count, r, g, b);
    shown_ok = had;
  } else {
    size_t n = (size_t)(count > 0 ? count : 0) * MB_INST * sizeof(float);
    fr->inst = pool_take(n > 0 ? n : sizeof(float), &fr->cap);
    if (n > 0) memcpy(fr->inst, f, n);
    fr->texs = frame_texs(f, count);
    keep_last(count, r, g, b);
    shown_ok = 1;
    MBRenderWin *w = rw;
    MBFrame *was;
    int merged = -1, lat = fr->lat;
    os_unfair_lock_lock(&present_mu);
    was = w->pending;
    if (was) {
      // (it takes the place of one not drawn yet: what that said differs is still to be drawn, the events that one
      // showed this one shows)
      dirty_join(&fr->parts, &was->parts);
      if (fr->glass) fr->parts.glass_closure = 1;
      if (was->tw != fr->tw || was->th != fr->th || was->scale != fr->scale) fr->parts.whole = fr->parts.told = 1;
      if (was->lat >= 0 && lat >= 0) merged = was->lat;
      else if (was->lat >= 0) fr->lat = was->lat;
    }
    w->pending = fr;
    if (was) present_dropped++;
    int go = !w->queued;
    w->queued = 1;
    os_unfair_lock_unlock(&present_mu);
    if (merged >= 0) mb_latency_merged(merged, lat);
    if (go) dispatch_async(present_q, ^{
      present_job(w);
    });
  }
  shown_w = vw;
  shown_h = vh;
  shown_scale = vscale;
  shown_streams = streams;
  mb_text_flush_ahead();
}

void mb_render_set_fixed_dt(double dt) {
  present_drain();
  ad_fixed_dt = dt > 0.0 ? dt : 0.0;
}

void mb_render_adaptive_reset(void) {
  present_drain();
  ad_forget(rw);
}

int mb_render_adaptive_settling(void) {
  return rw ? atomic_load(&rw->ad_settling) : 0;
}

int mb_render_adaptive_settling_of(int wid) {
  if (wid < 0 || wid >= MB_WINS) return 0;
  MBRenderWin *w = &render_wins[wid];
  if (atomic_load(&w->ad_settling)) return 1;
  if (atomic_load(&w->ad_has_adaptive)) {
    os_unfair_lock_lock(&present_mu);
    int q = w->queued;
    os_unfair_lock_unlock(&present_mu);
    if (q || atomic_load(&w->ad_drawing)) return 1;
  }
  return 0;
}

int mb_render_adaptive_step(int wid) {
  if (wid < 0 || wid >= MB_WINS) return 0;
  MBRenderWin *w = &render_wins[wid];
  if (!w->inst || !w->layer) return 0;
  if (!atomic_load(&w->ad_settling)) {
    if (atomic_load(&w->ad_has_adaptive)) {
      os_unfair_lock_lock(&present_mu);
      int q = w->queued;
      os_unfair_lock_unlock(&present_mu);
      if (q || atomic_load(&w->ad_drawing)) return 1;
    }
    return 0;
  }
  os_unfair_lock_lock(&present_mu);
  MBDirty sd = w->ad_settle_dirty;
  os_unfair_lock_unlock(&present_mu);
  if (!sd.told) return 0;
  MBFrame *fr = [MBFrame new];
  fr->count = w->count;
  fr->bg[0] = w->bg[0]; fr->bg[1] = w->bg[1]; fr->bg[2] = w->bg[2];
  fr->w = w->w; fr->h = w->h; fr->scale = w->scale;
  fr->tw = (int)w->layer.drawableSize.width;
  fr->th = (int)w->layer.drawableSize.height;
  fr->glass = 1;
  fr->parts = sd;
  fr->through_n = w->last_through_n;
  memcpy(fr->through_tid, w->last_through_tid, sizeof fr->through_tid);
  fr->atlas_seq = atlas_seq;
  fr->lat = -1;
  size_t n = (size_t)(w->count > 0 ? w->count : 0) * MB_INST * sizeof(float);
  fr->inst = pool_take(n > 0 ? n : sizeof(float), &fr->cap);
  if (n > 0) memcpy(fr->inst, w->inst, n);
  fr->texs = frame_texs(w->inst, w->count);
  os_unfair_lock_lock(&present_mu);
  MBFrame *was = w->pending;
  if (was) dirty_join(&fr->parts, &was->parts);
  w->pending = fr;
  int go = !w->queued;
  w->queued = 1;
  os_unfair_lock_unlock(&present_mu);
  if (go) dispatch_async(present_q, ^{
    present_job(w);
  });
  return 1;
}

// MBUI_SAFE's bands (mac_app.m), in points, and their ground
static double band_top, band_bottom;
static uint32_t band_ground;

void mb_render_set_bands(double top, double bottom, uint32_t ground) {
  band_top = top;
  band_bottom = bottom;
  band_ground = ground;
}

// Instances `f` (`count` of them, as a frame's floats are) drawn on the ground `bg` into a texture the CPU reads, at
// `sw` × `sh` points, `scale` pixels a point, and its pixels: BGRA, premultiplied, rows from the top, *pw × *ph; the
// caller frees them. On the main thread, what present_q holds drawn first (its state is then the main thread's), every
// glyph kept put in, no native view's frost through it, the pictures' textures as they are now.
// ad_mode: 1 = update adaptive state (film/read), 0 = read-only (snapshot/drawn_pixels), -1 = stateless (pixels_of)
static uint8_t *read_of(const float *f, int32_t count, const double bg[3], double alpha, double sw, double sh,
                        double scale, int ad_mode, int *pw, int *ph) {
  present_drain();
  atlas_flush(UINT64_MAX);
  at_w = sw;
  at_h = sh;
  at_scale = scale;
  at_texs = nil;
  at_through_n = 0;
  at_win = ad_mode < 0 ? NULL : rw;
  double ad_dt = ad_mode > 0 ? ad_next_dt(at_win) : -1.0;
  uint64_t ad_serial = (ad_mode > 0 && at_win) ? ++at_win->ad_frame_serial : 0;
  int w = (int)round(sw * scale), h = (int)round(sh * scale);
  if (!read_tex || (int)read_tex.width != w || (int)read_tex.height != h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
#if TARGET_OS_OSX
    // (an Apple GPU: the CPU reads what it drew where it drew it, no copy to sync; an Intel Mac's integrated GPU
    // shares memory too but keeps no texture shared, so it stays managed)
    td.storageMode = [dev supportsFamily:MTLGPUFamilyApple1] ? MTLStorageModeShared : MTLStorageModeManaged;
#else
    td.storageMode = MTLStorageModeShared;
#endif
    read_tex = [dev newTextureWithDescriptor:td];
  }
  id<MTLTexture> tex = read_tex;
  id<MTLBuffer> buf = [dev newBufferWithBytes:f length:(count > 0 ? count : 1) * MB_INST * sizeof(float) options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> cb = [queue commandBuffer];
  encode(cb, tex, rpipe_off, buf, count, bg[0], bg[1], bg[2], alpha, NULL, ad_dt, ad_serial);
  if (ad_mode > 0 && at_win) ad_finalize_frame(at_win, 1, ad_serial);
  at_win = NULL;
#if TARGET_OS_OSX
  if (tex.storageMode == MTLStorageModeManaged) {
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl synchronizeResource:tex];
    [bl endEncoding];
  }
#endif
  [cb commit];
  [cb waitUntilCompleted];
  size_t bpr = (size_t)w * 4;
  uint8_t *px = malloc(bpr * h);
  [tex getBytes:px bytesPerRow:bpr fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
  *pw = w;
  *ph = h;
  return px;
}

// What a texture the GPU keeps (a canvas: private) holds, once what is drawn into it is done, read back as read_of
// reads: the caller frees it.
static uint8_t *read_texture(id<MTLTexture> src, int *pw, int *ph) {
  present_drain();
  int w = (int)src.width, h = (int)src.height;
  uint8_t *px = malloc((size_t)w * h * 4);
  @autoreleasepool {
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
#if TARGET_OS_OSX
    td.storageMode = MTLStorageModeManaged;
#else
    td.storageMode = MTLStorageModeShared;
#endif
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(w, h, 1) toTexture:tex destinationSlice:0 destinationLevel:0
      destinationOrigin:MTLOriginMake(0, 0, 0)];
#if TARGET_OS_OSX
    [bl synchronizeResource:tex];
#endif
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    [tex getBytes:px bytesPerRow:(size_t)w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
  }
  *pw = w;
  *ph = h;
  return px;
}

// The window's last frame (keep_last's) drawn again at the size set (mb_render_set_size), read back as read_of reads.
static uint8_t *read_last(int *pw, int *ph) {
  return read_of(last_inst, last_count, last_bg, 1, vw, vh, vscale, 0, pw, ph);
}

// A frame drawn offscreen and read back (a film's, mbui/src/film): `inst` as mb_render_present takes them, kept as
// the last frame, and its pixels as read_of (updating adaptive state) gives them.
uint8_t *mb_render_read(const double *inst, int32_t count, double r, double g, double b, int *pw, int *ph) {
  @autoreleasepool {
    present_drain();
    next_as_is(inst, count);
    keep_last(count, r, g, b);
    return read_of(last_inst, last_count, last_bg, 1, vw, vh, vscale, 1, pw, ph);
  }
}

// A picture's frame (`inst` as mb_render_present takes them, its groups' headers drawn out) kept as the last frame and
// drawn offscreen as mb_render_snapshot (or, `transparent`, mb_render_snapshot_transparent) draws the last frame: its
// pixels as read_of gives them (the caller frees them). What a present would hand to be shown is not made: offscreen
// there is no layer to show it, and the snapshot draws the frame again anyway.
uint8_t *mb_render_read_snapshot_inst(const double *inst, int32_t count, double r, double g, double b, int transparent,
                                      int *pw, int *ph) {
  @autoreleasepool {
    present_drain();
    float *f = next_room(count);
    mb_groups_floats(f, inst, count);
    keep_last(count, r, g, b);
    static const double clear[3] = {0, 0, 0};
    return read_of(last_inst, last_count, transparent ? clear : last_bg, transparent ? 0 : 1, vw, vh, vscale, 0, pw,
                   ph);
  }
}

uint8_t *mb_render_read_step(int *pw, int *ph) {
  @autoreleasepool {
    if (!last_inst) return NULL;
    return read_of(last_inst, last_count, last_bg, 1, vw, vh, vscale, 1, pw, ph);
  }
}

// The frame presented last drawn again (read_last), and its pixels in the rectangle x, y, w × h (from its top left)
// into `out` as RGBA, premultiplied, rows from the top (host_native.mbt drawn_pixels): 1 when read, 0 where the
// rectangle is not all in the frame or none was presented.
int32_t mb_render_drawn_pixels(int32_t x, int32_t y, int32_t w, int32_t h, moonbit_bytes_t out) {
  if (!dev || !last_inst || w <= 0 || h <= 0 || x < 0 || y < 0) return 0;
  if ((size_t)Moonbit_array_length(out) < (size_t)w * h * 4) return 0;
  @autoreleasepool {
    int fw, fh;
    uint8_t *px = read_last(&fw, &fh);
    if (x + w > fw || y + h > fh) {
      free(px);
      return 0;
    }
    for (int r = 0; r < h; r++) {
      const uint8_t *s = px + ((size_t)(y + r) * fw + x) * 4;
      uint8_t *d = out + (size_t)r * w * 4;
      for (int k = 0; k < w; k++, s += 4, d += 4) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
      }
    }
    free(px);
  }
  return 1;
}

// Instances never presented (a shot's, mbui/src/ui/shot.mbt) drawn as a frame is, but `scale` pixels a point into a
// picture of their own w × h (their 0, 0 its top left), on the ground r, g, b, and its pixels into `out` as
// mb_render_drawn_pixels gives the frame's (the last frame kept as it was, the size frames are drawn at put back):
// 1 when read.
int32_t mb_render_pixels_of(const double *inst, int32_t count, double r, double g, double b, double scale, int32_t w,
                            int32_t h, moonbit_bytes_t out) {
  if (!dev || count < 0 || !(scale > 0) || w <= 0 || h <= 0) return 0;
  if ((size_t)Moonbit_array_length(out) < (size_t)w * h * 4) return 0;
  int ok = 0;
  @autoreleasepool {
    size_t n = (size_t)count * MB_INST;
    float *f = malloc((n > 0 ? n : MB_INST) * sizeof(float));
    for (size_t i = 0; i < n; i++) f[i] = (float)inst[i];
    double bg[3] = {r, g, b};
    // (drawn as a frame of that size is)
    int fw, fh;
    uint8_t *px = read_of(f, count, bg, 1, w / scale, h / scale, scale, -1, &fw, &fh);
    free(f);
    if (fw == w && fh == h) {
      for (int k = 0; k < h; k++) {
        const uint8_t *s = px + (size_t)k * fw * 4;
        uint8_t *d = out + (size_t)k * w * 4;
        for (int j = 0; j < w; j++, s += 4, d += 4) {
          d[0] = s[2];
          d[1] = s[1];
          d[2] = s[0];
          d[3] = s[3];
        }
      }
      ok = 1;
    }
    free(px);
  }
  return ok;
}

static void png_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static int png_chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
  uint8_t head[8];
  png_be32(head, (uint32_t)len);
  memcpy(head + 4, type, 4);
  uLong crc = crc32(0L, (const Bytef *)type, 4);
  if (len) crc = crc32(crc, data, (uInt)len);
  uint8_t tail[4];
  png_be32(tail, (uint32_t)crc);
  return fwrite(head, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4;
}

// Pixels `px` (BGRA, premultiplied, w × h, rows from the top: read_of's) written to `path` as an 8-bit RGBA PNG whose
// pixels are those ImageIO writes of them (snapshot_of before: straight alpha, each colour (c * 255 + a / 2) / a, at most
// 255; 0 where a is 0; its sRGB and Exif size chunks too). Each row filtered by Sub and the rows deflated in strips at
// once (zlib, level 4), each strip ending on a byte (a sync flush) so the strips join into one stream; the strips
// fixed by the size alone, the file is the same for the same pixels. 1, or 0 when it could not be written.
int mb_png_write_bgra_fast(const uint8_t *px, int w, int h, const char *path) {
  if (!px || w <= 0 || h <= 0 || !path) return 0;
  size_t st = (size_t)w * 4, raw_row = st + 1;
  int n = h / 48;
  if (n < 1) n = 1;
  if (n > 64) n = 64;
  uint8_t **outs = calloc((size_t)n, sizeof *outs);
  size_t *lens = calloc((size_t)n, sizeof *lens), *raws = calloc((size_t)n, sizeof *raws);
  uLong *adlers = calloc((size_t)n, sizeof *adlers);
  atomic_int failed_box = 0, *failed = &failed_box;
  if (!outs || !lens || !raws || !adlers) {
    free(outs);
    free(lens);
    free(raws);
    free(adlers);
    return 0;
  }
  dispatch_apply((size_t)n, DISPATCH_APPLY_AUTO, ^(size_t k) {
    int y0 = (int)((int64_t)h * (int64_t)k / n), y1 = (int)((int64_t)h * (int64_t)(k + 1) / n);
    size_t raw = raw_row * (size_t)(y1 - y0);
    uint8_t *buf = malloc(raw), *rgba = malloc(st);
    if (!buf || !rgba) {
      free(buf);
      free(rgba);
      atomic_store(failed, 1);
      return;
    }
    for (int y = y0; y < y1; y++) {
      const uint8_t *s = px + (size_t)y * st;
      for (int x = 0; x < w; x++, s += 4) {
        uint8_t *d = rgba + (size_t)x * 4;
        unsigned a = s[3];
        if (a == 255) {
          d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
        } else if (a == 0) {
          d[0] = d[1] = d[2] = d[3] = 0;
        } else {
          unsigned half = a >> 1;
          unsigned rr = (s[2] * 255u + half) / a, gg = (s[1] * 255u + half) / a, bb = (s[0] * 255u + half) / a;
          d[0] = (uint8_t)(rr > 255 ? 255 : rr);
          d[1] = (uint8_t)(gg > 255 ? 255 : gg);
          d[2] = (uint8_t)(bb > 255 ? 255 : bb);
          d[3] = (uint8_t)a;
        }
      }
      uint8_t *r = buf + raw_row * (size_t)(y - y0);
      r[0] = 1;
      memcpy(r + 1, rgba, 4);
      for (size_t i = 4; i < st; i++) r[i + 1] = (uint8_t)(rgba[i] - rgba[i - 4]);
    }
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (deflateInit2(&zs, 4, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
      free(buf);
      free(rgba);
      atomic_store(failed, 1);
      return;
    }
    size_t cap = deflateBound(&zs, (uLong)raw) + 16;
    uint8_t *o = malloc(cap);
    int rc = Z_STREAM_ERROR;
    if (o) {
      zs.next_in = buf;
      zs.avail_in = (uInt)raw;
      zs.next_out = o;
      zs.avail_out = (uInt)cap;
      rc = deflate(&zs, k + 1 == (size_t)n ? Z_FINISH : Z_SYNC_FLUSH);
    }
    if (!o || (k + 1 == (size_t)n ? rc != Z_STREAM_END : rc != Z_OK) || zs.avail_in != 0 || zs.avail_out == 0) {
      free(o);
      atomic_store(failed, 1);
    } else {
      outs[k] = o;
      lens[k] = zs.total_out;
      raws[k] = raw;
      adlers[k] = adler32(1L, buf, (uInt)raw);
    }
    deflateEnd(&zs);
    free(buf);
    free(rgba);
  });
  int ok = !atomic_load(failed);
  FILE *f = ok ? fopen(path, "wb") : NULL;
  ok = f != NULL;
  if (ok) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13];
    png_be32(ihdr, (uint32_t)w);
    png_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // RGBA
    ihdr[10] = 0;  // deflate
    ihdr[11] = 0;  // adaptive filtering
    ihdr[12] = 0;  // not interlaced
    static const uint8_t srgb[1] = {0};  // perceptual, as ImageIO writes a device RGB image
    // (ImageIO's Exif: big-endian TIFF, an Exif IFD with PixelXDimension and PixelYDimension)
    uint8_t exif[56] = {'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1, 0x87, 0x69, 0, 4, 0, 0, 0, 1, 0, 0, 0, 26, 0, 0, 0, 0,
                        0, 2, 0xA0, 0x02, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0, 0xA0, 0x03, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0,
                        0, 0, 0, 0};
    png_be32(exif + 36, (uint32_t)w);
    png_be32(exif + 48, (uint32_t)h);
    static const uint8_t zhead[2] = {0x78, 0x5E};  // deflate, 32 KB window, level 4's
    uLong adler = adlers[0];
    for (int k = 1; k < n; k++) adler = adler32_combine(adler, adlers[k], (z_off_t)raws[k]);
    uint8_t ztail[4];
    png_be32(ztail, (uint32_t)adler);
    ok = fwrite(sig, 1, 8, f) == 8 && png_chunk(f, "IHDR", ihdr, 13) && png_chunk(f, "sRGB", srgb, 1) &&
         png_chunk(f, "eXIf", exif, sizeof exif) && png_chunk(f, "IDAT", zhead, 2);
    for (int k = 0; ok && k < n; k++) ok = png_chunk(f, "IDAT", outs[k], lens[k]);
    ok = ok && png_chunk(f, "IDAT", ztail, 4) && png_chunk(f, "IEND", NULL, 0);
    if (fclose(f) != 0) ok = 0;
  }
  for (int k = 0; k < n; k++) free(outs[k]);
  free(outs);
  free(lens);
  free(raws);
  free(adlers);
  return ok;
}

// Whether a snapshot is the frame's pixels alone (no native view under them, no bands round them): what
// mb_png_write_bgra_fast writes, off the main thread too.
int mb_render_snapshot_plain(void) {
  return mb_views_count() == 0 && !(band_top > 0 || band_bottom > 0);
}

// `img` written to `path` as a PNG by ImageIO: 1, or 0.
static int png_write_cgimage(CGImageRef img, const char *path) {
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
  if (!url) return 0;
  CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
  int ok = 0;
  if (dst) {
    CGImageDestinationAddImage(dst, img, NULL);
    ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
  }
  CFRelease(url);
  return ok;
}

// Pixels `px` (read_of's, w × h, freed here) into a PNG at `path`, the native views under them and the bands round
// them as the window shows them.
static int snapshot_of(uint8_t *px, int w, int h, const char *path) {
  if (mb_render_snapshot_plain()) {
    int ok = mb_png_write_bgra_fast(px, w, h, path);
    free(px);
    return ok;
  }
  size_t bpr = (size_t)w * 4;
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx = CGBitmapContextCreate(px, w, h, 8, bpr, cs,
      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  if (mb_views_count() > 0) {
    // the native views' content under the canvas, seen through its holes
    CGContextRef under = CGBitmapContextCreate(NULL, w, h, 8, bpr, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    double view_scale = mlayer && mlayer.contentsScale > 0 ? mlayer.contentsScale : vscale;
    mb_views_draw_under(under, view_scale);
    CGContextDrawImage(under, CGRectMake(0, 0, w, h), img);
    CGImageRelease(img);
    img = CGBitmapContextCreateImage(under);
    CGContextRelease(under);
  }
  if (band_top > 0 || band_bottom > 0) {
    // the window as it shows: the bands above and under the app's view in their ground
    int bt = (int)round(band_top * vscale), bb = (int)round(band_bottom * vscale);
    int fh = h + bt + bb;
    CGContextRef whole = CGBitmapContextCreate(NULL, w, fh, 8, bpr, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGContextSetRGBFillColor(whole, ((band_ground >> 16) & 0xFF) / 255.0, ((band_ground >> 8) & 0xFF) / 255.0,
                             (band_ground & 0xFF) / 255.0, 1);
    CGContextFillRect(whole, CGRectMake(0, 0, w, fh));
    // (a bitmap context's y up: the bottom band first)
    CGContextDrawImage(whole, CGRectMake(0, bb, w, h), img);
    CGImageRelease(img);
    img = CGBitmapContextCreateImage(whole);
    CGContextRelease(whole);
  }
  int ok = png_write_cgimage(img, path);
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  free(px);
  return ok;
}

// Pixels `px` (read_of's, w × h, not freed) written to `path` by ImageIO as snapshots wrote every picture before
// mb_png_write_bgra_fast (and still write one with views or bands): what the fast writer's pixels are checked against.
int mb_png_write_bgra_imageio(const uint8_t *px, int w, int h, const char *path) {
  if (!px || w <= 0 || h <= 0 || !path) return 0;
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx = CGBitmapContextCreate((void *)px, w, h, 8, (size_t)w * 4, cs,
      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
  CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
  int ok = img ? png_write_cgimage(img, path) : 0;
  if (img) CGImageRelease(img);
  if (ctx) CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return ok;
}

// The last frame drawn again as mb_render_snapshot (`transparent`: mb_render_snapshot_transparent) draws it and
// written by ImageIO (mb_png_write_bgra_imageio): the reference a test holds a snapshot's PNG to. 1, or 0.
int mb_render_snapshot_reference(const char *path, int transparent) {
  if (!dev || !last_inst) return 0;
  @autoreleasepool {
    int w, h;
    static const double clear[3] = {0, 0, 0};
    uint8_t *px = read_of(last_inst, last_count, transparent ? clear : last_bg, transparent ? 0 : 1, vw, vh, vscale, 0,
                          &w, &h);
    int ok = mb_png_write_bgra_imageio(px, w, h, path);
    free(px);
    return ok;
  }
}

// Pixels `px` (read_of's, freed here) written to `path` as a snapshot writes them.
int mb_render_snapshot_px(uint8_t *px, int w, int h, const char *path) {
  if (!px) return 0;
  return snapshot_of(px, w, h, path);
}

int mb_render_snapshot(const char *path) {
  int w, h;
  uint8_t *px = read_last(&w, &h);
  return snapshot_of(px, w, h, path);
}

// The same last frame over a transparent ground, for an image used in another composition. Clear RGB to zero with
// alpha so the readback remains premultiplied; the window and video paths continue to clear opaque.
int mb_render_snapshot_transparent(const char *path) {
  int w, h;
  const double clear[3] = {0, 0, 0};
  uint8_t *px = read_of(last_inst, last_count, clear, 0, vw, vh, vscale, 0, &w, &h);
  return snapshot_of(px, w, h, path);
}

static id<MTLTexture> off_target;

// The offscreen target as its frames left it (not the last frame drawn again: what drawing only the parts that
// differ made of it, mbui-render's MBUI_DL_FROM), as a PNG.
int mb_render_snapshot_drawn(const char *path) {
  if (!off_target) return 0;
  int w, h;
  uint8_t *px = read_texture(off_target, &w, &h);
  return snapshot_of(px, w, h, path);
}

// Without a window (the bench, src/bench): the renderer with no layer, and
// frames drawn into a texture kept across frames, the size a window's
// drawable would be, as nanoui's hill-climb draws its own (off_target, above);
// whether it holds the frame drawn last (partial redraw: draw_parts).
static int off_ok;

int mb_render_init_offscreen(double w, double h, double scale) {
  mb_render_init(nil);
  if (!dev) return 0;
  // (with every kind's pipeline: what is drawn here is what a window draws once they are made)
  dispatch_group_wait(kinds_made, DISPATCH_TIME_FOREVER);
  mb_render_set_size(w, h, scale);
  mb_text_set_scale(scale);
  NSUInteger pw = (NSUInteger)round(w * scale);
  NSUInteger ph = (NSUInteger)round(h * scale);
  if (pw == 0 || ph == 0) return 0;
  off_ok = 0;
  mb_render_adaptive_reset();
  slots_invalidate();
  if (off_target && off_target.width == pw && off_target.height == ph) {
    return 1;
  }
  MTLTextureDescriptor *td = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                   width:pw
                                  height:ph
                               mipmapped:NO];
  td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  td.storageMode = MTLStorageModePrivate;
  off_target = [dev newTextureWithDescriptor:td];
  return off_target ? 1 : 0;
}

// Draws a frame into the offscreen texture as a window's frame is drawn
// (the ring of buffers, one command buffer), waits for it and times it:
// out[0] the CPU time of uploading and encoding it, out[1] its GPU time
// (the command buffer's timestamps), in ms.
void mb_render_draw_timed(const double *inst, int32_t count, double r, double g, double b, double *out) {
  @autoreleasepool {
    present_drain();
    dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
    double t0 = CACurrentMediaTime();
    next_as_is(inst, count);
    keep_last(count, r, g, b);
    atlas_flush(UINT64_MAX);
    at_w = vw;
    at_h = vh;
    at_scale = vscale;
    at_texs = nil;
    at_through_n = 0;
    at_win = rw;
    double ad_dt = ad_next_dt(rw);
    uint64_t ad_serial = ++rw->ad_frame_serial;
    id<MTLBuffer> buf = ring_put(last_inst, count, r, g, b, 1.0f);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    // (only the parts said to differ, as a window's canvas is drawn: the target is kept across frames too)
    MBDirty said = rw->said;
    rw->said = (MBDirty){0};
    int parts_n = draw_parts(cb, off_target, buf, count, r, g, b, off_ok, &said, ad_dt, ad_serial);
    ad_finalize_frame(rw, parts_n < 0, ad_serial);
    at_win = NULL;
    off_ok = 1;
    [cb commit];
    double t1 = CACurrentMediaTime();
    [cb waitUntilCompleted];
    out[0] = (t1 - t0) * 1000.0;
    out[1] = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
    dispatch_semaphore_signal(inflight);
  }
}

// The GPU's time for the last frame presented that it finished (ms).
double mb_host_gpu_ms(void) { return last_gpu; }

int32_t mb_host_frames_dropped(void) { return present_dropped; }

int32_t mb_host_layer_supported(void) {
  dispatch_group_wait(prepared, DISPATCH_TIME_FOREVER);
  return dev != nil ? 1 : 0;
}

int32_t mb_host_layer_render(int32_t layer_id, int32_t phys_w, int32_t phys_h, double scale, const double *inst,
                             int32_t count) {
  if (layer_id <= 0 || phys_w <= 0 || phys_h <= 0 || !(scale > 0.0) || count < 0 || (count > 0 && !inst)) return 0;
  dispatch_group_wait(prepared, DISPATCH_TIME_FOREVER);
  if (!dev || !queue || !rpipe_off) return 0;
  @autoreleasepool {
    present_drain();
    atlas_flush(UINT64_MAX);
    if (!layer_texs) layer_texs = [NSMutableDictionary new];
    NSNumber *key = @(layer_id);
    id<MTLTexture> tex = layer_texs[key];
    if (!tex || (int)tex.width != phys_w || (int)tex.height != phys_h) {
      MTLTextureDescriptor *td = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                       width:(NSUInteger)phys_w
                                      height:(NSUInteger)phys_h
                                   mipmapped:NO];
      td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      td.storageMode = MTLStorageModePrivate;
      tex = [dev newTextureWithDescriptor:td];
      if (!tex) return 0;
      layer_texs[key] = tex;
    }
    double saved_w = at_w, saved_h = at_h, saved_scale = at_scale;
    NSDictionary<NSNumber *, id> *saved_texs = at_texs;
    int saved_through_n = at_through_n;
    MBRenderWin *saved_win = at_win;
    at_w = (double)phys_w / scale;
    at_h = (double)phys_h / scale;
    at_scale = scale;
    at_texs = nil;
    at_through_n = 0;
    at_win = NULL;

    size_t nfloats = (size_t)(count > 0 ? count : 1) * MB_INST;
    float *tmp = malloc(nfloats * sizeof(float));
    if (!tmp) {
      at_w = saved_w;
      at_h = saved_h;
      at_scale = saved_scale;
      at_texs = saved_texs;
      at_through_n = saved_through_n;
      at_win = saved_win;
      return 0;
    }
    if (count > 0) mb_groups_floats(tmp, inst, count);
    else memset(tmp, 0, nfloats * sizeof(float));

    id<MTLBuffer> buf = [dev newBufferWithBytes:tmp
                                         length:nfloats * sizeof(float)
                                        options:MTLResourceStorageModeShared];
    free(tmp);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    encode(cb, tex, rpipe_off, buf, count, 0.0, 0.0, 0.0, 0.0, NULL, -1.0, 0);
    [cb commit];

    at_w = saved_w;
    at_h = saved_h;
    at_scale = saved_scale;
    at_texs = saved_texs;
    at_through_n = saved_through_n;
    at_win = saved_win;
    if (rw) shown_ok = 0;
  }
  return 1;
}

void mb_host_layer_destroy(int32_t layer_id) {
  if (layer_id <= 0 || !layer_texs) return;
  present_drain();
  [layer_texs removeObjectForKey:@(layer_id)];
  if (rw) shown_ok = 0;
}

// ---- the glyph atlases' pages (atlas_pages.h) ----
int mb_atlas_pages_said;
void mb_render_set_atlas_max_pages(int n) { mb_atlas_pages_said = n > 0 ? n : 0; }

// how many pages each atlas has for what is put and drawn from now on (the main thread's: what the ops kept say)
static int atlas_pages_n[2] = {1, 1};

int32_t mb_host_atlas_pages_most(int32_t colour) {
  // (Metal's arrays take 2048 layers)
  return colour ? mb_atlas_pages_cap(MB_ATLAS_COLOR_SIDE, 4, 2048) : mb_atlas_pages_cap(MB_ATLAS_SIDE, 1, 2048);
}

int32_t mb_host_atlas_pages_now(int32_t colour) { return atlas_pages_n[colour ? 1 : 0]; }

// How many pages each atlas's texture has, once the queue has done what it holds (for tests).
int32_t mb_render_atlas_pages_held(int32_t colour) {
  dispatch_group_wait(prepared, DISPATCH_TIME_FOREVER);
  __block int32_t n = 0;
  void (^look)(void) = ^{
    id<MTLTexture> t = colour ? catlas : atlas;
    n = t ? (int32_t)t.arrayLength : 0;
  };
  if (present_q) dispatch_sync(present_q, look);
  else look();
  return n;
}

// The PAGES ops kept done on the queue though no frame is to be drawn (one given back as the app rests): what it holds
// first, as any frame's.
static void atlas_ops_kick(void) {
  os_unfair_lock_lock(&present_mu);
  int owed = atlas_ops_owed;
  uint64_t upto = atlas_seq;
  os_unfair_lock_unlock(&present_mu);
  if (owed > 0 && present_q) dispatch_async(present_q, ^{
    @autoreleasepool {
      atlas_flush(upto);
    }
  });
}

// The atlas `colour` made `n` pages for what is put and drawn after this: a texture of n made here, put in by the
// queue in order with the glyphs put before and after it (atlas_flush). Given back, done on the queue at once.
int32_t mb_host_atlas_pages(int32_t colour, int32_t n) {
  int c = colour ? 1 : 0;
  if (n == atlas_pages_n[c]) return 1;
  if (n < 1 || n > mb_host_atlas_pages_most(colour)) return 0;
  dispatch_group_wait(prepared, DISPATCH_TIME_FOREVER);
  if (!dev) return 0;
  id<MTLTexture> t = atlas_texture(c, c ? MB_ATLAS_COLOR_SIDE : MB_ATLAS_SIDE, n);
  if (!t) return 0;
  os_unfair_lock_lock(&present_mu);
  if (atlas_puts_head > 0 && atlas_puts_head == atlas_puts_n) atlas_puts_head = atlas_puts_n = 0;
  if (atlas_puts_n == atlas_puts_cap) {
    if (atlas_puts_head > 0) {
      memmove(atlas_puts, atlas_puts + atlas_puts_head, (atlas_puts_n - atlas_puts_head) * sizeof *atlas_puts);
      atlas_puts_n -= atlas_puts_head;
      atlas_puts_head = 0;
    }
    if (atlas_puts_n == atlas_puts_cap) {
      atlas_puts_cap = atlas_puts_cap ? atlas_puts_cap * 2 : 64;
      atlas_puts = realloc(atlas_puts, atlas_puts_cap * sizeof *atlas_puts);
    }
  }
  atlas_puts[atlas_puts_n++] = (AtlasPut){++atlas_seq, c, 0, 0, 0, 0, NULL, n, (void *)CFBridgingRetain(t)};
  atlas_ops_owed++;
  os_unfair_lock_unlock(&present_mu);
  int fewer = n < atlas_pages_n[c];
  atlas_pages_n[c] = n;
  if (fewer) atlas_ops_kick();
  return 1;
}
