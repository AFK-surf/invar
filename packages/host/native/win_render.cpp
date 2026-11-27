// Direct3D 11 renderer: one instanced quad per scene instance, the Metal/GLES
// shader in HLSL, made a pixel shader for each kind it draws most (kind_ps).
// Without a usable GPU (CI) the device is WARP.
// The canvas is a composition swap chain with alpha in a DirectComposition
// tree, over a visual the native views (win_views.cpp) go in: they show through
// the holes the app paints (kind 9, written transparent without blending), as
// on the other hosts. Where DirectComposition cannot be had, the swap chain is
// the window's own and there are no native views.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include "win.h"
#ifdef MB_NO_MOONBIT_H
extern "C" size_t invar_test_array_length(const void *p);
#ifndef Moonbit_array_length
#define Moonbit_array_length(x) invar_test_array_length(x)
#endif
#endif
#include "corner_sl.h"
#include "contained.h"
#include "bands.h"
// the groups of instances the host keeps, drawn out as a frame is taken (submit)
#include "groups.h"
// Exposes only ud_inflate() in this TU (do not expect full ud_apply later in the same TU).
#define UD_INFLATE_ONLY
#include "update_delta.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

int mb_atlas_size = 4096;
static int video_support = 1;
static ID3D11Texture2D *atlas;

static int valid_atlas_size(int size) {
  return size >= 512 && size <= 4096 && (size & (size - 1)) == 0;
}

static void init_render_knobs(void) {
  static int done;
  if (done) return;
  done = 1;
  const wchar_t *a = mb_win_env(L"MBUI_ATLAS_SIZE");
  if (a && *a) {
    int sz = (int)wcstol(a, NULL, 10);
    if (valid_atlas_size(sz)) mb_atlas_size = sz;
  }
  const wchar_t *nv = mb_win_env(L"MBUI_NO_VIDEO");
  if (nv && *nv && *nv != L'0') video_support = 0;
}

extern "C" void mb_render_set_atlas_size(int size) {
  if (valid_atlas_size(size) && !atlas) mb_atlas_size = size;
}

extern "C" void mb_render_set_video_support(int on) {
  video_support = on != 0;
}

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
// a ground's flat middle (bands.h mb_pieces)
static ID3D11PixelShader *ps_flat;
// lossless fast path for axis-aligned solid/translucent rects (kind == 0)
static ID3D11PixelShader *rect_ps;
static ID3D11InputLayout *layout;
static ID3D11Buffer *ubuf;
static ID3D11ShaderResourceView *atlas_srv;
static ID3D11SamplerState *sampler;
static ID3D11BlendState *blend;
// the composition's device, the windows' (NULL: each swap chain is its window's own)
static IDCompositionDevice *dcomp;
// what makes a window's swap chain, kept for the windows of the run opened after the first (mb_render_open)
static IDXGIFactory2 *dxgi_factory;
static ID3D11RasterizerState *raster, *raster_cut;
// the swap chain is a flip model's (its Present1 takes the part that changed)
static int flip_chain;
// glass (src/ui/glass.mbt): its passes, their constants, the copies they work in (level j 1/2^j of the largest
// capture yet, the glass and the blur's margin round it, not of the target: a composer's strip, not the window; [4] a
// Gaussian's first half), the glass's own constants, the blurred copy in view
static ID3D11VertexShader *pvs;
static ID3D11PixelShader *pps, *edge_ps, *prog_ps, *ad_ps;
static ID3D11Buffer *pbuf, *gbuf, *ad_cb;
struct level { ID3D11Texture2D *t; ID3D11RenderTargetView *rt; ID3D11ShaderResourceView *srv; int w, h; };
static level lv[5];
static MBContained post_edge;
static int lv_w, lv_h;
#define MB_ADAPTIVE_MAX 16
#define MB_AD_TAU 0.070
#define MB_AD_SETTLE_SEC 0.42
#define MB_AD_MATCH_DIST 64.0
struct MBAdSlot {
  int active;
  float cx, cy, hx, hy;
  int is_dark;
  int through;
  float tint_a;
  double settle_left;
  uint64_t updated_serial;
  uint64_t seen_serial;
  uint64_t under_hash;
  int under_valid;
};
struct GU { float map[4]; float uvc[4]; float gb[4]; float shape[8][4]; float rad[8][4]; float misc[4]; float ad_box[MB_ADAPTIVE_MAX][4]; float ad_meta[4]; };
static GU gu;
static ID3D11ShaderResourceView *gsrv;
// What is a window's own (win_app.c: the windows of one run): its swap chain and its buffer's view, its composition's
// target, root, the native views' visual under the canvas's and the canvas's (NULL: the swap chain is the window's own),
// its buffer's size in pixels and its size in points and scale (mb_render_set_size's); the last frame, kept for
// snapshots (its own vertex buffer still holds it) and to find the runs of each image; the canvas its frames are drawn
// into and what of it is said to differ (partial redraw, below). Put in place for a window by mb_render_use.
#define MB_PARTS 8
#define MB_WIN_GAP_MAX 4
struct WinDropStats {
  UINT last_present_count;
  UINT last_present_refresh;
  int64_t last_present_qpc;
  int has_frame_stats;
  int gap_owed;
  int gap_unknown_open;   // unsealed unknown gap (GetLastPresentCount failed / OCCLUDED / DISJOINT / init)
  int has_unknown_until;  // suppress transitions with prev < unknown_until until fs_present >= unknown_until
  UINT unknown_until;
  UINT gap_starts[MB_WIN_GAP_MAX];
  int gap_count;
};

static void win_drop_stats_reset(WinDropStats *st) {
  memset(st, 0, sizeof *st);
  st->gap_owed = 1;
  st->gap_unknown_open = 1;
}

struct RenderWin {
  HWND target_hwnd;
  IDXGISwapChain1 *chain;
  ID3D11RenderTargetView *rtv;
  IDCompositionTarget *dtarget;
  IDCompositionVisual *droot, *dunder, *dcanvas;
  int fb_w, fb_h;
  double vw, vh, vscale;
  ID3D11Buffer *win_vbuf;
  size_t win_vbuf_cap;
  int32_t last_count;
  float *last_inst;
  size_t last_cap;
  float last_bg[3];
  int has_glass;
  ID3D11Texture2D *canvas;
  ID3D11RenderTargetView *canvas_rtv;
  int canvas_w, canvas_h, canvas_ok;
  double dirty[MB_PARTS][4];
  int dirty_n, dirty_whole, dirty_told;
  // a picture the frame drawn last had none of yet (drawn without it): the next drawn whole
  int pictures_missing;
  // transparent ground for borderless composed child windows (mb_render_set_clear)
  int clear;
  level ad_tex[2];
  int ad_cur;
  MBAdSlot ad_slots[MB_ADAPTIVE_MAX];
  double ad_last_time;
  uint64_t ad_frame_serial;
  int ad_settling;
  WinDropStats drop_stats;
  int last_presented;
  int shown_ok;
  int shown_streams;
  int shown_has_images;
  double shown_w, shown_h, shown_scale;
  int shown_fb_w, shown_fb_h, shown_clear;
  float shown_light;
  int32_t shown_atlas_gen;
  uint64_t shown_atlas_overwrite_seq;
  uint64_t shown_image_gen;
  uint64_t shown_image_free_seq;
  int swap_needs_full;
  int commit_owed;
  float *next_inst;
  size_t next_cap;
};
static RenderWin render_wins[MB_WINS];
static RenderWin *rw = &render_wins[0];
static void render_win_fresh(RenderWin *w) {
  memset(w, 0, sizeof *w);
  w->vw = 800, w->vh = 600, w->vscale = 1;
  win_drop_stats_reset(&w->drop_stats);
}
static struct RenderWinsMade {
  RenderWinsMade() {
    for (int k = 0; k < MB_WINS; k++) render_win_fresh(&render_wins[k]);
  }
} render_wins_made;
#define target_hwnd (rw->target_hwnd)
#define chain (rw->chain)
#define rtv (rw->rtv)
#define dtarget (rw->dtarget)
#define droot (rw->droot)
#define dunder (rw->dunder)
#define dcanvas (rw->dcanvas)
#define fb_w (rw->fb_w)
#define fb_h (rw->fb_h)
#define vw (rw->vw)
#define vh (rw->vh)
#define vscale (rw->vscale)
#define vbuf (rw->win_vbuf)
#define vbuf_cap (rw->win_vbuf_cap)
#define last_count (rw->last_count)
#define last_inst (rw->last_inst)
#define last_cap (rw->last_cap)
#define last_bg (rw->last_bg)
#define canvas (rw->canvas)
#define canvas_rtv (rw->canvas_rtv)
#define canvas_w (rw->canvas_w)
#define canvas_h (rw->canvas_h)
#define canvas_ok (rw->canvas_ok)
#define dirty (rw->dirty)
#define dirty_n (rw->dirty_n)
#define dirty_whole (rw->dirty_whole)
#define dirty_told (rw->dirty_told)
#define pictures_missing (rw->pictures_missing)

// The window of the run drawn now (win_app.c's win_use): its swap chain, its size, its last frame and its canvas.
void mb_render_use(int wid) {
  if (wid >= 0 && wid < MB_WINS) rw = &render_wins[wid];
}

extern "C" void mb_render_set_clear(int wid, int on) {
  if (wid >= 0 && wid < MB_WINS) render_wins[wid].clear = on ? 1 : 0;
}

// the numbers of an instance (src/ui/scene.mbt)
#define MB_INST 60
#include "glass_damage.h"

static const char *shader_src =
    "// the kind of what this pixel shader draws, fixed (kind_ps); -1 any, as the instance says\n"
    "#ifndef ONE_KIND\n"
    "#define ONE_KIND -1\n"
    "#endif\n"
    "cbuffer U : register(b0) { float2 vp; float scale; float inv_atlas; float2 origin; float2 _pad2; };\n"
    "// glass (src/ui/glass.mbt): where a point is in the blurred copy under it (uv = p * gmap.xy + gmap.zw, kept in guvc),\n"
    "// a liquid group's bounds, shapes, corners (x of each), how many\n"
    "cbuffer G : register(b1) { float4 gmap; float4 guvc; float4 ggb; float4 gshape[8]; float4 grad[8]; float4 gmisc; float4 gad_box[16]; float4 gad_meta; };\n"
    "Texture2D atlas : register(t0);\n"
    "Texture2D img : register(t1);\n"
    "Texture2D bd : register(t2);\n"
    "// the pieces of the frame's shapes (src/ui/shape.mbt: the instances of kind 12 after every one drawn), 15 fours each\n"
    "Buffer<float4> segs : register(t3);\n"
    "Texture2D ad_tex : register(t5);\n"
    "SamplerState samp : register(s0);\n"
    "struct VIn { float4 rect : INST0; float4 p : INST1; float4 color : INST2; float4 c2 : INST3;\n"
    "  float4 clip : INST4; float4 pd : INST5; float4 rc : INST6; float4 rs : INST7; float4 rot : INST8;\n"
    "  float4 rr : INST9; float4 oclip : INST10; float4 orc : INST11; float4 ors : INST12; float4 fade : INST13; float4 fadex : INST14;\n"
    "  uint vid : SV_VertexID; };\n"
    "struct VOut { float4 pos : SV_Position; nointerpolation float4 rot : ROT;\n"
    "  nointerpolation float4 rect : INST0; nointerpolation float4 prm : INST1;\n"
    "  nointerpolation float4 color : INST2; nointerpolation float4 c2 : INST3;\n"
    "  nointerpolation float4 clip : INST4; nointerpolation float4 pd : INST5; nointerpolation float4 rc : INST6;\n"
    "  nointerpolation float4 rs : INST7; nointerpolation float4 rr : INST8;\n"
    "  nointerpolation float4 oclip : INST9; nointerpolation float4 orc : INST10; nointerpolation float4 ors : INST11;\n"
    "  nointerpolation float4 fade : INST12; nointerpolation float4 fadex : INST13; };\n"
    "VOut vmain(VIn I) {\n"
    "  uint vid = I.vid;\n"
    "  float2 corner = float2((vid == 1 || vid == 4 || vid == 5) ? 1.0 : 0.0,\n"
    "                         (vid == 2 || vid == 3 || vid == 5) ? 1.0 : 0.0);\n"
    "  int kind = int(I.p.y + 0.5);\n"
    "  bool merged_liquid = kind == 6 && I.pd.w > 0.0;\n"
    "  float grow = kind == 2 ? I.p.w * 3.0 + 2.0 : (kind == 6 && I.pd.w <= 0.0) ? 2.0 : ((kind == 0 || kind == 4 || kind == 5 || kind == 8) ? 1.0 : 0.0);\n"
    "  float2 pos = I.rect.xy - grow + corner * (I.rect.zw + 2.0 * grow);\n"
    "  if (merged_liquid) pos = lerp(ggb.xy, ggb.zw, corner);\n"
    "  // the pieces of shapes (kind 12): nothing drawn\n"
    "  if (kind == 12) pos = float2(0.0, 0.0);\n"
    "  // turned (scene.mbt 32-35): drawn turned about its point, everything of how in its own place (o.p), its clip\n"
    "  // there too or (35: one from outside what turned) where it is drawn (o.cp)\n"
    "  float2 at = pos;\n"
    "  float4 vrot = float4(0.0, 0.0, 0.0, 0.0);\n"
    "  if (I.rot.x != 0.0) {\n"
    "    float c = cos(I.rot.x), s = sin(I.rot.x);\n"
    "    float2 d = pos - I.rot.yz;\n"
    "    at = I.rot.yz + float2(c * d.x - s * d.y, s * d.x + c * d.y);\n"
    "    float k = I.rot.w > 0.5 ? 2.0 : 1.0;\n"
    "    vrot = float4(c * k, s * k, I.rot.yz);\n"
    "  }\n"
    "  VOut o;\n"
    "  float2 rel = at - origin;\n"
    "  o.pos = float4(rel.x / vp.x * 2.0 - 1.0, 1.0 - rel.y / vp.y * 2.0, 0.0, 1.0);\n"
    "  o.rot = vrot; o.rect = I.rect; o.prm = I.p; o.color = I.color; o.c2 = I.c2; o.clip = I.clip; o.pd = I.pd; o.rc = I.rc; o.rs = I.rs; o.rr = I.rr;\n"
    "  if (kind == 1) o.prm.x = I.rot.x;\n"
    "  if (kind == 4 && I.pd.z > 0.5) o.pd.x = I.rot.w;\n"
    "  o.oclip = I.oclip; o.orc = I.orc; o.ors = I.ors; o.fade = I.fade; o.fadex = I.fadex;\n"
    "  return o;\n"
    "}\n"
    MB_CORNER_SL
    "#if ONE_KIND == 4\n"
    "float mb_cr(float x) {\n"
    "  x = abs(x);\n"
    "  if (x >= 2.0) return 0.0;\n"
    "  if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;\n"
    "  return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;\n"
    "}\n"
    "void mb_cr5(float tc, float inv_sz, float s, out float u[5], out float w[5]) {\n"
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
    "float4 mb_sample_pic(float2 uv, float2 dudx, float2 dudy) {\n"
    "  uint tw, th, mips;\n"
    "  img.GetDimensions(0, tw, th, mips);\n"
    "  float2 sz = float2(tw, th);\n"
    "  float2 rate = float2(length(dudx * sz), length(dudy * sz));\n"
    "  if (max(abs(rate.x - 1.0), abs(rate.y - 1.0)) < 0.005)\n"
    "    return img.SampleLevel(samp, uv, 0.0);\n"
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
    "      (img.SampleLevel(samp, float2(t0.x,  t0.y),  0.0) * w0.x  +\n"
    "       img.SampleLevel(samp, float2(t12.x, t0.y),  0.0) * w12.x +\n"
    "       img.SampleLevel(samp, float2(t3.x,  t0.y),  0.0) * w3.x)  * w0.y +\n"
    "      (img.SampleLevel(samp, float2(t0.x,  t12.y), 0.0) * w0.x  +\n"
    "       img.SampleLevel(samp, float2(t12.x, t12.y), 0.0) * w12.x +\n"
    "       img.SampleLevel(samp, float2(t3.x,  t12.y), 0.0) * w3.x)  * w12.y +\n"
    "      (img.SampleLevel(samp, float2(t0.x,  t3.y),  0.0) * w0.x  +\n"
    "       img.SampleLevel(samp, float2(t12.x, t3.y),  0.0) * w12.x +\n"
    "       img.SampleLevel(samp, float2(t3.x,  t3.y),  0.0) * w3.x)  * w3.y;\n"
    "    c.a = saturate(c.a);\n"
    "    c.rgb = clamp(c.rgb, 0.0, c.a);\n"
    "    return c;\n"
    "  }\n"
    "  float rmin = min(rate.x, rate.y);\n"
    "  uint L = (mips > 1 && rmin > 2.0) ? min((uint)max(0.0, ceil(log2(rmin)) - 1.0), mips - 1) : 0;\n"
    "  uint mw, mh, nl;\n"
    "  img.GetDimensions(L, mw, mh, nl);\n"
    "  float2 msz = float2(mw, mh);\n"
    "  float2 s = float2(length(dudx * msz), length(dudy * msz));\n"
    "  if (max(s.x, s.y) > 2.05) return img.SampleGrad(samp, uv, dudx, dudy);\n"
    "  float ux[5], wx[5], uy[5], wy[5];\n"
    "  mb_cr5(uv.x * msz.x - 0.5, 1.0 / msz.x, max(s.x, 1.0), ux, wx);\n"
    "  mb_cr5(uv.y * msz.y - 0.5, 1.0 / msz.y, max(s.y, 1.0), uy, wy);\n"
    "  float4 c = float4(0.0, 0.0, 0.0, 0.0);\n"
    "  float lv = (float)L;\n"
    "  [unroll] for (int j = 0; j < 5; j++) {\n"
    "    float4 row = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    [unroll] for (int i = 0; i < 5; i++)\n"
    "      row += img.SampleLevel(samp, float2(ux[i], uy[j]), lv) * wx[i];\n"
    "    c += row * wy[j];\n"
    "  }\n"
    "  c.a = saturate(c.a);\n"
    "  c.rgb = clamp(c.rgb, 0.0, c.a);\n"
    "  return c;\n"
    "}\n"
    "#else\n"
    "float4 mb_sample_pic(float2 uv, float2 dudx, float2 dudy) {\n"
    "  return img.SampleGrad(samp, uv, dudx, dudy);\n"
    "}\n"
    "#endif\n"
    "float sd_rr(float2 p, float4 r, float4 rad) {\n"
    "  float2 hs = r.zw * 0.5;\n"
    "  return mb_sd_box(p - r.xy - hs, hs, mb_corner_of(p - r.xy - hs, rad), 1e9);\n"
    "}\n"
    "// a notch cut into the top edge (scene.mbt 21-23): a circle about the top's middle (pad.y below it, pad.z its radius),\n"
    "// met by the edge through fillets (pad.w): the rounded intersection with what is outside the circle\n"
    "float notch_cut(float d, float2 p, float4 r, float4 pad) {\n"
    "  if (pad.z <= 0.0) return d;\n"
    "  float b = pad.z - length(p - float2(r.x + r.z * 0.5, r.y + pad.y));\n"
    "  float f = max(pad.w, 0.0);\n"
    "  float2 u = max(float2(f + d, f + b), 0.0);\n"
    "  return min(-f, max(d, b)) + length(u);\n"
    "}\n"
    "float smin_k(float a, float b, float k) {\n"
    "  if (k <= 0.0) return min(a, b);\n"
    "  float h = max(k - abs(a - b), 0.0) / k;\n"
    "  return min(a, b) - h * h * k * 0.25;\n"
    "}\n"
    "float sd_group(float2 p, float k) {\n"
    "  float d = 1e9;\n"
    "  int n = int(gmisc.x + 0.5);\n"
    "  [loop] for (int i = 0; i < 8; i++) {\n"
    "    if (i >= n) break;\n"
    "    d = smin_k(d, sd_rr(p, gshape[i], grad[i]), k);\n"
    "  }\n"
    "  return d;\n"
    "}\n"
    "// Integral of linear ramp from 0 to w (contourOffset = 0, width w in device px)\n"
    "float mb_contour_int(float t, float w) {\n"
    "  float u = clamp(t, 0.0, w);\n"
    "  return u - u * u / (2.0 * w);\n"
    "}\n"
    "static float mb_para_int(float t, float w) {\n"
    "  float u = saturate(t / max(w, 0.0001));\n"
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
    "float4 backdrop4(float2 p) {\n"
    "  return bd.SampleLevel(samp, clamp(p * gmap.xy + gmap.zw, guvc.xy, guvc.zw), 0);\n"
    "}\n"
    "float3 backdrop(float2 p) {\n"
    "  return backdrop4(p).rgb;\n"
    "}\n"
    "float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }\n"
    "static float mb_ink_luma(float3 c) { return dot(c * c, float3(0.2126, 0.7152, 0.0722)); }\n"
    "float mb_backdrop_luma_rect(float2 c, float2 h, float hole_y) {\n"
    "  float sum = 0.0;\n"
    "  float mx1 = -1.0, mx2 = -1.0;\n"
    "  float mn1 = 2.0, mn2 = 2.0;\n"
    "  [unroll] for (int iy = -1; iy <= 1; iy++) {\n"
    "    float py = c.y + float(iy) * (h.y * 0.60);\n"
    "    [unroll] for (int ix = -2; ix <= 2; ix++) {\n"
    "      float px = c.x + float(ix) * (h.x * 0.38);\n"
    "      float4 smp = backdrop4(float2(px, py));\n"
    "      float a = (gmisc.y > 0.5) ? saturate(smp.a) : 1.0;\n"
    "      float y = mb_ink_luma(saturate(smp.rgb + (1.0 - a) * hole_y));\n"
    "      sum += y;\n"
    "      if (y > mx1) { mx2 = mx1; mx1 = y; } else if (y > mx2) { mx2 = y; }\n"
    "      if (y < mn1) { mn2 = mn1; mn1 = y; } else if (y < mn2) { mn2 = y; }\n"
    "    }\n"
    "  }\n"
    "  return (sum - mx1 - mx2 - mn1 - mn2) / 11.0;\n"
    "}\n"
    "float mb_unpack16(float2 rg) {\n"
    "  float hi = floor(saturate(rg.x) * 255.0 + 0.5);\n"
    "  float lo = floor(saturate(rg.y) * 255.0 + 0.5);\n"
    "  return (hi * 256.0 + lo) / 65535.0;\n"
    "}\n"
    "float ad_sample(float2 ac, float fallback_r, bool is_dark_glass, float tint_a) {\n"
    "  int ad_n = int(gad_meta.x + 0.5);\n"
    "  [branch] if (ad_n > 0) {\n"
    "    int best = -1;\n"
    "    float best_d = 1e9;\n"
    "    [loop] for (int i = 0; i < 16; i++) {\n"
    "      if (i >= ad_n) break;\n"
    "      float d = length(ac - gad_box[i].xy);\n"
    "      if (d <= 2.0 && d < best_d) { best = i; best_d = d; }\n"
    "    }\n"
    "    if (best < 0) {\n"
    "      best_d = 1e9;\n"
    "      [loop] for (int i = 0; i < 16; i++) {\n"
    "        if (i >= ad_n) break;\n"
    "        float2 diff = abs(ac - gad_box[i].xy);\n"
    "        if (diff.x <= gad_box[i].z + 2.0 && diff.y <= gad_box[i].w + 2.0) {\n"
    "          float d = length(diff);\n"
    "          if (d < best_d) { best = i; best_d = d; }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    if (best >= 0) {\n"
    "      return mb_unpack16(ad_tex.Load(int3(best, 0, 0)).rg);\n"
    "    }\n"
    "  }\n"
    "  float bgL = mb_backdrop_luma_rect(ac, float2(max(fallback_r, 1.0), max(fallback_r, 1.0)), is_dark_glass ? 0.0 : 1.0);\n"
    "  if (is_dark_glass) return smoothstep(0.35, 0.85, bgL);\n"
    "  float baseDens = lerp(0.404, tint_a, smoothstep(0.405, 0.46, tint_a));\n"
    "  float extra = saturate((baseDens - 0.404) / (1.0 - 0.404));\n"
    "  float c_eq = lerp(sqrt(max(bgL, 0.0)), 0.992, pow(extra, 1.35));\n"
    "  return 1.0 - smoothstep(0.12, 0.21, c_eq * c_eq);\n"
    "}\n"
    "float3 ad_sample_ink(float2 ac, float fallback_r) {\n"
    "  int ad_n = int(gad_meta.x + 0.5);\n"
    "  [branch] if (ad_n > 0) {\n"
    "    int best = -1;\n"
    "    float best_d = 1e9;\n"
    "    for (int i = 0; i < 16; i++) {\n"
    "      if (i >= ad_n) break;\n"
    "      float d = length(ac - gad_box[i].xy);\n"
    "      if (d <= 2.0 && d < best_d) { best = i; best_d = d; }\n"
    "    }\n"
    "    if (best < 0) {\n"
    "      best_d = 1e9;\n"
    "      for (int i = 0; i < 16; i++) {\n"
    "        if (i >= ad_n) break;\n"
    "        float2 diff = abs(ac - gad_box[i].xy);\n"
    "        if (diff.x <= gad_box[i].z + 2.0 && diff.y <= gad_box[i].w + 2.0) {\n"
    "          float d = length(diff);\n"
    "          if (d < best_d) { best = i; best_d = d; }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    if (best >= 0) {\n"
    "      float4 px = ad_tex.Load(int3(best, 0, 0));\n"
    "      return float3(mb_unpack16(px.rg), px.a, px.b);\n"
    "    }\n"
    "  }\n"
    "  float bgL = mb_backdrop_luma_rect(ac, float2(max(fallback_r, 1.0), max(fallback_r, 1.0)), 1.0);\n"
    "  return float3(1.0 - smoothstep(0.12, 0.21, bgL), bgL, 0.0);\n"
    "}\n"
    "float3 mb_adaptive_ink_at(float3 rgb, float2 ac, float fallback_r, float4 rect) {\n"
    "  float ch = max(max(rgb.r, rgb.g), rgb.b) - min(min(rgb.r, rgb.g), rgb.b);\n"
    "  float origL = luma(rgb);\n"
    "  if (ch > 0.08 || origL >= 0.5) return rgb;\n"
    "  float3 cap = ad_sample_ink(ac, fallback_r);\n"
    "  float capsuleDarkT = cap.x;\n"
    "  float inkT = smoothstep(0.0, 1.0, capsuleDarkT);\n"
    "  float lightL = clamp(1.06 - origL, 0.68, 0.98);\n"
    "  return lerp(rgb, float3(lightL, lightL, lightL), inkT);\n"
    "}\n"
    "// how much of the pixel at p its rounded clip keeps (r: the corners' radii, top left, then clockwise; s: how smooth\n"
    "// each is), its edge covered as a box's own (0.5 - d * scale), so what it cuts meets the ground of the box that cuts\n"
    "// it\n"
    "float clip_cover(float2 p, float4 c, float4 r, float4 s, float sc) {\n"
    "  float d = mb_sd_clip(p, c, r, s, 1.0 + 2.0 / sc);\n"
    "  return saturate(0.5 - d * sc);\n"
    "}\n"
    "#if ONE_KIND < 0 || ONE_KIND == 10\n"
    "float mb_sd_quad_bezier(float2 pos, float2 A, float2 B, float2 C) {\n"
    "  float2 a = B - A;\n"
    "  float2 b = A - 2.0 * B + C;\n"
    "  float2 c = a * 2.0;\n"
    "  float2 d = A - pos;\n"
    "  float2 ac = C - A;\n"
    "  float bb = dot(b, b);\n"
    "  float t_lin = saturate(dot(pos - A, ac) / max(dot(ac, ac), 1e-12));\n"
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
    "    float t = saturate(uv.x + uv.y - kx);\n"
    "    float2 v = d + (c + b * t) * t;\n"
    "    float dd = dot(v, v);\n"
    "    if (dd < res) { res = dd; bestT = t; }\n"
    "  } else {\n"
    "    float z = sqrt(-p);\n"
    "    float denom = min(p * z * 2.0, -1e-12);\n"
    "    float ang = acos(clamp(q / denom, -1.0, 1.0)) / 3.0;\n"
    "    float m = cos(ang);\n"
    "    float n = sin(ang) * 1.732050808;\n"
    "    float t0 = saturate((m + m) * z - kx);\n"
    "    float t1 = saturate((-n - m) * z - kx);\n"
    "    float t2 = saturate((n - m) * z - kx);\n"
    "    float2 v0 = d + (c + b * t0) * t0;\n"
    "    float2 v1 = d + (c + b * t1) * t1;\n"
    "    float2 v2 = d + (c + b * t2) * t2;\n"
    "    float d0 = dot(v0, v0), d1 = dot(v1, v1), d2 = dot(v2, v2);\n"
    "    if (d0 < res) { res = d0; bestT = t0; }\n"
    "    if (d1 < res) { res = d1; bestT = t1; }\n"
    "    if (d2 < res) { res = d2; bestT = t2; }\n"
    "  }\n"
    "  // 2 safeguarded Newton-Raphson steps eliminate float32 Cardano cancellation on shallow curves\n"
    "  [unroll] for (int ni = 0; ni < 2; ni++) {\n"
    "    float2 vn = d + (c + b * bestT) * bestT;\n"
    "    float2 dp = c + 2.0 * b * bestT;\n"
    "    float fp = dot(dp, dp) + 2.0 * dot(vn, b);\n"
    "    if (fp > 1e-12) {\n"
    "      float nt = saturate(bestT - dot(vn, dp) / fp);\n"
    "      float2 nv = d + (c + b * nt) * nt;\n"
    "      float nd = dot(nv, nv);\n"
    "      if (nd < res) { res = nd; bestT = nt; } else { break; }\n"
    "    } else { break; }\n"
    "  }\n"
    "  return sqrt(res);\n"
    "}\n"
    "#endif\n"
    "// A glyph's coverage, a pixel's three stripes each its own (ClearType: win_text.cpp draw_layout), blended stripe by\n"
    "// stripe as the second source (the blend's INV_SRC1_COLOR); what else is drawn, by its alpha as before. Every\n"
    "// target is cleared opaque (encode), so the stripes always fall on a ground of the pixel's own.\n"
    "static float3 stripes;\n"
    "static bool striped;\n"
    "float4 fmain0(VOut I);\n"
    "struct FOut { float4 c : SV_Target0; float4 w : SV_Target1; };\n"
    "FOut fmain(VOut I) {\n"
    "  striped = false;\n"
    "  FOut o;\n"
    "  o.c = fmain0(I);\n"
    "  o.w = striped ? float4(stripes, o.c.a) : o.c.aaaa;\n"
    "  return o;\n"
    "}\n"
    "float4 fmain0(VOut I) {\n"
    "  float2 at_p = I.pos.xy / scale + origin;\n"
    "  float2 p = at_p, cp = at_p;\n"
    "  float rl = dot(I.rot.xy, I.rot.xy);\n"
    "  if (rl > 0.5) {\n"
    "    float2 cs = rl > 2.0 ? I.rot.xy * 0.5 : I.rot.xy;\n"
    "    float2 d = at_p - I.rot.zw;\n"
    "    p = I.rot.zw + float2(cs.x * d.x + cs.y * d.y, -cs.y * d.x + cs.x * d.y);\n"
    "    cp = rl > 2.0 ? at_p : p;\n"
    "  }\n"
    "  // cut to its clip: a rect's, hard; a rounded one's, covered as edges are\n"
    "  float cover = 1.0;\n"
    "  if (max(max(I.rc.x, I.rc.y), max(I.rc.z, I.rc.w)) > 0.0) {\n"
    "    cover = clip_cover(cp, I.clip, I.rc, I.rs, scale);\n"
    "    if (cover <= 0.0) discard;\n"
    "  } else if (cp.x < I.clip.x || cp.y < I.clip.y || cp.x > I.clip.z || cp.y > I.clip.w) discard;\n"
    "  // and by the clip outside it, where it has one (scene.mbt 40-51: a pane round an image's frame in its corner)\n"
    "  if (max(max(I.orc.x, I.orc.y), max(I.orc.z, I.orc.w)) > 0.0) {\n"
    "    cover *= clip_cover(cp, I.oclip, I.orc, I.ors, scale);\n"
    "    if (cover <= 0.0) discard;\n"
    "  }\n"
    "  int kind = ONE_KIND == 13 ? 6 : (ONE_KIND >= 0 ? ONE_KIND : int(I.prm.y + 0.5));\n"
    "  bool prog_glass = (kind == 5 && I.pd.z < -0.5);\n"
    "  // and faded out towards a cut, each pixel by where it is (scene.mbt 52-59)\n"
    "  float fade_r = 1.0;\n"
    "  if (I.fade.y > 0.0) fade_r *= saturate((cp.y - I.fade.x) / I.fade.y);\n"
    "  if (I.fade.w > 0.0) fade_r *= saturate((I.fade.z - cp.y) / I.fade.w);\n"
    "  if (I.fadex.y > 0.0) fade_r *= saturate((cp.x - I.fadex.x) / I.fadex.y);\n"
    "  if (I.fadex.w > 0.0) fade_r *= saturate((I.fadex.z - cp.x) / I.fadex.w);\n"
    "  if (fade_r <= 0.0) discard;\n"
    "  if (!prog_glass) {\n"
    "    cover *= fade_r;\n"
    "    if (cover <= 0.0) discard;\n"
    "  }\n"
    "  if (kind == 9) { float2 hh = I.rect.zw * 0.5; float rr = min(mb_corner_of(p - I.rect.xy - hh, I.rr), min(hh.x, hh.y)); float2 q = abs(p - I.rect.xy - hh) - hh + rr; if (length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rr > 0.0) discard; return (float4(0.0, 0.0, 0.0, 0.0)) * cover; }\n"
    "  float4 col = I.color;\n"
    "  if (kind == 11) {\n"
    "    // a band of a shape (src/ui/shape.mbt): c2.y of its pieces from c2.x; prm.x half its stroke's width (0: a fill,\n"
    "    // c2.z 1 by the even-odd rule)\n"
    "    int first = int(I.c2.x + 0.5), n = int(I.c2.y + 0.5);\n"
    "    float a;\n"
    "    if (I.prm.x > 0.0) {\n"
    "      float d = 1e9;\n"
    "      for (int i = first; i < first + n; i++) d = min(d, mb_seg_dist(segs.Load((i / 6) * 15 + mb_seg_slot(i)), I.rect, p));\n"
    "      a = saturate(0.5 - (d - I.prm.x) * scale);\n"
    "    } else {\n"
    "      float4 w = float4(0.0, 0.0, 0.0, 0.0);\n"
    "      for (int i = first; i < first + n; i++) w += mb_path_rows(segs.Load((i / 6) * 15 + mb_seg_slot(i)), I.rect, p, scale);\n"
    "      a = mb_path_cover(w, I.c2.z);\n"
    "    }\n"
    "    [branch] if (I.pd.w > 0.0) col.rgb = mb_adaptive_ink_at(col.rgb, I.pd.yz, I.pd.w * 0.5, I.rect);\n"
    "    return (float4(col.rgb, 1.0) * col.a * a) * cover;\n"
    "  }\n"
    "  if (kind == 10) {\n"
    "    // a line segment (prm.z == 0) or analytic quadratic Bezier stroke (prm.z > 0.5, control point in rr.xy)\n"
    "    float2 lp = p - I.rect.xy;\n"
    "    float d;\n"
    // (mb_sd_quad_bezier is made only where kind 10 can be drawn: a shader of another fixed kind has none to call)
    "#if ONE_KIND < 0 || ONE_KIND == 10\n"
    "    if (I.prm.z > 0.5) {\n"
    "      float s = max(max(I.rect.z, I.rect.w), 1.0);\n"
    "      float2 sz = I.rect.zw / s;\n"
    "      d = mb_sd_quad_bezier(lp / s, I.c2.xy * sz, I.rr.xy * sz, I.c2.zw * sz) * s - I.prm.x;\n"
    "    } else\n"
    "#endif\n"
    "    {\n"
    "      float2 a = I.c2.xy * I.rect.zw, c = I.c2.zw * I.rect.zw;\n"
    "      float2 ab = c - a;\n"
    "      float h = saturate(dot(lp - a, ab) / max(dot(ab, ab), 1e-8));\n"
    "      d = length(lp - a - ab * h) - I.prm.x;\n"
    "    }\n"
    "    [branch] if (I.pd.w > 0.0) col.rgb = mb_adaptive_ink_at(col.rgb, I.pd.yz, I.pd.w * 0.5, I.rect);\n"
    "    return (float4(col.rgb, 1.0) * col.a * saturate(0.5 - d * scale)) * cover;\n"
    "  }\n"
    "  if (kind == 1 || kind == 3 || kind == 7) {\n"
    "    // within its own cell (c2: where in the atlas, in texels), however its rect is stretched or turned: a sample\n"
    "    // past the cell's edge texels would blend in what lies beside it (another glyph, one left from before the atlas\n"
    "    // started over); an unscaled, unrotated glyph snaps to exact texel centres so float32 rounding at 1.25x/1.5x\n"
    "    // never bleeds adjacent ClearType stripes.\n"
    "    float2 off = abs(I.rect.zw * scale - I.c2.zw);\n"
    "    // (exact only where its corner is on a pixel too: one half way between two had a row of its texels taken twice\n"
    "    // and the next not at all, where floor's step fell)\n"
    "    float2 r0 = I.rect.xy * scale, r0i = floor(r0 + 0.5);\n"
    "    bool exact = kind == 1 && max(off.x, off.y) <= 0.05 && max(abs(r0.x - r0i.x), abs(r0.y - r0i.y)) <= 0.05 && I.prm.x == 0.0;\n"
    "    float2 tex = exact ? floor(p * scale - r0i) + 0.5 : (p - I.rect.xy) / I.rect.zw * I.c2.zw;\n"
    "    float2 uv = clamp(I.c2.xy + tex, I.c2.xy + 0.5, I.c2.xy + I.c2.zw - 0.5) * inv_atlas;\n"
    "    float4 t = atlas.SampleLevel(samp, uv, 0);\n"
    "    if (kind == 1) {\n"
    "      // (a mask drawn in gray has its three the same as its alpha: as before; a glyph drawn larger or smaller than\n"
    "      // its picture, its stripes no longer on the screen's: in gray, their mean). Its cover blended as it is, light\n"
    "      // words on dark as dark on light, as Metal's does (apple_render.m: its colour times its cover)\n"
    "      float3 cov = max(off.x, off.y) > 0.05 ? t.aaa : t.rgb;\n"
    "      stripes = cov * (col.a * cover);\n"
    "      striped = true;\n"
    "      return float4(col.rgb * stripes, dot(stripes, float3(1.0, 1.0, 1.0) / 3.0));\n"
    "    }\n"
    "    if (kind == 7) {\n"
    "      // inked by how light what is under its glass is, looked at round the glass's middle and locally\n"
    "      float3 ink = mb_adaptive_ink_at(col.rgb, I.pd.yz, I.pd.w * 0.5, I.rect);\n"
    "      return (float4(ink, 1.0) * (t.a * col.a)) * cover;\n"
    "    }\n"
    "    return (t * col.a) * cover;\n"
    "  }\n"
    "  if (kind == 6) {\n"
    "    // Copyright 2025, Tim Lehmann for whynotmake.it. Adapted from flutter_liquid_glass\n"
    "    // liquid_glass_final_render_core.glsl, Apache-2.0.\n"
    "    float k = I.pd.w;\n"
    "    float4 s_rr = max(max(I.rr.x, I.rr.y), max(I.rr.z, I.rr.w)) > 0.0 ? I.rr : float4(I.prm.x, I.prm.x, I.prm.x, I.prm.x);\n"
    "    float d;\n"
    "    float halfMinor = 1e9;\n"
    "    float2 extSpineVec = float2(0.0, 0.0);\n"
    "#if ONE_KIND == 13\n"
    "    d = sd_rr(p, I.rect, s_rr);\n"
    "    float2 hs = I.rect.zw * 0.5;\n"
    "    halfMinor = min(hs.x, hs.y);\n"
    "    float2 ct = I.rect.xy + hs;\n"
    "    float2 ch = max(hs - float2(halfMinor, halfMinor), float2(0.0, 0.0));\n"
    "    extSpineVec = p - clamp(p, ct - ch, ct + ch);\n"
    "#else\n"
    "    [branch] if (k <= 0.0) {\n"
    "      d = sd_rr(p, I.rect, s_rr);\n"
    "      float2 hs = I.rect.zw * 0.5;\n"
    "      halfMinor = min(hs.x, hs.y);\n"
    "      float2 ct = I.rect.xy + hs;\n"
    "      float2 ch = max(hs - float2(halfMinor, halfMinor), float2(0.0, 0.0));\n"
    "      extSpineVec = p - clamp(p, ct - ch, ct + ch);\n"
    "    } else {\n"
    "      d = 1e9;\n"
    "      int n_sh = int(gmisc.x + 0.5);\n"
    "      [loop] for (int i = 0; i < 8; i++) {\n"
    "        if (i >= n_sh) break;\n"
    "        float di = sd_rr(p, gshape[i], grad[i]);\n"
    "        float2 hsi = gshape[i].zw * 0.5;\n"
    "        float hi = min(hsi.x, hsi.y);\n"
    "        float2 cti = gshape[i].xy + hsi;\n"
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
    "          halfMinor = lerp(halfMinor, hi, w);\n"
    "          extSpineVec = lerp(extSpineVec, vi, w);\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "#endif\n"
    "    halfMinor = max(halfMinor, 1.0);\n"
    "    float sd_px = -d * scale;\n"
    "    float materialAlpha = saturate(sd_px + 0.5);\n"
    "    float edgeW = max(0.90 * scale, 2.6);\n"
    "    float outward = -sd_px;\n"
    "    float ext_cov = mb_contour_int(max(outward + 0.5, 0.0), edgeW)\n"
    "                  - mb_contour_int(max(outward - 0.5, 0.0), edgeW);\n"
    "    if (materialAlpha < 0.01 && ext_cov < 0.01) discard;\n"
    "    float regMix = smoothstep(0.105, 0.305, col.a);\n"
    "    bool is_clear = (regMix <= 0.0);\n"
    "    bool is_adaptive = I.c2.z < -0.5;\n"
    "    float spot_raw = is_adaptive ? (-I.c2.z - 1.0) : I.c2.z;\n"
    "    bool is_dark = (!is_clear && luma(col.rgb) < 0.35);\n"
    "    float clearVeil = is_clear ? smoothstep(0.005, 0.102, col.a) : 0.0;\n"
    "    float droplet = is_clear ? (1.0 - clearVeil) : 0.0;\n"
    "#if ONE_KIND == 13\n"
    "    bool is_drop = (droplet > 0.001);\n"
    "#else\n"
    "    bool is_drop = (droplet > 0.001) && (k <= 0.0);\n"
    "#endif\n"
    "    float inward = max(-d, 0.0);\n"
    "    float rawBevel = max(I.c2.y, 0.0);\n"
    "    float rawAmount = max(I.pd.y, 0.0);\n"
    "    float colChroma = max(max(col.r, col.g), col.b) - min(min(col.r, col.g), col.b);\n"
    "    float solidFade = 1.0 - smoothstep(0.88, 0.99, col.a);\n"
    "    float lensScale = min(1.0, halfMinor / max(rawBevel, 0.001));\n"
    "    float clearBevel = min(rawBevel * lensScale, halfMinor * 0.72);\n"
    "    float clearAmount = min(rawAmount * lensScale, halfMinor * 0.58);\n"
    "    float regBevel = min(rawBevel * 0.90, 0.58 * halfMinor);\n"
    "    float regAmount = min(rawAmount * 0.96, 0.88 * halfMinor);\n"
    "    float bevel = lerp(clearBevel, regBevel, regMix);\n"
    "    float amount = lerp(clearAmount, regAmount, regMix);\n"
    "    float2 surfaceNormal = float2(0.0, 0.0);\n"
    "    float shortening = 0.0;\n"
    "    float2 disp = float2(0.0, 0.0);\n"
    "    bool need_grad = (inward < max(bevel, 23.0)) || (ext_cov > 0.0) || is_drop;\n"
    "    [branch] if (need_grad) {\n"
    "      float e = 0.5;\n"
    "#if ONE_KIND == 13\n"
    "      float2 sdfGrad = float2(\n"
    "          sd_rr(p + float2(e, 0.0), I.rect, s_rr) - sd_rr(p - float2(e, 0.0), I.rect, s_rr),\n"
    "          sd_rr(p + float2(0.0, e), I.rect, s_rr) - sd_rr(p - float2(0.0, e), I.rect, s_rr)\n"
    "        ) / (2.0 * e);\n"
    "#else\n"
    "      float2 sdfGrad;\n"
    "      [branch] if (k <= 0.0) {\n"
    "        sdfGrad = float2(\n"
    "          sd_rr(p + float2(e, 0.0), I.rect, s_rr) - sd_rr(p - float2(e, 0.0), I.rect, s_rr),\n"
    "          sd_rr(p + float2(0.0, e), I.rect, s_rr) - sd_rr(p - float2(0.0, e), I.rect, s_rr)\n"
    "        ) / (2.0 * e);\n"
    "      } else {\n"
    "        sdfGrad = float2(\n"
    "          sd_group(p + float2(e, 0.0), k) - sd_group(p - float2(e, 0.0), k),\n"
    "          sd_group(p + float2(0.0, e), k) - sd_group(p - float2(0.0, e), k)\n"
    "        ) / (2.0 * e);\n"
    "      }\n"
    "#endif\n"
    "      float gradLen = length(sdfGrad);\n"
    "      surfaceNormal = gradLen > 0.0001 ? sdfGrad / gradLen : float2(0.0, 0.0);\n"
    "      shortening = sqrt(saturate(gradLen));\n"
    "      if (inward < bevel && bevel > 0.001 && amount > 0.0) {\n"
    "        float bevelX = 1.0 - saturate(inward / bevel);\n"
    "        // iOS 27.0 Glass::clear() inverted-refraction caustic spans ~73% of 14pt bevel (2.20x rawAmount).\n"
    "        float clearVeilR = smoothstep(0.005, 0.102, col.a) * (1.0 - smoothstep(0.001, 0.008, abs(I.prm.w))) * (1.0 - regMix);\n"
    "        float clearBevelX = pow(bevelX, lerp(1.0, 0.78, clearVeilR));\n"
    "        float clearAmt = amount * lerp(1.0, 2.20, clearVeilR);\n"
    "        float clearMag = clearAmt * (1.0 - sqrt(max(1.0 - clearBevelX * clearBevelX, 0.0))) * shortening;\n"
    "        float regMag = amount * (1.0 - sqrt(max(1.0 - bevelX * bevelX, 0.0))) * shortening;\n"
    "        float mag = lerp(clearMag, regMag, regMix);\n"
    "        disp = surfaceNormal * (-mag);\n"
    "      }\n"
    "    }\n"
    "    int dropRole = (I.prm.w <= -8.0) ? (int)floor((-I.prm.w + 4.0) / 16.0) : 0;\n"
    "    float rawZoomDelta = (I.prm.w <= -8.0) ? ((-I.prm.w) - (float)dropRole * 16.0) : ((I.prm.w < 0.0) ? (-I.prm.w) : 0.0);\n"
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
    "#if ONE_KIND == 13\n"
    "      float2 zoomC = I.rect.xy + I.rect.zw * 0.5;\n"
    "#else\n"
    "      float2 zoomC = (k <= 0.0) ? (I.rect.xy + I.rect.zw * 0.5) : ((ggb.xy + ggb.zw) * 0.5);\n"
    "#endif\n"
    "      sampleBase = zoomC + (p - zoomC) / lensZoom;\n"
    "    }\n"
    "    float dropHairline = 0.0;\n"
    "    float pressAmt = 0.0;\n"
    "    float menAA = 0.0;\n"
    "    if (is_drop) {\n"
    "      pressAmt = saturate(I.c2.x);\n"
    "      dropHairline = (1.0 - smoothstep(0.18, 0.62, inward)) * pressAmt;\n"
    "      if (isModernDrop) {\n"
    "        float2 sn = mb_droplet_snell(inward, rawAmount, halfMinor, dropRole, 1.52);\n"
    "        snellSLip = sn.y;\n"
    "        snellDmag = clamp(sn.x * shortening, -halfMinor, halfMinor);\n"
    "        float2 dropDisp = surfaceNormal * snellDmag;\n"
    "        if (isRoleSlid && snellSLip > 0.6 && pressAmt > 0.001) {\n"
    "          menAA = 0.35 * smoothstep(0.6, 2.0, snellSLip) * pressAmt * droplet;\n"
    "        }\n"
    "        disp = lerp(disp, dropDisp, droplet);\n"
    "      } else {\n"
    "        float bevelX = (bevel > 0.001) ? (1.0 - saturate(inward / bevel)) : 0.0;\n"
    "        float2 paraxialDisp = -surfaceNormal * (bevelX * bevelX * 3.6 * pressAmt);\n"
    "        float2 dropDisp = lerp(paraxialDisp, surfaceNormal * 0.85, dropHairline);\n"
    "        float dLen = length(dropDisp);\n"
    "        if (dLen > halfMinor) dropDisp *= (halfMinor / dLen);\n"
    "        disp = lerp(disp, dropDisp, droplet);\n"
    "      }\n"
    "    }\n"
    "    float3 refractColor;\n"
    "    if (isModernDrop) {\n"
    "      if (abs(I.pd.z) > 0.001 && snellSLip > 0.15 && pressAmt > 0.001) {\n"
    "        float dispR = mb_droplet_snell(inward, rawAmount, halfMinor, dropRole, 1.47).x * shortening;\n"
    "        float dispB = mb_droplet_snell(inward, rawAmount, halfMinor, dropRole, 1.58).x * shortening;\n"
    "        float caW = smoothstep(0.15, 1.20, snellSLip) * saturate(abs(I.pd.z) / 0.04) * droplet;\n"
    "        float dR = clamp(lerp(snellDmag, dispR, caW), -halfMinor, halfMinor);\n"
    "        float dB = clamp(lerp(snellDmag, dispB, caW), -halfMinor, halfMinor);\n"
    "        float2 dispR2 = lerp(disp, surfaceNormal * dR, droplet);\n"
    "        float2 dispB2 = lerp(disp, surfaceNormal * dB, droplet);\n"
    "        refractColor = float3(\n"
    "          backdrop(sampleBase + dispR2).r,\n"
    "          backdrop(sampleBase + disp).g,\n"
    "          backdrop(sampleBase + dispB2).b\n"
    "        );\n"
    "      } else {\n"
    "        refractColor = backdrop(sampleBase + disp);\n"
    "      }\n"
    "    } else if (inward >= bevel || abs(I.pd.z) * (1.0 - regMix) * amount * scale <= 0.25) {\n"
    "      refractColor = backdrop(sampleBase + disp);\n"
    "    } else {\n"
    "      float ds = I.pd.z * 0.5 * (1.0 - regMix);\n"
    "      refractColor = float3(\n"
    "        backdrop(sampleBase + disp * (1.0 + ds)).r,\n"
    "        backdrop(sampleBase + disp).g,\n"
    "        backdrop(sampleBase + disp * (1.0 - ds)).b\n"
    "      );\n"
    "    }\n"
    "    if (menAA > 0.01) {\n"
    "      float aaOff = isModernDrop ? 0.25 : 0.35;\n"
    "      float2 nOff = surfaceNormal * aaOff;\n"
    "      float2 tOff = float2(-surfaceNormal.y, surfaceNormal.x) * aaOff;\n"
    "      float2 mc = sampleBase + disp;\n"
    "      float3 menAvg = (backdrop(mc + nOff + tOff) + backdrop(mc + nOff - tOff)\n"
    "                     + backdrop(mc - nOff + tOff) + backdrop(mc - nOff - tOff)) * 0.25;\n"
    "      refractColor = lerp(refractColor, menAvg, menAA);\n"
    "    }\n"
    "    float bgA = (gmisc.y > 0.5) ? saturate(backdrop4(sampleBase + disp).a) : 1.0;\n"
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
    "      transmittance = lerp(1.0, 0.95, clearVeil);\n"
    "      lift = 0.0;\n"
    "      chromaGain = lerp(1.08, 1.06, clearVeil);\n"
    "      contourStr = 0.0;\n"
    "      contourDirAmt = lerp(0.32, 0.77, clearVeil);\n"
    "      bevelShadowStr = 0.0;\n"
    "      glintLum = lerp(1.85, 1.35, clearVeil);\n"
    "      glintFaceGain = lerp(0.14, 0.25, clearVeil);\n"
    "      glintVib = lerp(2.4, 1.5, clearVeil);\n"
    "    } else {\n"
    "      [branch] if (is_adaptive) {\n"
    "#if ONE_KIND == 13\n"
    "        float2 ac = I.rect.xy + I.rect.zw * 0.5;\n"
    "        float ar = max(min(I.rect.z, I.rect.w) * 0.5, 1.0) * 0.5;\n"
    "#else\n"
    "        float2 ac = (k <= 0.0) ? (I.rect.xy + I.rect.zw * 0.5) : ((ggb.xy + ggb.zw) * 0.5);\n"
    "        float ar = ((k <= 0.0) ? max(min(I.rect.z, I.rect.w) * 0.5, 1.0) : max(min(ggb.z - ggb.x, ggb.w - ggb.y) * 0.5, 1.0)) * 0.5;\n"
    "#endif\n"
    "        float val = ad_sample(ac, ar, is_dark, col.a);\n"
    "        if (gmisc.y > 0.5) val *= bgA;\n"
    "        float baseDens = lerp(0.404, col.a, smoothstep(0.405, 0.46, col.a));\n"
    "        if (is_dark) {\n"
    "          float extra = saturate((baseDens - 0.404) / (1.0 - 0.404));\n"
    "          float brightT = val * solidFade * (1.0 - extra);\n"
    "          fogT = brightT;\n"
    "          brightTSaved = brightT;\n"
    "          regExtra = extra;\n"
    "          float darkPow = pow(extra, 1.00);\n"
    "          float darkCPow = pow(extra, 1.05);\n"
    "          float darkBump = 3.5 * smoothstep(0.0, 0.16, extra) * (1.0 - smoothstep(0.22, 0.38, extra));\n"
    "          float emBase = (34.0 + darkBump) / 255.0;\n"
    "          float3 darkEm = lerp(float3(emBase, emBase, emBase), col.rgb, pow(extra, 1.5));\n"
    "          float darkTrans = lerp(0.59, 0.0, darkPow);\n"
    "          float darkLift = lerp(0.55, 0.0, extra);\n"
    "          float darkChroma = lerp(1.05, 0.0, darkCPow);\n"
    "          emission = lerp(darkEm, float3(0.495, 0.495, 0.495), brightT);\n"
    "          transmittance = lerp(darkTrans, 0.187, brightT);\n"
    "          lift = lerp(darkLift, 0.04, brightT);\n"
    "          chromaGain = lerp(darkChroma, 0.45, brightT);\n"
    "          darkAdGlareW = (1.0 - brightT) * (1.0 - extra);\n"
    "          contourStr = 0.88; contourDirAmt = 1.0; bevelShadowStr = 0.036;\n"
    "          glintLum = 1.6; glintFaceGain = 0.0; glintVib = 2.85;\n"
    "        } else {\n"
    "          float darkMix = val * solidFade;\n"
    "          fogT = darkMix;\n"
    "          darkMixSaved = darkMix;\n"
    "          float extra = saturate((baseDens - 0.404) / (1.0 - 0.404));\n"
    "          float lightPow = pow(extra, 1.35);\n"
    "          float lightBaseDens = lerp(0.525, 1.0, lightPow);\n"
    "          float darkDens = max(baseDens, lerp(baseDens, 0.64, darkMix));\n"
    "          float darkExtra = saturate((darkDens - 0.404) / (1.0 - 0.404));\n"
    "          regExtra = lerp(extra, darkExtra, darkMix);\n"
    "          float darkPow = pow(darkExtra, 1.00);\n"
    "          float darkCPow = pow(darkExtra, 1.05);\n"
    "          float darkBump = 3.5 * smoothstep(0.0, 0.16, darkExtra) * (1.0 - smoothstep(0.22, 0.38, darkExtra));\n"
    "          float darkEmBase = (34.0 + darkBump) / 255.0;\n"
    "          float3 lightEm = lerp(float3(253.0 / 255.0, 252.0 / 255.0, 253.0 / 255.0), col.rgb, extra) * lightBaseDens;\n"
    "          float3 darkEm = lerp(float3(darkEmBase, darkEmBase, darkEmBase), float3(32.0 / 255.0, 32.0 / 255.0, 32.0 / 255.0), pow(darkExtra, 1.5));\n"
    "          float lightLift = lerp(0.04, 0.14, smoothstep(0.0, 0.16, extra)) * (1.0 - smoothstep(0.40, 0.95, extra));\n"
    "          emission = lerp(lightEm, darkEm, darkMix);\n"
    "          transmittance = lerp(1.0 - lightBaseDens, lerp(0.59, 0.0, darkPow), darkMix);\n"
    "          lift = lerp(lightLift, lerp(0.55, 0.0, darkExtra), darkMix);\n"
    "          chromaGain = lerp(lerp(1.00, 0.0, lightPow), lerp(1.05, 0.0, darkCPow), darkMix);\n"
    "          contourStr = lerp(0.43, 0.88, darkMix);\n"
    "          contourDirAmt = lerp(0.77, 1.0, darkMix);\n"
    "          bevelShadowStr = 0.036;\n"
    "          glintLum = 1.6; glintFaceGain = 0.0; glintVib = 2.85;\n"
    "        }\n"
    "      } else if (is_dark) {\n"
    "        float darkDens = lerp(0.404, col.a, smoothstep(0.405, 0.46, col.a));\n"
    "        float extra = saturate((darkDens - 0.404) / (1.0 - 0.404));\n"
    "        regExtra = extra;\n"
    "        float darkPow = pow(extra, 1.00);\n"
    "        float darkCPow = pow(extra, 1.05);\n"
    "        float darkBump = 3.5 * smoothstep(0.0, 0.16, extra) * (1.0 - smoothstep(0.22, 0.38, extra));\n"
    "        float emBase = (34.0 + darkBump) / 255.0;\n"
    "        emission = lerp(float3(emBase, emBase, emBase), col.rgb, pow(extra, 1.5));\n"
    "        transmittance = lerp(0.59, 0.0, darkPow);\n"
    "        lift = lerp(0.55, 0.0, extra);\n"
    "        chromaGain = lerp(1.05, 0.0, darkCPow);\n"
    "        contourStr = 0.88; contourDirAmt = 1.0; bevelShadowStr = 0.036;\n"
    "        glintLum = 1.6; glintFaceGain = 0.0; glintVib = 2.85;\n"
    "      } else {\n"
    "        float baseDens = lerp(0.404, col.a, smoothstep(0.405, 0.46, col.a));\n"
    "        float extra = saturate((baseDens - 0.404) / (1.0 - 0.404));\n"
    "        regExtra = extra;\n"
    "        float lightPow = pow(extra, 1.35);\n"
    "        float lightDens = lerp(0.525, 1.0, lightPow);\n"
    "        emission = lerp(float3(253.0 / 255.0, 252.0 / 255.0, 253.0 / 255.0), col.rgb, extra) * lightDens;\n"
    "        transmittance = 1.0 - lightDens;\n"
    "        lift = lerp(0.04, 0.14, smoothstep(0.0, 0.16, extra)) * (1.0 - smoothstep(0.40, 0.95, extra));\n"
    "        chromaGain = lerp(1.00, 0.0, lightPow);\n"
    "        contourStr = 0.43; contourDirAmt = 0.77; bevelShadowStr = 0.036;\n"
    "        glintLum = 1.6; glintFaceGain = 0.0; glintVib = 2.85;\n"
    "      }\n"
    "    }\n"
    "    [branch] if (regMix > 0.001 && fogT > 0.001) {\n"
    "      float2 fc = sampleBase + disp;\n"
    "      float rD = lerp(3.2, 4.2, fogT);\n"
    "      float3 s1 = backdrop(fc + float2( rD,  rD));\n"
    "      float3 s2 = backdrop(fc + float2(-rD,  rD));\n"
    "      float3 s3 = backdrop(fc + float2( rD, -rD));\n"
    "      float3 s4 = backdrop(fc + float2(-rD, -rD));\n"
    "      float3 lowC = (refractColor * 2.0 + s1 + s2 + s3 + s4) * (1.0 / 6.0);\n"
    "      float interior = (bevel > 0.001) ? smoothstep(0.30 * bevel, 0.85 * bevel, inward) : 1.0;\n"
    "      float dampW = interior * (fogT * 0.72) * regMix;\n"
    "      refractColor = lerp(refractColor, lowC, dampW);\n"
    "    }\n"
    "    if (regMix > 0.0 && regMix < 1.0) {\n"
    "      emission = lerp(float3(0.1255, 0.1255, 0.1255), emission, regMix);\n"
    "      transmittance = lerp(0.95, transmittance, regMix);\n"
    "      lift = lerp(0.0, lift, regMix);\n"
    "      chromaGain = lerp(1.06, chromaGain, regMix);\n"
    "      contourStr = lerp(0.0, contourStr, regMix);\n"
    "      contourDirAmt = lerp(0.77, contourDirAmt, regMix);\n"
    "      bevelShadowStr = lerp(0.0, bevelShadowStr, regMix);\n"
    "      glintLum = lerp(1.35, glintLum, regMix);\n"
    "      glintFaceGain = lerp(0.25, glintFaceGain, regMix);\n"
    "      glintVib = lerp(1.5, glintVib, regMix);\n"
    "    }\n"
    "    float3 rawRefractColor = refractColor;\n"
    "    float3 extRimEnv = refractColor;\n"
    "    float extRimValid = 0.0;\n"
    "    float extBlurT = 0.0;\n"
    "    float extDensFade = 1.0 - smoothstep(0.35, 0.85, regExtra);\n"
    "    float extRadLen = length(extSpineVec);\n"
    "    float2 extRadDir = extRadLen > 0.001 ? extSpineVec / extRadLen : float2(0.0, 1.0);\n"
    "    [branch] if (regMix > 0.001 && solidFade > 0.001 && extDensFade > 0.001 && colChroma < 0.16 && inward < 76.0) {\n"
    "      float extBlur = max(I.prm.w, 0.0);\n"
    "      float extRefrSigned = min(I.pd.y, I.pd.y * (1.0 + abs(I.pd.z)));\n"
    "      float extRefrGate = smoothstep(-1.0, 0.0, I.pd.y);\n"
    "      float extMaxReach = max(3.0 * extBlur + extRefrSigned - 2.0, 0.0) * extRefrGate;\n"
    "      [branch] if (extMaxReach > 1.0) {\n"
    "        extBlurT = smoothstep(2.0, 10.0, extBlur);\n"
    "        float extBaseValid = smoothstep(1.0, 4.0, extMaxReach) * extDensFade * extRefrGate;\n"
    "        float extTintGate = 1.0 - smoothstep(0.08, 0.16, colChroma);\n"
    "        float diffTail = exp(-max(inward - 2.0, 0.0) / 30.0) * (1.0 - smoothstep(52.0, 76.0, inward));\n"
    "        float radCenterGate = smoothstep(0.18, 0.55, extRadLen / max(halfMinor, 1.0));\n"
    "        float wideGate = smoothstep(4.0, 10.0, extBlur) * smoothstep(24.0, 48.0, halfMinor) * diffTail * radCenterGate;\n"
    "        [branch] if (wideGate > 0.001) {\n"
    "          float2 rDir = extRadDir;\n"
    "          float2 tDir = float2(-rDir.y, rDir.x);\n"
    "          float dOutNear = inward + min(lerp(10.0, 22.0, extBlurT), extMaxReach * 0.60);\n"
    "          float dOutFar  = inward + min(lerp(18.0, 40.0, extBlurT), extMaxReach * 0.88);\n"
    "          float spread   = min(14.0 + 0.36 * inward, extMaxReach * 0.45);\n"
    "          float dBack    = min(lerp(10.0, 22.0, extBlurT), extMaxReach * 0.45);\n"
    "          float3 wNear = backdrop(p + rDir * dOutNear);\n"
    "          float3 wFarL = backdrop(p + rDir * dOutFar - tDir * spread);\n"
    "          float3 wFarR = backdrop(p + rDir * dOutFar + tDir * spread);\n"
    "          float3 wBack = backdrop(p - rDir * dBack);\n"
    "          float3 wideAmb = wNear * 0.24 + (wFarL + wFarR) * 0.32 + wBack * 0.12;\n"
    "          float ambDelta = smoothstep(0.008, 0.052, length(wideAmb - refractColor));\n"
    "          float ambY = luma(wideAmb);\n"
    "          float3 ambCh = wideAmb - ambY.xxx;\n"
    "          float ambChLen = length(ambCh);\n"
    "          float refY = luma(refractColor);\n"
    "          float refChLen = length(refractColor - refY.xxx);\n"
    "          float neutralRefGate = 1.0 - smoothstep(0.06, 0.16, refChLen);\n"
    "          float regDarkAmb = is_dark ? (1.0 - brightTSaved) : darkMixSaved;\n"
    "          float lightHeadroom = min(refY + 0.036 * diffTail * smoothstep(0.01, 0.08, ambChLen) * neutralRefGate, lerp(0.995, 0.945, smoothstep(0.015, 0.08, ambChLen)));\n"
    "          float lightAmbY = clamp(lightHeadroom, 0.0, 0.995);\n"
    "          float3 lightAmbD = ambCh * lerp(1.00, 1.48, diffTail * neutralRefGate);\n"
    "          float sLA = 1.0;\n"
    "          if (lightAmbD.r > 0.0001) sLA = min(sLA, (1.0 - lightAmbY) / lightAmbD.r); else if (lightAmbD.r < -0.0001) sLA = min(sLA, lightAmbY / (-lightAmbD.r));\n"
    "          if (lightAmbD.g > 0.0001) sLA = min(sLA, (1.0 - lightAmbY) / lightAmbD.g); else if (lightAmbD.g < -0.0001) sLA = min(sLA, lightAmbY / (-lightAmbD.g));\n"
    "          if (lightAmbD.b > 0.0001) sLA = min(sLA, (1.0 - lightAmbY) / lightAmbD.b); else if (lightAmbD.b < -0.0001) sLA = min(sLA, lightAmbY / (-lightAmbD.b));\n"
    "          float3 lightAmbTarget = saturate(lightAmbY.xxx + lightAmbD * saturate(sLA));\n"
    "          float darkAmbY = clamp(lerp(refY, ambY, 0.65) + 0.024 * diffTail * smoothstep(0.01, 0.08, ambChLen), 0.0, 0.995);\n"
    "          float3 darkAmbD = ambCh * lerp(0.90, 1.12, diffTail);\n"
    "          float sDA = 1.0;\n"
    "          if (darkAmbD.r > 0.0001) sDA = min(sDA, (1.0 - darkAmbY) / darkAmbD.r); else if (darkAmbD.r < -0.0001) sDA = min(sDA, darkAmbY / (-darkAmbD.r));\n"
    "          if (darkAmbD.g > 0.0001) sDA = min(sDA, (1.0 - darkAmbY) / darkAmbD.g); else if (darkAmbD.g < -0.0001) sDA = min(sDA, darkAmbY / (-darkAmbD.g));\n"
    "          if (darkAmbD.b > 0.0001) sDA = min(sDA, (1.0 - darkAmbY) / darkAmbD.b); else if (darkAmbD.b < -0.0001) sDA = min(sDA, darkAmbY / (-darkAmbD.b));\n"
    "          float3 darkAmbTarget = saturate(darkAmbY.xxx + darkAmbD * saturate(sDA));\n"
    "          float3 ambTarget = lerp(lightAmbTarget, darkAmbTarget, regDarkAmb);\n"
    "          float ambStr = lerp(0.48, 0.30, regDarkAmb) * neutralRefGate;\n"
    "          float ambW = ambStr * wideGate * extBaseValid * extTintGate * solidFade * regMix * ambDelta * bgA;\n"
    "          refractColor = lerp(refractColor, ambTarget, ambW);\n"
    "        }\n"
    "        [branch] if (need_grad && inward < 1.3) {\n"
    "          float2 rimDirRaw = lerp(surfaceNormal, extRadDir, 0.36 * extBlurT);\n"
    "          float rimDirLen = length(rimDirRaw);\n"
    "          float2 rimDir = rimDirLen > 0.001 ? rimDirRaw / rimDirLen : surfaceNormal;\n"
    "          float2 pEdge = p + surfaceNormal * inward;\n"
    "          float dRimNear = min(lerp(6.5, 18.0, extBlurT), extMaxReach);\n"
    "          float dRimFar  = min(lerp(10.0, 38.0, extBlurT), extMaxReach);\n"
    "          float3 rNear = backdrop(pEdge + rimDir * dRimNear);\n"
    "          float3 rFar  = backdrop(pEdge + rimDir * dRimFar);\n"
    "          extRimEnv = lerp(rNear, rFar, lerp(0.42, 0.72, extBlurT));\n"
    "          extRimValid = extBaseValid * shortening * bgA;\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    float tintDarkEnv = (1.0 - smoothstep(0.18, 0.55, luma(refractColor))) * bgA;\n"
    "    float regDarkContour = tintDarkEnv * max(is_dark ? 1.0 : darkMixSaved, smoothstep(0.08, 0.16, colChroma));\n"
    "    float regExtContour = lerp(0.34, 0.85, regDarkContour) * solidFade;\n"
    "    contourStr = lerp(contourStr, regExtContour, regMix);\n"
    "    float Y = luma(refractColor);\n"
    "    float regDarkOpt = is_dark ? 1.0 : darkMixSaved;\n"
    "    float transLum = saturate(Y * (1.0 + lift * (1.0 - Y)));\n"
    "    float Lt = saturate(transLum * transmittance);\n"
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
    "    float3 chromaVec = (refractColor - Y.xxx) * (chromaGain * I.prm.z * kneeScale);\n"
    "    if (darkAdGlareW > 0.001) {\n"
    "      if (baseLum > 0.18) {\n"
    "        float over = baseLum - 0.18;\n"
    "        float capped = 0.18 + 0.115 * (over / (over + 0.115));\n"
    "        float newLum = lerp(baseLum, capped, darkAdGlareW);\n"
    "        chromaVec *= (newLum / max(baseLum, 0.0001));\n"
    "        baseLum = newLum;\n"
    "      }\n"
    "      float3 testRaw = emission + baseLum.xxx + chromaVec;\n"
    "      float peak = max(testRaw.r, max(testRaw.g, testRaw.b));\n"
    "      if (peak > 0.44) {\n"
    "        float lum0 = luma(emission) + baseLum;\n"
    "        float s = saturate((0.44 - lum0) / max(peak - lum0, 0.0001));\n"
    "        chromaVec *= lerp(1.0, s, darkAdGlareW);\n"
    "      }\n"
    "    }\n"
    "    float3 transmittedColor = baseLum.xxx + chromaVec;\n"
    "    float3 rawBase = emission + transmittedColor;\n"
    "    // Exact Wave 3 legacy mapping for zero-veil lenses (col.a == 0); channel-clamped saturation for Clear glass\n"
    "    float maxC = max(rawBase.r, max(rawBase.g, rawBase.b));\n"
    "    float3 legacyBase = rawBase;\n"
    "    if (maxC > 1.0) {\n"
    "      float lumL = clamp(luma(emission) + baseLum, 0.0, 0.999);\n"
    "      float sL = saturate((1.0 - lumL) / max(maxC - lumL, 0.0001));\n"
    "      legacyBase = lumL.xxx + (rawBase - lumL.xxx) * sL;\n"
    "    }\n"
    "    legacyBase = saturate(legacyBase);\n"
    "    float clearClampW = smoothstep(0.005, 0.102, col.a);\n"
    "    float3 baseColor = lerp(legacyBase, saturate(rawBase), clearClampW);\n"
    "    if (regMix > 0.001) {\n"
    "      float lumR = clamp(luma(emission) + baseLum, 0.001, 0.999);\n"
    "      float3 dC = rawBase - lumR.xxx;\n"
    "      float s = 1.0;\n"
    "      if (dC.r > 0.0001) s = min(s, (1.0 - lumR) / dC.r);\n"
    "      else if (dC.r < -0.0001) s = min(s, lumR / (-dC.r));\n"
    "      if (dC.g > 0.0001) s = min(s, (1.0 - lumR) / dC.g);\n"
    "      else if (dC.g < -0.0001) s = min(s, lumR / (-dC.g));\n"
    "      if (dC.b > 0.0001) s = min(s, (1.0 - lumR) / dC.b);\n"
    "      else if (dC.b < -0.0001) s = min(s, lumR / (-dC.b));\n"
    "      float3 regProjected = saturate(lumR.xxx + dC * saturate(s));\n"
    "      float projMix = lerp(0.25, lerp(0.08, 0.35, regDarkOpt), smoothstep(0.08, 0.16, regExtra));\n"
    "      float3 regMapped = lerp(saturate(rawBase), regProjected, projMix);\n"
    "      baseColor = lerp(baseColor, regMapped, regMix);\n"
    "    }\n"
    "    float holeTrans = is_dark ? min(transmittance, lerp(0.295, 0.247, darkKneeW)) : transmittance;\n"
    "    float ga = (gmisc.y > 0.5) ? (1.0 - (1.0 - bgA) * saturate(holeTrans)) : 1.0;\n"
    "    // Honor explicit chromatic tints (non-neutral) if a caller requests colored liquid glass\n"
    "    if (colChroma > 0.08 && col.a > 0.001) {\n"
    "      float tintW = col.a <= 0.40\n"
    "        ? col.a * 1.15\n"
    "        : lerp(0.46, 0.98, smoothstep(0.40, 0.58, col.a));\n"
    "      baseColor = lerp(baseColor, col.rgb, tintW);\n"
    "      ga = lerp(ga, 1.0, tintW);\n"
    "    }\n"
    "    float spot = 0.0;\n"
    "    if (spot_raw > 0.5) {\n"
    "      float q = spot_raw - 1.0;\n"
    "      float qx = floor(q * 0.000244140625);\n"
    "      float qy = q - qx * 4096.0;\n"
    "      float2 gp = I.rect.xy + float2(qx * 0.5 - 256.0, qy * 0.5 - 256.0);\n"
    "      float gr = max(min(I.rect.z, I.rect.w) * 1.5, 64.0);\n"
    "      spot = 1.0 - smoothstep(gr * 0.1, gr, length(p - gp));\n"
    "      baseColor = saturate(baseColor + (0.08 * spot).xxx);\n"
    "    }\n"
    "    float2 rn = float2(0.0, 0.0);\n"
    "    float tangency = 0.0;\n"
    "    [branch] if (need_grad) {\n"
    "      float2 la = gmisc.zw;\n"
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
    "      float bevelDir = lerp(1.0, wrappedFacing, 0.5);\n"
    "      float bevelShadow = saturate(bevelLeading * bevelFalloff * bevelDir * bevelShadowStr);\n"
    "      result = max(result - transmittedColor * bevelShadow, float3(0.0, 0.0, 0.0));\n"
    "    }\n"
    "    // (b) Glint (clear glass top/bottom specular rim + unified bevel wall for Glass::clear)\n"
    "    float clearVeilC = smoothstep(0.005, 0.102, col.a);\n"
    "    if (I.c2.x > 0.01 && inward < 6.0 && regMix < 0.999) {\n"
    "      float glintProfile = lerp(\n"
    "                         saturate(1.0 - inward / 1.2) + 0.21 * saturate(1.0 - inward / 4.8),\n"
    "                         saturate(1.0 - inward / 1.35) + 0.24 * saturate(1.0 - inward / 5.5),\n"
    "                         clearVeilC);\n"
    "      float dirLobe = pow(max(1.0 - tangency, 0.0), lerp(0.85, 1.35, clearVeilC));\n"
    "      float lobe = lerp(lerp(0.30, 0.08, clearVeilC), 1.0, dirLobe);\n"
    "      float glintGain = 0.58 * clearVeilC;\n"
    "      float glint = saturate(I.c2.x * glintGain * lobe * glintProfile + spot * 0.08 * glintProfile) * (1.0 - regMix);\n"
    "      float resLum = luma(result);\n"
    "      float3 glintTarget = glintLum.xxx + result * glintFaceGain + (result - resLum.xxx) * glintVib;\n"
    "      result = saturate(lerp(result, glintTarget, glint));\n"
    "      ga = lerp(ga, 1.0, glint);\n"
    "    }\n"
    "    float clrWallW = clearVeilC * (1.0 - regMix);\n"
    "    if (clrWallW > 0.001) {\n"
    "      float clrAcross = pow(max(1.0 - rn.y * rn.y, 0.0), 1.8);\n"
    "      float clrInvS = 1.0 / max(scale, 1.0);\n"
    "      float clrW0 = lerp(max(0.20, 0.50 * clrInvS), max(0.28, 0.75 * clrInvS), clrAcross);\n"
    "      float clrL  = lerp(0.06, 0.12, clrAcross);\n"
    "      float clrCov = mb_bevel_cov(sd_px, scale, clrW0, clrL);\n"
    "      float clrHair = saturate(clrCov / max(materialAlpha, 0.001)) * clrAcross * clrWallW;\n"
    "      if (clrHair > 0.001) {\n"
    "        float3 clrEnv = backdrop(p + surfaceNormal * 1.2);\n"
    "        float clrBrightGate = smoothstep(0.55, 0.90, luma(clrEnv)) * 0.55;\n"
    "        float3 clrWall = pow(max(clrEnv, float3(1e-6, 1e-6, 1e-6)), float3(1.75, 1.75, 1.75)) * 0.695;\n"
    "        result = lerp(result, clrWall, clrHair * clrBrightGate);\n"
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
    "        float regLobe = 1.0 * (lit2 * lit2) + lerp(0.96, 0.94, regDark) * (opp2 * opp2) + lerp(0.22, 0.30, regDark) * (diagPos * diagPos) + lerp(0.18, 0.24, regDark) * (diagNeg * diagNeg) + 0.06 * regDark;\n"
    "        float regCore = exp(-max(0.0, inward - 0.15) / 0.55);\n"
    "        float regTailLight = exp(-inward / 2.2);\n"
    "        float regTailDark  = exp(-inward / 3.8);\n"
    "        float regTail = lerp(regTailLight, regTailDark, regDark);\n"
    "        float resY = luma(result);\n"
    "        float regEaseLight = 1.0 / max(1.0 - 0.75 * resY, 0.25);\n"
    "        float regEase = lerp(regEaseLight, pow(max(1.0 - resY, 0.0), 0.25), regDarkRim);\n"
    "        float regRimGain = 1.0 * clamp(I.c2.x / 0.6, 0.0, 1.5);\n"
    "        float regCoreW = lerp(0.21, 0.24, regDark) * (1.0 - tintChroma);\n"
    "        float regTailW = lerp(0.030, 0.055, regDark) * (1.0 - tintChroma);\n"
    "        float lipY = resY;\n"
    "        if (inward < 3.0 && regDarkRim < 0.999 && tintChroma < 0.999) {\n"
    "          float2 tang = float2(-surfaceNormal.y, surfaceNormal.x) * 3.0;\n"
    "          float3 lt1 = backdrop(sampleBase + disp + tang);\n"
    "          float3 lt2 = backdrop(sampleBase + disp - tang);\n"
    "          float avgBgY = (Y + luma(lt1) + luma(lt2)) * (1.0 / 3.0);\n"
    "          lipY = saturate(luma(emission) + saturate(avgBgY * (1.0 + lift * (1.0 - avgBgY)) * transmittance));\n"
    "        }\n"
    "        float brightLip = smoothstep(0.77, 0.92, lipY) * (1.0 - regDarkRim) * (1.0 - tintChroma);\n"
    "        float lipProfile = exp(-max(0.0, inward - 0.70) / 0.55);\n"
    "        float regRim = (regRimGain * regLobe * (regEase * (regCoreW * regCore + regTailW * regTail) + 0.72 * brightLip * lipProfile) + spot * 0.10 * regCore) * solidFade * regMix;\n"
    "        float regDarkEff = is_dark ? (1.0 - brightTSaved) : darkMixSaved;\n"
    "        float extRimCoreMask = 1.0 - smoothstep(0.55, 0.95, inward);\n"
    "        float3 rimSrc = lerp(refractColor, extRimEnv, extRimValid * extRimCoreMask * (1.0 - tintChroma));\n"
    "        float rimSrcY = luma(rimSrc);\n"
    "        float3 rimSrcChroma = rimSrc - rimSrcY.xxx;\n"
    "        float rimPeak = max(0.0, max(rimSrcChroma.r, max(rimSrcChroma.g, rimSrcChroma.b)));\n"
    "        float extRimW = extRimValid * extRimCoreMask * (1.0 - tintChroma);\n"
    "        float3 rimTarget = saturate((1.0 - rimPeak * 0.25 * extRimW).xxx + rimSrcChroma * lerp(1.0, lerp(1.12, 1.40, regDarkEff), extRimW));\n"
    "        float3 neutralRimRes = lerp(result, rimTarget, saturate(regRim));\n"
    "        float gaRim = lerp(ga, 1.0, saturate(regRim));\n"
    "        if (extRimValid > 0.001 && inward < 1.2 && tintChroma < 0.999) {\n"
    "          float extRimGate = smoothstep(0.0, 0.6, I.c2.x);\n"
    "          float extDiff = smoothstep(0.02, 0.18, length(extRimEnv - rawRefractColor));\n"
    "          float extLip = exp(-inward / 0.28) * (1.0 - smoothstep(0.65, 1.05, inward));\n"
    "          float extDir = lerp(0.62, 1.0, min(abs(rn.y) * 1.15, 1.0));\n"
    "          float extY = luma(extRimEnv);\n"
    "          float3 extCh = extRimEnv - extY.xxx;\n"
    "          float lightLum = clamp(lerp(0.985, extY, lerp(0.24, 0.14, extBlurT)), 0.84, 0.985);\n"
    "          float3 lightD = extCh * lerp(0.95, 0.55, extBlurT);\n"
    "          float sL = 1.0;\n"
    "          if (lightD.r > 0.0001) sL = min(sL, (1.0 - lightLum) / lightD.r); else if (lightD.r < -0.0001) sL = min(sL, lightLum / (-lightD.r));\n"
    "          if (lightD.g > 0.0001) sL = min(sL, (1.0 - lightLum) / lightD.g); else if (lightD.g < -0.0001) sL = min(sL, lightLum / (-lightD.g));\n"
    "          if (lightD.b > 0.0001) sL = min(sL, (1.0 - lightLum) / lightD.b); else if (lightD.b < -0.0001) sL = min(sL, lightLum / (-lightD.b));\n"
    "          float3 extReflLight = saturate(lightLum.xxx + lightD * saturate(sL));\n"
    "          float darkLum = clamp(extY * 1.28 + 0.16, 0.0, 0.90);\n"
    "          float3 darkD = extCh * lerp(2.15, 2.75, extBlurT);\n"
    "          float sD = 1.0;\n"
    "          if (darkD.r > 0.0001) sD = min(sD, (1.0 - darkLum) / darkD.r); else if (darkD.r < -0.0001) sD = min(sD, darkLum / (-darkD.r));\n"
    "          if (darkD.g > 0.0001) sD = min(sD, (1.0 - darkLum) / darkD.g); else if (darkD.g < -0.0001) sD = min(sD, darkLum / (-darkD.g));\n"
    "          if (darkD.b > 0.0001) sD = min(sD, (1.0 - darkLum) / darkD.b); else if (darkD.b < -0.0001) sD = min(sD, darkLum / (-darkD.b));\n"
    "          float3 extReflDark = saturate(darkLum.xxx + darkD * saturate(sD));\n"
    "          float3 extRefl = lerp(extReflLight, extReflDark, regDarkEff);\n"
    "          float extMixW = clamp(lerp(lerp(0.65, 0.52, extBlurT), lerp(0.88, 0.86, extBlurT), regDarkEff) * extRimGate * extDir * extLip * extDiff * extRimValid * solidFade * regMix * (1.0 - tintChroma), 0.0, 0.90);\n"
    "          neutralRimRes = lerp(neutralRimRes, extRefl, extMixW);\n"
    "          gaRim = lerp(gaRim, 1.0, extMixW);\n"
    "        }\n"
    "        if (tintChroma > 0.001) {\n"
    "          float tintCore = exp(-inward / 0.80);\n"
    "          float tintTail = exp(-inward / 3.2);\n"
    "          float tintRimAmt = saturate(regRimGain * regLobe * (0.95 * tintCore + 0.12 * tintTail) * solidFade * regMix);\n"
    "          float3 rLin = pow(max(result, float3(0.0, 0.0, 0.0)), float3(2.2, 2.2, 2.2));\n"
    "          float brightTint = smoothstep(0.40, 0.70, resY);\n"
    "          float tintAdd = lerp(0.007, 0.002, brightTint) * tintRimAmt;\n"
    "          float3 tintLit = pow(saturate(rLin * (1.0 + lerp(1.05, 0.65, brightTint) * tintRimAmt) + tintAdd.xxx), float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));\n"
    "          result = lerp(neutralRimRes, tintLit, tintChroma);\n"
    "          ga = lerp(gaRim, lerp(ga, 1.0, tintRimAmt), tintChroma);\n"
    "        } else {\n"
    "          result = neutralRimRes;\n"
    "          ga = gaRim;\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    if (isModernDrop) {\n"
    "      float backdropLuma = saturate(Y);\n"
    "      float brightBg = smoothstep(0.35, 0.75, backdropLuma);\n"
    "      float liftGain = isRoleTab ? 0.025 : (isRoleSeg ? 0.014 : 0.098);\n"
    "      float liftAdd = liftGain * pressAmt * (1.0 - backdropLuma);\n"
    "      float3 dropRes = saturate(refractColor + liftAdd.xxx);\n"
    "      float domeScale = clamp(I.rect.w / 38.4, 0.80, 1.35);\n"
    "      float sizeGate = smoothstep(28.0, 36.0, I.rect.w);\n"
    "      float roleVol = isRoleSeg ? 0.65 : (isRoleTab ? 0.35 : 1.0);\n"
    "      float upperFace = smoothstep(-0.15, 0.85, -surfaceNormal.y);\n"
    "      float qTop = (inward - 8.5 * domeScale) / (6.2 * domeScale);\n"
    "      float topEnv = exp(-qTop * qTop) * upperFace * smoothstep(0.55, 1.80, inward);\n"
    "      dropRes *= (1.0 - lerp(0.20, 0.082, brightBg) * roleVol * sizeGate * pressAmt * topEnv);\n"
    "      float sideArc = pow(max(abs(surfaceNormal.x), 0.0), 1.6);\n"
    "      float sideEnv = exp(-max(inward - 1.5, 0.0) / 4.2) * smoothstep(0.25, 1.20, inward);\n"
    "      dropRes *= (1.0 - lerp(0.16, 0.065, brightBg) * (isRoleSeg ? 0.55 : (isRoleTab ? 0.25 : 1.0)) * sizeGate * pressAmt * sideArc * sideEnv);\n"
    "      float lowerFace = smoothstep(0.10, 0.85, surfaceNormal.y);\n"
    "      float lowEnv = exp(-max(inward - 2.0, 0.0) / 5.5) * smoothstep(0.50, 1.60, inward);\n"
    "      float lowAdd = lerp(0.032, 0.025, brightBg) * lowerFace * lowEnv * (isRoleSeg ? 0.45 : (isRoleTab ? 0.30 : 1.0)) * sizeGate * pressAmt;\n"
    "      dropRes = saturate(dropRes + lowAdd.xxx);\n"
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
    "        float rimStr = (isRoleTab ? lerp(0.28, 0.16, brightBg) : lerp(0.24, 0.14, brightBg)) * dirLobe * rimEnv * pressAmt;\n"
    "        dropRes = lerp(dropRes, float3(1.0, 1.0, 1.0), saturate(rimStr));\n"
    "      }\n"
    "      // Stage B: Slim Apple-exact closed-form analytic 3D bevel wall\n"
    "      float across = pow(max(1.0 - surfaceNormal.y * surfaceNormal.y, 0.0), 1.35);\n"
    "      float invS = 1.0 / max(scale, 1.0);\n"
    "      float hsWall = clamp(halfMinor / 19.2, 0.72, 1.25);\n"
    "      float w0_top = (isRoleSw ? 0.30 : (isRoleTab ? 0.18 : 0.24)) * hsWall;\n"
    "      float w0_side = isRoleTab ? max(0.30 * hsWall, 1.10 * invS) : max(0.52 * hsWall, 1.55 * invS);\n"
    "      float w0_eff = lerp(w0_top, w0_side, across);\n"
    "      float L_eff  = (isRoleTab ? lerp(0.08, 0.16, across) : lerp(0.12, 0.42, across)) * hsWall;\n"
    "      float wallCov = mb_bevel_cov(sd_px, scale, w0_eff, L_eff);\n"
    "      float wallRaw = saturate(wallCov / max(materialAlpha, 0.001));\n"
    "      float wallBlend = wallRaw * (isRoleTab ? 0.82 : 0.94) * pressAmt;\n"
    "      if (wallBlend > 0.001) {\n"
    "        float3 envBg = backdrop(p + surfaceNormal * 1.2);\n"
    "        float envY = luma(envBg);\n"
    "        float envChroma = max(envBg.r, max(envBg.g, envBg.b)) - min(envBg.r, min(envBg.g, envBg.b));\n"
    "        float brightEnv = smoothstep(0.35, 0.75, envY);\n"
    "        float satEnv = smoothstep(0.10, 0.35, envChroma);\n"
    "        float wallMix = isRoleTab ? (wallRaw * lerp(0.62, 0.82, brightEnv) * pressAmt) : wallBlend;\n"
    "        float neutralMul = isRoleTab ? lerp(0.50, lerp(0.82, 0.75, across), brightEnv) : lerp(0.26, lerp(0.80, 0.72, across), brightEnv);\n"
    "        float3 neutralTint = envBg * neutralMul;\n"
    "        float3 satTint = pow(max(envBg, float3(0.0001, 0.0001, 0.0001)), float3(1.25, 1.25, 1.25)) * 0.72;\n"
    "        dropRes = lerp(dropRes, lerp(neutralTint, satTint, satEnv), wallMix);\n"
    "      }\n"
    "      // Stage C: Segmented prismatic edge dispersion AFTER Stage B\n"
    "      if (pressAmt > 0.001 && inward < 12.0 && abs(I.pd.z) > 0.005) {\n"
    "        float chromaGate = smoothstep(0.005, 0.04, abs(I.pd.z));\n"
    "        float pBand = smoothstep(0.45, 0.75, inward) * (1.0 - smoothstep(1.35, 2.20, inward)) * pressAmt * chromaGate;\n"
    "        float hw = smoothstep(0.35, 0.85, abs(surfaceNormal.x));\n"
    "        float3 pDark = lerp(float3(-0.025, 0.015, 0.045), float3(0.048, 0.012, -0.032), hw);\n"
    "        float3 pLight = lerp(float3(-0.085, 0.0, 0.0), float3(-0.015, -0.075, -0.135), hw);\n"
    "        float3 pAdd = lerp(pDark, pLight, brightBg);\n"
    "        dropRes = saturate(dropRes + pAdd * pBand);\n"
    "      }\n"
    "      result = lerp(result, dropRes, droplet);\n"
    "    } else if (is_drop) {\n"
    "      float backdropLuma = saturate(Y);\n"
    "      float brightBg = smoothstep(0.35, 0.75, backdropLuma);\n"
    "      float liftGain = isRoleTab ? 0.025 : (isRoleSeg ? 0.008 : 0.101);\n"
    "      float lensLift = liftGain * pressAmt * (1.0 - backdropLuma) * (1.0 - dropHairline);\n"
    "      result = saturate(result + lensLift.xxx);\n"
    "      float domeScale = clamp(I.rect.w / 38.0, 0.85, 1.35);\n"
    "      float sizeGate = smoothstep(28.0, 36.0, I.rect.w);\n"
    "      float upperFace = smoothstep(0.0, 0.75, -surfaceNormal.y);\n"
    "      float qDome = (inward - 9.2 * domeScale) / (5.6 * domeScale);\n"
    "      float domeEnv = exp(-qDome * qDome) * upperFace * smoothstep(0.65, 1.85, inward);\n"
    "      float domeStr = lerp(0.196, 0.086, brightBg) * sizeGate;\n"
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
    "        float ease = pow(max(1.0 - saturate(luma(result)), 0.0), 0.30);\n"
    "        float rimGain = isRoleSeg ? 0.85 : (isRoleTab ? 0.75 : 1.0);\n"
    "        float rimLight = pressAmt * rimGain * lobe * ease * (0.205 * core + 0.025 * shoulder) * (1.0 - dropHairline);\n"
    "        result = lerp(result, float3(1.0, 1.0, 1.0), saturate(rimLight));\n"
    "      }\n"
    "      if (isRoleSeg && pressAmt > 0.01) {\n"
    "        float prismBand = smoothstep(0.40, 0.80, inward) * (1.0 - smoothstep(1.15, 1.85, inward))\n"
    "                        * (1.0 - dropHairline) * pressAmt;\n"
    "        if (prismBand > 0.001) {\n"
    "          float hW = smoothstep(0.35, 0.85, abs(surfaceNormal.x));\n"
    "          float3 rawD = lerp(float3(-0.045, 0.015, 0.030), float3(0.075, -0.015, -0.060), hW);\n"
    "          float rawDL = luma(rawD);\n"
    "          float3 darkPrism = (rawD - rawDL.xxx) * 0.60 * prismBand;\n"
    "          float3 lightPrism = lerp(float3(-0.068, 0.0, 0.0), float3(-0.055, -0.148, -0.188), hW) * prismBand;\n"
    "          float3 pD = lerp(darkPrism, lightPrism, brightBg);\n"
    "          float sP = 1.0;\n"
    "          if (pD.r > 0.0001) sP = min(sP, (1.0 - result.r) / pD.r); else if (pD.r < -0.0001) sP = min(sP, result.r / (-pD.r));\n"
    "          if (pD.g > 0.0001) sP = min(sP, (1.0 - result.g) / pD.g); else if (pD.g < -0.0001) sP = min(sP, result.g / (-pD.g));\n"
    "          if (pD.b > 0.0001) sP = min(sP, (1.0 - result.b) / pD.b); else if (pD.b < -0.0001) sP = min(sP, result.b / (-pD.b));\n"
    "          result = saturate(result + pD * saturate(sP));\n"
    "        }\n"
    "      }\n"
    "      if (dropHairline > 0.001) {\n"
    "        float across = pow(max(1.0 - surfaceNormal.y * surfaceNormal.y, 0.0), 1.75);\n"
    "        float shadeTB = lerp(0.047, 0.161, brightBg);\n"
    "        float shadeEnds = lerp(0.063, 0.239, brightBg);\n"
    "        float shade = lerp(shadeTB, shadeEnds, across) * pressAmt;\n"
    "        result = max(result - (shade * dropHairline).xxx, float3(0.0, 0.0, 0.0));\n"
    "      }\n"
    "    }\n"
    "    result = saturate(result + mb_dither(p * scale) / 255.0);\n"
    "    // (c) External dark contour (iOS 27.0: Regular controls place their bevel contour strictly outside the silhouette so circles keep full geometric width)\n"
    "    float extUseCov = ext_cov;\n"
    "    float contourDir = lerp(1.0, tangency, contourDirAmt);\n"
    "    if (regMix > 0.001 && solidFade > 0.001) {\n"
    "      float extAcross = pow(max(1.0 - rn.y * rn.y, 0.0), lerp(1.6, 2.2, regDarkContour));\n"
    "      float extRegDir = lerp(lerp(0.30, 1.0, extAcross), extAcross, regDarkContour);\n"
    "      float extW0 = lerp(0.28, 0.36, extAcross);\n"
    "      float extL  = lerp(0.08, 0.14, extAcross);\n"
    "      float regExtCov = mb_bevel_cov(outward, scale, extW0, extL);\n"
    "      extUseCov = lerp(ext_cov, regExtCov, regMix);\n"
    "      contourDir = lerp(contourDir, extRegDir, regMix);\n"
    "    }\n"
    "    float extA = extUseCov * contourStr * contourDir * I.c2.w;\n"
    "    float visA = materialAlpha * I.c2.w;\n"
    "    if (gmisc.y > 0.5) ga = max(ga, max(result.r, max(result.g, result.b)));\n"
    "    return float4(result * visA, saturate(visA * ga + extA)) * cover;\n"
    "  }\n"
    "  if (kind == 8) {\n"
    "    float4 s_rr = max(max(I.rr.x, I.rr.y), max(I.rr.z, I.rr.w)) > 0.0 ? I.rr : I.prm.xxxx;\n"
    "    float d = sd_rr(p, I.rect, s_rr);\n"
    "    float materialAlpha = saturate(0.5 - d * scale);\n"
    "    if (materialAlpha < 0.005) discard;\n"
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
    "      float shortening = sqrt(saturate(gradLen));\n"
    "      float bevelX = 1.0 - saturate(inward / bevel);\n"
    "      float mag = amount * (1.0 - sqrt(max(1.0 - bevelX * bevelX, 0.0))) * shortening;\n"
    "      disp = surfaceNormal * (-mag);\n"
    "    }\n"
    "    float2 uv = (p + disp) * gmap.xy + gmap.zw;\n"
    "    float4 fg = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    if (uv.x >= guvc.x && uv.x <= guvc.z && uv.y >= guvc.y && uv.y <= guvc.w) {\n"
    "      fg = bd.SampleLevel(samp, uv, 0);\n"
    "    }\n"
    "    return fg * (materialAlpha * col.a) * cover;\n"
    "  }\n"
    "  float2 hs = I.rect.zw * 0.5;\n"
    "  float2 c = I.rect.xy + hs;\n"
    "  // worked out as near the curve as its cover, its edge or its blur reaches\n"
    "  float band = 1.0 + 2.0 / scale + (kind == 2 ? max(I.prm.w, 0.5) : max(I.prm.z, 0.0));\n"
    "  // (its corner by p: rr, each its own)\n"
    "  float rad = mb_corner_of(p - c, I.rr);\n"
    "  float d = mb_sd_box(p - c, hs, rad, band);\n"
    "  if ((kind == 0 && I.pd.w >= 0.0) || kind == 5) d = notch_cut(d, p, I.rect, I.pd);\n"
    "  if (kind == 4) {\n"
    "    float2 uv = I.c2.xy + saturate((p - I.rect.xy) / I.rect.zw) * I.c2.zw;\n"
    "    float2 dudx = ddx(p) / max(I.rect.zw, 1e-6) * I.c2.zw;\n"
    "    float2 dudy = ddy(p) / max(I.rect.zw, 1e-6) * I.c2.zw;\n"
    "    float4 pic = mb_sample_pic(saturate(uv), dudx, dudy);\n"
    "    float edge = saturate(0.5 - d * scale);\n"
    "    if (I.pd.z > 0.5) {\n"
    "      // Only picture edges inside the frame cut it; the frame's clip covers shared edges once.\n"
    "      float2 size = I.rect.zw / I.c2.zw, lo = I.rect.xy - I.c2.xy * size, hi = lo + size;\n"
    "      float2 a = lo - p, b = p - hi;\n"
    "      if (I.c2.x >= 0.0) a.x = -1e9; if (I.c2.y >= 0.0) a.y = -1e9;\n"
    "      if (I.c2.x + I.c2.z <= 1.0) b.x = -1e9; if (I.c2.y + I.c2.w <= 1.0) b.y = -1e9;\n"
    "      float2 q = max(a, b);\n"
    "      float pd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);\n"
    "      pic *= I.pd.w * saturate(0.5 - pd * scale);\n"
    "      pic += float4(col.rgb * I.pd.y, I.pd.y) * (1.0 - pic.a);\n"
    "      if (I.pd.x < 0.5 && max(max(I.rr.x, I.rr.y), max(I.rr.z, I.rr.w)) <= 0.0 &&\n"
    "          max(max(I.rc.x, I.rc.y), max(I.rc.z, I.rc.w)) > 0.0 &&\n"
    "          all(I.clip.xy >= I.rect.xy) && all(I.clip.zw <= I.rect.xy + I.rect.zw)) edge = 1.0;\n"
    "    }\n"
    "    return (pic * (col.a * edge)) * cover;\n"
    "  }\n"
    "  if (kind == 2) {\n"
    "    // as Core Animation's: the shape blurred, sigma its radius (corner.sl mb_shadow)\n"
    "    float a = mb_shadow(p - c, hs, rad, max(I.prm.w, 0.25));\n"
    "    // glass's: only outside its box (c2.y above it)\n"
    "    if (I.c2.x > 0.5) {\n"
    "      float2 q = p - c + float2(0.0, I.c2.y);\n"
    "      float db = mb_sd_box(q, hs, mb_corner_of(q, I.rr), band);\n"
    "      a *= saturate(0.5 + db * scale);\n"
    "    }\n"
    "    return (float4(col.rgb, 1.0) * col.a * a) * cover;\n"
    "  }\n"
    "  float a = clamp(0.5 - d * scale, 0.0, 1.0);\n"
    "  if (kind == 5) {\n"
    "    // frosted: the blurred copy, saturated, the tint over it; the rim (a hairline inside the edge, the top left lit,\n"
    "    // the foot and right less), 0.5 wide\n"
    "    float3 g3 = backdrop(p);\n"
    "    float lg = luma(g3);\n"
    "    float fr_s = fade_r * fade_r * (3.0 - 2.0 * fade_r);\n"
    "    float ps = prog_glass ? fr_s : 1.0;\n"
    "    float ts = prog_glass ? fr_s : 1.0;\n"
    "    float sat = prog_glass ? lerp(1.0, I.prm.z, ps) : I.prm.z;\n"
    "    float bg_diff = prog_glass ? smoothstep(0.22, 0.72, abs(luma(col.rgb) - lg)) : 0.0;\n"
    "    float base_a = prog_glass ? lerp(col.a, min(0.86, col.a + 0.34), bg_diff) : col.a;\n"
    "    float ta = prog_glass ? (base_a * ts) : col.a;\n"
    "    g3 = max(lerp(float3(lg, lg, lg), g3, sat), 0.0);\n"
    "    g3 = g3 * (1.0 - ta) + col.rgb * ta;\n"
    "    if (I.c2.x + I.c2.y + I.c2.z > 0.0) {\n"
    "      float edge = saturate(0.5 + (d + 0.5) * scale);\n"
    "      float lit = saturate(0.5 + notch_cut(sd_rr(p - 0.5, I.rect, I.rr), p - 0.5, I.rect, I.pd) * scale);\n"
    "      float back = saturate(0.5 + notch_cut(sd_rr(p + 0.5, I.rect, I.rr), p + 0.5, I.rect, I.pd) * scale);\n"
    "      g3 = lerp(g3, float3(1.0, 1.0, 1.0), I.c2.x * edge);\n"
    "      g3 = lerp(g3, float3(1.0, 1.0, 1.0), I.c2.y * lit);\n"
    "      g3 = lerp(g3, float3(1.0, 1.0, 1.0), I.c2.z * back);\n"
    "    }\n"
    "    // dithered as it is written to 8 bits, so its slopes do not band (corner.sl mb_dither)\n"
    "    g3 += mb_dither(p * scale) * ps / 255.0;\n"
    "    float edge_a = prog_glass ? smoothstep(0.0, 0.04, fade_r) : 1.0;\n"
    "    return (float4(g3, 1.0) * (a * I.c2.w * edge_a)) * cover;\n"
    "  }\n"
    "  [branch] if (kind == 0 && I.pd.w < 0.0) {\n"
    "    float ch = max(max(col.r, col.g), col.b) - min(min(col.r, col.g), col.b);\n"
    "    float darkT = ad_sample_ink(I.pd.yz, -I.pd.w * 0.5).x;\n"
    "    if (ch <= 0.08 && luma(col.rgb) < 0.5) {\n"
    "      col.rgb = lerp(col.rgb, float3(1.0, 1.0, 1.0), darkT);\n"
    "      col.a = lerp(col.a, (col.a >= 0.75) ? col.a : min(col.a * 1.45, 0.28), darkT);\n"
    "    } else if (ch > 0.08 && luma(col.rgb) < 0.35) {\n"
    "      col.rgb = lerp(col.rgb, min(col.rgb * 1.65 + float3(0.28, 0.20, 0.18), float3(1.0, 1.0, 1.0)), darkT);\n"
    "      col.a = lerp(col.a, (col.a >= 0.75) ? col.a : min(col.a * 1.45, 0.36), darkT);\n"
    "    }\n"
    "  }\n"
    "  float4 fill = float4(col.rgb, 1.0) * col.a;\n"
    "  float bw = I.prm.z;\n"
    "  if (bw > 0.0) {\n"
    "    float inner = clamp(0.5 - (d + bw) * scale, 0.0, 1.0);\n"
    "    float4 bc = float4(I.c2.rgb, 1.0) * I.c2.a;\n"
    "    fill = lerp(bc, fill, inner);\n"
    "  }\n"
    "  return (fill * a) * cover;\n"
    "}\n"
    "// the flat pipeline: a ground's colour where it is covered whole, cut by straight clips alone (bands.h mb_pieces):\n"
    "// what fmain writes there, nothing worked out\n"
    "FOut fflat(VOut I) {\n"
    "  FOut o;\n"
    "  o.c = float4(I.color.rgb, 1.0) * I.color.a;\n"
    "  o.w = o.c.aaaa;\n"
    "  return o;\n"
    "}\n";

// glass's passes over its copy of what is under it
static const char *post_src =
    "// The backdrop's passes: pa.xy the source's texel, pa.z 1 halving (dual Kawase's down), 2 a Gaussian along pb.zw of\n"
    "// deviation pa.w (texels); pb.xy the last texel's centre of what the source holds\n"
    "cbuffer PU : register(b0) { float4 pa; float4 pb; float4 pc[9]; };\n"
    "Texture2D src : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "float4 vpost(uint vid : SV_VertexID) : SV_Position {\n"
    "  return float4(vid == 1 ? 3.0 : -1.0, vid == 2 ? -3.0 : 1.0, 0.0, 1.0);\n"
    "}\n"
    "float4 fpost(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 ts = pa.xy;\n"
    "  float2 lo = ts * 0.5, hi = pb.xy;\n"
    "  if (pa.z > 5.5) {\n"
    "    float2 uv = pos.xy * ts;\n"
    "    float2 dir = pb.zw * ts;\n"
    "    float sg = pa.w;\n"
    "    float k = -0.5 / (sg * sg);\n"
    "    float4 c0 = src.SampleLevel(samp, clamp(uv, lo, hi), 0);\n"
    "    float4 oS = c0;\n"
    "    float3 oL = pow(max(c0.rgb, float3(0.0, 0.0, 0.0)), float3(2.2, 2.2, 2.2));\n"
    "    float wsum = 1.0;\n"
    "    int n = int(ceil(sg * 3.0));\n"
    "    [loop] for (int i = 1; i <= 64; i += 2) {\n"
    "      if (i > n) break;\n"
    "      float w1 = exp(float(i * i) * k), w2 = exp(float((i + 1) * (i + 1)) * k);\n"
    "      float w = w1 + w2;\n"
    "      float off = (float(i) * w1 + float(i + 1) * w2) / w;\n"
    "      float4 cp = src.SampleLevel(samp, clamp(uv + dir * off, lo, hi), 0);\n"
    "      float4 cm = src.SampleLevel(samp, clamp(uv - dir * off, lo, hi), 0);\n"
    "      oS += (cp + cm) * w;\n"
    "      oL += (pow(max(cp.rgb, float3(0.0, 0.0, 0.0)), float3(2.2, 2.2, 2.2)) + pow(max(cm.rgb, float3(0.0, 0.0, 0.0)), float3(2.2, 2.2, 2.2))) * w;\n"
    "      wsum += 2.0 * w;\n"
    "    }\n"
    "    float4 outS = oS / wsum;\n"
    "    // Separable H+V bright-gated linear-light wide blur: exact for axis-aligned features, mildly directional on diagonals.\n"
    "    float3 outL = pow(max(oL / wsum, float3(0.0, 0.0, 0.0)), float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));\n"
    "    float bw = smoothstep(0.55, 0.82, 0.2126 * outS.r + 0.7152 * outS.g + 0.0722 * outS.b);\n"
    "    return float4(lerp(outS.rgb, outL, bw), outS.a);\n"
    "  }\n"
    "  if (pa.z < 1.5) {\n"
    "    float2 uv = pos.xy * 2.0 * ts;\n"
    "    float4 s = src.SampleLevel(samp, clamp(uv, lo, hi), 0) * 4.0;\n"
    "    s += src.SampleLevel(samp, clamp(uv - ts, lo, hi), 0);\n"
    "    s += src.SampleLevel(samp, clamp(uv + ts, lo, hi), 0);\n"
    "    s += src.SampleLevel(samp, clamp(uv + float2(ts.x, -ts.y), lo, hi), 0);\n"
    "    s += src.SampleLevel(samp, clamp(uv + float2(-ts.x, ts.y), lo, hi), 0);\n"
    "    return s / 8.0;\n"
    "  }\n"
    "  float2 uv = pos.xy * ts;\n"
    "  float2 dir = pb.zw * ts;\n"
    "  float sg = pa.w;\n"
    "  float k = -0.5 / (sg * sg);\n"
    "  float4 s = src.SampleLevel(samp, clamp(uv, lo, hi), 0);\n"
    "  float wsum = 1.0;\n"
    "  int n = int(ceil(sg * 3.0));\n"
    "  [loop] for (int i = 1; i <= 64; i += 2) {\n"
    "    if (i > n) break;\n"
    "    float w1 = exp(float(i * i) * k), w2 = exp(float((i + 1) * (i + 1)) * k);\n"
    "    float w = w1 + w2;\n"
    "    float off = (float(i) * w1 + float(i + 1) * w2) / w;\n"
    "    s += (src.SampleLevel(samp, clamp(uv + dir * off, lo, hi), 0) + src.SampleLevel(samp, clamp(uv - dir * off, lo, hi), 0)) * w;\n"
    "    wsum += 2.0 * w;\n"
    "  }\n"
    "  return s / wsum;\n"
    "}\n";

// The corner extension is a pass of its own, before the ordinary blur.
static const char *ad_src =
    "cbuffer AdU : register(b0) { float4 u_map; float4 u_uvc; float4 u_box; float4 u_param; };\n"
    "Texture2D bd : register(t0);\n"
    "Texture2D prev_ad : register(t1);\n"
    "SamplerState samp : register(s0);\n"
    "float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }\n"
    "static float mb_ink_luma(float3 c) { return dot(c * c, float3(0.2126, 0.7152, 0.0722)); }\n"
    "float mb_unpack16(float2 rg) {\n"
    "  float hi = floor(saturate(rg.x) * 255.0 + 0.5);\n"
    "  float lo = floor(saturate(rg.y) * 255.0 + 0.5);\n"
    "  return (hi * 256.0 + lo) / 65535.0;\n"
    "}\n"
    "float2 mb_pack16(float v) {\n"
    "  float q = floor(saturate(v) * 65535.0 + 0.5);\n"
    "  float hi = floor(q / 256.0);\n"
    "  float lo = q - hi * 256.0;\n"
    "  return float2(hi / 255.0, lo / 255.0);\n"
    "}\n"
    "float4 fad(float4 pos : SV_Position) : SV_Target {\n"
    "  float is_dark = u_param.x;\n"
    "  bool is_through = u_param.y > 1.5;\n"
    "  float is_new = u_param.y - (is_through ? 2.0 : 0.0);\n"
    "  float dt = u_param.z;\n"
    "  float hole_y = (is_dark > 0.5) ? 0.0 : 1.0;\n"
    "  float2 c = u_box.xy, h = u_box.zw;\n"
    "  float sum = 0.0, mx1 = -1.0, mx2 = -1.0, mn1 = 2.0, mn2 = 2.0;\n"
    "  [unroll] for (int iy = -1; iy <= 1; iy++) {\n"
    "    float py = c.y + float(iy) * (h.y * 0.60);\n"
    "    [unroll] for (int ix = -2; ix <= 2; ix++) {\n"
    "      float px = c.x + float(ix) * (h.x * 0.38);\n"
    "      float2 uv = clamp(float2(px, py) * u_map.xy + u_map.zw, u_uvc.xy, u_uvc.zw);\n"
    "      float4 smp = bd.SampleLevel(samp, uv, 0);\n"
    "      float a = is_through ? saturate(smp.a) : 1.0;\n"
    "      float y = mb_ink_luma(saturate(smp.rgb + (1.0 - a) * hole_y));\n"
    "      sum += y;\n"
    "      if (y > mx1) { mx2 = mx1; mx1 = y; } else if (y > mx2) { mx2 = y; }\n"
    "      if (y < mn1) { mn2 = mn1; mn1 = y; } else if (y < mn2) { mn2 = y; }\n"
    "    }\n"
    "  }\n"
    "  float bgL = (sum - mx1 - mx2 - mn1 - mn2) / 11.0;\n"
    "  int sid = int(u_param.w + 0.5);\n"
    "  float4 prev = prev_ad.Load(int3(sid, 0, 0));\n"
    "  float prevT = mb_unpack16(prev.rg);\n"
    "  float latch, target;\n"
    "  if (is_dark > 0.5) {\n"
    "    target = smoothstep(0.35, 0.85, bgL);\n"
    "    latch = target;\n"
    "  } else {\n"
    "    float tint_a = max(-is_dark, 0.0);\n"
    "    float baseDens = lerp(0.404, tint_a, smoothstep(0.405, 0.46, tint_a));\n"
    "    float extra = saturate((baseDens - 0.404) / (1.0 - 0.404));\n"
    "    float c_eq = lerp(sqrt(max(bgL, 0.0)), 0.992, pow(extra, 1.35));\n"
    "    float effBgL = c_eq * c_eq;\n"
    "    int pcode = int(floor(saturate(prev.b) * 255.0 + 0.5));\n"
    "    bool prev_dark = (is_new <= 0.5) && (pcode > 208);\n"
    "    bool on = prev_dark ? (effBgL <= 0.21) : (effBgL < 0.14);\n"
    "    target = on ? 1.0 : 0.0;\n"
    "    float q = floor(extra * 46.0 + 0.5);\n"
    "    latch = on ? ((255.0 - q) / 255.0) : (q / 255.0);\n"
    "  }\n"
    "  float nextT;\n"
    "  if (is_new > 0.5 || is_dark > 0.5 || dt >= 1.0 || (is_dark <= 0.5 && -is_dark >= 0.88)) nextT = target;\n"
    "  else if (dt <= 0.0) nextT = prevT;\n"
    "  else {\n"
    "    nextT = lerp(prevT, target, 1.0 - exp(-dt / 0.070));\n"
    "    if (abs(nextT - target) < 0.003) nextT = target;\n"
    "  }\n"
    "  return float4(mb_pack16(nextT), latch, saturate(bgL));\n"
    "}\n";

static const char *edge_src =
    "#define MB_CONTAINED_LOOP [loop]\n"
    MB_CORNER_SL
    "cbuffer PU : register(b0) { float4 pa; float4 pb; float4 pc[9]; };\n"
    "Texture2D src : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "float4 fedge(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 ts = pa.xy;\n"
    "  float2 lo = ts * 0.5, hi = pb.xy;\n"
    "  float2 p = pc[0].xy + pos.xy * pc[0].zw;\n"
    "  p = mb_contained_point(p, pc[1], pc[2], pc[3], pc[4], pc[5], pc[6], pc[7], pc[8], pc[0].z);\n"
    "  return src.SampleLevel(samp, clamp((p - pc[0].xy) / pc[0].zw * ts, lo, hi), 0);\n"
    "}\n";

static const char *prog_src =
    "cbuffer PU : register(b0) { float4 pa; float4 pb; float4 pc[9]; };\n"
    "Texture2D lv0 : register(t0); Texture2D lv1 : register(t1); Texture2D lv2 : register(t2); Texture2D lv3 : register(t3);\n"
    "SamplerState samp : register(s0);\n"
    "float4 fprog(float4 pos : SV_Position) : SV_Target {\n"
    "  if (pa.z > 4.5) {\n"
    "    float2 ts = pa.xy;\n"
    "    float2 lo = ts * 0.5, hi = pb.xy;\n"
    "    float4 cSharp;\n"
    "    float sg = pa.w;\n"
    "    if (sg > 0.3) {\n"
    "      float2 uv = pos.xy * ts;\n"
    "      float2 dir = pb.zw * ts;\n"
    "      float k = -0.5 / (sg * sg);\n"
    "      float4 o = lv0.SampleLevel(samp, clamp(uv, lo, hi), 0);\n"
    "      float wsum = 1.0;\n"
    "      int n = int(ceil(sg * 3.0));\n"
    "      [loop] for (int i = 1; i <= 64; i += 2) {\n"
    "        if (i > n) break;\n"
    "        float w1 = exp(float(i * i) * k), w2 = exp(float((i + 1) * (i + 1)) * k);\n"
    "        float w = w1 + w2;\n"
    "        float off = (float(i) * w1 + float(i + 1) * w2) / w;\n"
    "        o += (lv0.SampleLevel(samp, clamp(uv + dir * off, lo, hi), 0) + lv0.SampleLevel(samp, clamp(uv - dir * off, lo, hi), 0)) * w;\n"
    "        wsum += 2.0 * w;\n"
    "      }\n"
    "      cSharp = o / wsum;\n"
    "    } else if (pc[4].w > 0.001) {\n"
    "      float2 uv = pos.xy * ts;\n"
    "      float2 ox = float2(pc[4].w, 0.0) * ts;\n"
    "      float2 oy = float2(0.0, pc[4].w) * ts;\n"
    "      cSharp = (lv0.SampleLevel(samp, clamp(uv - ox - oy, lo, hi), 0) +\n"
    "                lv0.SampleLevel(samp, clamp(uv + ox - oy, lo, hi), 0) +\n"
    "                lv0.SampleLevel(samp, clamp(uv - ox + oy, lo, hi), 0) +\n"
    "                lv0.SampleLevel(samp, clamp(uv + ox + oy, lo, hi), 0)) * 0.25;\n"
    "    } else {\n"
    "      cSharp = lv0.SampleLevel(samp, clamp(pos.xy * ts, lo, hi), 0);\n"
    "    }\n"
    "    float4 cWide = lv3.SampleLevel(samp, clamp(pos.xy * pc[5].xy, pc[5].zw * 0.5, pc[4].xy), 0);\n"
    "    return lerp(cWide, cSharp, pc[4].z);\n"
    "  }\n"
    "  float2 p = pc[0].xy + pos.xy * pc[0].zw;\n"
    "  float ramp = 1.0;\n"
    "  if (pc[1].y > 0.0) ramp *= saturate((p.y - pc[1].x) / pc[1].y);\n"
    "  if (pc[1].w > 0.0) ramp *= saturate((pc[1].z - p.y) / pc[1].w);\n"
    "  if (pc[2].y > 0.0) ramp *= saturate((p.x - pc[2].x) / pc[2].y);\n"
    "  if (pc[2].w > 0.0) ramp *= saturate((pc[2].z - p.x) / pc[2].w);\n"
    "  float sp = max(pc[3].x, 1.0);\n"
    "  float kf = pc[3].y;\n"
    "  float4 m0 = pc[4], m1 = pc[5], m2 = pc[6], m3 = pc[7];\n"
    "  if (kf < 0.5) return lv0.SampleLevel(samp, clamp(pos.xy * m0.xy, m0.xy * 0.5, m0.zw), 0);\n"
    "  float r = saturate(ramp);\n"
    "  if (r <= 0.0) return lv0.SampleLevel(samp, clamp(pos.xy * m0.xy, m0.xy * 0.5, m0.zw), 0);\n"
    "  if (r >= 1.0) {\n"
    "    int ki = int(kf + 0.5);\n"
    "    if (ki <= 1) return lv1.SampleLevel(samp, clamp(pos.xy * m1.xy, m1.xy * 1.0, m1.zw), 0);\n"
    "    if (ki == 2) return lv2.SampleLevel(samp, clamp(pos.xy * m2.xy, m2.xy * 2.0, m2.zw), 0);\n"
    "    return lv3.SampleLevel(samp, clamp(pos.xy * m3.xy, m3.xy * 4.0, m3.zw), 0);\n"
    "  }\n"
    "  float sig1 = pow(sp, 1.0 / kf);\n"
    "  float rs = r * r * (3.0 - 2.0 * r);\n"
    "  float target_px = sp * rs;\n"
    "  if (target_px <= sig1 || kf <= 1.05) {\n"
    "    float2 uv0_min = m0.xy * 0.5, uv0_max = m0.zw;\n"
    "    float2 c_uv = pos.xy * m0.xy;\n"
    "    float4 g_small;\n"
    "    if (target_px <= 0.01) {\n"
    "      g_small = lv0.SampleLevel(samp, clamp(c_uv, uv0_min, uv0_max), 0);\n"
    "    } else if (target_px <= 0.55) {\n"
    "      // Exact separable 3x3 Gaussian via 2x2 bilinear taps (center weight 0.5 split across +-o1)\n"
    "      float inv2 = -0.5 / (target_px * target_px);\n"
    "      float w1 = exp(inv2);\n"
    "      float o1 = w1 / (0.5 + w1);\n"
    "      float2 ox = float2(o1, 0.0) * m0.xy;\n"
    "      float2 oy = float2(0.0, o1) * m0.xy;\n"
    "      g_small = (lv0.SampleLevel(samp, clamp(c_uv - ox - oy, uv0_min, uv0_max), 0) +\n"
    "                 lv0.SampleLevel(samp, clamp(c_uv + ox - oy, uv0_min, uv0_max), 0) +\n"
    "                 lv0.SampleLevel(samp, clamp(c_uv - ox + oy, uv0_min, uv0_max), 0) +\n"
    "                 lv0.SampleLevel(samp, clamp(c_uv + ox + oy, uv0_min, uv0_max), 0)) * 0.25;\n"
    "    } else {\n"
    "      // Exact separable 7x7 Gaussian via 4x4 bilinear taps (zero gaps, zero comb ghosting)\n"
    "      float inv2 = -0.5 / (target_px * target_px);\n"
    "      float w1 = exp(inv2);\n"
    "      float w2 = exp(4.0 * inv2);\n"
    "      float w3 = exp(9.0 * inv2);\n"
    "      float W1 = 0.5 + w1;\n"
    "      float W2 = w2 + w3;\n"
    "      float o1 = w1 / W1;\n"
    "      float o2 = (2.0 * w2 + 3.0 * w3) / max(W2, 1e-6);\n"
    "      float inv_sum = 0.5 / (W1 + W2);\n"
    "      float p1 = W1 * inv_sum;\n"
    "      float p2 = W2 * inv_sum;\n"
    "      float ox[4] = {-o2, -o1, o1, o2};\n"
    "      float pw[4] = {p2, p1, p1, p2};\n"
    "      g_small = float4(0.0, 0.0, 0.0, 0.0);\n"
    "      [unroll] for (int iy = 0; iy < 4; iy++) {\n"
    "        float4 row = float4(0.0, 0.0, 0.0, 0.0);\n"
    "        float uy = clamp(c_uv.y + ox[iy] * m0.y, uv0_min.y, uv0_max.y);\n"
    "        [unroll] for (int ix = 0; ix < 4; ix++) {\n"
    "          float ux = clamp(c_uv.x + ox[ix] * m0.x, uv0_min.x, uv0_max.x);\n"
    "          row += lv0.SampleLevel(samp, float2(ux, uy), 0) * pw[ix];\n"
    "        }\n"
    "        g_small += row * pw[iy];\n"
    "      }\n"
    "    }\n"
    "    float blend_lo = min(1.1, sig1 * 0.45);\n"
    "    float bt = saturate((target_px - blend_lo) / max(sig1 - blend_lo, 1e-4));\n"
    "    float blend1 = bt > 0.0 ? pow(bt, 1.8) : 0.0;\n"
    "    if (blend1 > 0.0) {\n"
    "      float4 c_lv1 = lv1.SampleLevel(samp, clamp(pos.xy * m1.xy, m1.xy * 1.0, m1.zw), 0);\n"
    "      return lerp(g_small, c_lv1, blend1);\n"
    "    }\n"
    "    return g_small;\n"
    "  }\n"
    "  float lv_u = clamp(1.0 + (kf - 1.0) * log(target_px / sig1) / max(log(sp / sig1), 1e-4), 1.0, kf);\n"
    "  int idx = min(int(floor(lv_u)), int(kf + 0.5) - 1);\n"
    "  if (idx < 0) idx = 0;\n"
    "  float f = lv_u - float(idx);\n"
    "  float4 c0, c1;\n"
    "  if (idx == 0) {\n"
    "    c0 = lv0.SampleLevel(samp, clamp(pos.xy * m0.xy, m0.xy * 0.5, m0.zw), 0);\n"
    "    c1 = lv1.SampleLevel(samp, clamp(pos.xy * m1.xy, m1.xy * 1.0, m1.zw), 0);\n"
    "  } else if (idx == 1) {\n"
    "    c0 = lv1.SampleLevel(samp, clamp(pos.xy * m1.xy, m1.xy * 1.0, m1.zw), 0);\n"
    "    c1 = lv2.SampleLevel(samp, clamp(pos.xy * m2.xy, m2.xy * 2.0, m2.zw), 0);\n"
    "  } else {\n"
    "    c0 = lv2.SampleLevel(samp, clamp(pos.xy * m2.xy, m2.xy * 2.0, m2.zw), 0);\n"
    "    c1 = lv3.SampleLevel(samp, clamp(pos.xy * m3.xy, m3.xy * 4.0, m3.zw), 0);\n"
    "  }\n"
    "  return lerp(c0, c1, f);\n"
    "}\n";

// Separate HLSL source for the lossless axis-aligned rect fast path (kept outside shader_src so baked shader keys stay intact).
static const char *rect_src =
    "cbuffer U : register(b0) { float2 vp; float scale; float inv_atlas; float2 origin; float2 _pad2; };\n"
    "struct VOut { float4 pos : SV_Position; nointerpolation float4 rot : ROT;\n"
    "  nointerpolation float4 rect : INST0; nointerpolation float4 prm : INST1;\n"
    "  nointerpolation float4 color : INST2; nointerpolation float4 c2 : INST3;\n"
    "  nointerpolation float4 clip : INST4; nointerpolation float4 pd : INST5; nointerpolation float4 rc : INST6;\n"
    "  nointerpolation float4 rs : INST7; nointerpolation float4 rr : INST8;\n"
    "  nointerpolation float4 oclip : INST9; nointerpolation float4 orc : INST10; nointerpolation float4 ors : INST11;\n"
    "  nointerpolation float4 fade : INST12; nointerpolation float4 fadex : INST13; };\n"
    "struct FOut { float4 c : SV_Target0; float4 w : SV_Target1; };\n"
    "FOut frect(VOut I) {\n"
    "  float2 p = I.pos.xy / scale + origin;\n"
    "  if (p.x < I.clip.x || p.y < I.clip.y || p.x > I.clip.z || p.y > I.clip.w) discard;\n"
    "  float2 hs = I.rect.zw * 0.5;\n"
    "  float2 q = abs(p - (I.rect.xy + hs)) - hs;\n"
    "  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);\n"
    "  float a = clamp(0.5 - d * scale, 0.0, 1.0);\n"
    "  FOut o;\n"
    "  o.c = (float4(I.color.rgb, 1.0) * I.color.a) * a;\n"
    "  o.w = o.c.aaaa;\n"
    "  return o;\n"
    "}\n";

static int fast_rect_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const wchar_t *e = mb_win_env(L"MBUI_FAST_RECT");
    enabled = (e && e[0] == L'0' && e[1] == L'\0') ? 0 : 1;
  }
  return enabled;
}

static int is_simple_rect(const float *f, int32_t i) {
  const float *b = f + (size_t)i * MB_INST;
  return (int)(b[5] + 0.5f) == 0 && b[2] >= 0.0f && b[3] >= 0.0f && b[6] <= 0.0f &&
         b[20] == 0.0f && b[21] == 0.0f && b[22] == 0.0f && b[23] == 0.0f &&
         b[32] == 0.0f && b[4] <= 0.0f && b[36] <= 0.0f && b[37] <= 0.0f && b[38] <= 0.0f && b[39] <= 0.0f &&
         b[24] <= 0.0f && b[25] <= 0.0f && b[26] <= 0.0f && b[27] <= 0.0f &&
         b[44] <= 0.0f && b[45] <= 0.0f && b[46] <= 0.0f && b[47] <= 0.0f &&
         b[53] <= 0.0f && b[55] <= 0.0f && b[57] <= 0.0f && b[59] <= 0.0f;
}

// A shader's bytecode kept on disk once compiled (shader_dir: an app's own folder, or the one its program says; a
// program that is neither, a demo, keeps none), so a run after the first takes it from there: compiling these at level
// 3 took 2.4 to 3.6 s of a window's start on a fast machine (measured on an RTX 5080's, 2026-11-13), all of it before
// the first frame. A file's name is of all it is compiled of (its source, entry, target, defines, the compiler's
// version and the level), so a changed shader, or another compiler, is compiled anew and kept beside the old, and
// programs that share a folder share what is the same. A file says how long its bytecode is and its hash before it:
// one cut short or damaged is deleted and compiled again, never handed to the device. One that cannot be read or
// written is compiled as before.
#include "win_shaders_baked.h"

struct ShaderKept {
  char magic[4];  // "MBSH"
  UINT32 size;
  UINT64 hash;
};

static UINT64 shader_fnv(const void *p, size_t n, UINT64 h) {
  const unsigned char *b = (const unsigned char *)p;
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static const UINT64 shader_fnv_start = 1469598103934665603ULL;

// The folder the bytecode is kept in, found once ("": none is kept). MBUI_SHADERS names it where the program says so
// (its environment or its own: win_life.h's app_env; %NAMES% in it read of the environment), for a program that keeps
// its shaders and is no app (an app's id brings its one instance, its tray icon and its logs with it); else it is the
// app's own, %LOCALAPPDATA%\<the app's id>\shaders. %LOCALAPPDATA% is read of the environment, not asked of the shell:
// shell32 loaded for it here was 0.27 s of a start that took its shaders from the folder (measured, 2026-11-13).
static wchar_t shader_dir[MAX_PATH * 2];
static INIT_ONCE shader_dir_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK shader_dir_find(PINIT_ONCE, PVOID, PVOID *) {
  wchar_t dir[MAX_PATH * 2] = L"";
  const wchar_t *said = mb_win_env(L"MBUI_SHADERS");
  if (said && *said) {
    DWORD n = ExpandEnvironmentStringsW(said, dir, MAX_PATH * 2);
    if (n == 0 || n > MAX_PATH * 2) return TRUE;
  } else {
    const wchar_t *id = mb_win_app_id();
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!id || !*id || n == 0 || n >= MAX_PATH) return TRUE;
    wchar_t app[MAX_PATH * 2];
    int m = _snwprintf(app, MAX_PATH * 2, L"%ls\\%ls", local, id);
    if (m < 0 || m >= MAX_PATH * 2) return TRUE;
    CreateDirectoryW(app, NULL);
    m = _snwprintf(dir, MAX_PATH * 2, L"%ls\\shaders", app);
    if (m < 0 || m >= MAX_PATH * 2) return TRUE;
  }
  // (its last folder made here; a said one's parents are the program's to have made)
  if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return TRUE;
  wcscpy(shader_dir, dir);
  return TRUE;
}

// Where the bytecode of `key` is kept (false: nowhere).
static bool shader_path(wchar_t *out, size_t cap, UINT64 key) {
  InitOnceExecuteOnce(&shader_dir_once, shader_dir_find, NULL, NULL);
  if (!*shader_dir) return false;
  int n = _snwprintf(out, cap, L"%ls\\%016llx.cso", shader_dir, (unsigned long long)key);
  return n > 0 && (size_t)n < cap;
}

// Local ID3DBlob for cached or baked bytecode so reading shaders never loads d3dcompiler_47.dll.
struct KeptBlob : public ID3DBlob {
  LONG refs = 1;
  SIZE_T size = 0;
  void *bytes = NULL;
  static KeptBlob *made(SIZE_T n) {
    KeptBlob *b = new (std::nothrow) KeptBlob();
    if (!b) return NULL;
    b->bytes = malloc(n ? n : 1);
    if (!b->bytes) {
      delete b;
      return NULL;
    }
    b->size = n;
    return b;
  }
  ~KeptBlob() { free(bytes); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out) return E_POINTER;
    if (id == __uuidof(IUnknown) || id == __uuidof(ID3D10Blob)) {
      *out = static_cast<ID3DBlob *>(this);
      AddRef();
      return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG n = InterlockedDecrement(&refs);
    if (!n) delete this;
    return (ULONG)n;
  }
  LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return bytes; }
  SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return size; }
};

// The bytecode kept at `path`, whole and as it was written; NULL (the file deleted where it is there and is not that).
static ID3DBlob *shader_kept(const wchar_t *path) {
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return NULL;
  ShaderKept head;
  LARGE_INTEGER size;
  ID3DBlob *blob = NULL;
  DWORD got = 0;
  bool ok = GetFileSizeEx(f, &size) && ReadFile(f, &head, sizeof head, &got, NULL) && got == sizeof head &&
            memcmp(head.magic, "MBSH", 4) == 0 && head.size > 0 && head.size < (64u << 20) &&
            size.QuadPart == (LONGLONG)sizeof head + (LONGLONG)head.size &&
            (blob = KeptBlob::made(head.size)) != NULL &&
            ReadFile(f, blob->GetBufferPointer(), head.size, &got, NULL) && got == head.size &&
            shader_fnv(blob->GetBufferPointer(), head.size, shader_fnv_start) == head.hash;
  CloseHandle(f);
  if (ok) return blob;
  if (blob) blob->Release();
  DeleteFileW(path);
  return NULL;
}

static void shader_keep(const wchar_t *path, ID3DBlob *code) {
  if (code->GetBufferSize() == 0 || code->GetBufferSize() >= (64u << 20)) return;
  // (written whole under another name and moved into place: a run reading it meanwhile reads none or all of it; the
  // name its thread's, the kinds' shaders being compiled on another: make_kinds)
  wchar_t part[MAX_PATH * 2 + 32];
  int n = _snwprintf(part, MAX_PATH * 2 + 32, L"%ls.%lu.%lu.part", path, GetCurrentProcessId(), GetCurrentThreadId());
  if (n < 0 || n >= MAX_PATH * 2 + 32) return;
  HANDLE f = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  ShaderKept head;
  memcpy(head.magic, "MBSH", 4);
  head.size = (UINT32)code->GetBufferSize();
  head.hash = shader_fnv(code->GetBufferPointer(), head.size, shader_fnv_start);
  DWORD put = 0;
  bool ok = WriteFile(f, &head, sizeof head, &put, NULL) && put == sizeof head &&
            WriteFile(f, code->GetBufferPointer(), head.size, &put, NULL) && put == head.size;
  CloseHandle(f);
  if (!ok || !MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(part);
}

static volatile LONG shader_baked_hits = 0;
static volatile LONG shader_disk_hits = 0;
static volatile LONG shader_runtime_compiles = 0;

extern "C" void invar_win_shader_stats(int *baked_hits, int *disk_hits, int *runtime_compiles) {
  if (baked_hits) *baked_hits = (int)InterlockedCompareExchange(&shader_baked_hits, 0, 0);
  if (disk_hits) *disk_hits = (int)InterlockedCompareExchange(&shader_disk_hits, 0, 0);
  if (runtime_compiles) *runtime_compiles = (int)InterlockedCompareExchange(&shader_runtime_compiles, 0, 0);
}

extern "C" void invar_win_shader_stats_reset(void) {
  InterlockedExchange(&shader_baked_hits, 0);
  InterlockedExchange(&shader_disk_hits, 0);
  InterlockedExchange(&shader_runtime_compiles, 0);
}

static ID3DBlob *compile(const char *entry, const char *target, const char *src = shader_src,
                         const D3D_SHADER_MACRO *defines = NULL) {
  UINT64 key = shader_fnv(src, strlen(src), shader_fnv_start);
  key = shader_fnv(entry, strlen(entry) + 1, key);
  key = shader_fnv(target, strlen(target) + 1, key);
  for (const D3D_SHADER_MACRO *d = defines; d && d->Name; d++) {
    key = shader_fnv(d->Name, strlen(d->Name) + 1, key);
    key = shader_fnv(d->Definition, strlen(d->Definition) + 1, key);
  }
  UINT32 made_by[2] = {D3D_COMPILER_VERSION, D3DCOMPILE_OPTIMIZATION_LEVEL3};
  key = shader_fnv(made_by, sizeof made_by, key);
  // (baked into the program: compiled when it was built, tools/bake_win_shaders.c)
  const wchar_t *ign = mb_win_env(L"INVAR_IGNORE_BAKED");
  bool ignore_baked = (ign && *ign && *ign != L'0');
  if (!ignore_baked) {
    for (int i = 0; baked_shaders[i].code; i++) {
      if (baked_shaders[i].key != key) continue;
      ID3DBlob *blob = KeptBlob::made(baked_shaders[i].size);
      if (blob) {
        size_t made = 0, took = 0;
        const char *why = ud_inflate(
            baked_shaders[i].code, baked_shaders[i].zsize,
            (unsigned char *)blob->GetBufferPointer(), baked_shaders[i].size,
            &made, &took);
        if (!why && made == baked_shaders[i].size && took == baked_shaders[i].zsize) {
          InterlockedIncrement(&shader_baked_hits);
          return blob;
        }
        fprintf(stderr, "baked shader %016llx: %s\n", (unsigned long long)key, why ? why : "size mismatch");
        blob->Release();
      }
    }
  }
  wchar_t path[MAX_PATH * 2 + 32];
  bool kept = shader_path(path, MAX_PATH * 2, key);
  if (kept) {
    ID3DBlob *was = shader_kept(path);
    if (was) {
      InterlockedIncrement(&shader_disk_hits);
      return was;
    }
  }
  ID3DBlob *code = NULL, *err = NULL;
  HRESULT hr = D3DCompile(src, strlen(src), "invar", defines, NULL, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
  if (FAILED(hr)) fprintf(stderr, "shader %s: %s\n", entry, err ? (char *)err->GetBufferPointer() : "?");
  if (err) err->Release();
  if (code) {
    InterlockedIncrement(&shader_runtime_compiles);
    if (kept) shader_keep(path, code);
  }
  return code;
}

static HRESULT make_device(D3D_DRIVER_TYPE type, UINT flags) {
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  return D3D11CreateDevice(NULL, type, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT | flags, levels, 3,
                           D3D11_SDK_VERSION, &dev, NULL, &ctx);
}

// The window in place's canvas as a composition swap chain (premultiplied alpha) on top of a DirectComposition tree whose
// other visual, under it, holds the native views, the composition's device made with the first; 0 (nothing made) where
// that cannot be had. MBUI_NO_DCOMP=1: not tried.
static int make_composed(IDXGIDevice1 *xdev, HWND hwnd) {
  const wchar_t *no = mb_win_env(L"MBUI_NO_DCOMP");
  if (no && *no && *no != L'0') return 0;
  RECT rc;
  GetClientRect(hwnd, &rc);
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = rc.right > 0 ? (UINT)rc.right : 1;
  sd.Height = rc.bottom > 0 ? (UINT)rc.bottom : 1;
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  int made_device = !dcomp;
  HRESULT hr = dcomp ? S_OK : DCompositionCreateDevice(xdev, __uuidof(IDCompositionDevice), (void **)&dcomp);
  if (SUCCEEDED(hr)) hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, &dtarget);
  if (SUCCEEDED(hr)) hr = dxgi_factory->CreateSwapChainForComposition(dev, &sd, NULL, &chain);
  if (SUCCEEDED(hr)) hr = dcomp->CreateVisual(&droot);
  if (SUCCEEDED(hr)) hr = dcomp->CreateVisual(&dunder);
  if (SUCCEEDED(hr)) hr = dcomp->CreateVisual(&dcanvas);
  if (SUCCEEDED(hr)) hr = dcanvas->SetContent(chain);
  if (SUCCEEDED(hr)) hr = droot->AddVisual(dunder, FALSE, NULL);
  if (SUCCEEDED(hr)) hr = droot->AddVisual(dcanvas, TRUE, dunder);
  if (SUCCEEDED(hr)) hr = dtarget->SetRoot(droot);
  if (SUCCEEDED(hr)) hr = dcomp->Commit();
  if (SUCCEEDED(hr)) {
    fb_w = (int)sd.Width;
    fb_h = (int)sd.Height;
    win_drop_stats_reset(&rw->drop_stats);
    return 1;
  }
  fprintf(stderr, "no DirectComposition (0x%08lx): the window's own swap chain, no native views\n", (unsigned long)hr);
  if (dcanvas) dcanvas->Release();
  if (dunder) dunder->Release();
  if (droot) droot->Release();
  if (chain) chain->Release();
  if (dtarget) dtarget->Release();
  if (dcomp && made_device) dcomp->Release(), dcomp = NULL;
  dcanvas = dunder = droot = NULL;
  chain = NULL;
  dtarget = NULL;
  return 0;
}

// The window in place's swap chain, its own where there is no composition: a flip model's, or before Windows 10 a
// blit model's (flip_chain says which, the first window's for all)
static int make_own_chain(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.Scaling = DXGI_SCALING_NONE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  if (flip_chain && SUCCEEDED(dxgi_factory->CreateSwapChainForHwnd(dev, hwnd, &sd, NULL, NULL, &chain))) {
    win_drop_stats_reset(&rw->drop_stats);
    return 1;
  }
  // before Windows 10
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  sd.BufferCount = 1;
  flip_chain = 0;
  if (SUCCEEDED(dxgi_factory->CreateSwapChainForHwnd(dev, hwnd, &sd, NULL, NULL, &chain))) {
    win_drop_stats_reset(&rw->drop_stats);
    return 1;
  }
  return 0;
}

static bool make_pipeline(void);
static void release_pipeline(void);

// The device, with its video side where the GPU has one (a remote screen decoded and shown by the GPU, win_stream.cpp),
// else without, else WARP's.
static bool make_any_device(void) {
  init_render_knobs();
  if (video_support && SUCCEEDED(make_device(D3D_DRIVER_TYPE_HARDWARE, D3D11_CREATE_DEVICE_VIDEO_SUPPORT))) return true;
  return SUCCEEDED(make_device(D3D_DRIVER_TYPE_HARDWARE, 0)) || SUCCEEDED(make_device(D3D_DRIVER_TYPE_WARP, 0));
}

// The device and its pipeline made as the program starts, on a thread of their own, while the window's thread runs the
// app up to its window (its tree, CreateWindowEx): neither needs the window, and they were ~125 ms of a window's start
// made after it, the driver loaded and the shaders handed to it (an RTX 4080's, 2026-11-22). mb_render_init waits for
// it and makes only what is of the window (its swap chain, the composition); a program that draws without a window
// (mb_render_init_offscreen: WARP's) lets what it made go. MBUI_NO_WARM=1: made in mb_render_init, as before.
static HANDLE warm_thread;
static bool warm_made;
static DWORD WINAPI warm_make(LPVOID) {
  if (make_any_device()) {
    if (make_pipeline()) {
      warm_made = true;
    } else {
      if (ctx) {
        ctx->Release();
        ctx = NULL;
      }
      if (dev) {
        dev->Release();
        dev = NULL;
      }
    }
  }
  return 0;
}
static struct WarmStart {
  WarmStart() {
    const wchar_t *no = mb_win_env(L"MBUI_NO_WARM");
    if (no && *no && *no != L'0') return;
    warm_thread = CreateThread(NULL, 0, warm_make, NULL, 0, NULL);
  }
} warm_start;

// What the start's thread made, once it is done (false: nothing, made here then).
static bool warm_taken(void) {
  if (!warm_thread) return false;
  WaitForSingleObject(warm_thread, INFINITE);
  CloseHandle(warm_thread);
  warm_thread = NULL;
  return warm_made;
}

int mb_render_init(void *hwnd) {
  bool warm = warm_taken();
  if (!warm && !make_any_device()) {
    fprintf(stderr, "no Direct3D 11 device\n");
    return 0;
  }
  IDXGIDevice1 *xdev = NULL;
  IDXGIAdapter *adapter = NULL;
  dev->QueryInterface(__uuidof(IDXGIDevice1), (void **)&xdev);
  xdev->SetMaximumFrameLatency(1);  // input to photon over throughput
  xdev->GetAdapter(&adapter);
  adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&dxgi_factory);
  flip_chain = 1;
  target_hwnd = (HWND)hwnd;
  if (make_composed(xdev, (HWND)hwnd)) {
    // the composition's swap chain, made at the window's size (its first buffers' view: at the first frame's size)
    fb_w = fb_h = 0;
  } else {
    make_own_chain((HWND)hwnd);
  }
  dxgi_factory->MakeWindowAssociation((HWND)hwnd, DXGI_MWA_NO_ALT_ENTER);
  adapter->Release();
  xdev->Release();
  if (!warm && !make_pipeline()) {
    if (chain) {
      chain->Release();
      chain = NULL;
    }
    if (dxgi_factory) {
      dxgi_factory->Release();
      dxgi_factory = NULL;
    }
    if (ctx) {
      ctx->Release();
      ctx = NULL;
    }
    if (dev) {
      dev->Release();
      dev = NULL;
    }
    return 0;
  }
  if (!chain) {
    release_pipeline();
    if (dxgi_factory) {
      dxgi_factory->Release();
      dxgi_factory = NULL;
    }
    if (ctx) {
      ctx->Release();
      ctx = NULL;
    }
    if (dev) {
      dev->Release();
      dev = NULL;
    }
    return 0;
  }
  return 1;
}

// Window `wid` of the run beside the first (win_app.c win_open), its HWND made: its swap chain as the first's is (in the
// composition, the views under it, where the first's is), nothing drawn in it yet. Window 0 put back in place after.
int mb_render_open(int wid, void *hwnd) {
  if (wid <= 0 || wid >= MB_WINS || !dev || !dxgi_factory) return 0;
  RenderWin *back = rw;
  rw = &render_wins[wid];
  render_win_fresh(rw);
  target_hwnd = (HWND)hwnd;
  int ok;
  if (dcomp) {
    ok = make_composed(NULL, (HWND)hwnd);
    if (ok) fb_w = fb_h = 0;
  } else {
    ok = make_own_chain((HWND)hwnd);
  }
  if (ok) dxgi_factory->MakeWindowAssociation((HWND)hwnd, DXGI_MWA_NO_ALT_ENTER);
  rw = back;
  return ok;
}

// What window `wid` kept let go of, as it closes (its views taken off it first: win_views.cpp mb_views_forget): the
// next window given its number starts as none was drawn.
void mb_render_forget(int wid) {
  if (wid <= 0 || wid >= MB_WINS) return;
  RenderWin *back = rw;
  rw = &render_wins[wid];
  if (canvas_rtv) canvas_rtv->Release();
  if (canvas) canvas->Release();
  for (int i = 0; i < 2; i++) {
    if (rw->ad_tex[i].srv) rw->ad_tex[i].srv->Release();
    if (rw->ad_tex[i].rt) rw->ad_tex[i].rt->Release();
    if (rw->ad_tex[i].t) rw->ad_tex[i].t->Release();
  }
  if (rtv) rtv->Release();
  if (dtarget) dtarget->SetRoot(NULL);
  if (dcanvas) dcanvas->SetContent(NULL);
  if (droot) {
    droot->RemoveAllVisuals();
    droot->Release();
  }
  if (dunder) {
    dunder->RemoveAllVisuals();
    dunder->Release();
  }
  if (dcanvas) dcanvas->Release();
  if (chain) chain->Release();
  if (dtarget) dtarget->Release();
  if (dcomp) dcomp->Commit();
  if (vbuf) {
    if (ctx) {
      ID3D11Buffer *no_vb = NULL;
      UINT z = 0;
      ctx->IASetVertexBuffers(0, 1, &no_vb, &z, &z);
    }
    vbuf->Release();
  }
  free(last_inst);
  free(rw->next_inst);
  // (the device's state left naming none of its views)
  ctx->OMSetRenderTargets(0, NULL, NULL);
  ctx->Flush();
  render_win_fresh(rw);
  rw = back == &render_wins[wid] ? &render_wins[0] : back;
}

void *mb_render_views_visual_of(int wid) {
  if (wid < 0 || wid >= MB_WINS) return NULL;
  RenderWin *back = rw;
  rw = &render_wins[wid];
  IDCompositionVisual *v = dunder;
  rw = back;
  return v;
}

// The kinds drawn by a pixel shader of their own: the shader with its kind fixed (ONE_KIND), so each is compiled to its
// kind's branch alone and a pixel of it is shaded with the registers that branch needs, not the most any kind's does
// (as Metal's: apple_render.m kind_pipe says what it saved there), what it draws the same to the byte. Compiled after
// the rest, on a thread of their own (kinds_thread), the device's own to make them on: until they are, the one for
// any kind draws every kind, the same pixels.
#define MB_KIND_LIQUID_SINGLE 13
#define MB_KINDS 14
static int has_own_ps(int kind) {
  return (kind >= 0 && kind <= 8) || kind == 10 || kind == 11 || kind == MB_KIND_LIQUID_SINGLE;
}
static ID3D11PixelShader *kps[MB_KINDS];
static HANDLE kinds_thread;
static volatile LONG kinds_ready;

static DWORD WINAPI make_kinds(void *arg) {
  ID3D11Device *d = (ID3D11Device *)arg;
  // Compile kind 4 (images, Catmull-Rom resampler) first and publish each shader as soon as it is ready.
  static const int order[] = {4, 0, 1, 2, 3, 5, 6, 7, 8, 10, 11, MB_KIND_LIQUID_SINGLE};
  for (int i = 0; i < (int)(sizeof order / sizeof order[0]); i++) {
    int k = order[i];
    if (!has_own_ps(k)) continue;
    char one[8];
    snprintf(one, sizeof one, "%d", k);
    D3D_SHADER_MACRO defines[2] = {{"ONE_KIND", one}, {NULL, NULL}};
    ID3DBlob *code = compile("fmain", "ps_4_0", shader_src, defines);
    if (code) {
      ID3D11PixelShader *s = NULL;
      if (SUCCEEDED(d->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &s)))
        InterlockedExchangePointer((PVOID *)&kps[k], s);
      code->Release();
    }
  }
  InterlockedExchange(&kinds_ready, 1);
  return 0;
}

// The pixel shader that draws a run of instances of `kind`.
static ID3D11PixelShader *kind_ps(int kind) {
  if (!has_own_ps(kind)) return ps;
  ID3D11PixelShader *s = (ID3D11PixelShader *)InterlockedCompareExchangePointer((PVOID *)&kps[kind], NULL, NULL);
  return s ? s : ps;
}

// The shaders, buffers, atlas and states every target is drawn with (the window's, the offscreen one).
static void blur_layers_release(void);
static void win_layers_release(void);
static ID3D11Buffer *bl_comp_vbuf;
static ID3D11Buffer *layer_vbuf;
static size_t layer_vbuf_cap;

static void release_pipeline(void) {
  if (kinds_thread) {
    WaitForSingleObject(kinds_thread, INFINITE);
    CloseHandle(kinds_thread);
    kinds_thread = NULL;
  }
  kinds_ready = 0;
  for (int k = 0; k < MB_KINDS; k++) {
    if (kps[k]) {
      kps[k]->Release();
      kps[k] = NULL;
    }
  }
  if (bl_comp_vbuf) { bl_comp_vbuf->Release(); bl_comp_vbuf = NULL; }
  if (layer_vbuf) { layer_vbuf->Release(); layer_vbuf = NULL; layer_vbuf_cap = 0; }
  if (rect_ps) { rect_ps->Release(); rect_ps = NULL; }
  if (prog_ps) { prog_ps->Release(); prog_ps = NULL; }
  if (ad_ps) { ad_ps->Release(); ad_ps = NULL; }
  if (edge_ps) { edge_ps->Release(); edge_ps = NULL; }
  if (pps) { pps->Release(); pps = NULL; }
  if (pvs) { pvs->Release(); pvs = NULL; }
  if (raster_cut) { raster_cut->Release(); raster_cut = NULL; }
  if (raster) { raster->Release(); raster = NULL; }
  if (blend) { blend->Release(); blend = NULL; }
  if (sampler) { sampler->Release(); sampler = NULL; }
  if (atlas_srv) { atlas_srv->Release(); atlas_srv = NULL; }
  if (atlas) { atlas->Release(); atlas = NULL; }
  if (ad_cb) { ad_cb->Release(); ad_cb = NULL; }
  if (gbuf) { gbuf->Release(); gbuf = NULL; }
  if (pbuf) { pbuf->Release(); pbuf = NULL; }
  if (ubuf) { ubuf->Release(); ubuf = NULL; }
  if (layout) { layout->Release(); layout = NULL; }
  if (ps_flat) { ps_flat->Release(); ps_flat = NULL; }
  if (ps) { ps->Release(); ps = NULL; }
  if (vs) { vs->Release(); vs = NULL; }
}

static bool make_pipeline(void) {
  // (those of a device before, if there was one, let go once made)
  release_pipeline();
  ID3DBlob *vcode = compile("vmain", "vs_4_0"), *pcode = compile("fmain", "ps_4_0"), *fcode = compile("fflat", "ps_4_0");
  if (!vcode || !pcode || !fcode) {
    fprintf(stderr, "win_render: core shader compile failed (vmain=%p fmain=%p fflat=%p)\n",
            (void *)vcode, (void *)pcode, (void *)fcode);
    if (vcode) vcode->Release();
    if (pcode) pcode->Release();
    if (fcode) fcode->Release();
    return false;
  }
  HRESULT hr_vs = dev->CreateVertexShader(vcode->GetBufferPointer(), vcode->GetBufferSize(), NULL, &vs);
  HRESULT hr_ps = dev->CreatePixelShader(pcode->GetBufferPointer(), pcode->GetBufferSize(), NULL, &ps);
  HRESULT hr_pf = dev->CreatePixelShader(fcode->GetBufferPointer(), fcode->GetBufferSize(), NULL, &ps_flat);
  fcode->Release();
  // the instance's fifteen float4s (scene.mbt): rect, p, color, c2, clip, pad, the clip's corners' radii, how smooth,
  // how it is turned, a box's corners' radii, the outer clip's rect, corners' radii and how smooth, how it fades down
  // and across
  D3D11_INPUT_ELEMENT_DESC el[15];
  for (UINT i = 0; i < 15; i++)
    el[i] = {"INST", i, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, i * 16, D3D11_INPUT_PER_INSTANCE_DATA, 1};
  HRESULT hr_il = dev->CreateInputLayout(el, 15, vcode->GetBufferPointer(), vcode->GetBufferSize(), &layout);
  vcode->Release();
  pcode->Release();
  if (FAILED(hr_vs) || !vs || FAILED(hr_ps) || !ps || FAILED(hr_pf) || !ps_flat || FAILED(hr_il) || !layout) {
    fprintf(stderr, "win_render: core shader/input-layout creation failed (vs=0x%lx ps=0x%lx flat=0x%lx layout=0x%lx)\n",
            (unsigned long)hr_vs, (unsigned long)hr_ps, (unsigned long)hr_pf, (unsigned long)hr_il);
    release_pipeline();
    return false;
  }
  ID3DBlob *pv = compile("vpost", "vs_4_0", post_src), *pp = compile("fpost", "ps_4_0", post_src);
  ID3DBlob *ep = compile("fedge", "ps_4_0", edge_src);
  ID3DBlob *gp = compile("fprog", "ps_4_0", prog_src);
  ID3DBlob *ap = compile("fad", "ps_4_0", ad_src);
  if (pv) {
    if (FAILED(dev->CreateVertexShader(pv->GetBufferPointer(), pv->GetBufferSize(), NULL, &pvs)))
      pvs = NULL;
    pv->Release();
  } else {
    fprintf(stderr, "win_render: vpost shader compile failed\n");
    pvs = NULL;
  }
  if (pp) {
    if (FAILED(dev->CreatePixelShader(pp->GetBufferPointer(), pp->GetBufferSize(), NULL, &pps)))
      pps = NULL;
    pp->Release();
  } else {
    fprintf(stderr, "win_render: fpost shader compile failed\n");
    pps = NULL;
  }
  if (ep) {
    if (FAILED(dev->CreatePixelShader(ep->GetBufferPointer(), ep->GetBufferSize(), NULL, &edge_ps)))
      edge_ps = NULL;
    ep->Release();
  } else {
    fprintf(stderr, "win_render: fedge shader compile failed\n");
    edge_ps = NULL;
  }
  if (gp) {
    if (FAILED(dev->CreatePixelShader(gp->GetBufferPointer(), gp->GetBufferSize(), NULL, &prog_ps)))
      prog_ps = NULL;
    gp->Release();
  }
  if (ap) {
    if (FAILED(dev->CreatePixelShader(ap->GetBufferPointer(), ap->GetBufferSize(), NULL, &ad_ps)))
      ad_ps = NULL;
    ap->Release();
  }
  ID3DBlob *rcode = compile("frect", "ps_4_0", rect_src);
  if (rcode) {
    if (FAILED(dev->CreatePixelShader(rcode->GetBufferPointer(), rcode->GetBufferSize(), NULL, &rect_ps)))
      rect_ps = NULL;
    rcode->Release();
  }

  D3D11_BUFFER_DESC ud = {32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  HRESULT hr_ub = dev->CreateBuffer(&ud, NULL, &ubuf);
  D3D11_BUFFER_DESC pd = {32 + sizeof(MBContained), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  HRESULT hr_pb = dev->CreateBuffer(&pd, NULL, &pbuf);
  D3D11_BUFFER_DESC gd = {(UINT)sizeof(GU), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  HRESULT hr_gb = dev->CreateBuffer(&gd, NULL, &gbuf);
  D3D11_BUFFER_DESC adbd = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  HRESULT hr_ab = dev->CreateBuffer(&adbd, NULL, &ad_cb);
  if (FAILED(hr_ub) || !ubuf || FAILED(hr_pb) || !pbuf || FAILED(hr_gb) || !gbuf || FAILED(hr_ab) || !ad_cb) {
    fprintf(stderr, "win_render: constant buffer creation failed\n");
    release_pipeline();
    return false;
  }

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = (UINT)mb_atlas_size;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  HRESULT hr_at = dev->CreateTexture2D(&td, NULL, &atlas);
  HRESULT hr_srv = SUCCEEDED(hr_at) && atlas ? dev->CreateShaderResourceView(atlas, NULL, &atlas_srv) : E_FAIL;
  if (FAILED(hr_at) || !atlas || FAILED(hr_srv) || !atlas_srv) {
    fprintf(stderr, "win_render: glyph atlas texture/SRV creation failed\n");
    release_pipeline();
    return false;
  }

  D3D11_SAMPLER_DESC smp = {};
  smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  smp.MaxLOD = D3D11_FLOAT32_MAX;
  HRESULT hr_smp = dev->CreateSamplerState(&smp, &sampler);

  // premultiplied alpha, as on Metal and GLES
  D3D11_BLEND_DESC bd = {};
  D3D11_RENDER_TARGET_BLEND_DESC &b = bd.RenderTarget[0];
  b.BlendEnable = TRUE;
  b.SrcBlend = b.SrcBlendAlpha = D3D11_BLEND_ONE;
  b.DestBlend = D3D11_BLEND_INV_SRC1_COLOR;
  b.DestBlendAlpha = D3D11_BLEND_INV_SRC1_ALPHA;
  b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  HRESULT hr_bl = dev->CreateBlendState(&bd, &blend);

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  HRESULT hr_rs = dev->CreateRasterizerState(&rd, &raster);
  // (a part drawn again: cut to it)
  rd.ScissorEnable = TRUE;
  HRESULT hr_rc = dev->CreateRasterizerState(&rd, &raster_cut);
  if (FAILED(hr_smp) || !sampler || FAILED(hr_bl) || !blend || FAILED(hr_rs) || !raster || FAILED(hr_rc) || !raster_cut) {
    fprintf(stderr, "win_render: sampler/blend/rasterizer state creation failed\n");
    release_pipeline();
    return false;
  }
  kinds_thread = CreateThread(NULL, 0, make_kinds, dev, 0, NULL);
  return true;
}

void *mb_render_device(void) { return dev; }
void *mb_render_context(void) { return ctx; }

void *mb_render_dcomp(void) { return dcomp; }
void *mb_render_views_visual(void) { return dunder; }
#ifndef MB_WIN_DCOMP_COMMIT
#define MB_WIN_DCOMP_COMMIT(dc) ((dc)->Commit())
#endif
void mb_render_commit(void) {
  if (dcomp) MB_WIN_DCOMP_COMMIT(dcomp);
}

// A view placed (win_views.cpp apply_place) is committed with the frame that paints its hole where it now is: just
// after that frame's Present, both then taken by the same composition. Committed as it was placed, the view moved a
// frame ahead of its hole while a list scrolled, the window's black showing along its edge in the hole (2026-10-29).
static bool commit_after_present;
extern "C" void mb_host_request_frame(void);
void mb_render_commit_with_frame(void) {
  commit_after_present = true;
  rw->commit_owed = 1;
  mb_host_request_frame();
}
void mb_render_commit_soon(void) {
  if (!commit_after_present) mb_render_commit();
}
static void commit_presented(void) {
  if (!commit_after_present) return;
  commit_after_present = false;
  mb_render_commit();
}

static void ensure_canvas(int tw, int th);

void mb_render_set_size(int pw, int ph, double w, double h, double scale) {
  vw = w;
  vh = h;
  vscale = scale;
  if (!chain || (pw == fb_w && ph == fb_h && rtv)) return;
  if (rtv) rtv->Release();
  rtv = NULL;
  ctx->OMSetRenderTargets(0, NULL, NULL);
  ensure_canvas(0, 0);
  blur_layers_release();
  chain->ResizeBuffers(0, (UINT)pw, (UINT)ph, DXGI_FORMAT_UNKNOWN, 0);
  ID3D11Texture2D *back = NULL;
  if (SUCCEEDED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back))) {
    dev->CreateRenderTargetView(back, NULL, &rtv);
    back->Release();
  }
  fb_w = pw;
  fb_h = ph;
  rw->shown_ok = 0;
  rw->swap_needs_full = 1;
  if (dcomp) {
    commit_after_present = true;
    rw->commit_owed = 1;
  }
}

static uint64_t g_atlas_overwrite_seq = 0;
static uint64_t **g_atlas_occ_plots = NULL;
static int g_atlas_occ_grid = 0;
static int32_t g_atlas_occ_gen = 0;
static int g_atlas_occ_has_gen = 0;

static void atlas_occ_reset(int32_t gen) {
  int total = g_atlas_occ_grid * g_atlas_occ_grid;
  for (int i = 0; i < total; i++) {
    if (g_atlas_occ_plots[i]) memset(g_atlas_occ_plots[i], 0, 512 * 8 * sizeof(uint64_t));
  }
  g_atlas_occ_gen = gen;
  g_atlas_occ_has_gen = 1;
}

// Tracks occupied atlas pixels in lazy 512x512 1-bit bitmaps (32 KiB per touched plot).
// Normal glyph appends land on fresh pixels (returns 0, preserving partial redraw and other windows'
// same_as_shown); any overwrite of an existing UV region returns 1 to bump g_atlas_overwrite_seq.
static int atlas_occ_mark(int x, int y, int w, int h) {
  int32_t gen = mb_atlas_gen_now();
  int grid = (mb_atlas_size + 511) >> 9;
  if (grid <= 0) return 1;
  if (grid != g_atlas_occ_grid) {
    int old_total = g_atlas_occ_grid * g_atlas_occ_grid;
    for (int i = 0; i < old_total; i++) free(g_atlas_occ_plots[i]);
    free(g_atlas_occ_plots);
    g_atlas_occ_plots = (uint64_t **)calloc((size_t)grid * (size_t)grid, sizeof(uint64_t *));
    if (!g_atlas_occ_plots) {
      g_atlas_occ_grid = 0;
      return 1;
    }
    g_atlas_occ_grid = grid;
    g_atlas_occ_gen = gen;
    g_atlas_occ_has_gen = 1;
  } else if (!g_atlas_occ_has_gen || gen != g_atlas_occ_gen) {
    atlas_occ_reset(gen);
  }
  int overlap = 0;
  int px0 = x >> 9, px1 = (x + w - 1) >> 9;
  int py0 = y >> 9, py1 = (y + h - 1) >> 9;
  for (int py = py0; py <= py1; py++) {
    int gy0 = py << 9;
    int ly0 = y > gy0 ? y - gy0 : 0;
    int ly1 = (y + h) < (gy0 + 512) ? (y + h - gy0) : 512;
    for (int px = px0; px <= px1; px++) {
      int idx = py * g_atlas_occ_grid + px;
      uint64_t *bits = g_atlas_occ_plots[idx];
      if (!bits) {
        bits = (uint64_t *)calloc(512 * 8, sizeof(uint64_t));
        if (!bits) return 1;
        g_atlas_occ_plots[idx] = bits;
      }
      int gx0 = px << 9;
      int lx0 = x > gx0 ? x - gx0 : 0;
      int lx1 = (x + w) < (gx0 + 512) ? (x + w - gx0) : 512;
      int w0 = lx0 >> 6, w1 = (lx1 - 1) >> 6;
      for (int ly = ly0; ly < ly1; ly++) {
        uint64_t *row = bits + (size_t)ly * 8;
        for (int wi = w0; wi <= w1; wi++) {
          int b0 = (wi == w0) ? (lx0 & 63) : 0;
          int b1 = (wi == w1) ? (((lx1 - 1) & 63) + 1) : 64;
          uint64_t hi = (b1 == 64) ? ~0ULL : ((1ULL << b1) - 1ULL);
          uint64_t lo = (b0 == 0) ? 0ULL : ((1ULL << b0) - 1ULL);
          uint64_t mask = hi & ~lo;
          if (row[wi] & mask) overlap = 1;
          row[wi] |= mask;
        }
      }
    }
  }
  return overlap;
}

void mb_atlas_upload(int x, int y, int w, int h, const void *rgba) {
  // (a region outside the atlas or empty: its occupancy unknown, so every window's next frame drawn again)
  if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > mb_atlas_size || y + h > mb_atlas_size || atlas_occ_mark(x, y, w, h))
    g_atlas_overwrite_seq++;
  D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
  ctx->UpdateSubresource(atlas, 0, &box, rgba, (UINT)w * 4, 0);
}

// ---------------------------------------------------------------------------
// Glass: when the instances reach a run of glass (slot 20 negative: the capture it shares), what is drawn so far
// under it (its rects with room for the blur round them) is copied out, halved down to where the blur is a few texels
// wide (dual Kawase's downsample, at most 1/8), blurred there by a Gaussian of what deviation is left, and drawn
// through. Level 0, the copy, has 8 bits a channel as the target has; what the passes write keeps 10 (R10G10B10A2, in
// as many bytes): at 8 each pass rounds the frost's slow slopes into flat steps, a texel of the last level 8 pixels
// wide on the screen (a blur under 4 pixels, too narrow to halve, is written back into the copy: it leaves no slope
// that wide).

static level level_tex(DXGI_FORMAT format, int w, int h) {
  level l = {NULL, NULL, NULL, w > 0 ? w : 1, h > 0 ? h : 1};
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)l.w;
  td.Height = (UINT)l.h;
  td.MipLevels = td.ArraySize = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(dev->CreateTexture2D(&td, NULL, &l.t)) || !l.t) return level{};
  if (FAILED(dev->CreateRenderTargetView(l.t, NULL, &l.rt)) || !l.rt) {
    l.t->Release();
    return level{};
  }
  if (FAILED(dev->CreateShaderResourceView(l.t, NULL, &l.srv)) || !l.srv) {
    l.rt->Release();
    l.t->Release();
    return level{};
  }
  return l;
}

static void glass_free(void);
static level lens_lv[2];
static int lens_w, lens_h;

static int ensure_lens_levels(int w, int h) {
  if (lens_lv[0].t && lens_w >= w && lens_h >= h) return 1;
  for (int j = 0; j < 2; j++) {
    if (lens_lv[j].srv) lens_lv[j].srv->Release();
    if (lens_lv[j].rt) lens_lv[j].rt->Release();
    if (lens_lv[j].t) lens_lv[j].t->Release();
    lens_lv[j] = level{};
  }
  lens_w = w > lens_w ? w : lens_w;
  lens_h = h > lens_h ? h : lens_h;
  for (int j = 0; j < 2; j++) {
    lens_lv[j] = level_tex(DXGI_FORMAT_B8G8R8A8_UNORM, lens_w, lens_h);
    if (!lens_lv[j].t || !lens_lv[j].rt || !lens_lv[j].srv) {
      glass_free();
      return 0;
    }
  }
  return 1;
}

static int ensure_levels(int w, int h, int rt_w, int rt_h) {
  if (lv[0].t && lv_w >= w && lv_h >= h) return 1;
  for (int j = 0; j < 5; j++) {
    if (lv[j].srv) lv[j].srv->Release();
    if (lv[j].rt) lv[j].rt->Release();
    if (lv[j].t) lv[j].t->Release();
    lv[j] = level{};
  }
  int nw = (w + 255) & ~255;
  int nh = (h + 255) & ~255;
  if (rt_w > 0 && nw > rt_w) nw = rt_w;
  if (rt_h > 0 && nh > rt_h) nh = rt_h;
  if (nw < w) nw = w;
  if (nh < h) nh = h;
  lv_w = nw > lv_w ? nw : lv_w;
  lv_h = nh > lv_h ? nh : lv_h;
  for (int j = 0; j < 4; j++) {
    lv[j] = level_tex(j == 0 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM, (lv_w + (1 << j) - 1) >> j,
                      (lv_h + (1 << j) - 1) >> j);
    if (!lv[j].t || !lv[j].rt || !lv[j].srv) {
      glass_free();
      return 0;
    }
  }
  lv[4] = level_tex(DXGI_FORMAT_R10G10B10A2_UNORM, lv_w, lv_h);
  if (!lv[4].t || !lv[4].rt || !lv[4].srv) {
    glass_free();
    return 0;
  }
  return 1;
}

// Glass's own textures let go of: the levels (made again at the size the next capture needs, ensure_levels, its blur
// the same to the byte). Unbound first where a pass or the glass's draw left them (t0-t2): the context holds what is
// bound till it is bound again.
static int ad_mode = 1;  // 1 = update, 0 = read-only, -1 = stateless
static int ad_backdrop_changed = 1;
static double ad_cur_dt = 0.0;

static void ad_forget(RenderWin *w) {
  if (!w) return;
  memset(w->ad_slots, 0, sizeof w->ad_slots);
  w->ad_last_time = 0.0;
  w->ad_settling = 0;
}

static int ensure_ad_tex(RenderWin *w) {
  if (!w || !dev) return 0;
  if (w->ad_tex[0].t) return 1;
  for (int i = 0; i < 2; i++) {
    w->ad_tex[i] = level_tex(DXGI_FORMAT_R8G8B8A8_UNORM, MB_ADAPTIVE_MAX, 1);
    if (!w->ad_tex[i].t) return 0;
  }
  w->ad_cur = 0;
  return 1;
}

static void ad_fill_gu(RenderWin *w) {
  if (!w || ad_mode < 0) {
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

struct WinAdPend {
  int sid, is_new, is_dark, through;
  float cx, cy, hx, hy, tint_a;
};

static inline uint64_t fnv1a_bytes(uint64_t h, const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) {
    h ^= (uint64_t)b[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static int ad_collect_and_match(RenderWin *w, const float *f, int32_t start, int32_t count, int backdrop_changed,
                                int under_missing, double dt, uint64_t serial, WinAdPend out[MB_ADAPTIVE_MAX]) {
  if (!w || ad_mode <= 0 || dt < 0.0) return 0;
  const float *a = f + (size_t)start * MB_INST;
  int kind = (int)(a[5] + 0.5f);
  if (kind != 6) return 0;
  int merged_liquid = a[23] > 0.0f;
  struct { float cx, cy, hx, hy; int is_dark; float tint_a; } items[MB_ADAPTIVE_MAX];
  int ni = 0;
  if (merged_liquid) {
    int is_clear = (a[7] <= 0.1f && a[11] < 0.12f);
    int is_adaptive = a[14] < -0.5f;
    if (is_adaptive && !is_clear) {
      float lum = 0.2126f * a[8] + 0.7152f * a[9] + 0.0722f * a[10];
      items[ni].cx = (gu.gb[0] + gu.gb[2]) * 0.5f;
      items[ni].cy = (gu.gb[1] + gu.gb[3]) * 0.5f;
      items[ni].hx = fmaxf((gu.gb[2] - gu.gb[0]) * 0.5f, 1.0f);
      items[ni].hy = fmaxf((gu.gb[3] - gu.gb[1]) * 0.5f, 1.0f);
      items[ni].is_dark = (a[11] >= 0.05f && lum < 0.35f) ? 1 : 0;
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
      items[ni].is_dark = (b[11] >= 0.05f && lum < 0.35f) ? 1 : 0;
      items[ni].tint_a = fminf(fmaxf(b[11], 0.0f), 1.0f);
      ni++;
    }
  }
  int cur_through = (gu.misc[1] > 0.5f) ? 1 : 0;
  int n_upd = 0;
  for (int k = 0; k < ni; k++) {
    int sid = -1, is_new = 0, mode_changed = 0;
    for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
      MBAdSlot *s = &w->ad_slots[i];
      if (s->active && s->seen_serial == serial && s->is_dark == items[k].is_dark &&
          hypotf(items[k].cx - s->cx, items[k].cy - s->cy) <= 2.0f) {
        sid = i;
        if (fabsf(s->tint_a - items[k].tint_a) > 0.002f || s->through != cur_through) {
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
        if (!s->active || s->seen_serial == serial || s->is_dark != items[k].is_dark) continue;
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
        memset(&w->ad_slots[sid], 0, sizeof w->ad_slots[sid]);
        w->ad_slots[sid].active = 1;
        w->ad_slots[sid].is_dark = items[k].is_dark;
        w->ad_slots[sid].through = cur_through;
        w->ad_slots[sid].tint_a = items[k].tint_a;
      } else if (fabsf(w->ad_slots[sid].tint_a - items[k].tint_a) > 0.002f ||
                 w->ad_slots[sid].through != cur_through) {
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
    int item_changed = mode_changed;
    if (backdrop_changed) {
      float gx0 = s->cx - s->hx, gy0 = s->cy - s->hy, gx1 = s->cx + s->hx, gy1 = s->cy + s->hy;
      uint64_t h = 14695981039346656037ULL;
      h = fnv1a_bytes(h, last_bg, sizeof last_bg);
      h = fnv1a_bytes(h, &fb_w, sizeof fb_w);
      h = fnv1a_bytes(h, &fb_h, sizeof fb_h);
      h = fnv1a_bytes(h, &vscale, sizeof vscale);
      int has_stream = under_missing ? 1 : 0;
      for (int32_t i = 0; i < start; i++) {
        const float *b = f + (size_t)i * MB_INST;
        if (b[32] == 0.0f) {
          float x0 = b[0], y0 = b[1], x1 = x0 + b[2], y1 = y0 + b[3];
          if (!(x1 > gx0 && x0 < gx1 && y1 > gy0 && y0 < gy1)) continue;
        }
        if (b[20] > (float)MB_STREAM_TEX) has_stream = 1;
        h = fnv1a_bytes(h, b, (size_t)MB_INST * sizeof(float));
      }
      if (is_new || !s->under_valid || has_stream || h != s->under_hash) item_changed = 1;
      s->under_hash = h;
      s->under_valid = !has_stream;
    }
    int need = is_new || item_changed || s->settle_left > 0.0;
    if (need) {
      if (is_new || s->is_dark > 0 || s->tint_a >= 0.88f) {
        s->settle_left = 0.0;
      } else if (item_changed) {
        s->settle_left = MB_AD_SETTLE_SEC;
      } else {
        s->settle_left -= dt;
        if (s->settle_left <= 0.0) s->settle_left = 0.0;
      }
      s->updated_serial = serial;
      out[n_upd++] = {sid, is_new, s->is_dark, s->through, s->cx, s->cy, s->hx, s->hy, s->tint_a};
    }
  }
  return n_upd;
}

static void ad_run_updates(RenderWin *w, const WinAdPend *pend, int n_upd, double dt) {
  if (!w || n_upd <= 0 || !gsrv || !ad_ps || !ad_cb) return;
  if (!ensure_ad_tex(w)) return;
  int cur = w->ad_cur, nxt = 1 - cur;
  ID3D11ShaderResourceView *none6[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
  ctx->PSSetShaderResources(0, 6, none6);
  ctx->CopyResource(w->ad_tex[nxt].t, w->ad_tex[cur].t);
  ctx->OMSetRenderTargets(1, &w->ad_tex[nxt].rt, NULL);
  D3D11_VIEWPORT vp = {0, 0, (float)MB_ADAPTIVE_MAX, 1.0f, 0, 1};
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetState(raster_cut);
  ctx->VSSetShader(pvs, NULL, 0);
  ctx->PSSetShader(ad_ps, NULL, 0);
  ctx->IASetInputLayout(NULL);
  ctx->OMSetBlendState(NULL, NULL, 0xffffffff);
  ctx->PSSetConstantBuffers(0, 1, &ad_cb);
  ID3D11ShaderResourceView *srvs[2] = {gsrv, w->ad_tex[cur].srv};
  ctx->PSSetShaderResources(0, 2, srvs);
  ctx->PSSetSamplers(0, 1, &sampler);
  for (int i = 0; i < n_upd; i++) {
    D3D11_RECT sc = {pend[i].sid, 0, pend[i].sid + 1, 1};
    ctx->RSSetScissorRects(1, &sc);
    struct { float map[4]; float uvc[4]; float box[4]; float param[4]; } u;
    memcpy(u.map, gu.map, sizeof u.map);
    memcpy(u.uvc, gu.uvc, sizeof u.uvc);
    u.box[0] = pend[i].cx; u.box[1] = pend[i].cy; u.box[2] = pend[i].hx; u.box[3] = pend[i].hy;
    u.param[0] = (pend[i].is_dark > 0) ? (float)pend[i].is_dark : -pend[i].tint_a;
    u.param[1] = (float)pend[i].is_new + (pend[i].through ? 2.0f : 0.0f);
    u.param[2] = (float)dt;
    u.param[3] = (float)pend[i].sid;
    ctx->UpdateSubresource(ad_cb, 0, NULL, &u, 0, 0);
    ctx->Draw(3, 0);
  }
  ctx->PSSetShaderResources(0, 2, none6);
  w->ad_cur = nxt;
}

static double ad_next_dt(RenderWin *w) {
  LARGE_INTEGER freq, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  double t = freq.QuadPart ? (double)now.QuadPart / (double)freq.QuadPart : 0.0;
  double dt = (w && w->ad_last_time > 0.0) ? (t - w->ad_last_time) : (1.0 / 60.0);
  if (w) w->ad_last_time = t;
  if (dt < 0.0) dt = 0.0;
  if (dt > 0.1) dt = 0.1;
  return dt;
}

static void ad_finalize_frame(RenderWin *w, int whole_frame, uint64_t serial) {
  if (!w) return;
  int settling = 0;
  for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
    MBAdSlot *s = &w->ad_slots[i];
    if (!s->active) continue;
    if (whole_frame && s->seen_serial != serial) {
      s->active = 0;
      s->settle_left = 0.0;
      continue;
    }
    if (s->settle_left > 0.0) settling = 1;
  }
  w->ad_settling = settling;
}

static void glass_free(void) {
  if (!ctx) return;
  ID3D11ShaderResourceView *none[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
  ctx->PSSetShaderResources(0, 6, none);
  gsrv = NULL;
  for (int j = 0; j < 5; j++) {
    if (lv[j].srv) lv[j].srv->Release();
    if (lv[j].rt) lv[j].rt->Release();
    if (lv[j].t) lv[j].t->Release();
    lv[j] = level{};
  }
  for (int k = 0; k < MB_WINS; k++) {
    for (int i = 0; i < 2; i++) {
      if (render_wins[k].ad_tex[i].srv) render_wins[k].ad_tex[i].srv->Release();
      if (render_wins[k].ad_tex[i].rt) render_wins[k].ad_tex[i].rt->Release();
      if (render_wins[k].ad_tex[i].t) render_wins[k].ad_tex[i].t->Release();
      render_wins[k].ad_tex[i] = level{};
    }
    ad_forget(&render_wins[k]);
  }
  lv_w = lv_h = 0;
  for (int j = 0; j < 2; j++) {
    if (lens_lv[j].srv) lens_lv[j].srv->Release();
    if (lens_lv[j].rt) lens_lv[j].rt->Release();
    if (lens_lv[j].t) lens_lv[j].t->Release();
    lens_lv[j] = level{};
  }
  lens_w = lens_h = 0;
}

// Glass's levels (as big as the largest capture yet) let go of once this many frames in a row drew no glass (a sheet
// closed, the composer gone): held on, they are tens of MB at a window's size.
#define MB_GLASS_IDLE 180
static int glass_idle_frames;
static int glass_drawn;  // a frame drawing now reached glass (encode)

static void glass_idle_tick(void) {
  int any_glass = glass_drawn;
  if (!any_glass) {
    for (int k = 0; k < MB_WINS; k++) {
      if (render_wins[k].has_glass) {
        any_glass = 1;
        break;
      }
    }
  }
  if (any_glass) glass_idle_frames = 0;
  else if (glass_idle_frames < MB_GLASS_IDLE && ++glass_idle_frames == MB_GLASS_IDLE && lv_w > 0) glass_free();
  glass_drawn = 0;
}

// Offscreen blur layers (kind == 14 PUSH_BLUR_LAYER / kind == 15 POP_BLUR_LAYER): lazily allocated
// B8G8R8A8_UNORM layer targets and blur pyramid (full 8-bit alpha for smooth premultiplied edges).
#define MB_BLUR_LAYERS 4
static level bl_lv[MB_BLUR_LAYERS];
static int bl_w, bl_h;
static level bl_py[5];
static int bl_py_w, bl_py_h;

static void blur_layers_release(void) {
  if (ctx) {
    ID3D11ShaderResourceView *none[3] = {NULL, NULL, NULL};
    ctx->PSSetShaderResources(0, 3, none);
  }
  for (int j = 0; j < MB_BLUR_LAYERS; j++) {
    if (bl_lv[j].srv) bl_lv[j].srv->Release();
    if (bl_lv[j].rt) bl_lv[j].rt->Release();
    if (bl_lv[j].t) bl_lv[j].t->Release();
    bl_lv[j] = level{};
  }
  bl_w = bl_h = 0;
  for (int j = 0; j < 5; j++) {
    if (bl_py[j].srv) bl_py[j].srv->Release();
    if (bl_py[j].rt) bl_py[j].rt->Release();
    if (bl_py[j].t) bl_py[j].t->Release();
    bl_py[j] = level{};
  }
  bl_py_w = bl_py_h = 0;
}

static int ensure_blur_layer(int depth, int tw, int th) {
  if (!dev || depth < 0 || depth >= MB_BLUR_LAYERS || tw <= 0 || th <= 0) return 0;
  if (bl_w != tw || bl_h != th) {
    for (int j = 0; j < MB_BLUR_LAYERS; j++) {
      if (bl_lv[j].srv) bl_lv[j].srv->Release();
      if (bl_lv[j].rt) bl_lv[j].rt->Release();
      if (bl_lv[j].t) bl_lv[j].t->Release();
      bl_lv[j] = level{};
    }
    bl_w = tw;
    bl_h = th;
  }
  if (!bl_lv[depth].t) {
    bl_lv[depth] = level_tex(DXGI_FORMAT_B8G8R8A8_UNORM, tw, th);
    if (!bl_lv[depth].t || !bl_lv[depth].rt || !bl_lv[depth].srv) {
      bl_lv[depth] = level{};
      return 0;
    }
  }
  return 1;
}

static int ensure_blur_py(int w, int h) {
  if (!dev || w <= 0 || h <= 0) return 0;
  if (bl_py[0].t && bl_py_w >= w && bl_py_h >= h) return 1;
  for (int j = 0; j < 5; j++) {
    if (bl_py[j].srv) bl_py[j].srv->Release();
    if (bl_py[j].rt) bl_py[j].rt->Release();
    if (bl_py[j].t) bl_py[j].t->Release();
    bl_py[j] = level{};
  }
  bl_py_w = w > bl_py_w ? w : bl_py_w;
  bl_py_h = h > bl_py_h ? h : bl_py_h;
  for (int j = 0; j < 4; j++) {
    bl_py[j] = level_tex(DXGI_FORMAT_B8G8R8A8_UNORM, (bl_py_w + (1 << j) - 1) >> j,
                         (bl_py_h + (1 << j) - 1) >> j);
    if (!bl_py[j].t || !bl_py[j].rt || !bl_py[j].srv) {
      blur_layers_release();
      return 0;
    }
  }
  bl_py[4] = level_tex(DXGI_FORMAT_B8G8R8A8_UNORM, bl_py_w, bl_py_h);
  if (!bl_py[4].t || !bl_py[4].rt || !bl_py[4].srv) {
    blur_layers_release();
    return 0;
  }
  return 1;
}

static int ensure_bl_comp_vbuf(void) {
  if (bl_comp_vbuf) return 1;
  if (!dev) return 0;
  D3D11_BUFFER_DESC bd = {(UINT)(MB_INST * sizeof(float)), D3D11_USAGE_DYNAMIC,
                          D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  if (FAILED(dev->CreateBuffer(&bd, NULL, &bl_comp_vbuf)) || !bl_comp_vbuf) {
    bl_comp_vbuf = NULL;
    return 0;
  }
  return 1;
}

// Physical-pixel offscreen tile layers (MB_LAYER_TEX + id): lazily allocated B8G8R8A8_UNORM texture + RTV + SRV.
struct MBWinLayer {
  int32_t id;
  ID3D11Texture2D *t;
  ID3D11RenderTargetView *rt;
  ID3D11ShaderResourceView *srv;
  int pw, ph;
};
static MBWinLayer *win_layers;
static int win_layers_n, win_layers_cap;

static ID3D11ShaderResourceView *mb_layer_srv(int id) {
  for (int i = 0; i < win_layers_n; i++)
    if (win_layers[i].id == id) return win_layers[i].srv;
  return NULL;
}

static void win_layers_release(void) {
  if (ctx) {
    ID3D11ShaderResourceView *none[3] = {NULL, NULL, NULL};
    ctx->PSSetShaderResources(0, 3, none);
  }
  for (int i = 0; i < win_layers_n; i++) {
    if (win_layers[i].srv) win_layers[i].srv->Release();
    if (win_layers[i].rt) win_layers[i].rt->Release();
    if (win_layers[i].t) win_layers[i].t->Release();
  }
  free(win_layers);
  win_layers = NULL;
  win_layers_n = win_layers_cap = 0;
}

// The pieces of the frame's shapes (src/ui/shape.mbt: the instances of kind 12 after every one drawn), for the pixel
// shader to read (segs, t3): as they are, 15 fours an instance.
static ID3D11Buffer *segs_buf;
static ID3D11ShaderResourceView *segs_srv;
static size_t segs_cap;

// What drawing holds beyond the frames given back (a window minimized or hidden: win_app.c): glass's levels,
// transient shape-segment buffer, and hidden primary window's canvas/swapchain, made again as the next frame draws.
void mb_render_trim(void) {
  glass_free();
  blur_layers_release();
  glass_idle_frames = 0;
  if (segs_srv) segs_srv->Release(), segs_srv = NULL;
  if (segs_buf) segs_buf->Release(), segs_buf = NULL;
  segs_cap = 0;
  RenderWin *back = rw;
  rw = &render_wins[0];
  if (target_hwnd && !IsWindowVisible(target_hwnd)) {
    if (ctx) ctx->OMSetRenderTargets(0, NULL, NULL);
    ensure_canvas(0, 0);
    rw->shown_ok = 0;
    rw->swap_needs_full = 1;
    if (chain && (fb_w > 16 || fb_h > 16)) {
      if (rtv) rtv->Release(), rtv = NULL;
      chain->ResizeBuffers(0, 16, 16, DXGI_FORMAT_UNKNOWN, 0);
      ID3D11Texture2D *back_tex = NULL;
      if (SUCCEEDED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back_tex))) {
        dev->CreateRenderTargetView(back_tex, NULL, &rtv);
        back_tex->Release();
      }
      fb_w = 16;
      fb_h = 16;
      if (rtv && ctx) {
        float clear[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(rtv, clear);
        chain->Present(0, 0);
        win_drop_stats_reset(&rw->drop_stats);
      }
    }
    if (dcomp) dcomp->Commit();
  }
  rw = back;
}

static void post_pass(level *src, int sw, int sh, level *dst, int w, int h, int mode, float sg, float dx, float dy) {
  ID3D11ShaderResourceView *none[3] = {NULL, NULL, NULL};
  ctx->PSSetShaderResources(0, 3, none);
  ctx->OMSetRenderTargets(1, &dst->rt, NULL);
  D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
  ctx->RSSetViewports(1, &vp);
  struct { float a[4], b[4]; MBContained edge; } u = {
    {1.0f / src->w, 1.0f / src->h, (float)mode, sg}, {(sw - 0.5f) / src->w, (sh - 0.5f) / src->h, dx, dy}, post_edge};
  ctx->UpdateSubresource(pbuf, 0, NULL, &u, 0, 0);
  ctx->PSSetConstantBuffers(0, 1, &pbuf);
  ctx->PSSetShaderResources(0, 1, &src->srv);
  ctx->Draw(3, 0);
}

// Partial redraw: a window's frame is drawn into a canvas of its own, kept across frames, and
// copied into the swap chain's buffer (a flip model's holds a frame before the last); where the app says only some
// parts differ from the frame before (@hostapi.render_dirty), only those parts of the canvas are drawn again, each cut
// to it (the scissor, `cut`, while `cut_on`), the frame's ground over it first (the instance after the frame's,
// submit's), then every instance. Without a window the offscreen target is the canvas.
// (the canvas, its size and whether it holds the last frame given are the window's: RenderWin above) what the offscreen
// target holds, the frame drawn last
static int off_ok;
// (the parts of the next frame that differ, points: x, y, w, h, all of it, `dirty_whole`, and whether any was said, the
// window's) a picture the frame being drawn has none of yet
static int encode_missing;
static int cut_on;
static D3D11_RECT cut;

static float g_light_angle = 0.0f;

extern "C" void mb_host_glass_light_angle(double rad) {
  g_light_angle = (float)rad;
}

extern "C" void mb_host_render_dirty(double x, double y, double w, double h) {
  dirty_told = 1;
  if (w < 0 || h < 0) {
    dirty_whole = 1;
  } else if (w > 0 && h > 0) {
    // (more than it keeps: the last one grown round it)
    if (dirty_n == MB_PARTS) {
      double *d = dirty[MB_PARTS - 1];
      double x1 = fmax(d[0] + d[2], x + w), y1 = fmax(d[1] + d[3], y + h);
      d[0] = fmin(d[0], x);
      d[1] = fmin(d[1], y);
      d[2] = x1 - d[0];
      d[3] = y1 - d[1];
    } else {
      double *d = dirty[dirty_n++];
      d[0] = x; d[1] = y; d[2] = w; d[3] = h;
    }
  }
}

// The parts of a target tw × th pixels the next frame draws, from what was said of them (taken: a frame nothing was
// said for is drawn whole), into `r`: how many (0 none), -1 all of it. `kept`: the target holds the frame before.
static int take_parts(int tw, int th, int kept, D3D11_RECT r[MB_PARTS]) {
  int told = dirty_told, whole = dirty_whole, n = dirty_n;
  dirty_told = dirty_whole = dirty_n = 0;
  if (!told || whole || !kept || pictures_missing) return -1;
  int k = 0;
  for (int i = 0; i < n; i++) {
    // (whole pixels round it, and one more)
    int x0 = (int)floor(dirty[i][0] * vscale) - 1, y0 = (int)floor(dirty[i][1] * vscale) - 1;
    int x1 = (int)ceil((dirty[i][0] + dirty[i][2]) * vscale) + 1, y1 = (int)ceil((dirty[i][1] + dirty[i][3]) * vscale) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw) x1 = tw;
    if (y1 > th) y1 = th;
    if (x1 > x0 && y1 > y0) r[k++] = {x0, y0, x1, y1};
  }
  return k;
}

// The canvas a window's frames are drawn into, tw × th pixels: made again at another size, what it held lost.
static void ensure_canvas(int tw, int th) {
  if (canvas && canvas_w == tw && canvas_h == th) return;
  if (canvas_rtv) canvas_rtv->Release();
  if (canvas) canvas->Release();
  canvas = NULL;
  canvas_rtv = NULL;
  canvas_ok = 0;
  canvas_w = tw;
  canvas_h = th;
  if (tw <= 0 || th <= 0) return;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)tw;
  td.Height = (UINT)th;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(dev->CreateTexture2D(&td, NULL, &canvas))) {
    canvas = NULL;
    return;
  }
  if (FAILED(dev->CreateRenderTargetView(canvas, NULL, &canvas_rtv))) {
    canvas->Release();
    canvas = NULL;
    canvas_rtv = NULL;
  }
}

// what the run [start, end) of glass sees, from target (pw × ph) into gsrv and gu, frosted glass of the same capture
// further on (glass.mbt shares_capture) too; 0 when none of it shows
static int capture(ID3D11RenderTargetView *target, int pw, int ph, const float *f, int32_t start, int32_t end,
                   int32_t count) {
  const float *a = f + start * MB_INST;
  // For frosted glass (kind 5), negative slot 7 is contained; for liquid glass (kind 6), negative slot 7 encodes clear-lens zoom.
  int kind = (int)(a[5] + 0.5f);
  int is_contained = (kind == 5 && a[7] < 0.0f);
  int is_progressive = (kind == 5 && a[22] < -0.5f);
  float raw_blur = (kind == 6 && a[7] < 0.0f) ? 0.0f : fabsf(a[7]);
  int is_clear_liq = (kind == 6 && a[7] >= -0.001f && a[7] <= 0.1f && a[11] < 0.12f);
  float col_ch = fmaxf(fmaxf(a[8], a[9]), a[10]) - fminf(fminf(a[8], a[9]), a[10]);
  float blur_r = (is_clear_liq && a[11] > 0.01f && raw_blur < 0.1f) ? 1.2f : raw_blur;
  float g_lo = fminf(fmaxf((a[11] - 0.02f) / 0.085f, 0.0f), 1.0f);
  g_lo = g_lo * g_lo * (3.0f - 2.0f * g_lo);
  float g_neu = fminf(fmaxf((a[11] - 0.405f) / 0.095f, 0.0f), 1.0f);
  g_neu = 1.0f - g_neu * g_neu * (3.0f - 2.0f * g_neu);
  float g_ch = fminf(fmaxf((col_ch - 0.08f) / 0.08f, 0.0f), 1.0f);
  g_ch = g_ch * g_ch * (3.0f - 2.0f * g_ch);
  float g_ta = fminf(fmaxf((a[11] - 0.72f) / 0.08f, 0.0f), 1.0f);
  g_ta = 1.0f - g_ta * g_ta * (3.0f - 2.0f * g_ta);
  float dual_b = fminf(fmaxf((blur_r - 0.2f) / 0.7f, 0.0f), 1.0f);
  dual_b = dual_b * dual_b * (3.0f - 2.0f * dual_b);
  float dual_hi = fminf(fmaxf((blur_r - 3.5f) / 1.0f, 0.0f), 1.0f);
  dual_hi = 1.0f - dual_hi * dual_hi * (3.0f - 2.0f * dual_hi);
  float dual_sz = fminf(fmaxf((fminf(a[2], a[3]) - 16.0f) / 12.0f, 0.0f), 1.0f);
  dual_sz = dual_sz * dual_sz * (3.0f - 2.0f * dual_sz);
  float min_dim = fminf(a[2], a[3]);
  float tall_t = fminf(fmaxf((min_dim - 44.0f) / 18.0f, 0.0f), 1.0f);
  tall_t = tall_t * tall_t * (3.0f - 2.0f * tall_t);
  float dual_base = 0.68f + 0.18f * tall_t;
  float dual_w = (kind == 6 && !is_clear_liq) ? (dual_base * dual_b * dual_hi * dual_sz * g_lo * fmaxf(g_neu, g_ch * g_ta)) : 0.0f;
  int is_dual = (dual_w > 0.005f);
  float dual_wsp = is_dual ? 14.0f : 0.0f;
  int merged_liquid = (kind == 6 && a[23] > 0.0f);
  int unmerged_liquid = (kind == 6 && !merged_liquid);
  float clear_refract_scale = (kind == 6 && fabsf(a[7]) < 0.01f && a[11] > 0.005f && a[11] < 0.31f) ? 2.20f : 1.0f;
  float margin = (is_contained || is_progressive) ? 0.0f : 3.0f * blur_r + (kind == 6 ? a[21] * clear_refract_scale * (1.0f + fabsf(a[22])) + fmaxf(a[23], 0.0f) : 0.0f) + 2.0f;
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  int n = 0;
  for (int32_t i = start; i < end; i++) {
    const float *b = f + i * MB_INST;
    float bx0 = b[0] > b[16] ? b[0] : b[16], by0 = b[1] > b[17] ? b[1] : b[17];
    float bx1 = b[0] + b[2] < b[18] ? b[0] + b[2] : b[18], by1 = b[1] + b[3] < b[19] ? b[1] + b[3] : b[19];
    if (kind == 6) {
      float m23 = fmaxf(unmerged_liquid ? b[23] : a[23], 0.0f);
      bx0 = b[0] - m23; by0 = b[1] - m23; bx1 = b[0] + b[2] + m23; by1 = b[1] + b[3] + m23;
      if (merged_liquid && n < 8) {
        for (int k = 0; k < 4; k++) gu.shape[n][k] = b[k];
        if (fmaxf(fmaxf(b[36], b[37]), fmaxf(b[38], b[39])) > 0.0f) {
          for (int k = 0; k < 4; k++) gu.rad[n][k] = b[36 + k];
        } else {
          for (int k = 0; k < 4; k++) gu.rad[n][k] = b[4];
        }
        n++;
      }
      if (unmerged_liquid) {
        float bm = 3.0f * blur_r + b[21] * clear_refract_scale * (1.0f + fabsf(b[22])) + m23 + 2.0f;
        if (bm > margin) margin = bm;
      }
    }
    if (bx0 < x0) x0 = bx0;
    if (by0 < y0) y0 = by0;
    if (bx1 > x1) x1 = bx1;
    if (by1 > y1) y1 = by1;
  }
  for (int32_t i = end; (kind == 5 || unmerged_liquid) && i < count; i++) {
    const float *b = f + i * MB_INST;
    if (b[20] != a[20]) continue;
    float bx0, by0, bx1, by1;
    if (unmerged_liquid) {
      float m23 = fmaxf(b[23], 0.0f);
      bx0 = b[0] - m23; by0 = b[1] - m23; bx1 = b[0] + b[2] + m23; by1 = b[1] + b[3] + m23;
      {
        float bm = 3.0f * blur_r + b[21] * clear_refract_scale * (1.0f + fabsf(b[22])) + m23 + 2.0f;
        if (bm > margin) margin = bm;
      }
    } else {
      bx0 = b[0] > b[16] ? b[0] : b[16]; by0 = b[1] > b[17] ? b[1] : b[17];
      bx1 = b[0] + b[2] < b[18] ? b[0] + b[2] : b[18]; by1 = b[1] + b[3] < b[19] ? b[1] + b[3] : b[19];
    }
    if (bx0 < x0) x0 = bx0;
    if (by0 < y0) y0 = by0;
    if (bx1 > x1) x1 = bx1;
    if (by1 > y1) y1 = by1;
  }
  gu.misc[0] = (float)n;
  float through_hole = 0.0f;
  if (kind == 6) {
    float gx0 = x0 - margin, gy0 = y0 - margin, gx1 = x1 + margin, gy1 = y1 + margin;
    for (int32_t hi = 0; hi < start; hi++) {
      const float *hb = f + (size_t)hi * MB_INST;
      if ((int)(hb[5] + 0.5f) != 9) continue;
      float hx0 = fmaxf(hb[0], hb[16]), hy0 = fmaxf(hb[1], hb[17]);
      float hx1 = fminf(hb[0] + hb[2], hb[18]), hy1 = fminf(hb[1] + hb[3], hb[19]);
      if (hx1 > hx0 && hy1 > hy0 && hx0 < gx1 && gx0 < hx1 && hy0 < gy1 && gy0 < hy1) {
        through_hole = 1.0f;
        break;
      }
    }
  }
  gu.misc[1] = through_hole;
  gu.misc[2] = cosf(g_light_angle);
  gu.misc[3] = sinf(g_light_angle);
  gu.gb[0] = x0 - 2; gu.gb[1] = y0 - 2; gu.gb[2] = x1 + 2; gu.gb[3] = y1 + 2;
  float sc = (float)vscale;
  int px0 = (int)floorf((x0 - margin) * sc), py0 = (int)floorf((y0 - margin) * sc);
  int px1 = (int)ceilf((x1 + margin) * sc), py1 = (int)ceilf((y1 + margin) * sc);
  if (px0 < 0) px0 = 0;
  if (py0 < 0) py0 = 0;
  if (px1 > pw) px1 = pw;
  if (py1 > ph) py1 = ph;
  if (px1 <= px0 || py1 <= py0 || x1 <= x0 || y1 <= y0) return 0;
  // (a capture outside the part drawn: neither copied nor drawn. One that meets it is in it whole: @damage)
  if (cut_on && (px1 <= cut.left || cut.right <= px0 || py1 <= cut.top || cut.bottom <= py0)) return 0;
  int w = px1 - px0, h = py1 - py0;
  int vis_x = 0, vis_y = 0, vis_w = w, vis_h = h;
  if (is_progressive) {
    int vx0 = (int)floorf(x0 * sc) - px0 - 2;
    int vy0 = (int)floorf(y0 * sc) - py0 - 2;
    int vx1 = (int)ceilf(x1 * sc) - px0 + 2;
    int vy1 = (int)ceilf(y1 * sc) - py0 + 2;
    if (vx0 < 0) vx0 = 0;
    if (vy0 < 0) vy0 = 0;
    if (vx1 > w) vx1 = w;
    if (vy1 > h) vy1 = h;
    if (vx1 <= vx0) { vx0 = 0; vx1 = w; }
    if (vy1 <= vy0) { vy0 = 0; vy1 = h; }
    vis_x = vx0; vis_y = vy0; vis_w = vx1 - vx0; vis_h = vy1 - vy0;
  }
  if (!ensure_levels(w, h, pw, ph)) return 0;
  ID3D11Resource *res = NULL;
  target->GetResource(&res);
  D3D11_BOX box = {(UINT)px0, (UINT)py0, 0, (UINT)px1, (UINT)py1, 1};
  ctx->CopySubresourceRegion(lv[0].t, 0, 0, 0, 0, res, 0, &box);
  res->Release();
  ctx->VSSetShader(pvs, NULL, 0);
  ctx->PSSetShader(pps, NULL, 0);
  ctx->IASetInputLayout(NULL);
  ctx->OMSetBlendState(NULL, NULL, 0xffffffff);
  // (the passes over the levels whole, not cut to the part)
  ctx->RSSetState(raster);
  int contained = is_contained ? mb_contained_make(&post_edge, a, px0 / sc, py0 / sc, 1.0f / sc, 1.0f / sc) : 0;
  float sharp_fade = fminf(fmaxf((blur_r - 1.5f) / 1.5f, 0.0f), 1.0f);
  sharp_fade = sharp_fade * sharp_fade * (3.0f - 2.0f * sharp_fade);
  float crisp = fminf(dual_w / 0.10f, 1.0f);
  float sp = blur_r * (1.0f - crisp * (1.0f - sharp_fade)) * sc;
  int k = 0;
  if (sp >= 2.4f) k = 1;
  while (k < 3 && sp / (float)(1 << (k + 1)) >= 2.0f) k++;
  if (is_dual && k == 0 && sc >= 2.0f) k = 1;
  if (is_progressive && k == 0 && sp > 0.5f) k = 1;
  if (is_progressive && sp >= 2.5f && k < 2) k = 2;
  if (k >= 3) { is_dual = 0; dual_w = 0.0f; dual_wsp = 0.0f; }
  int lw[4] = {w}, lh[4] = {h};
  for (int j = 1; j <= 3; j++) {
    lw[j] = (lw[j - 1] + 1) / 2;
    lh[j] = (lh[j - 1] + 1) / 2;
  }
  float sk_f = sp / (float)(1 << k);
  float pre_f = k > 0 ? 0.75f * (1.0f - powf(0.25f, (float)k)) / 3.0f + 1.0f / 6.0f : 0.0f;
  float sg_f = sqrtf(sk_f * sk_f - pre_f > 0 ? sk_f * sk_f - pre_f : 0);
  int dual_fused_v = 0;
  if (is_dual && prog_ps) {
    for (int j = 1; j <= 3; j++) {
      post_pass(&lv[j - 1], lw[j - 1], lh[j - 1], &lv[j], lw[j], lh[j], 1, 0, 0, 0);
    }
    float wsp = (dual_wsp > 0.0f ? dual_wsp : 14.0f) * sc;
    float wsk = wsp / 8.0f;
    float wpre = 0.75f * (1.0f - 1.0f / 64.0f) / 3.0f + 1.0f / 6.0f;
    float wsg = sqrtf(fmaxf(wsk * wsk - wpre, 0.0f));
    if (wsg > 0.3f) {
      post_pass(&lv[3], lw[3], lh[3], &lv[4], lw[3], lh[3], 6, wsg, 1, 0);
      post_pass(&lv[4], lw[3], lh[3], &lv[3], lw[3], lh[3], 6, wsg, 0, 1);
    }
    float dual_cut = k > 0 ? 0.55f : 0.3f;
    dual_fused_v = (sg_f > dual_cut);
    if (dual_fused_v) {
      post_pass(&lv[k], lw[k], lh[k], &lv[4], lw[k], lh[k], 2, sg_f, 1, 0);
    }
    memset(&post_edge, 0, sizeof post_edge);
    float d3 = (float)(1 << (3 - k));
    float tw3 = (float)lv[3].w, th3 = (float)lv[3].h;
    post_edge.rc[0] = ((float)lw[3] - 0.5f) / tw3;
    post_edge.rc[1] = ((float)lh[3] - 0.5f) / th3;
    post_edge.rc[2] = 1.0f - dual_w;
    post_edge.rc[3] = k > 0 ? 0.277f : 0.0f;
    post_edge.rs[0] = 1.0f / (d3 * tw3); post_edge.rs[1] = 1.0f / (d3 * th3);
    post_edge.rs[2] = 1.0f / tw3;        post_edge.rs[3] = 1.0f / th3;
    level *src = dual_fused_v ? &lv[4] : &lv[k];
    level *dst = dual_fused_v ? &lv[k] : &lv[4];
    ID3D11ShaderResourceView *none4[4] = {NULL, NULL, NULL, NULL};
    ctx->PSSetShaderResources(0, 4, none4);
    ctx->PSSetShader(prog_ps, NULL, 0);
    ctx->OMSetRenderTargets(1, &dst->rt, NULL);
    D3D11_VIEWPORT vp = {0, 0, (float)lw[k], (float)lh[k], 0, 1};
    ctx->RSSetViewports(1, &vp);
    struct { float a[4], b[4]; MBContained edge; } u = {
      {1.0f / src->w, 1.0f / src->h, 5.0f, dual_fused_v ? sg_f : 0.0f},
      {((float)lw[k] - 0.5f) / src->w, ((float)lh[k] - 0.5f) / src->h, 0.0f, dual_fused_v ? 1.0f : 0.0f},
      post_edge};
    ctx->UpdateSubresource(pbuf, 0, NULL, &u, 0, 0);
    ctx->PSSetConstantBuffers(0, 1, &pbuf);
    ID3D11ShaderResourceView *srvs[4] = {src->srv, NULL, NULL, lv[3].srv};
    ctx->PSSetShaderResources(0, 4, srvs);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 4, none4);
  } else if (is_progressive && prog_ps) {
    float prev_var = 0.0f;
    for (int j = 1; j <= k; j++) {
      post_pass(&lv[j - 1], lw[j - 1], lh[j - 1], &lv[j], lw[j], lh[j], 1, 0, 0, 0);
      float target_px = powf(fmaxf(sp, 1.0f), (float)j / (float)k);
      float have_var = prev_var + 0.75f * (float)(1 << (2 * j - 2));
      float need_var = fmaxf(target_px * target_px - have_var, 0.0f);
      float sg_j = sqrtf(need_var) / (float)(1 << j);
      if (sg_j > 0.3f) {
        post_pass(&lv[j], lw[j], lh[j], &lv[4], lw[j], lh[j], 2, sg_j, 1, 0);
        post_pass(&lv[4], lw[j], lh[j], &lv[j], lw[j], lh[j], 2, sg_j, 0, 1);
      }
      prev_var = fmaxf(have_var, target_px * target_px);
    }
    memset(&post_edge, 0, sizeof post_edge);
    post_edge.map[0] = (float)px0 / sc; post_edge.map[1] = (float)py0 / sc;
    post_edge.map[2] = 1.0f / sc; post_edge.map[3] = 1.0f / sc;
    for (int i = 0; i < 4; i++) { post_edge.box[i] = a[52 + i]; post_edge.rr[i] = a[56 + i]; }
    post_edge.clip[0] = sp; post_edge.clip[1] = (float)k;
    float *ms[4] = {post_edge.rc, post_edge.rs, post_edge.outer, post_edge.orc};
    for (int j = 0; j < 4; j++) {
      float dj = (float)(1 << j), tw = (float)lv[j].w, th = (float)lv[j].h;
      ms[j][0] = 1.0f / (dj * tw); ms[j][1] = 1.0f / (dj * th);
      ms[j][2] = ((float)lw[j] - 0.5f) / tw; ms[j][3] = ((float)lh[j] - 0.5f) / th;
    }
    ID3D11ShaderResourceView *none4[4] = {NULL, NULL, NULL, NULL};
    ctx->PSSetShaderResources(0, 4, none4);
    ctx->PSSetShader(prog_ps, NULL, 0);
    ctx->OMSetRenderTargets(1, &lv[4].rt, NULL);
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    struct { float a[4], b[4]; MBContained edge; } u = {
      {1.0f / lv[0].w, 1.0f / lv[0].h, 4.0f, 0.0f}, {(w - 0.5f) / lv[0].w, (h - 0.5f) / lv[0].h, 0.0f, 0.0f}, post_edge};
    ctx->UpdateSubresource(pbuf, 0, NULL, &u, 0, 0);
    ctx->PSSetConstantBuffers(0, 1, &pbuf);
    ID3D11ShaderResourceView *srvs[4] = {lv[0].srv, lv[1].srv, lv[2].srv, lv[3].srv};
    ctx->PSSetShaderResources(0, 4, srvs);
    ctx->RSSetState(raster_cut);
    D3D11_RECT vsc = {vis_x, vis_y, vis_x + vis_w, vis_y + vis_h};
    ctx->RSSetScissorRects(1, &vsc);
    ctx->Draw(3, 0);
    ctx->RSSetState(raster);
    ctx->PSSetShaderResources(0, 4, none4);
  } else {
    if (contained) {
      ctx->PSSetShader(edge_ps, NULL, 0);
      post_pass(&lv[0], w, h, &lv[4], w, h, 3, 0, 0, 0);
      ctx->PSSetShader(pps, NULL, 0);
    }
    for (int j = 1; j <= k; j++)
      post_pass(j == 1 && contained ? &lv[4] : &lv[j - 1], lw[j - 1], lh[j - 1], &lv[j], lw[j], lh[j], 1, 0, 0, 0);
    // what is left of the deviation once the halvings' and the last look's (bilinear) spread are taken out
    if (sg_f > 0.3f) {
      level *from = contained && k == 0 ? &lv[4] : &lv[k];
      level *tmp = contained && k == 0 ? &lv[0] : &lv[4];
      post_pass(from, lw[k], lh[k], tmp, lw[k], lh[k], 2, sg_f, 1, 0);
      post_pass(tmp, lw[k], lh[k], from, lw[k], lh[k], 2, sg_f, 0, 1);
    }
    ID3D11ShaderResourceView *none[3] = {NULL, NULL, NULL};
    ctx->PSSetShaderResources(0, 3, none);
  }
  level *L = ((is_progressive && prog_ps) || (is_dual && prog_ps && !dual_fused_v) || (contained && k == 0)) ? &lv[4] : &lv[k];
  int sk = (is_progressive && prog_ps) ? 0 : k;
  float d = (float)(1 << sk);
  gsrv = L->srv;
  gu.map[0] = sc / d / L->w; gu.map[1] = sc / d / L->h;
  gu.map[2] = -(float)px0 / d / L->w; gu.map[3] = -(float)py0 / d / L->h;
  if (is_progressive && prog_ps) {
    gu.uvc[0] = ((float)vis_x + 0.5f) / L->w; gu.uvc[1] = ((float)vis_y + 0.5f) / L->h;
    gu.uvc[2] = ((float)(vis_x + vis_w) - 0.5f) / L->w; gu.uvc[3] = ((float)(vis_y + vis_h) - 0.5f) / L->h;
  } else {
    gu.uvc[0] = 0.5f / L->w; gu.uvc[1] = 0.5f / L->h; gu.uvc[2] = (lw[sk] - 0.5f) / L->w; gu.uvc[3] = (lh[sk] - 0.5f) / L->h;
  }
  WinAdPend ad_pend[MB_ADAPTIVE_MAX];
  int ad_n_upd = ad_collect_and_match(rw, f, start, count, ad_backdrop_changed, encode_missing, ad_cur_dt,
                                      rw ? rw->ad_frame_serial : 0, ad_pend);
  if (ad_n_upd > 0) ad_run_updates(rw, ad_pend, ad_n_upd, ad_cur_dt);
  ad_fill_gu(rw);
  return 1;
}

// counters of the last encode_rects (and of segs uploads), read by the tests
static int encode_inst_submitted;
static int encode_lens_drawn;
static int segs_upload_calls;

// segs_buf / segs_srv / segs_cap declared above mb_render_trim

static void segs_upload(const float *f, int32_t count) {
  if (!dev || !ctx || count <= 0) return;
  int32_t d = count;
  while (d > 0 && (int)(f[(size_t)(d - 1) * MB_INST + 5] + 0.5f) == 12) d--;
  if (d == count) return;
  size_t n_seg = (size_t)(count - d);
  if (n_seg > (size_t)UINT_MAX / ((size_t)MB_INST * sizeof(float))) return;
  size_t need = n_seg * MB_INST * sizeof(float);
  if (!segs_buf || !segs_srv || segs_cap < need) {
    if (segs_srv) {
      segs_srv->Release();
      segs_srv = NULL;
    }
    if (segs_buf) {
      segs_buf->Release();
      segs_buf = NULL;
    }
    segs_cap = 0;
    size_t cap = need <= ((size_t)UINT_MAX / 2) ? need * 2 : need;
    D3D11_BUFFER_DESC bd = {(UINT)cap, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (FAILED(dev->CreateBuffer(&bd, NULL, &segs_buf)) || !segs_buf) {
      segs_buf = NULL;
      if (cap != need) {
        cap = need;
        bd.ByteWidth = (UINT)cap;
        if (FAILED(dev->CreateBuffer(&bd, NULL, &segs_buf)) || !segs_buf) {
          segs_buf = NULL;
          return;
        }
      } else {
        return;
      }
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.FirstElement = 0;
    sd.Buffer.NumElements = (UINT)(cap / 16);
    if (FAILED(dev->CreateShaderResourceView(segs_buf, &sd, &segs_srv)) || !segs_srv) {
      segs_srv = NULL;
      segs_buf->Release();
      segs_buf = NULL;
      return;
    }
    segs_cap = cap;
  }
  if (!segs_buf || !segs_srv) return;
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(segs_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
  segs_upload_calls++;
  memcpy(m.pData, f + (size_t)d * MB_INST, need);
  ctx->Unmap(segs_buf, 0);
}

// the main pipeline's state, at the start and after the passes
static void main_state(ID3D11RenderTargetView *target, int pw, int ph) {
  D3D11_VIEWPORT vp = {0, 0, (float)pw, (float)ph, 0, 1};
  UINT stride = MB_INST * sizeof(float), off = 0;
  ctx->OMSetRenderTargets(1, &target, NULL);
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetState(cut_on ? raster_cut : raster);
  if (cut_on) ctx->RSSetScissorRects(1, &cut);
  ctx->IASetInputLayout(layout);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetVertexBuffers(0, 1, &vbuf, &stride, &off);
  ctx->UpdateSubresource(gbuf, 0, NULL, &gu, 0, 0);
  ID3D11Buffer *cbs[2] = {ubuf, gbuf};
  ctx->VSSetShader(vs, NULL, 0);
  ctx->VSSetConstantBuffers(0, 2, cbs);
  ctx->PSSetShader(ps, NULL, 0);
  ctx->PSSetConstantBuffers(0, 2, cbs);
  ctx->PSSetShaderResources(0, 1, &atlas_srv);
  ctx->PSSetShaderResources(2, 1, gsrv ? &gsrv : &atlas_srv);
  ctx->PSSetShaderResources(3, 1, &segs_srv);
  ID3D11ShaderResourceView *ad_srv = (rw && rw->ad_tex[rw->ad_cur].srv) ? rw->ad_tex[rw->ad_cur].srv : atlas_srv;
  ctx->PSSetShaderResources(5, 1, &ad_srv);
  ctx->PSSetSamplers(0, 1, &sampler);
  ctx->OMSetBlendState(blend, NULL, 0xffffffff);
}

// Instances start to end drawn by `run`, each that pays for it in strips (bands.h), the rest as they come.
static void draw_banded(const float *f, int32_t start, int32_t end, int32_t count, int pw, int ph,
                        ID3D11PixelShader *run) {
  MBStrip to = cut_on ? mb_strip((int)cut.left, (int)cut.top, (int)cut.right, (int)cut.bottom) : mb_strip(0, 0, pw, ph);
  MBStrip st[MB_STRIPS];
  int32_t s = start;
  for (int32_t i = start; i < end; i++) {
    if (!mb_band_kind(f, i)) continue;
    int n = mb_bands(f, i, count, (float)vscale, to, st);
    if (n < 0) continue;
    if (i > s) ctx->DrawInstanced(6, (UINT)(i - s), 0, (UINT)s);
    ctx->RSSetState(raster_cut);
    for (int k = 0; k < n; k++) {
      if (k == 0 || st[k].flat != st[k - 1].flat) ctx->PSSetShader(st[k].flat ? ps_flat : run, NULL, 0);
      D3D11_RECT r = {st[k].x0, st[k].y0, st[k].x1, st[k].y1};
      ctx->RSSetScissorRects(1, &r);
      ctx->DrawInstanced(6, 1, 0, (UINT)i);
    }
    if (n > 0 && st[n - 1].flat) ctx->PSSetShader(run, NULL, 0);
    if (cut_on) ctx->RSSetScissorRects(1, &cut);
    else ctx->RSSetState(raster);
    s = i + 1;
  }
  if (end > s) ctx->DrawInstanced(6, (UINT)(end - s), 0, (UINT)s);
}

// Whether non-glass instance `b` can reach any pixel of the top-down pixel rect [rx0, ry0, rx1, ry1) of the target:
// its quad as the vertex shader grows it, cut to its clip (and outer clip) with a pixel of slack for rounded clips'
// edges and two for rasterization, or turned (rotation fields 32-35): always. Conservative: a false 1 only draws in vain.
#define MB_CULL_SPLIT_GAP 16
static int inst_in_px_rect(const float *b, float sx, float sy, int rx0, int ry0, int rx1, int ry1) {
  int kind = (int)(b[5] + 0.5f);
  if (kind == 12) return 0;
  if (b[32] != 0.0f) return 1;
  float grow = kind == 2 ? fabsf(b[7]) * 3.0f + 2.0f
             : (kind == 6 && b[23] <= 0.0f) ? 2.0f
             : ((kind == 0 || kind == 4 || kind == 5 || kind == 8) ? 1.0f : 0.0f);
  float x0 = fminf(b[0], b[0] + b[2]) - grow;
  float x1 = fmaxf(b[0], b[0] + b[2]) + grow;
  float y0 = fminf(b[1], b[1] + b[3]) - grow;
  float y1 = fmaxf(b[1], b[1] + b[3]) + grow;
  float cpad = (b[24] > 0.0f || b[25] > 0.0f || b[26] > 0.0f || b[27] > 0.0f) ? 1.0f : 0.0f;
  float cx0 = b[16] - cpad, cy0 = b[17] - cpad, cx1 = b[18] + cpad, cy1 = b[19] + cpad;
  if (cx0 > x0) x0 = cx0;
  if (cy0 > y0) y0 = cy0;
  if (cx1 < x1) x1 = cx1;
  if (cy1 < y1) y1 = cy1;
  if (b[44] > 0.0f || b[45] > 0.0f || b[46] > 0.0f || b[47] > 0.0f) {
    float ox0 = b[40] - 1.0f, oy0 = b[41] - 1.0f, ox1 = b[42] + 1.0f, oy1 = b[43] + 1.0f;
    if (ox0 > x0) x0 = ox0;
    if (oy0 > y0) y0 = oy0;
    if (ox1 < x1) x1 = ox1;
    if (oy1 < y1) y1 = oy1;
  }
  if (x1 <= x0 || y1 <= y0) return 0;
  float px0 = x0 * sx - 2.0f, py0 = y0 * sy - 2.0f;
  float px1 = x1 * sx + 2.0f, py1 = y1 * sy + 2.0f;
  return px1 > (float)rx0 && px0 < (float)rx1 && py1 > (float)ry0 && py0 < (float)ry1;
}

// `parts` (n_parts > 0): only those parts of the target drawn again, over what it holds (the frame's ground first:
// the instance after the frame's in the vertex buffer, submit's), one after another with one segs upload for all of
// them, and the runs of instances that cannot reach the part being drawn (the scissor, `cut`) left out on the CPU
// (inst_in_px_rect, a lens by its head); NULL (or n_parts < 0): all of it, cleared to the ground.
static void encode_rects(ID3D11RenderTargetView *target, int pw, int ph, int32_t count, const float *bg,
                         const float *f, const D3D11_RECT *parts = NULL, int n_parts = -1, int transparent = -1) {
  int is_partial = parts != NULL && n_parts > 0;
  if (parts != NULL && n_parts == 0) return;
  int clear_win = transparent < 0 ? rw->clear : transparent;
  float clear[4] = {clear_win ? 0.0f : bg[0], clear_win ? 0.0f : bg[1], clear_win ? 0.0f : bg[2],
                    clear_win ? 0.0f : 1.0f};
  encode_missing = 0;
  encode_inst_submitted = 0;
  encode_lens_drawn = 0;
  ctx->OMSetRenderTargets(1, &target, NULL);
  if (!is_partial) {
    cut_on = 0;
    ctx->ClearRenderTargetView(target, clear);
    if (count <= 0) return;
  }
  struct { float vp[2]; float scale; float inv_atlas; float origin[2]; float pad2[2]; } u = {
    {(float)vw, (float)vh}, (float)vscale, 1.0f / (float)mb_atlas_size, {0, 0}, {0, 0}};
  ctx->UpdateSubresource(ubuf, 0, NULL, &u, 0, 0);
  segs_upload(f, count);
  // (logical points to the target's pixels, as the vertex shader maps them: u.vp over pw × ph)
  float sx = vw > 0.0 ? (float)pw / (float)vw : (float)vscale;
  float sy = vh > 0.0 ? (float)ph / (float)vh : (float)vscale;
  struct BlurLayer {
    int px0, py0, w, h, active;
    float sigma;
    ID3D11RenderTargetView *prev_rtv;
    int prev_cut_on;
    D3D11_RECT prev_cut;
  };
  int pass_n = is_partial ? n_parts : 1;
  int missing_before = 0;  // (each part sees only its own missing pictures, as when drawn one encode at a time)
  for (int ri = 0; ri < pass_n; ri++) {
    missing_before |= encode_missing;
    encode_missing = 0;
    cut_on = is_partial;
    if (is_partial) cut = parts[ri];
    memset(&gu, 0, sizeof gu);
    ad_fill_gu(rw);
    gsrv = NULL;
    main_state(target, pw, ph);
    if (is_partial) {
      ctx->OMSetBlendState(NULL, NULL, 0xffffffff);
      ctx->DrawInstanced(6, 1, 0, (UINT)count);
      ctx->OMSetBlendState(blend, NULL, 0xffffffff);
    }
    // one draw per run of instances showing the same image (slot 20; 0: none, the atlas stands in) of one kind (its
    // pixel shader: kind_ps), or sharing one capture of glass (negative)
    BlurLayer bstack[MB_BLUR_LAYERS];
    int bdepth = 0, bover = 0;
    ID3D11RenderTargetView *cur_rtv = target;
    int32_t start = 0;
    int bound = 0, shows = 0;
    while (start < count) {
      int tid = (int)f[start * MB_INST + 20];
      int kind0 = (int)(f[start * MB_INST + 5] + 0.5f);
      if (kind0 == 14) {
        if (bdepth >= MB_BLUR_LAYERS) {
          bover++;
          start++;
          continue;
        }
        const float *a = f + (size_t)start * MB_INST;
        float sc = (float)vscale;
        float sigma = fabsf(a[7]);
        float margin = sigma > 0.0f ? ceilf(3.0f * sigma) + 2.0f : 0.0f;
        int px0 = (int)floorf((a[0] - margin) * sc), py0 = (int)floorf((a[1] - margin) * sc);
        int px1 = (int)ceilf((a[0] + a[2] + margin) * sc), py1 = (int)ceilf((a[1] + a[3] + margin) * sc);
        if (px0 < 0) px0 = 0;
        if (py0 < 0) py0 = 0;
        if (px1 > pw) px1 = pw;
        if (py1 > ph) py1 = ph;
        int bw = px1 - px0, bh = py1 - py0;
        BlurLayer L = {px0, py0, bw, bh, 0, sigma, cur_rtv, cut_on, cut};
        if (sigma > 0.0f && bw > 0 && bh > 0 && ensure_blur_layer(bdepth, pw, ph)) {
          static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          L.active = 1;
          ctx->ClearRenderTargetView(bl_lv[bdepth].rt, zero);
          cur_rtv = bl_lv[bdepth].rt;
          cut_on = 1;
          cut = {px0, py0, px1, py1};
          main_state(cur_rtv, pw, ph);
        } else {
          cut_on = 1;
          cut = {0, 0, 0, 0};
          ctx->RSSetState(raster_cut);
          ctx->RSSetScissorRects(1, &cut);
        }
        bstack[bdepth++] = L;
        start++;
        continue;
      }
      if (kind0 == 15) {
        if (bover > 0) {
          bover--;
          start++;
          continue;
        }
        if (bdepth <= 0) {
          start++;
          continue;
        }
        BlurLayer L = bstack[--bdepth];
        if (!L.active) {
          cur_rtv = L.prev_rtv;
          cut_on = L.prev_cut_on;
          cut = L.prev_cut;
          main_state(cur_rtv, pw, ph);
          start++;
          continue;
        }
        if (ensure_blur_py(L.w, L.h) && ensure_bl_comp_vbuf()) {
          static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          ID3D11ShaderResourceView *none[3] = {NULL, NULL, NULL};
          ctx->PSSetShaderResources(0, 3, none);
          ctx->ClearRenderTargetView(bl_py[0].rt, zero);
          D3D11_BOX box = {(UINT)L.px0, (UINT)L.py0, 0, (UINT)(L.px0 + L.w), (UINT)(L.py0 + L.h), 1};
          ctx->CopySubresourceRegion(bl_py[0].t, 0, 0, 0, 0, bl_lv[bdepth].t, 0, &box);
          ctx->VSSetShader(pvs, NULL, 0);
          ctx->PSSetShader(pps, NULL, 0);
          ctx->IASetInputLayout(NULL);
          ctx->OMSetBlendState(NULL, NULL, 0xffffffff);
          ctx->RSSetState(raster);
          float sc = (float)vscale;
          float sp = L.sigma * sc;
          int k = 0;
          while (k < 3 && sp / (float)(1 << (k + 1)) >= 2.0f) k++;
          int lw[4] = {L.w, 0, 0, 0}, lh[4] = {L.h, 0, 0, 0};
          for (int j = 1; j <= k; j++) {
            lw[j] = (lw[j - 1] + 1) / 2;
            lh[j] = (lh[j - 1] + 1) / 2;
            ctx->ClearRenderTargetView(bl_py[j].rt, zero);
            post_pass(&bl_py[j - 1], lw[j - 1], lh[j - 1], &bl_py[j], lw[j], lh[j], 1, 0, 0, 0);
          }
          float sk = sp / (float)(1 << k);
          float pre = k > 0 ? 0.75f * (1.0f - powf(0.25f, (float)k)) / 3.0f + 1.0f / 6.0f : 0.0f;
          float sg = sqrtf(sk * sk - pre > 0.0f ? sk * sk - pre : 0.0f);
          if (sg > 0.3f) {
            ctx->ClearRenderTargetView(bl_py[4].rt, zero);
            post_pass(&bl_py[k], lw[k], lh[k], &bl_py[4], lw[k], lh[k], 2, sg, 1, 0);
            ctx->ClearRenderTargetView(bl_py[k].rt, zero);
            post_pass(&bl_py[4], lw[k], lh[k], &bl_py[k], lw[k], lh[k], 2, sg, 0, 1);
          }
          ctx->PSSetShaderResources(0, 3, none);
          cur_rtv = L.prev_rtv;
          cut_on = L.prev_cut_on;
          cut = L.prev_cut;
          main_state(cur_rtv, pw, ph);
          float comp[MB_INST];
          memcpy(comp, f + (size_t)start * MB_INST, MB_INST * sizeof(float));
          float d = (float)(1 << k);
          comp[0] = (float)L.px0 / sc;
          comp[1] = (float)L.py0 / sc;
          comp[2] = (float)L.w / sc;
          comp[3] = (float)L.h / sc;
          comp[4] = 0.0f;
          comp[5] = 4.0f;
          comp[6] = 0.0f;
          comp[7] = 0.0f;
          comp[8] = 1.0f;
          comp[9] = 1.0f;
          comp[10] = 1.0f;
          comp[12] = 0.0f;
          comp[13] = 0.0f;
          comp[14] = (float)L.w / (d * (float)bl_py[k].w);
          comp[15] = (float)L.h / (d * (float)bl_py[k].h);
          comp[20] = 0.0f; comp[21] = 0.0f; comp[22] = 0.0f; comp[23] = 0.0f;
          comp[32] = 0.0f; comp[33] = 0.0f; comp[34] = 0.0f; comp[35] = 0.0f;
          comp[36] = 0.0f; comp[37] = 0.0f; comp[38] = 0.0f; comp[39] = 0.0f;
          D3D11_MAPPED_SUBRESOURCE cm;
          if (SUCCEEDED(ctx->Map(bl_comp_vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
            memcpy(cm.pData, comp, MB_INST * sizeof(float));
            ctx->Unmap(bl_comp_vbuf, 0);
            UINT stride = MB_INST * sizeof(float), off = 0;
            ctx->IASetVertexBuffers(0, 1, &bl_comp_vbuf, &stride, &off);
            ctx->PSSetShaderResources(1, 1, &bl_py[k].srv);
            ctx->PSSetShader(kind_ps(4), NULL, 0);
            ctx->DrawInstanced(6, 1, 0, 0);
            ctx->IASetVertexBuffers(0, 1, &vbuf, &stride, &off);
            ctx->PSSetShaderResources(1, 1, &atlas_srv);
          }
        } else {
          cur_rtv = L.prev_rtv;
          cut_on = L.prev_cut_on;
          cut = L.prev_cut;
          main_state(cur_rtv, pw, ph);
        }
        start++;
        continue;
      }
      int32_t end = start + 1;
      while (end < count && (int)f[end * MB_INST + 20] == tid) end++;
      if (tid < 0) {
        glass_drawn = 1;
        if (tid != bound) {
          shows = capture(cur_rtv, pw, ph, f, start, end, count);
          bound = tid;
          main_state(cur_rtv, pw, ph);
        }
        // merged liquid (merge > 0): the group as one shape, drawn once; unmerged liquid and frosted: each instance
        int gkind_raw = (int)(f[start * MB_INST + 5] + 0.5f);
        int merged_liquid = gkind_raw == 6 && f[start * MB_INST + 23] > 0.0f;
        int gkind = gkind_raw == 5 ? 5 : (gkind_raw == 6 ? (merged_liquid ? 6 : MB_KIND_LIQUID_SINGLE) : -1);
        ctx->PSSetShader(kind_ps(gkind), NULL, 0);
        if (shows) {
          ctx->DrawInstanced(6, merged_liquid ? 1 : (UINT)(end - start), 0, (UINT)start);
          encode_inst_submitted += merged_liquid ? 1 : (end - start);
        }
        start = end;
        continue;
      }
      // holes for native views (kind 9) apart, written without blending: transparent there
      int kind = kind0, hole = kind == 9, own = has_own_ps(kind);
      if (kind == 8) {
        int32_t child_n = (int32_t)(f[start * MB_INST + 12] + 0.5f);
        if (child_n < 0) child_n = 0;
        if (start + 1 + child_n > count) child_n = count - start - 1;
        const float *hb = f + (size_t)start * MB_INST;
        if (child_n <= 0 || hb[11] <= 0.001f || hb[2] <= 0.0f || hb[3] <= 0.0f) {
          start += 1 + child_n;
          continue;
        }
        glass_drawn = 1;
        float sc_f = (float)vscale;
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
        int lpw = (int)ceilf(((x1 + margin) - ox) * sc_f);
        int lph = (int)ceilf(((y1 + margin) - oy) * sc_f);
        int max_w = pw > 2048 ? pw : 2048;
        int max_h = ph > 2048 ? ph : 2048;
        if (lpw < 1) lpw = 1;
        if (lph < 1) lph = 1;
        if (lpw > max_w) lpw = max_w;
        if (lph > max_h) lph = max_h;
        // (a lens whose head, the one instance it draws onto the target, misses the part being drawn: neither its
        // children drawn nor it, unless adaptive ink after it, before the next glass or drawn lens, can reach the part
        // and would read the backdrop and state it leaves bound; past a blur layer's bounds, whose cut differs, any)
        int can_cull_lens = cut_on && !inst_in_px_rect(hb, sx, sy, (int)cut.left, (int)cut.top, (int)cut.right,
                                                       (int)cut.bottom);
        int cut_changed = 0;
        for (int32_t j = start + 1 + child_n; can_cull_lens && j < count; j++) {
          const float *jb = f + (size_t)j * MB_INST;
          int jk = (int)(jb[5] + 0.5f);
          if ((int)jb[20] < 0) break;
          if (jk == 8) {
            int32_t cn = (int32_t)(jb[12] + 0.5f);
            if (cn < 0) cn = 0;
            if (j + 1 + cn > count) cn = count - j - 1;
            if (cn > 0 && jb[11] > 0.001f && jb[2] > 0.0f && jb[3] > 0.0f) break;
            j += cn;
            continue;
          }
          if (jk == 14 || jk == 15) {
            cut_changed = 1;
            continue;
          }
          if ((jk == 1 || jk == 3 || jk == 7 || jk == 10 || jk == 11) && jb[23] > 0.0f &&
              (cut_changed || inst_in_px_rect(jb, sx, sy, (int)cut.left, (int)cut.top, (int)cut.right,
                                              (int)cut.bottom)))
            can_cull_lens = 0;
        }
        if (can_cull_lens) {
          bound = 0;
          start += 1 + child_n;
          continue;
        }
        if (!ensure_lens_levels(lpw, lph)) {
          start += 1 + child_n;
          continue;
        }
        // Pass 1: render children into lens_lv[0]
        ID3D11ShaderResourceView *none[3] = {NULL, NULL, NULL};
        ctx->PSSetShaderResources(0, 3, none);
        float zclear[4] = {0, 0, 0, 0};
        ctx->OMSetRenderTargets(1, &lens_lv[0].rt, NULL);
        ctx->ClearRenderTargetView(lens_lv[0].rt, zclear);
        D3D11_VIEWPORT lvp = {0, 0, (float)lpw, (float)lph, 0, 1};
        ctx->RSSetViewports(1, &lvp);
        ctx->RSSetState(raster);
        struct { float vp[2]; float scale; float inv_atlas; float origin[2]; float pad2[2]; } lu = {
          {(float)lpw / sc_f, (float)lph / sc_f}, sc_f, 1.0f / (float)mb_atlas_size, {ox, oy}, {0, 0}};
        ctx->UpdateSubresource(ubuf, 0, NULL, &lu, 0, 0);
        ctx->PSSetShaderResources(0, 1, &atlas_srv);
        ctx->PSSetShaderResources(2, 1, &atlas_srv);
        ctx->PSSetShaderResources(3, 1, &segs_srv);
        int32_t cs = start + 1, cend_all = start + 1 + child_n;
        while (cs < cend_all) {
          int ctid = (int)f[cs * MB_INST + 20];
          int ck = (int)(f[cs * MB_INST + 5] + 0.5f);
          if (ck == 5 || ck == 6 || ck == 8 || ck == 9 || ck == 12 || ck == 14 || ck == 15 || ctid < 0) {
            cs++;
            continue;
          }
          int cown = has_own_ps(ck);
          int32_t ce = cs + 1;
          while (ce < cend_all && (int)f[ce * MB_INST + 20] == ctid) {
            int kk = (int)(f[ce * MB_INST + 5] + 0.5f);
            if (kk == 5 || kk == 6 || kk == 8 || kk == 9 || kk == 12 || kk == 14 || kk == 15) break;
            if (cown ? kk != ck : has_own_ps(kk)) break;
            ce++;
          }
          ID3D11ShaderResourceView *ct = ctid > MB_STREAM_TEX ? (ID3D11ShaderResourceView *)mb_stream_srv(ctid - MB_STREAM_TEX)
                                        : ctid ? (ID3D11ShaderResourceView *)mb_image_srv(ctid) : atlas_srv;
          if (ctid > MB_STREAM_TEX && !ct) { cs = ce; continue; }
          if (ctid > 0 && ctid <= MB_STREAM_TEX && !ct) encode_missing = 1;
          ctx->PSSetShaderResources(1, 1, ct ? &ct : &atlas_srv);
          ctx->PSSetShader(cown ? kind_ps(ck) : ps, NULL, 0);
          ctx->DrawInstanced(6, (UINT)(ce - cs), 0, (UINT)cs);
          cs = ce;
        }
        // Pass 2: separable Gaussian blur in BGRA8
        float sp = blur_pt * sc_f;
        if (sp > 0.25f) {
          ctx->OMSetBlendState(NULL, NULL, 0xffffffff);
          ctx->IASetInputLayout(NULL);
          ctx->VSSetShader(pvs, NULL, 0);
          ctx->PSSetShader(pps, NULL, 0);
          post_pass(&lens_lv[0], lpw, lph, &lens_lv[1], lpw, lph, 2, sp, 1.0f, 0.0f);
          post_pass(&lens_lv[1], lpw, lph, &lens_lv[0], lpw, lph, 2, sp, 0.0f, 1.0f);
        }
        // Pass 3: composite refracted & SDF-clipped foreground lens onto cur_rtv
        ctx->UpdateSubresource(ubuf, 0, NULL, &u, 0, 0);
        memset(&gu, 0, sizeof gu);
        float W = (float)lens_lv[0].w, H = (float)lens_lv[0].h;
        gu.map[0] = sc_f / W; gu.map[1] = sc_f / H;
        gu.map[2] = -(ox * sc_f) / W; gu.map[3] = -(oy * sc_f) / H;
        gu.uvc[0] = 0.5f / W; gu.uvc[1] = 0.5f / H;
        gu.uvc[2] = ((float)lpw - 0.5f) / W; gu.uvc[3] = ((float)lph - 0.5f) / H;
        gsrv = lens_lv[0].srv;
        main_state(cur_rtv, pw, ph);
        ctx->PSSetShader(kind_ps(8), NULL, 0);
        ctx->DrawInstanced(6, 1, 0, (UINT)start);
        encode_inst_submitted++;
        encode_lens_drawn++;
        bound = 0;
        start += 1 + child_n;
        continue;
      }
      int fast_rect = !hole && tid == 0 && kind == 0 && rect_ps && fast_rect_enabled() && is_simple_rect(f, start);
      // (kinds without their own pixel shader one run, as before)
      end = start + 1;
      while (end < count && (int)f[end * MB_INST + 20] == tid) {
        int k = (int)(f[end * MB_INST + 5] + 0.5f);
        if (k == 8 || k == 14 || k == 15) break;
        if ((k == 9) != hole || (own ? k != kind : has_own_ps(k))) break;
        if (!hole && tid == 0 && kind == 0 && rect_ps && fast_rect_enabled() && is_simple_rect(f, end) != fast_rect) break;
        end++;
      }
      // (a hole's image is its view's, src/hostapi/host.mbt view_tex_base + its id: the view is under the canvas here)
      ID3D11ShaderResourceView *t = hole ? atlas_srv
                                    : tid > MB_LAYER_TEX ? mb_layer_srv(tid - MB_LAYER_TEX)
                                    : tid > MB_STREAM_TEX ? (ID3D11ShaderResourceView *)mb_stream_srv(tid - MB_STREAM_TEX)
                                    : tid ? (ID3D11ShaderResourceView *)mb_image_srv(tid) : atlas_srv;
      // (a stream or offscreen layer with no texture yet: nothing, not the atlas)
      if (!hole && tid > MB_STREAM_TEX && !t) {
        start = end;
        continue;
      }
      if (!hole && tid > 0 && tid <= MB_STREAM_TEX && !t) encode_missing = 1;
      ID3D11PixelShader *run = fast_rect ? rect_ps : own ? kind_ps(kind) : ps;
      if (!cut_on) {
        ctx->PSSetShaderResources(1, 1, t ? &t : &atlas_srv);
        ctx->PSSetShader(run, NULL, 0);
        if (hole) ctx->OMSetBlendState(NULL, NULL, 0xffffffff);
        if (hole) ctx->DrawInstanced(6, (UINT)(end - start), 0, (UINT)start);
        else draw_banded(f, start, end, count, pw, ph, run);
        encode_inst_submitted += end - start;
        if (hole) ctx->OMSetBlendState(blend, NULL, 0xffffffff);
      } else {
        // only the stretches of the run that can reach the cut, a gap of fewer than MB_CULL_SPLIT_GAP left out
        // instances kept in (one draw, not many small ones)
        int rx0 = (int)cut.left, ry0 = (int)cut.top, rx1 = (int)cut.right, ry1 = (int)cut.bottom;
        int bound_run = 0;
        int32_t cur = start;
        while (cur < end) {
          while (cur < end && !inst_in_px_rect(f + (size_t)cur * MB_INST, sx, sy, rx0, ry0, rx1, ry1)) cur++;
          if (cur >= end) break;
          int32_t lo = cur, hi = cur + 1;
          int32_t j = hi;
          while (j < end) {
            if (inst_in_px_rect(f + (size_t)j * MB_INST, sx, sy, rx0, ry0, rx1, ry1)) {
              hi = j + 1;
            } else if (j - hi + 1 >= MB_CULL_SPLIT_GAP) {
              break;
            }
            j++;
          }
          if (!bound_run) {
            ctx->PSSetShaderResources(1, 1, t ? &t : &atlas_srv);
            ctx->PSSetShader(run, NULL, 0);
            if (hole) ctx->OMSetBlendState(NULL, NULL, 0xffffffff);
            bound_run = 1;
          }
          if (hole) ctx->DrawInstanced(6, (UINT)(hi - lo), 0, (UINT)lo);
          else draw_banded(f, lo, hi, count, pw, ph, run);
          encode_inst_submitted += hi - lo;
          cur = j + 1;
        }
        if (bound_run && hole) ctx->OMSetBlendState(blend, NULL, 0xffffffff);
      }
      start = end;
    }
  }
  encode_missing |= missing_before;
  if (cut_on) ctx->RSSetState(raster);
  cut_on = 0;
}

static void encode(ID3D11RenderTargetView *target, int pw, int ph, int32_t count, const float *bg,
                   const float *f, const D3D11_RECT *part = NULL, int transparent = -1) {
  encode_rects(target, pw, ph, count, bg, f, part, part ? 1 : -1, transparent);
}

// Process-wide monotonic dropped-frame total across all windows (swapchain sampling state stays per RenderWin).
static int32_t present_dropped_total = 0;

#define MB_WIN_DROPPED_CLAMP 10
// Updates gap boundary tracking after a Present call (visible_ok: SUCCEEDED(hr) && hr != DXGI_STATUS_OCCLUDED).
static void win_stats_on_present(WinDropStats *st, int visible_ok, int pc_ok, UINT pc) {
  if (!visible_ok) {
    st->has_frame_stats = 0;
    st->gap_owed = 1;
    st->gap_unknown_open = 1;
    st->has_unknown_until = 0;
    st->gap_count = 0;
    return;
  }
  if (!st->gap_owed) return;
  if (!pc_ok) {
    st->gap_unknown_open = 1;
    return;
  }
  st->gap_owed = 0;
  if (st->gap_unknown_open) {
    st->gap_unknown_open = 0;
    if (!st->has_unknown_until || (int32_t)(pc - st->unknown_until) > 0) {
      st->has_unknown_until = 1;
      st->unknown_until = pc;
    }
    int keep = 0;
    for (int i = 0; i < st->gap_count; i++) {
      if ((int32_t)(st->gap_starts[i] - st->unknown_until) > 0) st->gap_starts[keep++] = st->gap_starts[i];
    }
    st->gap_count = keep;
    return;
  }
  if (st->has_unknown_until && (int32_t)(pc - st->unknown_until) <= 0) return;
  for (int i = 0; i < st->gap_count; i++) {
    if (st->gap_starts[i] == pc) return;
  }
  if (st->gap_count < MB_WIN_GAP_MAX) {
    st->gap_starts[st->gap_count++] = pc;
  } else {
    if (!st->has_unknown_until || (int32_t)(pc - st->unknown_until) > 0) {
      st->has_unknown_until = 1;
      st->unknown_until = pc;
    }
    st->gap_count = 0;
  }
}

// Pure state-machine step for counting dropped frames from DXGI_FRAME_STATISTICS samples.
// Uses PresentRefreshCount (actual display refresh, immune to SyncRefreshCount drift) with 32-bit wrap-safe
// deltas, a short-interval guard (dt_ms < 2.5 * period_ms) to ignore pauses, and exact gap_starts / unknown_until
// boundary checks so lagging samples from the previous continuous run still count while cross-gap transitions are
// suppressed and never permanently lock out subsequent drops.
static int32_t win_stats_step(WinDropStats *st, int fs_ok, int fs_disjoint,
                              UINT fs_present, UINT fs_refresh, int64_t fs_qpc,
                              int64_t qpc_freq, double period_ms, int beats) {
  if (fs_disjoint) {
    st->has_frame_stats = 0;
    st->gap_owed = 1;
    st->gap_unknown_open = 1;
    st->has_unknown_until = 0;
    st->gap_count = 0;
    return 0;
  }
  // Allow 0 on 32-bit wrap-around once history exists (or when DISJOINT/unknown gap sealed right at pc == 0).
  int cold_zero_ok = st->has_unknown_until && st->unknown_until == 0 && fs_present == 0 && fs_refresh > 0;
  if (!fs_ok || fs_qpc <= 0 || (!st->has_frame_stats && !cold_zero_ok && (fs_present == 0 || fs_refresh == 0))) {
    st->has_frame_stats = 0;
    return 0;
  }
  if (st->has_frame_stats && fs_present == st->last_present_count) return 0;
  int32_t dropped = 0;
  if (st->has_frame_stats) {
    int crossed_gap = st->gap_unknown_open ||
                      (st->has_unknown_until && (int32_t)(st->last_present_count - st->unknown_until) < 0);
    for (int i = 0; !crossed_gap && i < st->gap_count; i++) {
      UINT g = st->gap_starts[i];
      if ((int32_t)(st->last_present_count - g) < 0 && (int32_t)(fs_present - g) >= 0) {
        crossed_gap = 1;
      }
    }
    if (!crossed_gap && qpc_freq > 0 && period_ms > 0.0 && beats > 0 &&
        st->last_present_qpc > 0 && fs_qpc > st->last_present_qpc) {
      double dt_ms = (double)(fs_qpc - st->last_present_qpc) * 1000.0 / (double)qpc_freq;
      if (dt_ms < 2.5 * period_ms) {
        int32_t d_present = (int32_t)(fs_present - st->last_present_count);
        int32_t d_refresh = (int32_t)(fs_refresh - st->last_present_refresh);
        if (d_present > 0 && d_refresh > 0) {
          uint64_t expected_refresh = (uint64_t)(uint32_t)d_present * (uint64_t)beats;
          if ((uint64_t)(uint32_t)d_refresh > expected_refresh) {
            uint64_t d = ((uint64_t)(uint32_t)d_refresh - expected_refresh) / (uint64_t)beats;
            if (d > MB_WIN_DROPPED_CLAMP) d = MB_WIN_DROPPED_CLAMP;
            dropped = (int32_t)d;
          }
        }
      }
    }
  }
  int keep = 0;
  for (int i = 0; i < st->gap_count; i++) {
    if ((int32_t)(fs_present - st->gap_starts[i]) < 0) st->gap_starts[keep++] = st->gap_starts[i];
  }
  st->gap_count = keep;
  if (st->has_unknown_until && (int32_t)(fs_present - st->unknown_until) >= 0) st->has_unknown_until = 0;
  st->last_present_count = fs_present;
  st->last_present_refresh = fs_refresh;
  st->last_present_qpc = fs_qpc;
  st->has_frame_stats = 1;
  return dropped;
}

static double win_display_period_ms(int64_t qpc_freq) {
  DWM_TIMING_INFO ti = {};
  ti.cbSize = sizeof ti;
  if (qpc_freq > 0 && SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &ti)) && ti.qpcRefreshPeriod) {
    return (double)ti.qpcRefreshPeriod * 1000.0 / (double)qpc_freq * (double)mb_frame_beats();
  }
  return 1000.0 / 60.0;
}

#ifndef MB_WIN_CREATE_BUFFER
#define MB_WIN_CREATE_BUFFER(d, desc, init, out) ((d)->CreateBuffer((desc), (init), (out)))
#endif

// Ensures `vbuf` can hold at least `room` bytes: doubles when possible, falls back to exact `room`, and always clears
// `vbuf`/`vbuf_cap` on failure so callers never dereference a stale or NULL buffer.
static int ensure_vbuf(size_t room) {
  if (!dev || room == 0 || room > (size_t)UINT_MAX) return 0;
  if (vbuf && vbuf_cap >= room) return 1;
  if (vbuf) {
    vbuf->Release();
    vbuf = NULL;
  }
  vbuf_cap = 0;
  size_t cap = room <= ((size_t)UINT_MAX / 2) ? room * 2 : room;
  D3D11_BUFFER_DESC bd = {(UINT)cap, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER,
                          D3D11_CPU_ACCESS_WRITE, 0, 0};
  if (FAILED(MB_WIN_CREATE_BUFFER(dev, &bd, NULL, &vbuf)) || !vbuf) {
    vbuf = NULL;
    if (cap != room) {
      cap = room;
      bd.ByteWidth = (UINT)cap;
      if (FAILED(MB_WIN_CREATE_BUFFER(dev, &bd, NULL, &vbuf)) || !vbuf) {
        vbuf = NULL;
        return 0;
      }
    } else {
      return 0;
    }
  }
  vbuf_cap = cap;
  return 1;
}

#ifndef MB_WIN_MAP_VBUF
#define MB_WIN_MAP_VBUF(c, b, m) ((c)->Map((b), 0, D3D11_MAP_WRITE_DISCARD, 0, (m)))
#endif

static int fill_vbuf(const float *f, int32_t count, const float *bg = NULL);

// The frame's instances into the vertex buffer and kept (a snapshot draws them again), as floats with its groups'
// headers drawn out (groups.h: one pass, into what is kept), then drawn into the target: a window's (`chain`) into the
// canvas, only the parts that differ where it holds the frame before; how many in `parts_out` (-1 all of it, 0 none),
// the parts in `cuts`.
static int submit(const double *inst, int32_t count, double r, double g, double b, int *parts_out = NULL,
                  D3D11_RECT *cuts = NULL, int adaptive_damage = 0, const float *preconverted = NULL) {
  if (!rtv || !dev || !ctx || count < 0) return 0;
  size_t slots = (size_t)(count > 0 ? count : 1);
  const size_t stride_bytes = (size_t)MB_INST * sizeof(float);
  if (slots > ((size_t)UINT_MAX - stride_bytes) / stride_bytes) return 0;
  size_t need = slots * stride_bytes;
  // (and the frame's ground after its instances: a rect over all the window, opaque, a part drawn again first)
  size_t room = need + stride_bytes;
  size_t n = (size_t)count * MB_INST;
  if (last_cap < need || !last_inst) {
    size_t cap = need <= (SIZE_MAX / 2) ? need * 2 : need;
    float *grown = (float *)realloc(last_inst, cap);
    if (!grown && cap != need) {
      cap = need;
      grown = (float *)realloc(last_inst, cap);
    }
    if (!grown) return 0;
    last_inst = grown;
    last_cap = cap;
  }
  if (!ensure_vbuf(room)) return 0;
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(MB_WIN_MAP_VBUF(ctx, vbuf, &m))) {
    vbuf->Release();
    vbuf = NULL;
    vbuf_cap = 0;
    return 0;
  }
  float *f = (float *)m.pData;
  if (preconverted) {
    if (n > 0) memcpy(last_inst, preconverted, n * sizeof(float));
  } else {
    mb_groups_floats(last_inst, inst, count);
  }
  if (n > 0) memcpy(f, last_inst, n * sizeof(float));
  float *gr = f + n;
  memset(gr, 0, MB_INST * sizeof(float));
  gr[0] = -16.0f; gr[1] = -16.0f; gr[2] = (float)vw + 32.0f; gr[3] = (float)vh + 32.0f;
  gr[8] = rw->clear ? 0.0f : (float)r;
  gr[9] = rw->clear ? 0.0f : (float)g;
  gr[10] = rw->clear ? 0.0f : (float)b;
  gr[11] = rw->clear ? 0.0f : 1.0f;
  gr[16] = -1e9f; gr[17] = -1e9f; gr[18] = 1e9f; gr[19] = 1e9f;
  ctx->Unmap(vbuf, 0);
  last_count = count;
  last_bg[0] = (float)r;
  last_bg[1] = (float)g;
  last_bg[2] = (float)b;
  // a window's into its canvas; without one, the offscreen target is kept as a canvas is
  ID3D11RenderTargetView *target = rtv;
  int *kept = &off_ok;
  if (chain) {
    ensure_canvas(fb_w, fb_h);
    if (!canvas_rtv) {
      dirty_told = dirty_whole = dirty_n = 0;
      encode(rtv, fb_w, fb_h, count, last_bg, last_inst);
      pictures_missing = encode_missing;
      if (parts_out) *parts_out = -1;
      return 1;
    }
    target = canvas_rtv;
    kept = &canvas_ok;
  }
  if (adaptive_damage && !dirty_whole &&
      !mb_glass_damage_expand(last_inst, count, dirty, &dirty_n, fb_w / vscale, fb_h / vscale, vscale))
    dirty_whole = 1;
  D3D11_RECT parts[MB_PARTS];
  int np = take_parts(fb_w, fb_h, *kept, parts);
  int missing = 0;
  if (np != 0) {
    encode_rects(target, fb_w, fb_h, count, last_bg, last_inst, np < 0 ? NULL : parts, np);
    missing = encode_missing;
  }
  if (np != 0) pictures_missing = missing;
  *kept = 1;
  if (parts_out) *parts_out = np;
  if (cuts) for (int i = 0; i < np; i++) cuts[i] = parts[i];
  if (chain) {
    // the canvas whole into the swap chain's buffer
    ID3D11Resource *back = NULL;
    rtv->GetResource(&back);
    ctx->CopyResource(back, canvas);
    back->Release();
  }
  return 1;
}

// How many of the compositor's beats a frame takes: one, or as many as keep the window to MB_FRAMES_MOST frames a
// second where the display shows twice that or more (240 Hz: two, 120 frames a second; 360: three; 144 and 165: one,
// each of theirs, not half of them). No more than 120 by default, as the other hosts' displays show at most (the
// user, 2026-11-13: 「给still fail/screenreplay默认最高120就行」): what a frame costs is paid for each, and past 120 it
// is not seen (ScreenReplay's viewer playing: 24% of a core at 240). MBUI_FRAMES_MOST says another most (0: the
// display's every frame). Asked of the compositor again each second, a display's rate being the user's to change.
int mb_frame_beats(void) {
  if (!dcomp) return 1;
  static int beats = 1;
  static ULONGLONG asked;
  ULONGLONG now = GetTickCount64();
  if (asked && now - asked < 1000) return beats;
  asked = now ? now : 1;
  static int most = -1;
  if (most < 0) {
    const wchar_t *said = mb_win_env(L"MBUI_FRAMES_MOST");
    most = said && *said ? _wtoi(said) : 120;
    if (most < 0) most = 120;
  }
  beats = 1;
  DWM_TIMING_INFO ti = {};
  ti.cbSize = sizeof ti;
  if (most > 0 && SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &ti)) && ti.rateRefresh.uiDenominator) {
    // (a display's 239.76 is 240)
    double hz = (double)ti.rateRefresh.uiNumerator / (double)ti.rateRefresh.uiDenominator;
    int k = (int)((hz + 1.0) / (double)most);
    if (k > 1) beats = k;
  }
  return beats;
}

// what the frames presented since the last mb_render_pace owe the compositor's beat: 0 nothing, 1 the wait, 2 a window
// occluded (a hidden or minimized window does not wait for vsync: paced as if it did)
static int pace_owed;

// Records DXGI present statistics for the active RenderWin; returns 1 iff visibly presented (not occluded/failed).
static int win_record_present_stats(HRESULT hr) {
  int visible_ok = SUCCEEDED(hr) && hr != DXGI_STATUS_OCCLUDED;
  rw->last_presented = visible_ok;
  UINT pc = 0;
  int pc_ok = visible_ok && rw->drop_stats.gap_owed ? SUCCEEDED(chain->GetLastPresentCount(&pc)) : 0;
  win_stats_on_present(&rw->drop_stats, visible_ok, pc_ok, pc);
  if (!visible_ok) return 0;
  DXGI_FRAME_STATISTICS fs = {};
  HRESULT fs_hr = chain->GetFrameStatistics(&fs);
  int64_t qpc_freq = 0;
  double period_ms = 0.0;
  int beats = 0;
  if (SUCCEEDED(fs_hr) && rw->drop_stats.has_frame_stats && fs.PresentCount != rw->drop_stats.last_present_count) {
    LARGE_INTEGER freq_qpc;
    QueryPerformanceFrequency(&freq_qpc);
    qpc_freq = freq_qpc.QuadPart;
    beats = mb_frame_beats();
    period_ms = win_display_period_ms(qpc_freq);
  }
  present_dropped_total += win_stats_step(&rw->drop_stats, SUCCEEDED(fs_hr),
                                          fs_hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT,
                                          fs.PresentCount, fs.PresentRefreshCount,
                                          fs.SyncQPCTime.QuadPart, qpc_freq, period_ms, beats);
  return 1;
}

extern "C" void mb_render_mark_gap(int wid) {
  if (wid < 0) {
    for (int k = 0; k < MB_WINS; k++) render_wins[k].drop_stats.gap_owed = 1;
    return;
  }
  if (wid < MB_WINS) render_wins[wid].drop_stats.gap_owed = 1;
}

extern "C" int mb_render_last_presented(int wid) {
  if (wid < 0 || wid >= MB_WINS) return 0;
  return render_wins[wid].last_presented;
}

void mb_render_pace_skipped(void) {
  pace_owed = pace_owed ? pace_owed : 1;
}

int mb_render_present_opts(const double *inst, int32_t count, double r, double g, double b, int must_present) {
  int32_t atl_gen = mb_atlas_gen_now();
  uint64_t atl_overwrite = g_atlas_overwrite_seq;
  uint64_t img_gen = mb_image_generation();
  uint64_t img_free = mb_image_free_generation();
  float light_now = g_light_angle;

  const float *preconverted = NULL;
  // Fast-path no-op present skip (flip+DComp only): check cheap scalars before converting instances.
  // shown_image_gen == img_gen is included in the skip predicate so any newly decoded or freed image
  // falls through cleanly instead of risking a stale skip.
  if (flip_chain && dcomp && chain && rtv && dev && ctx && count >= 0 &&
      !must_present && !rw->ad_settling && !rw->commit_owed && !pictures_missing && !rw->swap_needs_full &&
      rw->shown_ok && !rw->shown_streams && count == last_count &&
      vw == rw->shown_w && vh == rw->shown_h && vscale == rw->shown_scale &&
      fb_w == rw->shown_fb_w && fb_h == rw->shown_fb_h &&
      rw->clear == rw->shown_clear && rw->shown_light == light_now &&
      rw->shown_atlas_gen == atl_gen && rw->shown_atlas_overwrite_seq == atl_overwrite &&
      rw->shown_image_gen == img_gen && rw->shown_image_free_seq == img_free &&
      (float)r == last_bg[0] && (float)g == last_bg[1] && (float)b == last_bg[2]) {
    int same = 0;
    if (count == 0) {
      same = 1;
    } else if ((size_t)count <= SIZE_MAX / ((size_t)MB_INST * sizeof(float))) {
      size_t need = (size_t)count * MB_INST * sizeof(float);
      if (rw->next_cap < need || !rw->next_inst) {
        size_t cap = need <= (SIZE_MAX / 2) ? need * 2 : need;
        float *grown = (float *)realloc(rw->next_inst, cap);
        if (!grown && cap != need) {
          cap = need;
          grown = (float *)realloc(rw->next_inst, cap);
        }
        if (grown) {
          rw->next_inst = grown;
          rw->next_cap = cap;
        }
      }
      if (rw->next_inst && rw->next_cap >= need) {
        mb_groups_floats(rw->next_inst, inst, count);
        preconverted = rw->next_inst;
        if (last_inst && memcmp(rw->next_inst, last_inst, need) == 0) same = 1;
      }
    }
    if (same) {
      // glass_idle_tick() is intentionally not advanced on skipped no-op frames so shared glass caches stay warm.
      dirty_told = dirty_whole = dirty_n = 0;
      rw->last_presented = 0;
      return 0;
    }
  }

  // Whole-canvas invalidation on host/global state changes or atlas UV overwrites; normal glyph appends with
  // dirty_n > 0 do not bump atl_overwrite and keep canvas_ok == 1 for partial redraw.
  if (canvas_ok &&
      (!rw->shown_ok ||
       rw->clear != rw->shown_clear || light_now != rw->shown_light ||
       vw != rw->shown_w || vh != rw->shown_h || vscale != rw->shown_scale ||
       atl_gen != rw->shown_atlas_gen || atl_overwrite != rw->shown_atlas_overwrite_seq ||
       (rw->shown_has_images && img_free != rw->shown_image_free_seq) ||
       (dirty_told && !dirty_whole && dirty_n == 0 && img_gen != rw->shown_image_gen))) {
    canvas_ok = 0;
  }

  int n = -1;
  D3D11_RECT parts[MB_PARTS];
  glass_drawn = 0;
  ad_mode = 1;
  ad_backdrop_changed = 1;
  ad_cur_dt = ad_next_dt(rw);
  rw->ad_frame_serial++;
  int adaptive_damage = dirty_told && !dirty_whole && rw->ad_settling;
  if (adaptive_damage) {
    for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
      if (rw->ad_slots[i].active && rw->ad_slots[i].settle_left > 0.0)
        mb_host_render_dirty(rw->ad_slots[i].cx - rw->ad_slots[i].hx - 4.0,
                             rw->ad_slots[i].cy - rw->ad_slots[i].hy - 4.0,
                             rw->ad_slots[i].hx * 2.0 + 8.0, rw->ad_slots[i].hy * 2.0 + 8.0);
    }
  }
  int drawn = submit(inst, count, r, g, b, &n, parts, adaptive_damage, preconverted);
  if (drawn) {
    if (n != 0) rw->has_glass = (chain != NULL && glass_drawn) ? 1 : 0;
    ad_finalize_frame(rw, n < 0, rw->ad_frame_serial);
  }
  glass_idle_tick();
  if (!drawn || !chain) {
    rw->last_presented = 0;
    commit_presented();
    if (drawn) rw->commit_owed = 0;
    return 0;
  }
  int streams = 0, has_images = 0;
  for (int32_t i = 0; i < last_count; i++) {
    float tid = last_inst[(size_t)i * MB_INST + 20];
    if (tid > (float)MB_STREAM_TEX) {
      streams = 1;
    } else if (tid > 0.0f) {
      has_images = 1;
    }
  }
  // a hidden or minimized window does not wait for vsync; pace it as if it did. A flip model's swap chain is told the
  // parts that changed (what the compositor takes again; none: a pixel's), all of its buffer the canvas's
  HRESULT hr;
  if (flip_chain && n >= 0 && !rw->swap_needs_full) {
    RECT dr[MB_PARTS];
    for (int i = 0; i < n; i++) dr[i] = {parts[i].left, parts[i].top, parts[i].right, parts[i].bottom};
    if (n == 0) dr[n++] = {0, 0, 1, 1};
    DXGI_PRESENT_PARAMETERS pp = {};
    pp.DirtyRectsCount = (UINT)n;
    pp.pDirtyRects = dr;
    hr = chain->Present1(1, 0, &pp);
  } else {
    hr = chain->Present(1, 0);
  }
  commit_presented();
  rw->commit_owed = 0;
  int visible_ok = win_record_present_stats(hr);
  if (visible_ok) {
    rw->shown_ok = 1;
    rw->swap_needs_full = 0;
    rw->shown_streams = streams;
    rw->shown_has_images = has_images;
    rw->shown_w = vw;
    rw->shown_h = vh;
    rw->shown_scale = vscale;
    rw->shown_fb_w = fb_w;
    rw->shown_fb_h = fb_h;
    rw->shown_clear = rw->clear;
    rw->shown_light = light_now;
    rw->shown_atlas_gen = atl_gen;
    rw->shown_atlas_overwrite_seq = atl_overwrite;
    rw->shown_image_gen = img_gen;
    rw->shown_image_free_seq = img_free;
  } else {
    rw->shown_ok = 0;
    rw->swap_needs_full = 1;
  }
  pace_owed = hr != DXGI_STATUS_OCCLUDED ? 1 : pace_owed ? pace_owed : 2;
  return SUCCEEDED(hr);
}

int mb_render_present(const double *inst, int32_t count, double r, double g, double b) {
  return mb_render_present_opts(inst, count, r, g, b, 0);
}

int mb_render_adaptive_settling(int wid) {
  if (wid < 0 || wid >= MB_WINS) return 0;
  return render_wins[wid].ad_settling;
}

int mb_render_adaptive_step(int wid) {
  if (wid < 0 || wid >= MB_WINS) return 0;
  RenderWin *back = rw;
  rw = &render_wins[wid];
  if (!rw->ad_settling || !last_inst || !rtv || !dev || !ctx || !chain) {
    rw->last_presented = 0;
    rw = back;
    return 0;
  }
  if (!vbuf && !fill_vbuf(last_inst, last_count, last_bg)) {
    rw->last_presented = 0;
    rw = back;
    return 0;
  }
  dirty_told = dirty_whole = dirty_n = 0;
  for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
    if (rw->ad_slots[i].active && rw->ad_slots[i].settle_left > 0.0)
      mb_host_render_dirty(rw->ad_slots[i].cx - rw->ad_slots[i].hx - 4.0,
                           rw->ad_slots[i].cy - rw->ad_slots[i].hy - 4.0,
                           rw->ad_slots[i].hx * 2.0 + 8.0, rw->ad_slots[i].hy * 2.0 + 8.0);
  }
  if (!dirty_whole &&
      !mb_glass_damage_expand(last_inst, last_count, dirty, &dirty_n, fb_w / vscale, fb_h / vscale, vscale))
    dirty_whole = 1;
  ad_mode = 1;
  ad_backdrop_changed = 0;
  ad_cur_dt = ad_next_dt(rw);
  rw->ad_frame_serial++;
  ensure_canvas(fb_w, fb_h);
  ID3D11RenderTargetView *target = canvas_rtv ? canvas_rtv : rtv;
  D3D11_RECT parts[MB_PARTS];
  int np = canvas_rtv ? take_parts(fb_w, fb_h, canvas_ok, parts) : -1;
  if (np != 0) encode_rects(target, fb_w, fb_h, last_count, last_bg, last_inst, np < 0 ? NULL : parts, np);
  if (canvas_rtv) {
    canvas_ok = 1;
    ID3D11Resource *back_res = NULL;
    rtv->GetResource(&back_res);
    ctx->CopyResource(back_res, canvas);
    back_res->Release();
  }
  ad_finalize_frame(rw, np < 0, rw->ad_frame_serial);
  HRESULT hr;
  if (flip_chain && np >= 0 && !rw->swap_needs_full) {
    RECT dr[MB_PARTS];
    for (int i = 0; i < np; i++) dr[i] = {parts[i].left, parts[i].top, parts[i].right, parts[i].bottom};
    if (np == 0) dr[np++] = {0, 0, 1, 1};
    DXGI_PRESENT_PARAMETERS pp = {};
    pp.DirtyRectsCount = (UINT)np;
    pp.pDirtyRects = dr;
    hr = chain->Present1(1, 0, &pp);
  } else {
    hr = chain->Present(1, 0);
  }
  int visible_ok = win_record_present_stats(hr);
  if (visible_ok) {
    rw->swap_needs_full = 0;
  } else {
    rw->shown_ok = 0;
    rw->swap_needs_full = 1;
  }
  pace_owed = hr != DXGI_STATUS_OCCLUDED ? 1 : pace_owed ? pace_owed : 2;
  rw = back;
  return SUCCEEDED(hr);
}

// A composition's swap chain is not held to the display by its Present, so each round of frames (win_app.c: every
// window of the run that wanted one presented its, then this, once: each waiting after its own, two windows drew at
// half the display's rate) waits here for the compositor to compose (DwmFlush): a frame for each of the display's, none between that no one sees. Without it a window that
  // moves something every frame (a playhead going, a motion) drew as many as the machine could: 1759 to 1794 a second
  // whole in view on a 240 Hz display, 118% of a core, and 238 to 240 a second, 24% of one, waiting so (measured on
  // ScreenReplay's viewer, an RTX 5080's machine, 2026-11-13; partly covered 461 to 484 and 212 to 217). A waitable
  // swap chain's handle held nothing there (1648 to 1691). The compositor's beat is the primary display's: a window
  // on another display of another rate draws at the primary's. Where the compositor will not say (FAILED), a
  // millisecond's rest, so nothing spins. A frame is as many beats as mb_frame_beats says, counted from the beat the
  // frame before was let go on, not waited after its work: where its work had taken one beat of them already, it waits
  // for the next alone. Waiting them all after the work, a frame whose work took 4.2 to 8.3 ms at 240 Hz (a fast drag
  // of ScreenReplay's playhead over a 4K picture) came every 12.5 ms instead of 8.3, half its frames so, 92 to 97 a
  // second in an uneven step; counted from the last, 118 to 120 a second, 8.3 ms apart (2026-11-15).
void mb_render_pace(void) {
  int owed = pace_owed;
  pace_owed = 0;
  if (!owed) return;
  if (owed == 2) Sleep(16);
  else if (dcomp) {
    static LARGE_INTEGER last_beat;
    int beats = mb_frame_beats();
    if (FAILED(DwmFlush())) {
      Sleep(1);
    } else if (beats > 1) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      DWM_TIMING_INFO ti = {};
      ti.cbSize = sizeof ti;
      // (the beats since the last frame's, to the nearest: a DwmFlush comes back a little after its beat)
      int passed = 1;
      if (last_beat.QuadPart && SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &ti)) && ti.qpcRefreshPeriod)
        passed = (int)((double)(now.QuadPart - last_beat.QuadPart) / (double)ti.qpcRefreshPeriod + 0.5);
      for (int i = passed < 1 ? 1 : passed; i < beats; i++)
        if (FAILED(DwmFlush())) break;
    }
    QueryPerformanceCounter(&last_beat);
  }
}

// Without a window (mbui-render, src/render): WARP's device whatever the machine has (the same rasterizer on every
// Windows: what the references are of), a texture the size a window's swap chain would be as
// the target, the text at the list's scale. A frame drawn there is drawn as a window's is, and its snapshot is of it.
static ID3D11Texture2D *off_target;

extern "C" int mb_render_init_offscreen(double w, double h, double scale) {
  init_render_knobs();
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  // (the start's device let go of: drawn here by WARP's alone; what its pipeline holds of it is left, a run's)
  if (warm_taken()) {
    blur_layers_release();
    win_layers_release();
    release_pipeline();
    ctx->Release();
    dev->Release();
    ctx = NULL;
    dev = NULL;
  }
  if (FAILED(make_device(D3D_DRIVER_TYPE_WARP, 0))) {
    fprintf(stderr, "no Direct3D 11 WARP device\n");
    return 0;
  }
  // which WARP drew it (the app's own beside it, as render_windows.sh puts the pinned one, or the system's)
  HMODULE warp = GetModuleHandleW(L"d3d10warp.dll");
  wchar_t warp_path[MAX_PATH];
  if (warp && GetModuleFileNameW(warp, warp_path, MAX_PATH)) fwprintf(stderr, L"WARP %ls\n", warp_path);
  if (!make_pipeline()) {
    fprintf(stderr, "win_render: offscreen pipeline creation failed\n");
    if (ctx) {
      ctx->Release();
      ctx = NULL;
    }
    if (dev) {
      dev->Release();
      dev = NULL;
    }
    return 0;
  }
  // (with every kind's pixel shader: what is drawn here is what a window draws once they are made)
  if (kinds_thread) WaitForSingleObject(kinds_thread, INFINITE);
  vw = w;
  vh = h;
  vscale = scale;
  fb_w = (int)floor(w * scale + 0.5);
  fb_h = (int)floor(h * scale + 0.5);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)fb_w;
  td.Height = (UINT)fb_h;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (off_target) {
    off_target->Release();
    off_target = NULL;
  }
  if (rtv) {
    rtv->Release();
    rtv = NULL;
  }
  HRESULT hr_tex = dev->CreateTexture2D(&td, NULL, &off_target);
  if (FAILED(hr_tex) || !off_target) {
    fprintf(stderr, "win_render: offscreen CreateTexture2D failed (0x%08lx)\n", (unsigned long)hr_tex);
    release_pipeline();
    if (ctx) {
      ctx->Release();
      ctx = NULL;
    }
    if (dev) {
      dev->Release();
      dev = NULL;
    }
    return 0;
  }
  HRESULT hr_rtv = dev->CreateRenderTargetView(off_target, NULL, &rtv);
  if (FAILED(hr_rtv) || !rtv) {
    fprintf(stderr, "win_render: offscreen CreateRenderTargetView failed (0x%08lx)\n", (unsigned long)hr_rtv);
    off_target->Release();
    off_target = NULL;
    release_pipeline();
    if (ctx) {
      ctx->Release();
      ctx = NULL;
    }
    if (dev) {
      dev->Release();
      dev = NULL;
    }
    return 0;
  }
  mb_text_set_scale(scale);
  if (rw) ad_forget(rw);
  return 1;
}

// A frame drawn into the offscreen target and finished: out[0] the CPU's time of it, out[1] the GPU's (not measured
// here: -1), in ms.
extern "C" void mb_render_draw_timed(const double *inst, int32_t count, double r, double g, double b, double *out) {
  if (!ctx || !rtv) {
    if (out) {
      out[0] = 0;
      out[1] = -1;
    }
    return;
  }
  LARGE_INTEGER f, t0, t1;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  submit(inst, count, r, g, b);
  ctx->Flush();
  QueryPerformanceCounter(&t1);
  out[0] = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
  out[1] = -1;
}

static int write_png(const wchar_t *path, const uint8_t *bgra, int w, int h, int stride) {
  IWICImagingFactory *wic = NULL;
  IWICStream *stream = NULL;
  IWICBitmapEncoder *enc = NULL;
  IWICBitmapFrameEncode *frame = NULL;
  int ok = 0;
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                 __uuidof(IWICImagingFactory), (void **)&wic)) &&
      SUCCEEDED(wic->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
      SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) &&
      SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(enc->CreateNewFrame(&frame, NULL)) && SUCCEEDED(frame->Initialize(NULL)) &&
      SUCCEEDED(frame->SetSize((UINT)w, (UINT)h))) {
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
         SUCCEEDED(frame->WritePixels((UINT)h, (UINT)stride, (UINT)(stride * h), (BYTE *)bgra)) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
  }
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (stream) stream->Release();
  if (wic) wic->Release();
  return ok;
}

// Instances `f` (`count` of them) drawn on the ground `bg` into a texture of their own, `w` × `h`, and copied where the
// CPU reads it (a staging texture, to Map; NULL where it could not be made). What the vertex buffer holds is drawn: the
// caller has put `f` there.
static ID3D11Texture2D *redrawn_of(int w, int h, int32_t count, const float *bg, const float *f, int allow_clear = 0) {
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)w;
  td.Height = (UINT)h;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *tex = NULL, *staging = NULL;
  ID3D11RenderTargetView *view = NULL;
  if (FAILED(dev->CreateTexture2D(&td, NULL, &tex))) return NULL;
  if (FAILED(dev->CreateRenderTargetView(tex, NULL, &view))) {
    tex->Release();
    return NULL;
  }
  int prev_ad_mode = ad_mode;
  if (ad_mode > 0) ad_mode = 0;
  encode(view, w, h, count, bg, f, NULL, allow_clear ? -1 : 0);
  ad_mode = prev_ad_mode;
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (SUCCEEDED(dev->CreateTexture2D(&td, NULL, &staging))) ctx->CopyResource(staging, tex);
  view->Release();
  tex->Release();
  if (rtv) ctx->OMSetRenderTargets(1, &rtv, NULL);
  return staging;
}

// The last presented frame drawn again (its own vertex buffer holds it), as redrawn_of draws.
static ID3D11Texture2D *redrawn(int w, int h) {
  if (!vbuf && !fill_vbuf(last_inst, last_count, last_bg)) return NULL;
  return redrawn_of(w, h, last_count, last_bg, last_inst, 1);
}

// `count` instances of `f` into the vertex buffer, grown to hold them (plus the partial-redraw ground rect at slot
// `count` when `bg` is given): 0 where it could not be written.
static int fill_vbuf(const float *f, int32_t count, const float *bg) {
  if (!dev || !ctx || count < 0) return 0;
  size_t slots = (size_t)(count > 0 ? count : 1);
  const size_t stride_bytes = (size_t)MB_INST * sizeof(float);
  if (bg) {
    if (slots > ((size_t)UINT_MAX - stride_bytes) / stride_bytes) return 0;
  } else if (slots > (size_t)UINT_MAX / stride_bytes) {
    return 0;
  }
  size_t need = (slots + (bg ? 1 : 0)) * stride_bytes;
  if (!ensure_vbuf(need)) return 0;
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(MB_WIN_MAP_VBUF(ctx, vbuf, &m))) {
    vbuf->Release();
    vbuf = NULL;
    vbuf_cap = 0;
    return 0;
  }
  float *dst = (float *)m.pData;
  if (count > 0) memcpy(dst, f, (size_t)count * stride_bytes);
  if (bg) {
    float *gr = dst + (size_t)count * MB_INST;
    memset(gr, 0, stride_bytes);
    gr[0] = -16.0f; gr[1] = -16.0f; gr[2] = (float)vw + 32.0f; gr[3] = (float)vh + 32.0f;
    // (as submit writes it: none on a transparent window's, mb_render_set_clear)
    gr[8] = rw->clear ? 0.0f : bg[0];
    gr[9] = rw->clear ? 0.0f : bg[1];
    gr[10] = rw->clear ? 0.0f : bg[2];
    gr[11] = rw->clear ? 0.0f : 1.0f;
    gr[16] = -1e9f; gr[17] = -1e9f; gr[18] = 1e9f; gr[19] = 1e9f;
  }
  ctx->Unmap(vbuf, 0);
  return 1;
}

// Instances never presented (a shot's, mbui/src/ui/shot.mbt) drawn as a frame is, but `scale` pixels a point into a
// picture of their own w × h (their 0, 0 its top left), on the ground r, g, b, and its pixels into `out` as
// mb_render_drawn_pixels gives the frame's; the size frames are drawn at put back after, and the vertex buffer given
// back the frame's (a snapshot draws it again). 1 when read.
extern "C" int32_t mb_render_pixels_of(const double *inst, int32_t count, double r, double g, double b, double scale,
                                       int32_t w, int32_t h, moonbit_bytes_t out) {
  if (!dev || !ctx || count <= 0 || !(scale > 0) || w <= 0 || h <= 0) return 0;
  if ((size_t)w > SIZE_MAX / 4 / (size_t)h) return 0;
  if ((size_t)Moonbit_array_length(out) < (size_t)w * (size_t)h * 4) return 0;
  if ((size_t)count > (size_t)UINT_MAX / ((size_t)MB_INST * sizeof(float))) return 0;
  size_t n = (size_t)count * MB_INST;
  float *f = (float *)malloc(n * sizeof(float));
  if (!f) return 0;
  for (size_t i = 0; i < n; i++) f[i] = (float)inst[i];
  float bg[3] = {(float)r, (float)g, (float)b};
  double kw = vw, kh = vh, ks = vscale;
  vscale = scale;
  vw = w / scale;
  vh = h / scale;
  int prev_ad_mode = ad_mode;
  ad_mode = -1;
  ID3D11Texture2D *staging = fill_vbuf(f, count) ? redrawn_of(w, h, count, bg, f) : NULL;
  ad_mode = prev_ad_mode;
  vw = kw;
  vh = kh;
  vscale = ks;
  free(f);
  // (the frame's own back where it was, including the partial-redraw ground slot)
  if (last_inst) fill_vbuf(last_inst, last_count, last_bg);
  if (!staging) return 0;
  int ok = 0;
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
    for (int k = 0; k < h; k++) {
      const uint8_t *s = (const uint8_t *)m.pData + (size_t)k * m.RowPitch;
      uint8_t *d = out + (size_t)k * w * 4;
      for (int j = 0; j < w; j++, s += 4, d += 4) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
      }
    }
    ctx->Unmap(staging, 0);
    ok = 1;
  }
  staging->Release();
  return ok;
}

extern "C" int32_t mb_render_drawn_pixels(int32_t x, int32_t y, int32_t w, int32_t h, moonbit_bytes_t out) {
  int fw = fb_w, fh = fb_h;
  if (!dev || !last_inst || fw <= 0 || fh <= 0 || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > fw || y + h > fh) return 0;
  if ((size_t)Moonbit_array_length(out) < (size_t)w * h * 4) return 0;
  ID3D11Texture2D *staging = redrawn(fw, fh);
  if (!staging) return 0;
  int ok = 0;
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
    for (int r = 0; r < h; r++) {
      const uint8_t *s = (const uint8_t *)m.pData + (size_t)(y + r) * m.RowPitch + (size_t)x * 4;
      uint8_t *d = out + (size_t)r * w * 4;
      for (int k = 0; k < w; k++, s += 4, d += 4) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
      }
    }
    ctx->Unmap(staging, 0);
    ok = 1;
  }
  staging->Release();
  return ok;
}

int mb_render_snapshot(const wchar_t *path) {
  int w = fb_w, h = fb_h;
  if (!dev || w <= 0 || h <= 0) return 0;
  ID3D11Texture2D *staging = redrawn(w, h);
  if (!staging) return 0;
  int ok = 0;
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
    const uint8_t *px = (const uint8_t *)m.pData;
    int stride = (int)m.RowPitch;
    uint8_t *out = NULL;
    if (mb_views_count() > 0) {
      // the native views' pictures under the canvas, seen through its holes (premultiplied: canvas + under × (1 - α))
      out = (uint8_t *)calloc((size_t)w * h, 4);
      mb_views_draw_under(out, w, h, mb_app_scale());
      for (int y = 0; y < h; y++) {
        const uint8_t *s = px + (size_t)y * stride;
        uint8_t *d = out + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, s += 4, d += 4) {
          int k = 255 - s[3];
          for (int c = 0; c < 3; c++) d[c] = (uint8_t)(s[c] + (d[c] * k + 127) / 255);
          d[3] = (uint8_t)(s[3] + (d[3] * k + 127) / 255);
        }
      }
      px = out;
      stride = w * 4;
    } else if (rw->clear) {
      out = (uint8_t *)calloc((size_t)w * h, 4);
      for (int y = 0; y < h; y++) {
        const uint8_t *s = px + (size_t)y * stride;
        uint8_t *d = out + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, s += 4, d += 4) {
          uint8_t a = s[3];
          d[3] = a;
          if (a == 0) {
            d[0] = d[1] = d[2] = 0;
          } else if (a == 255) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
          } else {
            for (int c = 0; c < 3; c++) {
              int v = ((int)s[c] * 255 + (a / 2)) / a;
              d[c] = (uint8_t)(v > 255 ? 255 : v);
            }
          }
        }
      }
      px = out;
      stride = w * 4;
    }
    ok = write_png(path, px, w, h, stride);
    free(out);
    ctx->Unmap(staging, 0);
  }
  staging->Release();
  return ok;
}

extern "C" int mb_render_snap_win(int wid, const wchar_t *path) {
  if (wid < 0 || wid >= MB_WINS || !dev || !path) return 0;
  RenderWin *back = rw;
  rw = &render_wins[wid];
  int ok = 0;
  if (last_inst && fb_w > 0 && fb_h > 0 && fill_vbuf(last_inst, last_count, last_bg)) {
    ok = mb_render_snapshot(path);
  }
  rw = back;
  if (last_inst) fill_vbuf(last_inst, last_count, last_bg);
  return ok;
}

extern "C" int mb_render_pictures_missing(void) { return pictures_missing; }

// Waits for the GPU to finish queued work (e.g. a window's first frame before reveal) using a reusable event query.
extern "C" void mb_render_finish(void) {
  static ID3D11Query *q;
  static ID3D11Device *q_dev;
  static HANDLE nap;
  if (!dev || !ctx) return;
  if (q && q_dev != dev) q->Release(), q = NULL;
  if (!q) {
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    if (FAILED(dev->CreateQuery(&qd, &q)) || !q) return;
    q_dev = dev;
  }
  if (!nap) nap = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  ctx->End(q);
  ctx->Flush();
  for (int i = 0; i < 4000 && ctx->GetData(q, NULL, 0, 0) == S_FALSE; i++) {
    LARGE_INTEGER due;
    due.QuadPart = -2500;  // 250 us
    if (nap && SetWaitableTimer(nap, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(nap, 5);
    else Sleep(0);
  }
}

// The offscreen target as its frames left it (not the last frame drawn again: what drawing only the parts that differ
// made of it, mbui-render's MBUI_DL_FROM), as a PNG.
extern "C" int mb_render_snapshot_drawn(const wchar_t *path) {
  if (!off_target) return 0;
  D3D11_TEXTURE2D_DESC td = {};
  off_target->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging = NULL;
  if (FAILED(dev->CreateTexture2D(&td, NULL, &staging))) return 0;
  ctx->CopyResource(staging, off_target);
  int ok = 0;
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
    ok = write_png(path, (const uint8_t *)m.pData, (int)td.Width, (int)td.Height, (int)m.RowPitch);
    ctx->Unmap(staging, 0);
  }
  staging->Release();
  return ok;
}

// What the screen shows of the window's client area (the desktop's composition: the canvas over the native views),
// as a PNG: a check of what the snapshot above puts together itself.
int mb_screen_snapshot(void *hwnd, const wchar_t *path) {
  RECT rc;
  GetClientRect((HWND)hwnd, &rc);
  POINT o = {0, 0};
  ClientToScreen((HWND)hwnd, &o);
  int w = rc.right, h = rc.bottom;
  if (w <= 0 || h <= 0) return 0;
  HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void *bits = NULL;
  HBITMAP bm = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  int ok = 0;
  if (bm) {
    HGDIOBJ was = SelectObject(mem, bm);
    if (BitBlt(mem, 0, 0, w, h, screen, o.x, o.y, SRCCOPY | CAPTUREBLT)) {
      GdiFlush();
      uint8_t *px = (uint8_t *)bits;
      for (int i = 0; i < w * h; i++) px[i * 4 + 3] = 255;
      ok = write_png(path, px, w, h, w * 4);
    }
    SelectObject(mem, was);
    DeleteObject(bm);
  }
  DeleteDC(mem);
  ReleaseDC(NULL, screen);
  return ok;
}

// The GPU's time is not measured here.
extern "C" double mb_host_gpu_ms(void) { return -1; }
// Frames dropped during continuous presentation (DXGI_FRAME_STATISTICS sync-refresh vs present-count delta).
extern "C" int32_t mb_host_frames_dropped(void) { return present_dropped_total; }

extern "C" int32_t mb_host_layer_supported(void) { return dev != NULL ? 1 : 0; }

extern "C" void mb_host_layer_destroy(int32_t id) {
  for (int i = 0; i < win_layers_n; i++) {
    if (win_layers[i].id != id) continue;
    if (ctx) {
      ID3D11ShaderResourceView *none = NULL;
      ctx->PSSetShaderResources(1, 1, &none);
    }
    if (win_layers[i].srv) win_layers[i].srv->Release();
    if (win_layers[i].rt) win_layers[i].rt->Release();
    if (win_layers[i].t) win_layers[i].t->Release();
    win_layers[i] = win_layers[--win_layers_n];
    return;
  }
}

extern "C" int32_t mb_host_layer_render(int32_t id, int32_t pw, int32_t ph, double scale, const double *inst,
                                        int32_t count) {
  if (!dev || !ctx || id <= 0 || pw <= 0 || ph <= 0 || !(scale > 0) || count < 0 || (count > 0 && !inst)) return 0;
  if ((size_t)count > (size_t)UINT_MAX / ((size_t)MB_INST * sizeof(float))) return 0;
  MBWinLayer *L = NULL;
  for (int i = 0; i < win_layers_n; i++) {
    if (win_layers[i].id == id) {
      L = &win_layers[i];
      break;
    }
  }
  if (!L) {
    if (win_layers_n == win_layers_cap) {
      int ncap = win_layers_cap ? win_layers_cap * 2 : 16;
      MBWinLayer *grown = (MBWinLayer *)realloc(win_layers, (size_t)ncap * sizeof(MBWinLayer));
      if (!grown) return 0;
      win_layers = grown;
      win_layers_cap = ncap;
    }
    L = &win_layers[win_layers_n++];
    memset(L, 0, sizeof *L);
    L->id = id;
  }
  ID3D11ShaderResourceView *none = NULL;
  ctx->PSSetShaderResources(1, 1, &none);
  if (!L->t || L->pw != pw || L->ph != ph) {
    if (L->srv) L->srv->Release();
    if (L->rt) L->rt->Release();
    if (L->t) L->t->Release();
    L->srv = NULL;
    L->rt = NULL;
    L->t = NULL;
    L->pw = L->ph = 0;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)pw;
    td.Height = (UINT)ph;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, NULL, &L->t)) || !L->t) {
      L->t = NULL;
      return 0;
    }
    if (FAILED(dev->CreateRenderTargetView(L->t, NULL, &L->rt)) || !L->rt) {
      L->t->Release();
      L->t = NULL;
      L->rt = NULL;
      return 0;
    }
    if (FAILED(dev->CreateShaderResourceView(L->t, NULL, &L->srv)) || !L->srv) {
      L->rt->Release();
      L->t->Release();
      L->rt = NULL;
      L->t = NULL;
      L->srv = NULL;
      return 0;
    }
    L->pw = pw;
    L->ph = ph;
  }
  float *lf = NULL;
  if (count > 0) {
    lf = (float *)malloc((size_t)count * MB_INST * sizeof(float));
    if (!lf) return 0;
    mb_groups_floats(lf, inst, count);
  }
  ID3D11Buffer *saved_vbuf = vbuf;
  size_t saved_vbuf_cap = vbuf_cap;
  vbuf = layer_vbuf;
  vbuf_cap = layer_vbuf_cap;
  int ok = (count == 0) || fill_vbuf(lf, count);
  if (ok) {
    static const float zero_bg[3] = {0.0f, 0.0f, 0.0f};
    double kw = vw, kh = vh, ks = vscale;
    vscale = scale;
    vw = (double)pw / scale;
    vh = (double)ph / scale;
    encode(L->rt, pw, ph, count, zero_bg, lf, NULL, 1);
    vw = kw;
    vh = kh;
    vscale = ks;
  }
  layer_vbuf = vbuf;
  layer_vbuf_cap = vbuf_cap;
  vbuf = saved_vbuf;
  vbuf_cap = saved_vbuf_cap;
  free(lf);
  if (rtv) ctx->OMSetRenderTargets(1, &rtv, NULL);
  else ctx->OMSetRenderTargets(0, NULL, NULL);
  return ok ? 1 : 0;
}
