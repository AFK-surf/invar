// The OpenGL ES 3 renderer of the hosts that draw with it, Android's (android_host.c) and Linux's (linux_render.c):
// the same instances and shader as Metal and WebGL2 (a program of it for each kind it draws most: use_prog), glass's
// passes, the frame presented last kept to be drawn again, its pixels read back. Included once, by the file that has
// GLES 3's functions declared (Android's <GLES3/gl3.h>, Linux's epoxy) and, before it:
//   LOG(...)                          a line of the host's log, printf's way
//   static GLuint image_tex(int id);  the texture an instance's image (slot 20) is drawn from; 0: none (yet)
//   static void view_parts_in(int tid, int32_t start, int32_t end);  a native view's picture's run (draw_kept below)
//   MB_VIEW_TEX                       the first of a native view picture's image ids (src/hostapi/host.mbt)
//   MB_VIEWS_UNDER (optional)         the host keeps its native views under the surface (Linux's pages), not drawn in it
//                                     (Android's; Linux's players): a view's run for which the host's
//                                     static int view_under(int tid) is true takes the canvas away where it shows
//                                     (draw_kept below)
//   MB_HOST_THREADS (optional)        the app runs on another thread than draws (Android's): the host defines
//                                     mb_host_render_dirty (what a frame says differs goes with the frame, handed to
//                                     gl_dirty_add as it is drawn), mb_render_drawn_pixels and mb_render_pixels_of
//                                     (done on the drawing thread: gles_drawn_pixels, gles_pixels_of) itself
// What is drawn where is the host's to say: the frame's size in points and its scale (cur_w, cur_h, cur_scale), the
// surface's in pixels (fb_w, fb_h), the framebuffer drawn into (target_fb); gl_setup once its context is current.
#define MB_GLES_RENDER 1  // (frame_mail.h, included after it, keeps its frames in it: fm_keep)
#include <time.h>
#include "contained.h"
#include "bands.h"
// the groups of instances the host keeps, drawn out as gl_present takes a frame
#include "groups.h"
#include "atlas_pages.h"
#include <stdatomic.h>

// the numbers of an instance (src/ui/scene.mbt)
#define MB_INST 60
#include "glass_damage.h"
#ifndef MB_LAYER_TEX
#define MB_LAYER_TEX 12582912
#endif
// the glyphs' atlases, square, what goes where in them MoonBit's (hostapi's atlas.mbt): the words' (a byte a texel,
// how much of it the glyph covers: drawn in the text's colour; as many bytes as one RGBA atlas half as wide) and colour
// glyphs' (RGBA: an emoji's own). A colour glyph's place across is MB_ATLAS_COLOR_U past its place in its atlas (what
// MoonBit says of it, @hostapi.colour_atlas_u): the shader takes the atlas by it, as Apple's does.
#define MB_ATLAS 4096
#define MB_ATLAS_COLOR 1024
#define MB_ATLAS_COLOR_U 16384

static const char *VS =
    "#version 300 es\n"
    "layout(location=0) in vec4 a_rect;\n"
    "layout(location=1) in vec4 a_p;\n"
    "layout(location=2) in vec4 a_color;\n"
    "layout(location=3) in vec4 a_c2;\n"
    "layout(location=4) in vec4 a_clip;\n"
    "layout(location=5) in vec4 a_pad;\n"
    "layout(location=6) in vec4 a_rc;\n"
    "layout(location=7) in vec4 a_rs;\n"
    "layout(location=8) in vec4 a_rot;\n"
    "layout(location=9) in vec4 a_rr;\n"
    "layout(location=10) in vec4 a_oclip;\n"
    "layout(location=11) in vec4 a_orc;\n"
    "layout(location=12) in vec4 a_ors;\n"
    "layout(location=13) in vec4 a_fade;\n"
    "layout(location=14) in vec4 a_fadex;\n"
    "uniform vec2 u_vp;\n"
    "uniform vec2 u_origin;\n"
    "uniform vec4 u_gb;\n"
    "flat out vec4 v_rot;\n"
    "flat out vec4 v_rect; flat out vec4 v_p; flat out vec4 v_color; flat out vec4 v_c2; flat out vec4 v_clip; flat out vec4 v_pad;\n"
    "flat out vec4 v_rc; flat out vec4 v_rs; flat out vec4 v_rr; flat out vec4 v_oclip; flat out vec4 v_orc; flat out vec4 v_ors;\n"
    "// how far each pixel is into its fades (scene.mbt 52-59: from its left, top, right, foot; 1 and past: whole), worked\n"
    "// out where it is drawn at its corners and taken across it as it goes straight\n"
    "out vec4 v_ramp;\n"
    "void main() {\n"
    "  int vid = gl_VertexID;\n"
    "  vec2 corner = vec2((vid == 1 || vid == 4 || vid == 5) ? 1.0 : 0.0, (vid == 2 || vid == 3 || vid == 5) ? 1.0 : 0.0);\n"
    "  int kind = int(a_p.y + 0.5);\n"
    "  bool merged_liquid = kind == 6 && a_pad.w > 0.0;\n"
    "  float grow = kind == 2 ? a_p.w * 3.0 + 2.0 : (kind == 6 && a_pad.w <= 0.0) ? 2.0 : ((kind == 0 || kind == 4 || kind == 5 || kind == 8) ? 1.0 : 0.0);\n"
    "  vec2 pos = a_rect.xy - grow + corner * (a_rect.zw + 2.0 * grow);\n"
    "  if (merged_liquid) pos = mix(u_gb.xy, u_gb.zw, corner);\n"
    "  // the pieces of shapes (kind 12): nothing drawn\n"
    "  if (kind == 12) pos = vec2(0.0);\n"
    "  // turned (scene.mbt 32-35): drawn turned about its point, everything of how in its own place (v_pos), its clip\n"
    "  // there too or (35: one from outside what turned) where it is drawn (v_cpos)\n"
    "  vec2 at = pos;\n"
    "  vec4 vrot = vec4(0.0);\n"
    "  if (a_rot.x != 0.0) {\n"
    "    float c = cos(a_rot.x), s = sin(a_rot.x);\n"
    "    vec2 d = pos - a_rot.yz;\n"
    "    at = a_rot.yz + vec2(c * d.x - s * d.y, s * d.x + c * d.y);\n"
    "    float k = a_rot.w > 0.5 ? 2.0 : 1.0;\n"
    "    vrot = vec4(c * k, s * k, a_rot.yz);\n"
    "  }\n"
    "  vec2 rel = at - u_origin;\n"
    "  gl_Position = vec4(rel.x / u_vp.x * 2.0 - 1.0, 1.0 - rel.y / u_vp.y * 2.0, 0.0, 1.0);\n"
    "  v_rot = vrot; v_rect = a_rect; v_p = a_p; v_color = a_color; v_c2 = a_c2; v_clip = a_clip; v_pad = a_pad; v_rc = a_rc; v_rs = a_rs; v_rr = a_rr;\n"
    "  if (kind == 4 && a_pad.z > 0.5) v_pad.x = a_rot.w;\n"
    "  // (a glyph's p.x, which it has no use for: how it is turned, for its texels taken whole where it is not)\n"
    "  if (kind == 1 || kind == 3 || kind == 7) v_p.x = a_rot.x;\n"
    "  v_oclip = a_oclip; v_orc = a_orc; v_ors = a_ors;\n"
    "  vec2 cq = a_rot.w > 0.5 ? at : pos;\n"
    "  v_ramp = vec4(a_fadex.y > 0.0 ? (cq.x - a_fadex.x) / a_fadex.y : 2.0, a_fade.y > 0.0 ? (cq.y - a_fade.x) / a_fade.y : 2.0,\n"
    "    a_fadex.w > 0.0 ? (a_fadex.z - cq.x) / a_fadex.w : 2.0, a_fade.w > 0.0 ? (a_fade.z - cq.y) / a_fade.w : 2.0);\n"
    "}\n";

// (after its version, ONE_KIND: the kind of what the program draws, fixed (use_prog); -1 any, as the instance says)
static const char *FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "// the droplet (an unmerged clear liquid lens: kind 6 with merge radius <= 0) lives only where an unmerged liquid\n"
    "// can be drawn: the program of any kind and the single liquid's; merged liquid (ONE_KIND 6) is drawn only for a\n"
    "// group (merge radius > 0, the draw loop's gkind), so its program carries neither the droplet nor the single shape\n"
    "#if ONE_KIND < 0 || ONE_KIND == 13\n"
    "#define MB_HAS_DROPLET 1\n"
    "#else\n"
    "#define MB_HAS_DROPLET 0\n"
    "#endif\n"
    "flat in vec4 v_rot;\n"
    "flat in vec4 v_rect; flat in vec4 v_p; flat in vec4 v_color; flat in vec4 v_c2; flat in vec4 v_clip; flat in vec4 v_pad;\n"
    "flat in vec4 v_rc; flat in vec4 v_rs; flat in vec4 v_rr; flat in vec4 v_oclip; flat in vec4 v_orc; flat in vec4 v_ors;\n"
    "in vec4 v_ramp;\n"
    "uniform float u_scale;\n"
    "uniform float u_top;\n"
    "uniform vec2 u_origin;\n"
    "// the glyphs' atlases: arrays of square pages (atlas_pages.h)\n"
    "uniform highp sampler2DArray u_atlas;\n"
    "uniform highp sampler2DArray u_catlas;\n"
    "uniform sampler2D u_img;\n"
    "uniform int u_img_mips;\n"
    "// glass (src/ui/glass.mbt): the blurred copy under it and where a point is in it (uv = p * u_map.xy + u_map.zw, kept\n"
    "// in u_uvc), a liquid group's shapes and corners, how many\n"
    "uniform highp sampler2D u_bd;\n"
    "uniform vec4 u_map;\n"
    "uniform vec4 u_uvc;\n"
    "// a liquid group's bounds (as the vertex shader has them)\n"
    "uniform vec4 u_gb;\n"
    "uniform vec4 u_shape[8];\n"
    "uniform vec4 u_rad[8];\n"
    "uniform int u_n;\n"
    "uniform float u_through;\n"
    "uniform vec2 u_glight;\n"
    "uniform highp sampler2D u_ad;\n"
    "uniform vec4 u_ad_box[16];\n"
    "uniform int u_ad_n;\n"
    "// the pieces of the frame's shapes (src/ui/shape.mbt: the instances of kind 12 after every one drawn), 2040 fours\n"
    "// (136 instances) a row\n"
    "uniform highp sampler2D u_segs;\n"
    "out vec4 o;\n"
    "#define float2 vec2\n"
    "#define float4 vec4\n"
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
    "vec4 mb_sample_pic(vec2 uv, vec2 dudx, vec2 dudy) {\n"
    "  vec2 sz = vec2(textureSize(u_img, 0));\n"
    "  vec2 rate = vec2(length(dudx * sz), length(dudy * sz));\n"
    "  if (max(abs(rate.x - 1.0), abs(rate.y - 1.0)) < 0.005)\n"
    "    return textureLod(u_img, uv, 0.0);\n"
    "  if (max(rate.x, rate.y) <= 1.005) {\n"
    "    vec2 pos = uv * sz - 0.5;\n"
    "    vec2 tc = floor(pos), f = pos - tc;\n"
    "    vec2 f2 = f * f, f3 = f2 * f;\n"
    "    vec2 w0 = -0.5 * f3 + f2 - 0.5 * f;\n"
    "    vec2 w1 =  1.5 * f3 - 2.5 * f2 + 1.0;\n"
    "    vec2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;\n"
    "    vec2 w3 =  0.5 * f3 - 0.5 * f2;\n"
    "    vec2 w12 = w1 + w2;\n"
    "    vec2 t0 = (tc - 0.5) / sz;\n"
    "    vec2 t12 = (tc + 0.5 + w2 / w12) / sz;\n"
    "    vec2 t3 = (tc + 2.5) / sz;\n"
    "    vec4 c =\n"
    "      (textureLod(u_img, vec2(t0.x,  t0.y),  0.0) * w0.x  +\n"
    "       textureLod(u_img, vec2(t12.x, t0.y),  0.0) * w12.x +\n"
    "       textureLod(u_img, vec2(t3.x,  t0.y),  0.0) * w3.x)  * w0.y +\n"
    "      (textureLod(u_img, vec2(t0.x,  t12.y), 0.0) * w0.x  +\n"
    "       textureLod(u_img, vec2(t12.x, t12.y), 0.0) * w12.x +\n"
    "       textureLod(u_img, vec2(t3.x,  t12.y), 0.0) * w3.x)  * w12.y +\n"
    "      (textureLod(u_img, vec2(t0.x,  t3.y),  0.0) * w0.x  +\n"
    "       textureLod(u_img, vec2(t12.x, t3.y),  0.0) * w12.x +\n"
    "       textureLod(u_img, vec2(t3.x,  t3.y),  0.0) * w3.x)  * w3.y;\n"
    "    c.a = clamp(c.a, 0.0, 1.0);\n"
    "    c.rgb = clamp(c.rgb, 0.0, c.a);\n"
    "    return c;\n"
    "  }\n"
    "  float rmin = min(rate.x, rate.y);\n"
    "  int mips = max(u_img_mips, 1);\n"
    "  int L = (mips > 1 && rmin > 2.0) ? min(int(max(0.0, ceil(log2(rmin)) - 1.0)), mips - 1) : 0;\n"
    "  vec2 msz = vec2(textureSize(u_img, L));\n"
    "  vec2 s = vec2(length(dudx * msz), length(dudy * msz));\n"
    "  if (max(s.x, s.y) > 2.05) return textureGrad(u_img, uv, dudx, dudy);\n"
    "  float ux[5], wx[5], uy[5], wy[5];\n"
    "  mb_cr5(uv.x * msz.x - 0.5, 1.0 / msz.x, max(s.x, 1.0), ux, wx);\n"
    "  mb_cr5(uv.y * msz.y - 0.5, 1.0 / msz.y, max(s.y, 1.0), uy, wy);\n"
    "  vec4 c = vec4(0.0);\n"
    "  float lv = float(L);\n"
    "  for (int j = 0; j < 5; j++) {\n"
    "    vec4 row = vec4(0.0);\n"
    "    for (int i = 0; i < 5; i++)\n"
    "      row += textureLod(u_img, vec2(ux[i], uy[j]), lv) * wx[i];\n"
    "    c += row * wy[j];\n"
    "  }\n"
    "  c.a = clamp(c.a, 0.0, 1.0);\n"
    "  c.rgb = clamp(c.rgb, 0.0, c.a);\n"
    "  return c;\n"
    "}\n"
    "#else\n"
    "vec4 mb_sample_pic(vec2 uv, vec2 dudx, vec2 dudy) {\n"
    "  return textureGrad(u_img, uv, dudx, dudy);\n"
    "}\n"
    "#endif\n"
    "float sd_rr(vec2 p, vec4 r, vec4 rad) {\n"
    "  vec2 hs = r.zw * 0.5;\n"
    "  return mb_sd_box(p - r.xy - hs, hs, mb_corner_of(p - r.xy - hs, rad), 1e9);\n"
    "}\n"
    "// a notch cut into the top edge (scene.mbt 21-23): a circle about the top's middle (pad.y below it, pad.z its radius),\n"
    "// met by the edge through fillets (pad.w): the rounded intersection with what is outside the circle\n"
    "float notch_cut(float d, vec2 p, vec4 r, vec4 pad) {\n"
    "  if (pad.z <= 0.0) return d;\n"
    "  float b = pad.z - length(p - vec2(r.x + r.z * 0.5, r.y + pad.y));\n"
    "  float f = max(pad.w, 0.0);\n"
    "  vec2 u = max(vec2(f + d, f + b), 0.0);\n"
    "  return min(-f, max(d, b)) + length(u);\n"
    "}\n"
    "float smin_k(float a, float b, float k) {\n"
    "  if (k <= 0.0) return min(a, b);\n"
    "  float h = max(k - abs(a - b), 0.0) / k;\n"
    "  return min(a, b) - h * h * k * 0.25;\n"
    "}\n"
    "float sd_group(vec2 p, float k) {\n"
    "  float d = 1e9;\n"
    "  for (int i = 0; i < 8; i++) {\n"
    "    if (i >= u_n) break;\n"
    "    d = smin_k(d, sd_rr(p, u_shape[i], u_rad[i]), k);\n"
    "  }\n"
    "  return d;\n"
    "}\n"
    "// Integral of linear ramp from 0 to w (contourOffset = 0, width w in device px)\n"
    "float mb_contour_int(float t, float w) {\n"
    "  float u = clamp(t, 0.0, w);\n"
    "  return u - u * u / (2.0 * w);\n"
    "}\n"
    "float mb_para_int(float t, float w) {\n"
    "  float u = clamp(t / max(w, 0.0001), 0.0, 1.0);\n"
    "  return w * (2.0 * u * u - (4.0 / 3.0) * u * u * u);\n"
    "}\n"
    "float mb_bevel_F(float t, float w0, float L) {\n"
    "  if (t <= w0) return t;\n"
    "  return w0 + L * (1.0 - exp(-(t - w0) / max(L, 0.0001)));\n"
    "}\n"
    "float mb_bevel_cov(float sd_px, float sc, float w0_pt, float L_pt) {\n"
    "  float invS = 1.0 / max(sc, 1.0);\n"
    "  float s0 = max(sd_px - 0.5, 0.0) * invS;\n"
    "  float s1 = max(sd_px + 0.5, 0.0) * invS;\n"
    "  if (s1 <= s0) return 0.0;\n"
    "  return max(sc * (mb_bevel_F(s1, w0_pt, L_pt) - mb_bevel_F(s0, w0_pt, L_pt)), 0.0);\n"
    "}\n"
    "#if MB_HAS_DROPLET\n"
    "float mb_snell_intersect(float s, float ior, float H_opt) {\n"
    "  float invLen = inversesqrt(1.0 + s * s);\n"
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
    "vec2 mb_droplet_snell(float d, float rawAmount, float halfMinor, int role, float ior) {\n"
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
    "  return vec2(mb_snell_intersect(s_lip - s_cap, ior, H_opt), s_lip);\n"
    "}\n"
    "#endif\n"
    "vec4 backdrop4(vec2 p) {\n"
    "  return textureLod(u_bd, clamp(p * u_map.xy + u_map.zw, u_uvc.xy, u_uvc.zw), 0.0);\n"
    "}\n"
    "vec3 backdrop(vec2 p) {\n"
    "  return backdrop4(p).rgb;\n"
    "}\n"
    "float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "float mb_ink_luma(vec3 c) { return dot(c * c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "float mb_backdrop_luma_rect(vec2 c, vec2 h, float hole_y) {\n"
    "  float sum = 0.0;\n"
    "  float mx1 = -1.0, mx2 = -1.0;\n"
    "  float mn1 = 2.0, mn2 = 2.0;\n"
    "  for (int iy = -1; iy <= 1; iy++) {\n"
    "    float py = c.y + float(iy) * (h.y * 0.60);\n"
    "    for (int ix = -2; ix <= 2; ix++) {\n"
    "      float px = c.x + float(ix) * (h.x * 0.38);\n"
    "      vec4 smp = backdrop4(vec2(px, py));\n"
    "      float a = (u_through > 0.5) ? clamp(smp.a, 0.0, 1.0) : 1.0;\n"
    "      float y = mb_ink_luma(clamp(smp.rgb + vec3((1.0 - a) * hole_y), 0.0, 1.0));\n"
    "      sum += y;\n"
    "      if (y > mx1) { mx2 = mx1; mx1 = y; } else if (y > mx2) { mx2 = y; }\n"
    "      if (y < mn1) { mn2 = mn1; mn1 = y; } else if (y < mn2) { mn2 = y; }\n"
    "    }\n"
    "  }\n"
    "  return (sum - mx1 - mx2 - mn1 - mn2) / 11.0;\n"
    "}\n"
    "float mb_unpack16(vec2 rg) {\n"
    "  float hi = floor(clamp(rg.x, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  float lo = floor(clamp(rg.y, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  return (hi * 256.0 + lo) / 65535.0;\n"
    "}\n"
    "vec2 ad_sample(vec2 ac, float fallback_r, bool is_dark_glass, float tint_a) {\n"
    "  if (u_ad_n > 0) {\n"
    "    int best = -1;\n"
    "    float best_d = 1e9;\n"
    "    for (int i = 0; i < 16; i++) {\n"
    "      if (i >= u_ad_n) break;\n"
    "      float d = length(ac - u_ad_box[i].xy);\n"
    "      if (d <= 2.0 && d < best_d) {\n"
    "        vec4 px = texelFetch(u_ad, ivec2(i, 0), 0);\n"
    "        int code = int(floor(clamp(px.b, 0.0, 1.0) * 255.0 + 0.5));\n"
    "        bool slot_dark = (code >= 48 && code <= 95);\n"
    "        if (slot_dark != is_dark_glass) continue;\n"
    "        best = i; best_d = d;\n"
    "      }\n"
    "    }\n"
    "    if (best < 0) {\n"
    "      best_d = 1e9;\n"
    "      for (int i = 0; i < 16; i++) {\n"
    "        if (i >= u_ad_n) break;\n"
    "        vec2 diff = abs(ac - u_ad_box[i].xy);\n"
    "        if (diff.x <= u_ad_box[i].z + 2.0 && diff.y <= u_ad_box[i].w + 2.0) {\n"
    "          float d = length(diff);\n"
    "          if (d < best_d) {\n"
    "            vec4 px = texelFetch(u_ad, ivec2(i, 0), 0);\n"
    "            int code = int(floor(clamp(px.b, 0.0, 1.0) * 255.0 + 0.5));\n"
    "            bool slot_dark = (code >= 48 && code <= 95);\n"
    "            if (slot_dark != is_dark_glass) continue;\n"
    "            best = i; best_d = d;\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    if (best >= 0) {\n"
    "      vec4 px = texelFetch(u_ad, ivec2(best, 0), 0);\n"
    "      return vec2(mb_unpack16(px.rg), 1.0);\n"
    "    }\n"
    "  }\n"
    "  float bgL = mb_backdrop_luma_rect(ac, vec2(max(fallback_r, 1.0)), is_dark_glass ? 0.0 : 1.0);\n"
    "  float baseDens = mix(0.404, tint_a, smoothstep(0.405, 0.46, tint_a));\n"
    "  float extra = clamp((baseDens - 0.404) / (1.0 - 0.404), 0.0, 1.0);\n"
    "  if (is_dark_glass) return vec2(smoothstep(0.08, 0.42, bgL) * (1.0 - extra), 0.0);\n"
    "  float c_eq = mix(sqrt(max(bgL, 0.0)), 0.992, pow(extra, 1.35));\n"
    "  float effBgL = c_eq * c_eq;\n"
    "  return vec2((1.0 - smoothstep(0.06, 0.34, effBgL)) * (1.0 - extra), 0.0);\n"
    "}\n"
    "vec3 ad_sample_ink(vec2 ac, float fallback_r) {\n"
    "  if (u_ad_n > 0) {\n"
    "    int best = -1;\n"
    "    float best_d = 1e9;\n"
    "    for (int i = 0; i < 16; i++) {\n"
    "      if (i >= u_ad_n) break;\n"
    "      float d = length(ac - u_ad_box[i].xy);\n"
    "      if (d <= 2.0 && d < best_d) { best = i; best_d = d; }\n"
    "    }\n"
    "    if (best < 0) {\n"
    "      best_d = 1e9;\n"
    "      for (int i = 0; i < 16; i++) {\n"
    "        if (i >= u_ad_n) break;\n"
    "        vec2 diff = abs(ac - u_ad_box[i].xy);\n"
    "        if (diff.x <= u_ad_box[i].z + 2.0 && diff.y <= u_ad_box[i].w + 2.0) {\n"
    "          float d = length(diff);\n"
    "          if (d < best_d) { best = i; best_d = d; }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    if (best >= 0) {\n"
    "      vec4 px = texelFetch(u_ad, ivec2(best, 0), 0);\n"
    "      return vec3(mb_unpack16(px.rg), px.a, px.b);\n"
    "    }\n"
    "  }\n"
    "  float bgL = mb_backdrop_luma_rect(ac, vec2(max(fallback_r, 1.0)), 1.0);\n"
    "  return vec3(1.0 - smoothstep(0.06, 0.34, bgL), bgL, 0.0);\n"
    "}\n"
    "vec3 mb_adaptive_ink_at(vec3 rgb, vec2 ac, float fallback_r, vec4 rect) {\n"
    "  float ch = max(max(rgb.r, rgb.g), rgb.b) - min(min(rgb.r, rgb.g), rgb.b);\n"
    "  float origL = luma(rgb);\n"
    "  if (ch > 0.08 || origL >= 0.5) return rgb;\n"
    "  vec3 cap = ad_sample_ink(ac, fallback_r);\n"
    "  float capsuleDarkT = cap.x;\n"
    "  int code = int(floor(clamp(cap.z, 0.0, 1.0) * 255.0 + 0.5));\n"
    "  if (code >= 160) {\n"
    "    float inkT = smoothstep(0.0, 1.0, capsuleDarkT);\n"
    "    float lightL = clamp(1.06 - origL, 0.68, 0.98);\n"
    "    return mix(rgb, vec3(lightL), inkT);\n"
    "  }\n"
    "  if (origL > 0.16) {\n"
    "    float darkT = smoothstep(0.0, 1.0, capsuleDarkT);\n"
    "    return mix(rgb, min(rgb, vec3(0.20)), darkT);\n"
    "  }\n"
    "  return rgb;\n"
    "}\n"
    "// how much of the pixel at p its rounded clip keeps (r: the corners' radii, top left, then clockwise; s: how smooth\n"
    "// each is), its edge covered as a box's own (0.5 - d * scale), so what it cuts meets the ground of the box that cuts it\n"
    "float clip_cover(vec2 p, vec4 c, vec4 r, vec4 s, float scale) {\n"
    "  float d = mb_sd_clip(p, c, r, s, 1.0 + 2.0 / scale);\n"
    "  return clamp(0.5 - d * scale, 0.0, 1.0);\n"
    "}\n"
    "#if ONE_KIND < 0 || ONE_KIND == 10\n"
    "float mb_sd_quad_bezier(vec2 pos, vec2 A, vec2 B, vec2 C) {\n"
    "  vec2 a = B - A;\n"
    "  vec2 b = A - 2.0 * B + C;\n"
    "  vec2 c = a * 2.0;\n"
    "  vec2 d = A - pos;\n"
    "  vec2 ac = C - A;\n"
    "  float bb = dot(b, b);\n"
    "  float t_lin = clamp(dot(pos - A, ac) / max(dot(ac, ac), 1e-12), 0.0, 1.0);\n"
    "  // Collinear fallback for normalized [0,1] coords (residual <= ~1.25e-6 * chord length)\n"
    "  if (bb <= 1e-10 * max(dot(a, a), 1e-8)) {\n"
    "    return length(pos - A - ac * t_lin);\n"
    "  }\n"
    "  vec2 vl = d + (c + b * t_lin) * t_lin;\n"
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
    "    vec2 x = (vec2(sh, -sh) - q) * 0.5;\n"
    "    vec2 uv = sign(x) * pow(max(abs(x), vec2(1e-20)), vec2(1.0 / 3.0));\n"
    "    float t = clamp(uv.x + uv.y - kx, 0.0, 1.0);\n"
    "    vec2 v = d + (c + b * t) * t;\n"
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
    "    vec2 v0 = d + (c + b * t0) * t0;\n"
    "    vec2 v1 = d + (c + b * t1) * t1;\n"
    "    vec2 v2 = d + (c + b * t2) * t2;\n"
    "    float d0 = dot(v0, v0), d1 = dot(v1, v1), d2 = dot(v2, v2);\n"
    "    if (d0 < res) { res = d0; bestT = t0; }\n"
    "    if (d1 < res) { res = d1; bestT = t1; }\n"
    "    if (d2 < res) { res = d2; bestT = t2; }\n"
    "  }\n"
    "  // 2 safeguarded Newton-Raphson steps eliminate float32 Cardano cancellation on shallow curves\n"
    "  for (int ni = 0; ni < 2; ni++) {\n"
    "    vec2 vn = d + (c + b * bestT) * bestT;\n"
    "    vec2 dp = c + 2.0 * b * bestT;\n"
    "    float fp = dot(dp, dp) + 2.0 * dot(vn, b);\n"
    "    if (fp > 1e-12) {\n"
    "      float nt = clamp(bestT - dot(vn, dp) / fp, 0.0, 1.0);\n"
    "      vec2 nv = d + (c + b * nt) * nt;\n"
    "      float nd = dot(nv, nv);\n"
    "      if (nd < res) { res = nd; bestT = nt; } else { break; }\n"
    "    } else { break; }\n"
    "  }\n"
    "  return sqrt(res);\n"
    "}\n"
    "#endif\n"
    "// what it shows at p, as if uncut\n"
    "void shade(vec2 p, float fade_r) {\n"
    "  int kind = ONE_KIND == 13 ? 6 : (ONE_KIND >= 0 ? ONE_KIND : int(v_p.y + 0.5));\n"
    "  // where a native view shows: its picture (NativeViews.Picture), the part of it c2 says, cut to its corners\n"
    "  if (kind == 9) { vec2 hh = v_rect.zw * 0.5; float rr = min(mb_corner_of(p - v_rect.xy - hh, v_rr), min(hh.x, hh.y)); vec2 q = abs(p - v_rect.xy - hh) - hh + rr; float a = clamp(0.5 - (length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rr) * u_scale, 0.0, 1.0); if (a <= 0.0) discard; vec2 uv = v_c2.xy + clamp((p - v_rect.xy) / v_rect.zw, 0.0, 1.0) * v_c2.zw; if (uv.y < 0.0 || uv.y > 1.0) discard; o = texture(u_img, uv) * a; return; }\n"
    "  vec4 col = v_color;\n"
    "  if (kind == 11) {\n"
    "    // a band of a shape (src/ui/shape.mbt): c2.y of its pieces from c2.x; p.x half its stroke's width (0: a fill,\n"
    "    // c2.z 1 by the even-odd rule)\n"
    "    int first = int(v_c2.x + 0.5), n = int(v_c2.y + 0.5);\n"
    "    float hw = v_p.x;\n"
    "    float d = 1e9;\n"
    "    vec4 w = vec4(0.0);\n"
    "    for (int i = first; i < first + n; i++) {\n"
    "      int t = (i / 6) * 15 + mb_seg_slot(i);\n"
    "      vec4 seg = texelFetch(u_segs, ivec2(t % 2040, t / 2040), 0);\n"
    "      if (hw > 0.0) d = min(d, mb_seg_dist(seg, v_rect, p));\n"
    "      else w += mb_path_rows(seg, v_rect, p, u_scale);\n"
    "    }\n"
    "    float a = hw > 0.0 ? clamp(0.5 - (d - hw) * u_scale, 0.0, 1.0) : mb_path_cover(w, v_c2.z);\n"
    "    if (v_pad.w > 0.0) col.rgb = mb_adaptive_ink_at(col.rgb, v_pad.yz, v_pad.w * 0.5, v_rect);\n"
    "    o = vec4(col.rgb, 1.0) * col.a * a;\n"
    "    return;\n"
    "  }\n"
    "  if (kind == 10) {\n"
    "    // a line segment (p.z == 0) or analytic quadratic Bezier stroke (p.z > 0.5, control point in rr.xy)\n"
    "    vec2 lp = p - v_rect.xy;\n"
    "    float d;\n"
    // (mb_sd_quad_bezier is made only where kind 10 can be drawn: a program of another fixed kind has none to call)
    "#if ONE_KIND < 0 || ONE_KIND == 10\n"
    "    if (v_p.z > 0.5) {\n"
    "      float s = max(max(v_rect.z, v_rect.w), 1.0);\n"
    "      vec2 sz = v_rect.zw / s;\n"
    "      d = mb_sd_quad_bezier(lp / s, v_c2.xy * sz, v_rr.xy * sz, v_c2.zw * sz) * s - v_p.x;\n"
    "    } else\n"
    "#endif\n"
    "    {\n"
    "      vec2 a = v_c2.xy * v_rect.zw, c = v_c2.zw * v_rect.zw;\n"
    "      vec2 ab = c - a;\n"
    "      float h = clamp(dot(lp - a, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);\n"
    "      d = length(lp - a - ab * h) - v_p.x;\n"
    "    }\n"
    "    if (v_pad.w > 0.0) col.rgb = mb_adaptive_ink_at(col.rgb, v_pad.yz, v_pad.w * 0.5, v_rect);\n"
    "    o = vec4(col.rgb, 1.0) * col.a * clamp(0.5 - d * u_scale, 0.0, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  if (kind == 1 || kind == 3 || kind == 7) {\n"
    "    // within its own cell (c2: where in the atlas, in texels; a colour glyph's 16384 across past its place in theirs),\n"
    "    // however its rect is stretched or turned: a sample past the cell's edge texels would blend in what lies beside\n"
    "    // it (another glyph, one left there before). The words' atlas has a glyph's cover alone: white, premultiplied, as\n"
    "    // the colour one would have it\n"
    "    bool colour = v_c2.x >= 16384.0;\n"
    "    vec2 at = vec2(v_c2.x - (colour ? 16384.0 : 0.0), v_c2.y);\n"
    "    // (its page, a page's side of its atlas down for each)\n"
    "    float side = colour ? float(textureSize(u_catlas, 0).y) : float(textureSize(u_atlas, 0).y);\n"
    "    float layer = max(floor(at.y / side), 0.0);\n"
    "    at.y -= layer * side;\n"
    "    // One drawn as big as its picture, not turned, its corner on a pixel: each pixel its texel's centre, as\n"
    "    // win_render.cpp takes it (worked out of where it is across the rect, a float's division took a texel's\n"
    "    // neighbour in where its rounding fell, at 3, 2.625 and 1.5 pixels a point and far across)\n"
    "    vec2 off = abs(v_rect.zw * u_scale - v_c2.zw);\n"
    "    vec2 r0 = v_rect.xy * u_scale, r0i = floor(r0 + 0.5);\n"
    "    bool exact = v_p.x == 0.0 && max(off.x, off.y) <= 0.05 && max(abs(r0.x - r0i.x), abs(r0.y - r0i.y)) <= 0.05;\n"
    "    vec2 tex = exact ? floor(p * u_scale - r0i) + 0.5 : (p - v_rect.xy) / v_rect.zw * v_c2.zw;\n"
    "    vec2 uv = clamp(at + tex, at + 0.5, at + v_c2.zw - 0.5);\n"
    "    vec4 t = colour ? textureLod(u_catlas, vec3(uv / side, layer), 0.0)\n"
    "                    : vec4(textureLod(u_atlas, vec3(uv / side, layer), 0.0).r);\n"
    "    if (kind == 7) {\n"
    "      // inked by how light what is under its glass is, looked at round the glass's middle and locally\n"
    "      vec3 ink = mb_adaptive_ink_at(col.rgb, v_pad.yz, v_pad.w * 0.5, v_rect);\n"
    "      o = vec4(ink, 1.0) * (t.a * col.a);\n"
    "      return;\n"
    "    }\n"
    "    o = kind == 1 ? vec4(col.rgb, 1.0) * (t.a * col.a) : t * col.a;\n"
    "    return;\n"
    "  }\n"
    "  if (kind == 6) {\n"
    "    // Copyright 2025, Tim Lehmann for whynotmake.it. Adapted from flutter_liquid_glass\n"
    "    // liquid_glass_final_render_core.glsl, Apache-2.0.\n"
    "    float k = v_pad.w;\n"
    "#if ONE_KIND != 6\n"
    "    vec4 s_rr = max(max(v_rr.x, v_rr.y), max(v_rr.z, v_rr.w)) > 0.0 ? v_rr : vec4(v_p.x);\n"
    "#endif\n"
    "    float d;\n"
    "    float halfMinor = 1e9;\n"
    "    vec2 extSpineVec = vec2(0.0, 0.0);\n"
    "#if ONE_KIND == 13\n"
    "    d = sd_rr(p, v_rect, s_rr);\n"
    "    vec2 hs = v_rect.zw * 0.5;\n"
    "    halfMinor = min(hs.x, hs.y);\n"
    "    vec2 ct = v_rect.xy + hs;\n"
    "    vec2 ch = max(hs - vec2(halfMinor, halfMinor), vec2(0.0, 0.0));\n"
    "    extSpineVec = p - clamp(p, ct - ch, ct + ch);\n"
    "#else\n"
    "#if ONE_KIND != 6\n"
    "    if (k <= 0.0) {\n"
    "      d = sd_rr(p, v_rect, s_rr);\n"
    "      vec2 hs = v_rect.zw * 0.5;\n"
    "      halfMinor = min(hs.x, hs.y);\n"
    "      vec2 ct = v_rect.xy + hs;\n"
    "      vec2 ch = max(hs - vec2(halfMinor, halfMinor), vec2(0.0, 0.0));\n"
    "      extSpineVec = p - clamp(p, ct - ch, ct + ch);\n"
    "    } else\n"
    "#endif\n"
    "    {\n"
    "      d = 1e9;\n"
    "      for (int i = 0; i < 8; i++) {\n"
    "        if (i >= u_n) break;\n"
    "        float di = sd_rr(p, u_shape[i], u_rad[i]);\n"
    "        vec2 hsi = u_shape[i].zw * 0.5;\n"
    "        float hi = min(hsi.x, hsi.y);\n"
    "        vec2 cti = u_shape[i].xy + hsi;\n"
    "        vec2 chi = max(hsi - vec2(hi, hi), vec2(0.0, 0.0));\n"
    "        vec2 vi = p - clamp(p, cti - chi, cti + chi);\n"
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
    "#endif\n"
    "    halfMinor = max(halfMinor, 1.0);\n"
    "    float sd_px = -d * u_scale;\n"
    "    float materialAlpha = clamp(sd_px + 0.5, 0.0, 1.0);\n"
    "    float edgeW = max(0.90 * u_scale, 2.6);\n"
    "    float outward = -sd_px;\n"
    "    float ext_cov = mb_contour_int(max(outward + 0.5, 0.0), edgeW)\n"
    "                  - mb_contour_int(max(outward - 0.5, 0.0), edgeW);\n"
    "    if (materialAlpha < 0.01 && ext_cov < 0.01) discard;\n"
    "    float regMix = smoothstep(0.105, 0.305, col.a);\n"
    "    bool is_clear = (regMix <= 0.0);\n"
    "    bool is_adaptive = v_c2.z < -0.5;\n"
    "    float spot_raw = is_adaptive ? (-v_c2.z - 1.0) : v_c2.z;\n"
    "    bool is_dark = (!is_clear && luma(col.rgb) < 0.35);\n"
    "    float clearVeil = is_clear ? smoothstep(0.005, 0.102, col.a) : 0.0;\n"
    "#if MB_HAS_DROPLET\n"
    "    float droplet = is_clear ? (1.0 - clearVeil) : 0.0;\n"
    "#if ONE_KIND == 13\n"
    "    bool is_drop = (droplet > 0.001);\n"
    "#else\n"
    "    bool is_drop = (droplet > 0.001) && (k <= 0.0);\n"
    "#endif\n"
    "#endif\n"
    "    float inward = max(-d, 0.0);\n"
    "    float rawBevel = max(v_c2.y, 0.0);\n"
    "    float rawAmount = max(v_pad.y, 0.0);\n"
    "    float colChroma = max(max(col.r, col.g), col.b) - min(min(col.r, col.g), col.b);\n"
    "    float solidFade = 1.0 - smoothstep(0.88, 0.99, col.a);\n"
    "    float lensScale = min(1.0, halfMinor / max(rawBevel, 0.001));\n"
    "    float clearBevel = min(rawBevel * lensScale, halfMinor * 0.72);\n"
    "    float clearAmount = min(rawAmount * lensScale, halfMinor * 0.58);\n"
    "    float regBevel = min(rawBevel * 0.90, 0.58 * halfMinor);\n"
    "    float regAmount = min(rawAmount * 0.96, 0.88 * halfMinor);\n"
    "    float bevel = mix(clearBevel, regBevel, regMix);\n"
    "    float amount = mix(clearAmount, regAmount, regMix);\n"
    "    vec2 surfaceNormal = vec2(0.0, 0.0);\n"
    "    float shortening = 0.0;\n"
    "    vec2 disp = vec2(0.0, 0.0);\n"
    "#if MB_HAS_DROPLET\n"
    "    bool need_grad = (inward < max(bevel, 23.0)) || (ext_cov > 0.0) || is_drop;\n"
    "#else\n"
    "    bool need_grad = (inward < max(bevel, 23.0)) || (ext_cov > 0.0);\n"
    "#endif\n"
    "    if (need_grad) {\n"
    "      float e = 0.5;\n"
    "#if ONE_KIND == 13\n"
    "      vec2 grad = vec2(\n"
    "          sd_rr(p + vec2(e, 0.0), v_rect, s_rr) - sd_rr(p - vec2(e, 0.0), v_rect, s_rr),\n"
    "          sd_rr(p + vec2(0.0, e), v_rect, s_rr) - sd_rr(p - vec2(0.0, e), v_rect, s_rr)\n"
    "        ) / (2.0 * e);\n"
    "#elif ONE_KIND == 6\n"
    "      vec2 grad = vec2(\n"
    "          sd_group(p + vec2(e, 0.0), k) - sd_group(p - vec2(e, 0.0), k),\n"
    "          sd_group(p + vec2(0.0, e), k) - sd_group(p - vec2(0.0, e), k)\n"
    "        ) / (2.0 * e);\n"
    "#else\n"
    "      vec2 grad = k <= 0.0\n"
    "        ? vec2(\n"
    "            sd_rr(p + vec2(e, 0.0), v_rect, s_rr) - sd_rr(p - vec2(e, 0.0), v_rect, s_rr),\n"
    "            sd_rr(p + vec2(0.0, e), v_rect, s_rr) - sd_rr(p - vec2(0.0, e), v_rect, s_rr)\n"
    "          ) / (2.0 * e)\n"
    "        : vec2(\n"
    "            sd_group(p + vec2(e, 0.0), k) - sd_group(p - vec2(e, 0.0), k),\n"
    "            sd_group(p + vec2(0.0, e), k) - sd_group(p - vec2(0.0, e), k)\n"
    "          ) / (2.0 * e);\n"
    "#endif\n"
    "      float gradLen = length(grad);\n"
    "      surfaceNormal = gradLen > 0.0001 ? grad / gradLen : vec2(0.0, 0.0);\n"
    "      shortening = sqrt(clamp(gradLen, 0.0, 1.0));\n"
    "      if (inward < bevel && bevel > 0.001 && amount > 0.0) {\n"
    "        float bevelX = 1.0 - clamp(inward / bevel, 0.0, 1.0);\n"
    "        // iOS 27.0 Glass::clear() inverted-refraction caustic spans ~73% of 14pt bevel (2.20x rawAmount).\n"
    "        float clearVeilR = smoothstep(0.005, 0.102, col.a) * (1.0 - smoothstep(0.001, 0.008, abs(v_p.w))) * (1.0 - regMix);\n"
    "        float clearBevelX = pow(bevelX, mix(1.0, 0.78, clearVeilR));\n"
    "        float clearAmt = amount * mix(1.0, 2.20, clearVeilR);\n"
    "        float clearMag = clearAmt * (1.0 - sqrt(max(1.0 - clearBevelX * clearBevelX, 0.0))) * shortening;\n"
    "        float regMag = amount * (1.0 - sqrt(max(1.0 - bevelX * bevelX, 0.0))) * shortening;\n"
    "        float mag = mix(clearMag, regMag, regMix);\n"
    "        disp = surfaceNormal * (-mag);\n"
    "      }\n"
    "    }\n"
    "    int dropRole = (v_p.w <= -8.0) ? int(floor((-v_p.w + 4.0) / 16.0)) : 0;\n"
    "    float rawZoomDelta = (v_p.w <= -8.0) ? ((-v_p.w) - float(dropRole) * 16.0) : ((v_p.w < 0.0) ? (-v_p.w) : 0.0);\n"
    "    float lensZoom = clamp(1.0 + rawZoomDelta, 1.0, 3.0);\n"
    "    vec2 sampleBase = p;\n"
    "    if (lensZoom > 1.0001) {\n"
    "#if ONE_KIND == 13\n"
    "      vec2 zoomC = v_rect.xy + v_rect.zw * 0.5;\n"
    "#elif ONE_KIND == 6\n"
    "      vec2 zoomC = (u_gb.xy + u_gb.zw) * 0.5;\n"
    "#else\n"
    "      vec2 zoomC = (k <= 0.0) ? (v_rect.xy + v_rect.zw * 0.5) : ((u_gb.xy + u_gb.zw) * 0.5);\n"
    "#endif\n"
    "      float zoomMask = (dropRole == 4) ? smoothstep(0.22, 0.52, inward / halfMinor) : 1.0;\n"
    "      float invZoom = mix(1.0, 1.0 / lensZoom, zoomMask);\n"
    "      sampleBase = zoomC + (p - zoomC) * invZoom;\n"
    "    }\n"
    "#if MB_HAS_DROPLET\n"
    "    bool isRoleSlid = (dropRole == 1);\n"
    "    bool isRoleSw = (dropRole == 2);\n"
    "    bool isRoleSeg = (dropRole == 3);\n"
    "    bool isRoleTab = (dropRole == 4);\n"
    "    bool isModernDrop = is_drop && (dropRole >= 1) && (dropRole <= 4);\n"
    "    float snellSLip = 0.0;\n"
    "    float snellDmag = 0.0;\n"
    "    float dropHairline = 0.0;\n"
    "    float pressAmt = 0.0;\n"
    "    float menAA = 0.0;\n"
    "    if (is_drop) {\n"
    "      pressAmt = clamp(v_c2.x, 0.0, 1.0);\n"
    "      dropHairline = (1.0 - smoothstep(0.18, 0.62, inward)) * pressAmt;\n"
    "      if (isModernDrop) {\n"
    "        vec2 sn = mb_droplet_snell(inward, rawAmount, halfMinor, dropRole, 1.52);\n"
    "        snellSLip = sn.y;\n"
    "        snellDmag = clamp(sn.x * shortening, -halfMinor, halfMinor);\n"
    "        vec2 dropDisp = surfaceNormal * snellDmag;\n"
    "        if (isRoleSlid && snellSLip > 0.6 && pressAmt > 0.001) {\n"
    "          menAA = 0.35 * smoothstep(0.6, 2.0, snellSLip) * pressAmt * droplet;\n"
    "        }\n"
    "        disp = mix(disp, dropDisp, droplet);\n"
    "      } else {\n"
    "        float bevelX = (bevel > 0.001) ? (1.0 - clamp(inward / bevel, 0.0, 1.0)) : 0.0;\n"
    "        vec2 paraxialDisp = -surfaceNormal * (bevelX * bevelX * 3.6 * pressAmt);\n"
    "        vec2 dropDisp = mix(paraxialDisp, surfaceNormal * 0.85, dropHairline);\n"
    "        float dLen = length(dropDisp);\n"
    "        if (dLen > halfMinor) dropDisp *= (halfMinor / dLen);\n"
    "        disp = mix(disp, dropDisp, droplet);\n"
    "      }\n"
    "    }\n"
    "    vec3 refractColor;\n"
    "    if (isModernDrop) {\n"
    "      if (abs(v_pad.z) > 0.001 && snellSLip > 0.15 && pressAmt > 0.001) {\n"
    "        float dispR = mb_droplet_snell(inward, rawAmount, halfMinor, dropRole, 1.47).x * shortening;\n"
    "        float dispB = mb_droplet_snell(inward, rawAmount, halfMinor, dropRole, 1.58).x * shortening;\n"
    "        float caW = smoothstep(0.15, 1.20, snellSLip) * clamp(abs(v_pad.z) / 0.04, 0.0, 1.0) * droplet;\n"
    "        float dR = clamp(mix(snellDmag, dispR, caW), -halfMinor, halfMinor);\n"
    "        float dB = clamp(mix(snellDmag, dispB, caW), -halfMinor, halfMinor);\n"
    "        vec2 dispR2 = mix(disp, surfaceNormal * dR, droplet);\n"
    "        vec2 dispB2 = mix(disp, surfaceNormal * dB, droplet);\n"
    "        refractColor = vec3(\n"
    "          backdrop(sampleBase + dispR2).r,\n"
    "          backdrop(sampleBase + disp).g,\n"
    "          backdrop(sampleBase + dispB2).b\n"
    "        );\n"
    "      } else {\n"
    "        refractColor = backdrop(sampleBase + disp);\n"
    "      }\n"
    "    } else if (inward >= bevel || abs(v_pad.z) * (1.0 - regMix) * amount * u_scale <= 0.25) {\n"
    "      refractColor = backdrop(sampleBase + disp);\n"
    "    } else {\n"
    "      float ds = v_pad.z * 0.5 * (1.0 - regMix);\n"
    "      refractColor = vec3(\n"
    "        backdrop(sampleBase + disp * (1.0 + ds)).r,\n"
    "        backdrop(sampleBase + disp).g,\n"
    "        backdrop(sampleBase + disp * (1.0 - ds)).b\n"
    "      );\n"
    "    }\n"
    "    if (menAA > 0.01) {\n"
    "      float aaOff = isModernDrop ? 0.25 : 0.35;\n"
    "      vec2 nOff = surfaceNormal * aaOff;\n"
    "      vec2 tOff = vec2(-surfaceNormal.y, surfaceNormal.x) * aaOff;\n"
    "      vec2 mc = sampleBase + disp;\n"
    "      vec3 menAvg = (backdrop(mc + nOff + tOff) + backdrop(mc + nOff - tOff)\n"
    "                   + backdrop(mc - nOff + tOff) + backdrop(mc - nOff - tOff)) * 0.25;\n"
    "      refractColor = mix(refractColor, menAvg, menAA);\n"
    "    }\n"
    "#else\n"
    "    vec3 refractColor;\n"
    "    if (inward >= bevel || abs(v_pad.z) * (1.0 - regMix) * amount * u_scale <= 0.25) {\n"
    "      refractColor = backdrop(sampleBase + disp);\n"
    "    } else {\n"
    "      float ds = v_pad.z * 0.5 * (1.0 - regMix);\n"
    "      refractColor = vec3(\n"
    "        backdrop(sampleBase + disp * (1.0 + ds)).r,\n"
    "        backdrop(sampleBase + disp).g,\n"
    "        backdrop(sampleBase + disp * (1.0 - ds)).b\n"
    "      );\n"
    "    }\n"
    "#endif\n"
    "    float bgA = (u_through > 0.5) ? clamp(backdrop4(sampleBase + disp).a, 0.0, 1.0) : 1.0;\n"
    "    vec3 emission;\n"
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
    "      emission = vec3(1.0, 1.0, 1.0) * (0.1255 * clearVeil);\n"
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
    "      bool is_inverting = (v_p.z < 0.0);\n"
    "#if ONE_KIND == 13\n"
    "      vec2 ac = v_rect.xy + v_rect.zw * 0.5;\n"
    "      float ar = max(min(v_rect.z, v_rect.w) * 0.5, 1.0) * 0.5;\n"
    "#elif ONE_KIND == 6\n"
    "      vec2 ac = (u_gb.xy + u_gb.zw) * 0.5;\n"
    "      float ar = max(min(u_gb.z - u_gb.x, u_gb.w - u_gb.y) * 0.5, 1.0) * 0.5;\n"
    "#else\n"
    "      vec2 ac = (k <= 0.0) ? (v_rect.xy + v_rect.zw * 0.5) : ((u_gb.xy + u_gb.zw) * 0.5);\n"
    "      float ar = ((k <= 0.0) ? max(min(v_rect.z, v_rect.w) * 0.5, 1.0) : max(min(u_gb.z - u_gb.x, u_gb.w - u_gb.y) * 0.5, 1.0)) * 0.5;\n"
    "#endif\n"
    "      vec2 ad = ad_sample(ac, ar, is_dark, col.a);\n"
    "      float val = ad.x;\n"
    "      bool ad_slotted = (ad.y > 0.5);\n"
    "      if (u_through > 0.5) val *= bgA;\n"
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
    "        vec3 darkEm = mix(vec3((34.0 + darkBump) / 255.0), col.rgb, pow(extra, 1.5));\n"
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
    "        vec3 lightEm = mix(vec3(253.0 / 255.0, 252.0 / 255.0, 253.0 / 255.0), col.rgb, extra) * lightBaseDens;\n"
    "        vec3 darkEm = mix(vec3((34.0 + darkBump) / 255.0), vec3(32.0 / 255.0), pow(darkExtra, 1.5));\n"
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
    "        emission = mix(vec3(253.0 / 255.0, 252.0 / 255.0, 253.0 / 255.0), col.rgb, extra) * lightDens;\n"
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
    "      emission = mix(vec3((34.0 + darkBump) / 255.0), col.rgb, pow(extra, 1.5));\n"
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
    "      emission = mix(vec3(253.0 / 255.0, 252.0 / 255.0, 253.0 / 255.0), col.rgb, extra) * lightDens;\n"
    "      transmittance = 1.0 - lightDens;\n"
    "      lift = mix(0.04, 0.14, smoothstep(0.0, 0.16, extra)) * (1.0 - smoothstep(0.40, 0.95, extra));\n"
    "      chromaGain = mix(1.00, 0.0, lightPow);\n"
    "      contourStr = 0.43; contourDirAmt = 0.77; bevelShadowStr = 0.036;\n"
    "      glintLum = 1.6; glintFaceGain = 0.0; glintVib = 2.85;\n"
    "    }\n"
    "    if (regMix > 0.001 && fogT > 0.001) {\n"
    "      vec2 fc = sampleBase + disp;\n"
    "      float rD = mix(3.2, 4.2, fogT);\n"
    "      vec3 s1 = backdrop(fc + vec2( rD,  rD));\n"
    "      vec3 s2 = backdrop(fc + vec2(-rD,  rD));\n"
    "      vec3 s3 = backdrop(fc + vec2( rD, -rD));\n"
    "      vec3 s4 = backdrop(fc + vec2(-rD, -rD));\n"
    "      vec3 lowC = (refractColor * 2.0 + s1 + s2 + s3 + s4) * (1.0 / 6.0);\n"
    "      float interior = (bevel > 0.001) ? smoothstep(0.30 * bevel, 0.85 * bevel, inward) : 1.0;\n"
    "      float dampW = interior * (fogT * 0.72) * regMix;\n"
    "      refractColor = mix(refractColor, lowC, dampW);\n"
    "    }\n"
    "    if (regMix > 0.0 && regMix < 1.0) {\n"
    "      emission = mix(vec3(0.1255, 0.1255, 0.1255), emission, regMix);\n"
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
    "    vec3 rawRefractColor = refractColor;\n"
    "    vec3 extRimEnv = refractColor;\n"
    "    float extRimValid = 0.0;\n"
    "    float extBlurT = 0.0;\n"
    "    float extDensFade = 1.0 - smoothstep(0.35, 0.85, regExtra);\n"
    "    float extRadLen = length(extSpineVec);\n"
    "    vec2 extRadDir = extRadLen > 0.001 ? extSpineVec / extRadLen : vec2(0.0, 1.0);\n"
    "    if (regMix > 0.001 && solidFade > 0.001 && extDensFade > 0.001 && colChroma < 0.16 && inward < 76.0) {\n"
    "      float extBlur = max(v_p.w, 0.0);\n"
    "      float extRefrSigned = min(v_pad.y, v_pad.y * (1.0 + abs(v_pad.z)));\n"
    "      float extRefrGate = smoothstep(-1.0, 0.0, v_pad.y);\n"
    "      float extMaxReach = max(3.0 * extBlur + extRefrSigned - 2.0, 0.0) * extRefrGate;\n"
    "      if (extMaxReach > 1.0) {\n"
    "        extBlurT = smoothstep(2.0, 10.0, extBlur);\n"
    "        float extBaseValid = smoothstep(1.0, 4.0, extMaxReach) * extDensFade * extRefrGate;\n"
    "        float extTintGate = 1.0 - smoothstep(0.08, 0.16, colChroma);\n"
    "        float diffTail = exp(-max(inward - 2.0, 0.0) / 30.0) * (1.0 - smoothstep(52.0, 76.0, inward));\n"
    "        float radCenterGate = smoothstep(0.18, 0.55, extRadLen / max(halfMinor, 1.0));\n"
    "        float wideGate = smoothstep(4.0, 10.0, extBlur) * smoothstep(24.0, 48.0, halfMinor) * diffTail * radCenterGate;\n"
    "        if (wideGate > 0.001) {\n"
    "          vec2 rDir = extRadDir;\n"
    "          vec2 tDir = vec2(-rDir.y, rDir.x);\n"
    "          float dOutNear = inward + min(mix(10.0, 22.0, extBlurT), extMaxReach * 0.60);\n"
    "          float dOutFar  = inward + min(mix(18.0, 40.0, extBlurT), extMaxReach * 0.88);\n"
    "          float spread   = min(14.0 + 0.36 * inward, extMaxReach * 0.45);\n"
    "          float dBack    = min(mix(10.0, 22.0, extBlurT), extMaxReach * 0.45);\n"
    "          vec3 wNear = backdrop(p + rDir * dOutNear);\n"
    "          vec3 wFarL = backdrop(p + rDir * dOutFar - tDir * spread);\n"
    "          vec3 wFarR = backdrop(p + rDir * dOutFar + tDir * spread);\n"
    "          vec3 wBack = backdrop(p - rDir * dBack);\n"
    "          vec3 wideAmb = wNear * 0.24 + (wFarL + wFarR) * 0.32 + wBack * 0.12;\n"
    "          float ambDelta = smoothstep(0.008, 0.052, length(wideAmb - refractColor));\n"
    "          float ambY = luma(wideAmb);\n"
    "          vec3 ambCh = wideAmb - vec3(ambY);\n"
    "          float ambChLen = length(ambCh);\n"
    "          float refY = luma(refractColor);\n"
    "          float refChLen = length(refractColor - vec3(refY));\n"
    "          float neutralRefGate = 1.0 - smoothstep(0.06, 0.16, refChLen);\n"
    "          float regDarkAmb = is_dark ? (1.0 - brightTSaved) : darkMixSaved;\n"
    "          float lightHeadroom = min(refY + 0.036 * diffTail * smoothstep(0.01, 0.08, ambChLen) * neutralRefGate, mix(0.995, 0.945, smoothstep(0.015, 0.08, ambChLen)));\n"
    "          float lightAmbY = clamp(lightHeadroom, 0.0, 0.995);\n"
    "          vec3 lightAmbD = ambCh * mix(1.00, 1.48, diffTail * neutralRefGate);\n"
    "          float sLA = 1.0;\n"
    "          if (lightAmbD.r > 0.0001) sLA = min(sLA, (1.0 - lightAmbY) / lightAmbD.r); else if (lightAmbD.r < -0.0001) sLA = min(sLA, lightAmbY / (-lightAmbD.r));\n"
    "          if (lightAmbD.g > 0.0001) sLA = min(sLA, (1.0 - lightAmbY) / lightAmbD.g); else if (lightAmbD.g < -0.0001) sLA = min(sLA, lightAmbY / (-lightAmbD.g));\n"
    "          if (lightAmbD.b > 0.0001) sLA = min(sLA, (1.0 - lightAmbY) / lightAmbD.b); else if (lightAmbD.b < -0.0001) sLA = min(sLA, lightAmbY / (-lightAmbD.b));\n"
    "          vec3 lightAmbTarget = clamp(vec3(lightAmbY) + lightAmbD * clamp(sLA, 0.0, 1.0), 0.0, 1.0);\n"
    "          float darkAmbY = clamp(mix(refY, ambY, 0.65) + 0.024 * diffTail * smoothstep(0.01, 0.08, ambChLen), 0.0, 0.995);\n"
    "          vec3 darkAmbD = ambCh * mix(0.90, 1.12, diffTail);\n"
    "          float sDA = 1.0;\n"
    "          if (darkAmbD.r > 0.0001) sDA = min(sDA, (1.0 - darkAmbY) / darkAmbD.r); else if (darkAmbD.r < -0.0001) sDA = min(sDA, darkAmbY / (-darkAmbD.r));\n"
    "          if (darkAmbD.g > 0.0001) sDA = min(sDA, (1.0 - darkAmbY) / darkAmbD.g); else if (darkAmbD.g < -0.0001) sDA = min(sDA, darkAmbY / (-darkAmbD.g));\n"
    "          if (darkAmbD.b > 0.0001) sDA = min(sDA, (1.0 - darkAmbY) / darkAmbD.b); else if (darkAmbD.b < -0.0001) sDA = min(sDA, darkAmbY / (-darkAmbD.b));\n"
    "          vec3 darkAmbTarget = clamp(vec3(darkAmbY) + darkAmbD * clamp(sDA, 0.0, 1.0), 0.0, 1.0);\n"
    "          vec3 ambTarget = mix(lightAmbTarget, darkAmbTarget, regDarkAmb);\n"
    "          float ambStr = mix(0.48, 0.30, regDarkAmb) * neutralRefGate;\n"
    "          float ambW = ambStr * wideGate * extBaseValid * extTintGate * solidFade * regMix * ambDelta * bgA;\n"
    "          refractColor = mix(refractColor, ambTarget, ambW);\n"
    "        }\n"
    "        if (need_grad && inward < 1.3) {\n"
    "          vec2 rimDirRaw = mix(surfaceNormal, extRadDir, 0.36 * extBlurT);\n"
    "          float rimDirLen = length(rimDirRaw);\n"
    "          vec2 rimDir = rimDirLen > 0.001 ? rimDirRaw / rimDirLen : surfaceNormal;\n"
    "          vec2 pEdge = p + surfaceNormal * inward;\n"
    "          float dRimNear = min(mix(6.5, 18.0, extBlurT), extMaxReach);\n"
    "          float dRimFar  = min(mix(10.0, 38.0, extBlurT), extMaxReach);\n"
    "          vec3 rNear = backdrop(pEdge + rimDir * dRimNear);\n"
    "          vec3 rFar  = backdrop(pEdge + rimDir * dRimFar);\n"
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
    "    vec3 chromaVec = (refractColor - vec3(Y)) * (chromaGain * abs(v_p.z) * kneeScale);\n"
    "    if (darkAdGlareW > 0.001) {\n"
    "      if (baseLum > 0.18) {\n"
    "        float over = baseLum - 0.18;\n"
    "        float capped = 0.18 + 0.115 * (over / (over + 0.115));\n"
    "        float newLum = mix(baseLum, capped, darkAdGlareW);\n"
    "        chromaVec *= (newLum / max(baseLum, 0.0001));\n"
    "        baseLum = newLum;\n"
    "      }\n"
    "      vec3 testRaw = emission + vec3(baseLum) + chromaVec;\n"
    "      float peak = max(testRaw.r, max(testRaw.g, testRaw.b));\n"
    "      if (peak > 0.44) {\n"
    "        float lum0 = luma(emission) + baseLum;\n"
    "        float s = clamp((0.44 - lum0) / max(peak - lum0, 0.0001), 0.0, 1.0);\n"
    "        chromaVec *= mix(1.0, s, darkAdGlareW);\n"
    "      }\n"
    "    }\n"
    "    vec3 transmittedColor = vec3(baseLum) + chromaVec;\n"
    "    vec3 rawBase = emission + transmittedColor;\n"
    "    // Exact Wave 3 legacy mapping for zero-veil lenses (col.a == 0); channel-clamped saturation for Clear glass\n"
    "    float maxC = max(rawBase.r, max(rawBase.g, rawBase.b));\n"
    "    vec3 legacyBase = rawBase;\n"
    "    if (maxC > 1.0) {\n"
    "      float lumL = clamp(luma(emission) + baseLum, 0.0, 0.999);\n"
    "      float sL = clamp((1.0 - lumL) / max(maxC - lumL, 0.0001), 0.0, 1.0);\n"
    "      legacyBase = vec3(lumL) + (rawBase - vec3(lumL)) * sL;\n"
    "    }\n"
    "    legacyBase = clamp(legacyBase, 0.0, 1.0);\n"
    "    float clearClampW = smoothstep(0.005, 0.102, col.a);\n"
    "    vec3 baseColor = mix(legacyBase, clamp(rawBase, vec3(0.0), vec3(1.0)), clearClampW);\n"
    "    if (regMix > 0.001) {\n"
    "      float lumR = clamp(luma(emission) + baseLum, 0.001, 0.999);\n"
    "      vec3 dC = rawBase - vec3(lumR);\n"
    "      float s = 1.0;\n"
    "      if (dC.r > 0.0001) s = min(s, (1.0 - lumR) / dC.r);\n"
    "      else if (dC.r < -0.0001) s = min(s, lumR / (-dC.r));\n"
    "      if (dC.g > 0.0001) s = min(s, (1.0 - lumR) / dC.g);\n"
    "      else if (dC.g < -0.0001) s = min(s, lumR / (-dC.g));\n"
    "      if (dC.b > 0.0001) s = min(s, (1.0 - lumR) / dC.b);\n"
    "      else if (dC.b < -0.0001) s = min(s, lumR / (-dC.b));\n"
    "      vec3 regProjected = clamp(vec3(lumR) + dC * clamp(s, 0.0, 1.0), 0.0, 1.0);\n"
    "      float projMix = mix(0.25, mix(0.08, 0.35, regDarkOpt), smoothstep(0.08, 0.16, regExtra));\n"
    "      vec3 regMapped = mix(clamp(rawBase, 0.0, 1.0), regProjected, projMix);\n"
    "      baseColor = mix(baseColor, regMapped, regMix);\n"
    "    }\n"
    "    float holeTrans = is_dark ? min(transmittance, mix(0.295, 0.247, darkKneeW)) : transmittance;\n"
    "    float ga = (u_through > 0.5) ? (1.0 - (1.0 - bgA) * clamp(holeTrans, 0.0, 1.0)) : 1.0;\n"
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
    "      vec2 gp = v_rect.xy + vec2(qx * 0.5 - 256.0, qy * 0.5 - 256.0);\n"
    "      float gr = max(min(v_rect.z, v_rect.w) * 1.5, 64.0);\n"
    "      spot = 1.0 - smoothstep(gr * 0.1, gr, length(p - gp));\n"
    "      baseColor = clamp(baseColor + vec3(0.08 * spot), 0.0, 1.0);\n"
    "    }\n"
    "    vec2 rn = vec2(0.0, 0.0);\n"
    "    float tangency = 0.0;\n"
    "    if (need_grad) {\n"
    "      vec2 la = u_glight;\n"
    "      rn = vec2(\n"
    "        surfaceNormal.x * la.x + surfaceNormal.y * la.y,\n"
    "        -surfaceNormal.x * la.y + surfaceNormal.y * la.x\n"
    "      );\n"
    "      tangency = abs(rn.x);\n"
    "    }\n"
    "    vec3 result = baseColor;\n"
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
    "      result = max(result - transmittedColor * bevelShadow, vec3(0.0));\n"
    "    }\n"
    "    // (b) Glint (clear glass top/bottom specular rim + unified bevel wall for Glass::clear)\n"
    "    float clearVeilC = smoothstep(0.005, 0.102, col.a);\n"
    "    if (v_c2.x > 0.01 && inward < 6.0 && regMix < 0.999) {\n"
    "      float glintProfile = mix(\n"
    "                         clamp(1.0 - inward / 1.2, 0.0, 1.0) + 0.21 * clamp(1.0 - inward / 4.8, 0.0, 1.0),\n"
    "                         clamp(1.0 - inward / 1.35, 0.0, 1.0) + 0.24 * clamp(1.0 - inward / 5.5, 0.0, 1.0),\n"
    "                         clearVeilC);\n"
    "      float dirLobe = pow(max(1.0 - tangency, 0.0), mix(0.85, 1.35, clearVeilC));\n"
    "      float lobe = mix(mix(0.30, 0.08, clearVeilC), 1.0, dirLobe);\n"
    "      float glintGain = 0.58 * clearVeilC;\n"
    "      float glint = clamp(v_c2.x * glintGain * lobe * glintProfile + spot * 0.08 * glintProfile, 0.0, 1.0) * (1.0 - regMix);\n"
    "      float resLum = luma(result);\n"
    "      vec3 glintTarget = vec3(glintLum) + result * glintFaceGain + (result - vec3(resLum)) * glintVib;\n"
    "      result = clamp(mix(result, glintTarget, glint), 0.0, 1.0);\n"
    "      ga = mix(ga, 1.0, glint);\n"
    "    }\n"
    "    float clrWallW = clearVeilC * (1.0 - regMix);\n"
    "    if (clrWallW > 0.001) {\n"
    "      float clrAcross = pow(max(1.0 - rn.y * rn.y, 0.0), 1.8);\n"
    "      float clrInvS = 1.0 / max(u_scale, 1.0);\n"
    "      float clrW0 = mix(max(0.20, 0.50 * clrInvS), max(0.28, 0.75 * clrInvS), clrAcross);\n"
    "      float clrL  = mix(0.06, 0.12, clrAcross);\n"
    "      float clrCov = mb_bevel_cov(sd_px, u_scale, clrW0, clrL);\n"
    "      float clrHair = clamp(clrCov / max(materialAlpha, 0.001), 0.0, 1.0) * clrAcross * clrWallW;\n"
    "      if (clrHair > 0.001) {\n"
    "        vec3 clrEnv = backdrop(p + surfaceNormal * 1.2);\n"
    "        float clrBrightGate = smoothstep(0.55, 0.90, luma(clrEnv)) * 0.55;\n"
    "        vec3 clrWall = pow(max(clrEnv, vec3(1e-6)), vec3(1.75)) * 0.695;\n"
    "        result = mix(result, clrWall, clrHair * clrBrightGate);\n"
    "      }\n"
    "    }\n"
    "    if (regMix > 0.001) {\n"
    "      float regDark = is_dark ? 1.0 : darkMixSaved;\n"
    "      float regDarkRim = regDark * tintDarkEnv;\n"
    "      float tintChroma = smoothstep(0.08, 0.16, colChroma);\n"
    "      if (v_c2.x > 0.01 && inward < 12.0 && solidFade > 0.001) {\n"
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
    "        float regRimGain = 1.0 * clamp(v_c2.x / 0.6, 0.0, 1.5);\n"
    "        float regCoreW = mix(0.21, 0.24, regDark) * (1.0 - tintChroma);\n"
    "        float regTailW = mix(0.030, 0.055, regDark) * (1.0 - tintChroma);\n"
    "        float lipY = resY;\n"
    "        if (inward < 3.0 && regDarkRim < 0.999 && tintChroma < 0.999) {\n"
    "          vec2 tang = vec2(-surfaceNormal.y, surfaceNormal.x) * 3.0;\n"
    "          vec3 lt1 = backdrop(sampleBase + disp + tang);\n"
    "          vec3 lt2 = backdrop(sampleBase + disp - tang);\n"
    "          float avgBgY = (Y + luma(lt1) + luma(lt2)) * (1.0 / 3.0);\n"
    "          lipY = clamp(luma(emission) + clamp(avgBgY * (1.0 + lift * (1.0 - avgBgY)) * transmittance, 0.0, 1.0), 0.0, 1.0);\n"
    "        }\n"
    "        float brightLip = smoothstep(0.77, 0.92, lipY) * (1.0 - regDarkRim) * (1.0 - tintChroma);\n"
    "        float lipProfile = exp(-max(0.0, inward - 0.70) / 0.55);\n"
    "        float regRim = (regRimGain * regLobe * (regEase * (regCoreW * regCore + regTailW * regTail) + 0.72 * brightLip * lipProfile) + spot * 0.10 * regCore) * solidFade * regMix;\n"
    "        float regDarkEff = is_dark ? (1.0 - brightTSaved) : darkMixSaved;\n"
    "        float extRimCoreMask = 1.0 - smoothstep(0.55, 0.95, inward);\n"
    "        vec3 rimSrc = mix(refractColor, extRimEnv, extRimValid * extRimCoreMask * (1.0 - tintChroma));\n"
    "        float rimSrcY = luma(rimSrc);\n"
    "        vec3 rimSrcChroma = rimSrc - vec3(rimSrcY);\n"
    "        float rimPeak = max(0.0, max(rimSrcChroma.r, max(rimSrcChroma.g, rimSrcChroma.b)));\n"
    "        float extRimW = extRimValid * extRimCoreMask * (1.0 - tintChroma);\n"
    "        vec3 rimTarget = clamp(vec3(1.0 - rimPeak * 0.25 * extRimW) + rimSrcChroma * mix(1.0, mix(1.12, 1.40, regDarkEff), extRimW), 0.0, 1.0);\n"
    "        vec3 neutralRimRes = mix(result, rimTarget, clamp(regRim, 0.0, 1.0));\n"
    "        float gaRim = mix(ga, 1.0, clamp(regRim, 0.0, 1.0));\n"
    "        if (extRimValid > 0.001 && inward < 1.2 && tintChroma < 0.999) {\n"
    "          float extRimGate = smoothstep(0.0, 0.6, v_c2.x);\n"
    "          float extDiff = smoothstep(0.02, 0.18, length(extRimEnv - rawRefractColor));\n"
    "          float extLip = exp(-inward / 0.28) * (1.0 - smoothstep(0.65, 1.05, inward));\n"
    "          float extDir = mix(0.62, 1.0, min(abs(rn.y) * 1.15, 1.0));\n"
    "          float extY = luma(extRimEnv);\n"
    "          vec3 extCh = extRimEnv - vec3(extY);\n"
    "          float lightLum = clamp(mix(0.985, extY, mix(0.24, 0.14, extBlurT)), 0.84, 0.985);\n"
    "          vec3 lightD = extCh * mix(0.95, 0.55, extBlurT);\n"
    "          float sL = 1.0;\n"
    "          if (lightD.r > 0.0001) sL = min(sL, (1.0 - lightLum) / lightD.r); else if (lightD.r < -0.0001) sL = min(sL, lightLum / (-lightD.r));\n"
    "          if (lightD.g > 0.0001) sL = min(sL, (1.0 - lightLum) / lightD.g); else if (lightD.g < -0.0001) sL = min(sL, lightLum / (-lightD.g));\n"
    "          if (lightD.b > 0.0001) sL = min(sL, (1.0 - lightLum) / lightD.b); else if (lightD.b < -0.0001) sL = min(sL, lightLum / (-lightD.b));\n"
    "          vec3 extReflLight = clamp(vec3(lightLum) + lightD * clamp(sL, 0.0, 1.0), 0.0, 1.0);\n"
    "          float darkLum = clamp(extY * 1.28 + 0.16, 0.0, 0.90);\n"
    "          vec3 darkD = extCh * mix(2.15, 2.75, extBlurT);\n"
    "          float sD = 1.0;\n"
    "          if (darkD.r > 0.0001) sD = min(sD, (1.0 - darkLum) / darkD.r); else if (darkD.r < -0.0001) sD = min(sD, darkLum / (-darkD.r));\n"
    "          if (darkD.g > 0.0001) sD = min(sD, (1.0 - darkLum) / darkD.g); else if (darkD.g < -0.0001) sD = min(sD, darkLum / (-darkD.g));\n"
    "          if (darkD.b > 0.0001) sD = min(sD, (1.0 - darkLum) / darkD.b); else if (darkD.b < -0.0001) sD = min(sD, darkLum / (-darkD.b));\n"
    "          vec3 extReflDark = clamp(vec3(darkLum) + darkD * clamp(sD, 0.0, 1.0), 0.0, 1.0);\n"
    "          vec3 extRefl = mix(extReflLight, extReflDark, regDarkEff);\n"
    "          float extMixW = clamp(mix(mix(0.65, 0.52, extBlurT), mix(0.88, 0.86, extBlurT), regDarkEff) * extRimGate * extDir * extLip * extDiff * extRimValid * solidFade * regMix * (1.0 - tintChroma), 0.0, 0.90);\n"
    "          neutralRimRes = mix(neutralRimRes, extRefl, extMixW);\n"
    "          gaRim = mix(gaRim, 1.0, extMixW);\n"
    "        }\n"
    "        if (tintChroma > 0.001) {\n"
    "          float tintCore = exp(-inward / 0.80);\n"
    "          float tintTail = exp(-inward / 3.2);\n"
    "          float tintRimAmt = clamp(regRimGain * regLobe * (0.95 * tintCore + 0.12 * tintTail) * solidFade * regMix, 0.0, 1.0);\n"
    "          vec3 rLin = pow(max(result, vec3(0.0)), vec3(2.2));\n"
    "          float brightTint = smoothstep(0.40, 0.70, resY);\n"
    "          vec3 tintLit = pow(clamp(rLin * (1.0 + mix(1.05, 0.65, brightTint) * tintRimAmt) + vec3(mix(0.007, 0.002, brightTint) * tintRimAmt), vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2));\n"
    "          result = mix(neutralRimRes, tintLit, tintChroma);\n"
    "          ga = mix(gaRim, mix(ga, 1.0, tintRimAmt), tintChroma);\n"
    "        } else {\n"
    "          result = neutralRimRes;\n"
    "          ga = gaRim;\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "#if MB_HAS_DROPLET\n"
    "    if (isModernDrop) {\n"
    "      float backdropLuma = clamp(Y, 0.0, 1.0);\n"
    "      float brightBg = smoothstep(0.35, 0.75, backdropLuma);\n"
    "      float liftGain = isRoleTab ? 0.025 : (isRoleSeg ? 0.014 : 0.098);\n"
    "      vec3 dropRes = clamp(refractColor + vec3(liftGain * pressAmt * (1.0 - backdropLuma)), 0.0, 1.0);\n"
    "      float domeScale = clamp(v_rect.w / 38.4, 0.80, 1.35);\n"
    "      float sizeGate = smoothstep(28.0, 36.0, v_rect.w);\n"
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
    "      dropRes = clamp(dropRes + vec3(mix(0.032, 0.025, brightBg) * lowerFace * lowEnv * (isRoleSeg ? 0.45 : (isRoleTab ? mix(0.30, 0.60, brightBg) : 1.0)) * sizeGate * pressAmt), 0.0, 1.0);\n"
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
    "        dropRes = mix(dropRes, vec3(1.0), clamp(rimStr, 0.0, 1.0));\n"
    "      }\n"
    "      // Stage B: Slim Apple-exact closed-form analytic 3D bevel wall\n"
    "      float across = pow(max(1.0 - surfaceNormal.y * surfaceNormal.y, 0.0), 1.35);\n"
    "      float invS = 1.0 / max(u_scale, 1.0);\n"
    "      float hsWall = clamp(halfMinor / 19.2, 0.72, 1.25);\n"
    "      float w0_top = (isRoleSw ? 0.30 : (isRoleTab ? mix(0.18, 0.26, brightBg) : 0.24)) * hsWall;\n"
    "      float w0_side = isRoleTab ? max(0.30 * hsWall, 1.10 * invS) : max(0.52 * hsWall, 1.55 * invS);\n"
    "      float w0_eff = mix(w0_top, w0_side, across);\n"
    "      float L_eff  = (isRoleTab ? mix(mix(0.08, 0.16, across), mix(0.13, 0.16, across), brightBg) : mix(0.12, 0.42, across)) * hsWall;\n"
    "      float wallCov = mb_bevel_cov(sd_px, u_scale, w0_eff, L_eff);\n"
    "      float wallRaw = clamp(wallCov / max(materialAlpha, 0.001), 0.0, 1.0);\n"
    "      float wallBlend = wallRaw * (isRoleTab ? 0.82 : 0.94) * pressAmt;\n"
    "      if (wallBlend > 0.001) {\n"
    "        vec3 envBg = backdrop(p + surfaceNormal * 1.2);\n"
    "        float envY = luma(envBg);\n"
    "        float envChroma = max(envBg.r, max(envBg.g, envBg.b)) - min(envBg.r, min(envBg.g, envBg.b));\n"
    "        float brightEnv = smoothstep(0.35, 0.75, envY);\n"
    "        float satEnv = smoothstep(0.10, 0.35, envChroma);\n"
    "        float wallMix = isRoleTab ? (wallRaw * mix(0.62, 0.92, brightEnv) * pressAmt) : wallBlend;\n"
    "        float neutralMul = isRoleTab ? mix(0.50, mix(0.79, 0.72, across), brightEnv) : mix(0.26, mix(0.80, 0.72, across), brightEnv);\n"
    "        vec3 neutralTint = envBg * neutralMul;\n"
    "        vec3 satTint = pow(max(envBg, vec3(0.0001)), vec3(1.25)) * 0.72;\n"
    "        dropRes = mix(dropRes, mix(neutralTint, satTint, satEnv), wallMix);\n"
    "      }\n"
    "      // Stage C: Segmented prismatic edge dispersion AFTER Stage B\n"
    "      if (pressAmt > 0.001 && inward < 12.0 && abs(v_pad.z) > 0.005) {\n"
    "        float chromaGate = smoothstep(0.005, 0.04, abs(v_pad.z));\n"
    "        float pBand = smoothstep(0.45, 0.75, inward) * (1.0 - smoothstep(1.35, 2.20, inward)) * pressAmt * chromaGate;\n"
    "        float hw = smoothstep(0.35, 0.85, abs(surfaceNormal.x));\n"
    "        vec3 pDark = mix(vec3(-0.025, 0.015, 0.045), vec3(0.048, 0.012, -0.032), hw);\n"
    "        vec3 pLight = mix(vec3(-0.085, 0.0, 0.0), vec3(-0.015, -0.075, -0.135), hw);\n"
    "        vec3 pAdd = mix(pDark, pLight, brightBg);\n"
    "        dropRes = clamp(dropRes + pAdd * pBand, 0.0, 1.0);\n"
    "      }\n"
    "      result = mix(result, dropRes, droplet);\n"
    "    } else if (is_drop) {\n"
    "      float backdropLuma = clamp(Y, 0.0, 1.0);\n"
    "      float brightBg = smoothstep(0.35, 0.75, backdropLuma);\n"
    "      float liftGain = isRoleTab ? 0.025 : (isRoleSeg ? 0.008 : 0.101);\n"
    "      float lensLift = liftGain * pressAmt * (1.0 - backdropLuma) * (1.0 - dropHairline);\n"
    "      result = clamp(result + vec3(lensLift), 0.0, 1.0);\n"
    "      float domeScale = clamp(v_rect.w / 38.0, 0.85, 1.35);\n"
    "      float sizeGate = smoothstep(28.0, 36.0, v_rect.w);\n"
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
    "        result = mix(result, vec3(1.0), clamp(rimLight, 0.0, 1.0));\n"
    "      }\n"
    "      if (isRoleSeg && pressAmt > 0.01) {\n"
    "        float prismBand = smoothstep(0.40, 0.80, inward) * (1.0 - smoothstep(1.15, 1.85, inward))\n"
    "                        * (1.0 - dropHairline) * pressAmt;\n"
    "        if (prismBand > 0.001) {\n"
    "          float hW = smoothstep(0.35, 0.85, abs(surfaceNormal.x));\n"
    "          vec3 rawD = mix(vec3(-0.045, 0.015, 0.030), vec3(0.075, -0.015, -0.060), hW);\n"
    "          vec3 darkPrism = (rawD - vec3(luma(rawD))) * 0.60 * prismBand;\n"
    "          vec3 lightPrism = mix(vec3(-0.068, 0.0, 0.0), vec3(-0.055, -0.148, -0.188), hW) * prismBand;\n"
    "          vec3 pD = mix(darkPrism, lightPrism, brightBg);\n"
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
    "        result = max(result - vec3(shade * dropHairline), vec3(0.0));\n"
    "      }\n"
    "    }\n"
    "#endif\n"
    "    result = clamp(result + mb_dither(p * u_scale) / 255.0, 0.0, 1.0);\n"
    "    // (c) External dark contour (iOS 27.0: Regular controls place their bevel contour strictly outside the silhouette so circles keep full geometric width)\n"
    "    float extUseCov = ext_cov;\n"
    "    float contourDir = mix(1.0, tangency, contourDirAmt);\n"
    "    if (regMix > 0.001 && solidFade > 0.001) {\n"
    "      float extAcross = pow(max(1.0 - rn.y * rn.y, 0.0), mix(1.6, 2.2, regDarkContour));\n"
    "      float extRegDir = mix(mix(0.30, 1.0, extAcross), extAcross, regDarkContour);\n"
    "      float extW0 = mix(0.28, 0.36, extAcross);\n"
    "      float extL  = mix(0.08, 0.14, extAcross);\n"
    "      float regExtCov = mb_bevel_cov(outward, u_scale, extW0, extL);\n"
    "      extUseCov = mix(ext_cov, regExtCov, regMix);\n"
    "      contourDir = mix(contourDir, extRegDir, regMix);\n"
    "    }\n"
    "    float extA = extUseCov * contourStr * contourDir * v_c2.w;\n"
    "    float visA = materialAlpha * v_c2.w;\n"
    "    if (u_through > 0.5) ga = max(ga, max(result.r, max(result.g, result.b)));\n"
    "    o = vec4(result * visA, clamp(visA * ga + extA, 0.0, 1.0));\n"
    "    return;\n"
    "  }\n"
    "  if (kind == 8) {\n"
    "    vec4 s_rr = max(max(v_rr.x, v_rr.y), max(v_rr.z, v_rr.w)) > 0.0 ? v_rr : vec4(v_p.x);\n"
    "    float d = sd_rr(p, v_rect, s_rr);\n"
    "    float materialAlpha = clamp(0.5 - d * u_scale, 0.0, 1.0);\n"
    "    if (materialAlpha < 0.005) discard;\n"
    "    float halfMinor = max(min(v_rect.z, v_rect.w) * 0.5, 1.0);\n"
    "    float inward = max(-d, 0.0);\n"
    "    float rawBevel = max(v_c2.y, 0.0);\n"
    "    float rawAmount = max(v_c2.z, 0.0);\n"
    "    float lensScale = min(1.0, halfMinor / max(rawBevel, 0.001));\n"
    "    float bevel = rawBevel * lensScale;\n"
    "    float amount = rawAmount * lensScale;\n"
    "    vec2 disp = vec2(0.0, 0.0);\n"
    "    if (inward < bevel && bevel > 0.001 && amount > 0.001) {\n"
    "      float e = 0.5;\n"
    "      vec2 grad = vec2(\n"
    "        sd_rr(p + vec2(e, 0.0), v_rect, s_rr) - sd_rr(p - vec2(e, 0.0), v_rect, s_rr),\n"
    "        sd_rr(p + vec2(0.0, e), v_rect, s_rr) - sd_rr(p - vec2(0.0, e), v_rect, s_rr)\n"
    "      ) / (2.0 * e);\n"
    "      float gradLen = length(grad);\n"
    "      vec2 surfaceNormal = gradLen > 0.0001 ? grad / gradLen : vec2(0.0, 0.0);\n"
    "      float shortening = sqrt(clamp(gradLen, 0.0, 1.0));\n"
    "      float bevelX = 1.0 - clamp(inward / bevel, 0.0, 1.0);\n"
    "      float mag = amount * (1.0 - sqrt(max(1.0 - bevelX * bevelX, 0.0))) * shortening;\n"
    "      disp = surfaceNormal * (-mag);\n"
    "    }\n"
    "    vec2 uv = (p + disp) * u_map.xy + u_map.zw;\n"
    "    vec4 fg = vec4(0.0);\n"
    "    if (uv.x >= u_uvc.x && uv.x <= u_uvc.z && uv.y >= u_uvc.y && uv.y <= u_uvc.w) {\n"
    "      fg = texture(u_bd, uv);\n"
    "    }\n"
    "    o = fg * (materialAlpha * col.a);\n"
    "    return;\n"
    "  }\n"
    "  vec2 hs = v_rect.zw * 0.5;\n"
    "  vec2 c = v_rect.xy + hs;\n"
    "  // worked out as near the curve as its cover, its edge or its blur reaches\n"
    "  float band = 1.0 + 2.0 / u_scale + (kind == 2 ? max(v_p.w, 0.5) : max(v_p.z, 0.0));\n"
    "  // (its corner by p: v_rr, each its own)\n"
    "  float rad = mb_corner_of(p - c, v_rr);\n"
    "  float d = mb_sd_box(p - c, hs, rad, band);\n"
    "  if ((kind == 0 && v_pad.w >= 0.0) || kind == 5) d = notch_cut(d, p, v_rect, v_pad);\n"
    "  if (kind == 4) {\n"
    "    vec2 uv = v_c2.xy + clamp((p - v_rect.xy) / v_rect.zw, 0.0, 1.0) * v_c2.zw;\n"
    "    vec2 dudx = dFdx(p) / max(v_rect.zw, 1e-6) * v_c2.zw;\n"
    "    vec2 dudy = dFdy(p) / max(v_rect.zw, 1e-6) * v_c2.zw;\n"
    "    vec4 pic = mb_sample_pic(clamp(uv, 0.0, 1.0), dudx, dudy);\n"
    "    float edge = clamp(0.5 - d * u_scale, 0.0, 1.0);\n"
    "    if (v_pad.z > 0.5) {\n"
    "      // Only picture edges inside the frame cut it; the frame's clip covers shared edges once.\n"
    "      vec2 size = v_rect.zw / v_c2.zw, lo = v_rect.xy - v_c2.xy * size, hi = lo + size;\n"
    "      vec2 a = lo - p, b = p - hi;\n"
    "      if (v_c2.x >= 0.0) a.x = -1e9; if (v_c2.y >= 0.0) a.y = -1e9;\n"
    "      if (v_c2.x + v_c2.z <= 1.0) b.x = -1e9; if (v_c2.y + v_c2.w <= 1.0) b.y = -1e9;\n"
    "      vec2 q = max(a, b);\n"
    "      float pd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);\n"
    "      pic *= v_pad.w * clamp(0.5 - pd * u_scale, 0.0, 1.0);\n"
    "      pic += vec4(col.rgb * v_pad.y, v_pad.y) * (1.0 - pic.a);\n"
    "      if (v_pad.x < 0.5 && max(max(v_rr.x, v_rr.y), max(v_rr.z, v_rr.w)) <= 0.0 &&\n"
    "          max(max(v_rc.x, v_rc.y), max(v_rc.z, v_rc.w)) > 0.0 &&\n"
    "          all(greaterThanEqual(v_clip.xy, v_rect.xy)) && all(lessThanEqual(v_clip.zw, v_rect.xy + v_rect.zw))) edge = 1.0;\n"
    "    }\n"
    "    o = pic * (col.a * edge);\n"
    "    return;\n"
    "  }\n"
    "  if (kind == 2) {\n"
    "    // as Core Animation's: the shape blurred, sigma its radius (corner.sl mb_shadow)\n"
    "    float a = mb_shadow(p - c, hs, rad, max(v_p.w, 0.25));\n"
    "    // glass's: only outside its box (c2.y above it)\n"
    "    if (v_c2.x > 0.5) {\n"
    "      vec2 q = p - c + vec2(0.0, v_c2.y);\n"
    "      float db = mb_sd_box(q, hs, mb_corner_of(q, v_rr), band);\n"
    "      a *= clamp(0.5 + db * u_scale, 0.0, 1.0);\n"
    "    }\n"
    "    o = vec4(col.rgb, 1.0) * col.a * a;\n"
    "    return;\n"
    "  }\n"
    "  float a = clamp(0.5 - d * u_scale, 0.0, 1.0);\n"
    "  if (kind == 5) {\n"
    "    // frosted: the blurred copy, saturated, the tint over it; the rim (a hairline inside the edge, the top left lit,\n"
    "    // the foot and right less), 0.5 wide\n"
    "    bool prog_glass = (v_pad.z < -0.5);\n"
    "    vec3 g3 = backdrop(p);\n"
    "    float u_soft = smoothstep(0.0, 1.0, fade_r);\n"
    "    float soft_w = pow(u_soft, 1.75);\n"
    "    float ps = prog_glass ? soft_w : 1.0;\n"
    "    float sat = prog_glass ? mix(1.0, v_p.z, ps) : v_p.z;\n"
    "    g3 = max(mix(vec3(luma(g3)), g3, sat), 0.0);\n"
    "    float ta = prog_glass ? (col.a * soft_w) : col.a;\n"
    "    g3 = g3 * (1.0 - ta) + col.rgb * ta;\n"
    "    if (v_c2.x + v_c2.y + v_c2.z > 0.0) {\n"
    "      float edge = clamp(0.5 + (d + 0.5) * u_scale, 0.0, 1.0);\n"
    "      float lit = clamp(0.5 + notch_cut(sd_rr(p - 0.5, v_rect, v_rr), p - 0.5, v_rect, v_pad) * u_scale, 0.0, 1.0);\n"
    "      float back = clamp(0.5 + notch_cut(sd_rr(p + 0.5, v_rect, v_rr), p + 0.5, v_rect, v_pad) * u_scale, 0.0, 1.0);\n"
    "      g3 = mix(g3, vec3(1.0), v_c2.x * edge);\n"
    "      g3 = mix(g3, vec3(1.0), v_c2.y * lit);\n"
    "      g3 = mix(g3, vec3(1.0), v_c2.z * back);\n"
    "    }\n"
    "    // dithered as it is written to 8 bits, so its slopes do not band (corner.sl mb_dither)\n"
    "    g3 += mb_dither(p * u_scale) * ps / 255.0;\n"
    "    float edge_a = prog_glass ? smoothstep(0.0, 0.04, fade_r) : 1.0;\n"
    "    o = vec4(g3, 1.0) * (a * v_c2.w * edge_a);\n"
    "    return;\n"
    "  }\n"
    "  if (kind == 0 && v_pad.w < 0.0) {\n"
    "    float ch = max(max(col.r, col.g), col.b) - min(min(col.r, col.g), col.b);\n"
    "    vec3 cap = ad_sample_ink(v_pad.yz, -v_pad.w * 0.5);\n"
    "    int code = int(floor(clamp(cap.z, 0.0, 1.0) * 255.0 + 0.5));\n"
    "    float darkT = (code >= 160) ? cap.x : 0.0;\n"
    "    if (ch <= 0.08 && luma(col.rgb) < 0.5) {\n"
    "      col.rgb = mix(col.rgb, vec3(1.0), darkT);\n"
    "      col.a = mix(col.a, (col.a >= 0.75) ? col.a : min(col.a * 1.45, 0.28), darkT);\n"
    "    } else if (ch > 0.08 && luma(col.rgb) < 0.35) {\n"
    "      col.rgb = mix(col.rgb, min(col.rgb * 1.65 + vec3(0.28, 0.20, 0.18), vec3(1.0)), darkT);\n"
    "      col.a = mix(col.a, (col.a >= 0.75) ? col.a : min(col.a * 1.45, 0.36), darkT);\n"
    "    }\n"
    "  }\n"
    "  vec4 fill = vec4(col.rgb, 1.0) * col.a;\n"
    "  if (v_p.z > 0.0) {\n"
    "    float inner = clamp(0.5 - (d + v_p.z) * u_scale, 0.0, 1.0);\n"
    "    vec4 bc = vec4(v_c2.rgb, 1.0) * v_c2.a;\n"
    "    fill = mix(bc, fill, inner);\n"
    "  }\n"
    "  o = fill * a;\n"
    "}\n"
    "// cut to its clip: a rect's, hard; a rounded one's, covered as edges are\n"
    "void main() {\n"
    "  vec2 at_p = vec2(gl_FragCoord.x, u_top - gl_FragCoord.y) / u_scale + u_origin;\n"
    "  vec2 p = at_p, cp = at_p;\n"
    "  float rl = dot(v_rot.xy, v_rot.xy);\n"
    "  if (rl > 0.5) {\n"
    "    vec2 cs = rl > 2.0 ? v_rot.xy * 0.5 : v_rot.xy;\n"
    "    vec2 d = at_p - v_rot.zw;\n"
    "    p = v_rot.zw + vec2(cs.x * d.x + cs.y * d.y, -cs.y * d.x + cs.x * d.y);\n"
    "    cp = rl > 2.0 ? at_p : p;\n"
    "  }\n"
    "  float cover = 1.0;\n"
    "  if (max(max(v_rc.x, v_rc.y), max(v_rc.z, v_rc.w)) > 0.0) {\n"
    "    cover = clip_cover(cp, v_clip, v_rc, v_rs, u_scale);\n"
    "    if (cover <= 0.0) discard;\n"
    "  } else if (cp.x < v_clip.x || cp.y < v_clip.y || cp.x > v_clip.z || cp.y > v_clip.w) discard;\n"
    "  // and by the clip outside it, where it has one (scene.mbt 40-51: a pane round an image's frame in its corner)\n"
    "  if (max(max(v_orc.x, v_orc.y), max(v_orc.z, v_orc.w)) > 0.0) {\n"
    "    cover *= clip_cover(cp, v_oclip, v_orc, v_ors, u_scale);\n"
    "    if (cover <= 0.0) discard;\n"
    "  }\n"
    "  // and faded out towards a cut, each pixel by where it is (scene.mbt 52-59)\n"
    "  int kind = ONE_KIND == 13 ? 6 : (ONE_KIND >= 0 ? ONE_KIND : int(v_p.y + 0.5));\n"
    "  bool prog_glass = (kind == 5 && v_pad.z < -0.5);\n"
    "  vec4 ramp = clamp(v_ramp, 0.0, 1.0);\n"
    "  float fade_r = ramp.x * ramp.y * ramp.z * ramp.w;\n"
    "  if (fade_r <= 0.0) discard;\n"
    "  if (!prog_glass) {\n"
    "    cover *= fade_r;\n"
    "    if (cover <= 0.0) discard;\n"
    "  }\n"
    "  shade(p, fade_r);\n"
    "  o *= cover;\n"
    "}\n";

// glass's passes over its copy of what is under it
// the flat pipeline: a ground's colour where it is covered whole, cut by straight clips alone (bands.h mb_pieces): what
// FS writes there, nothing worked out
static const char *FS_FLAT =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "flat in vec4 v_color;\n"
    "out vec4 o;\n"
    "void main() { o = vec4(v_color.rgb, 1.0) * v_color.a; }\n";

// lossless fast path for unrotated axis-aligned solid/translucent rects (kind == 0, no radius/border/notch/rounded-clip/fade)
static const char *FS_RECT =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "flat in vec4 v_rect;\n"
    "flat in vec4 v_color;\n"
    "flat in vec4 v_clip;\n"
    "uniform float u_scale;\n"
    "uniform float u_top;\n"
    "uniform vec2 u_origin;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 p = vec2(gl_FragCoord.x, u_top - gl_FragCoord.y) / u_scale;\n"
    "  if (p.x < v_clip.x || p.y < v_clip.y || p.x > v_clip.z || p.y > v_clip.w) discard;\n"
    "  vec2 hs = v_rect.zw * 0.5;\n"
    "  vec2 q = abs(p - (v_rect.xy + hs)) - hs;\n"
    "  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);\n"
    "  float a = clamp(0.5 - d * u_scale, 0.0, 1.0);\n"
    "  o = (vec4(v_color.rgb, 1.0) * v_color.a) * a;\n"
    "}\n";

static const char *PVS =
    "#version 300 es\n"
    "void main() {\n"
    "  gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0);\n"
    "}\n";

static const char *PFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "#define float2 vec2\n"
    "#define float4 vec4\n"
    MB_CORNER_SL
    "// The backdrop's passes: u_a.xy the source's texel, u_a.z 1 halving (dual Kawase's down), 2 a Gaussian along u_b.zw\n"
    "// of deviation u_a.w (texels); u_b.xy the last texel's centre of what the source holds\n"
    "uniform highp sampler2D u_src;\n"
    "uniform highp sampler2D u_lv1;\n"
    "uniform highp sampler2D u_lv2;\n"
    "uniform highp sampler2D u_lv3;\n"
    "uniform vec4 u_a;\n"
    "uniform vec4 u_b;\n"
    "uniform vec4 u_c[9];\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 ts = u_a.xy;\n"
    "  vec2 lo = ts * 0.5, hi = u_b.xy;\n"
    "  if (u_a.z > 5.5) {\n"
    "    vec2 uv = gl_FragCoord.xy * ts;\n"
    "    vec2 dir = u_b.zw * ts;\n"
    "    float sg = u_a.w;\n"
    "    float k = -0.5 / (sg * sg);\n"
    "    vec4 c0 = texture(u_src, clamp(uv, lo, hi));\n"
    "    vec4 oS = c0;\n"
    "    vec3 oL = pow(max(c0.rgb, vec3(0.0)), vec3(2.2));\n"
    "    float wsum = 1.0;\n"
    "    int n = int(ceil(sg * 3.0));\n"
    "    for (int i = 1; i <= 64; i += 2) {\n"
    "      if (i > n) break;\n"
    "      float w1 = exp(float(i * i) * k), w2 = exp(float((i + 1) * (i + 1)) * k);\n"
    "      float w = w1 + w2;\n"
    "      float off = (float(i) * w1 + float(i + 1) * w2) / w;\n"
    "      vec4 cp = texture(u_src, clamp(uv + dir * off, lo, hi));\n"
    "      vec4 cm = texture(u_src, clamp(uv - dir * off, lo, hi));\n"
    "      oS += (cp + cm) * w;\n"
    "      oL += (pow(max(cp.rgb, vec3(0.0)), vec3(2.2)) + pow(max(cm.rgb, vec3(0.0)), vec3(2.2))) * w;\n"
    "      wsum += 2.0 * w;\n"
    "    }\n"
    "    vec4 outS = oS / wsum;\n"
    "    // Separable H+V bright-gated linear-light wide blur: exact for axis-aligned features, mildly directional on diagonals.\n"
    "    vec3 outL = pow(max(oL / wsum, vec3(0.0)), vec3(1.0 / 2.2));\n"
    "    float bw = smoothstep(0.55, 0.82, 0.2126 * outS.r + 0.7152 * outS.g + 0.0722 * outS.b);\n"
    "    o = vec4(mix(outS.rgb, outL, bw), outS.a);\n"
    "    return;\n"
    "  }\n"
    "  if (u_a.z > 4.5) {\n"
    "    vec4 cSharp;\n"
    "    float sg = u_a.w;\n"
    "    if (sg > 0.3) {\n"
    "      vec2 uv = gl_FragCoord.xy * ts;\n"
    "      vec2 dir = u_b.zw * ts;\n"
    "      float k = -0.5 / (sg * sg);\n"
    "      vec4 o_sum = texture(u_src, clamp(uv, lo, hi));\n"
    "      float wsum = 1.0;\n"
    "      int n = int(ceil(sg * 3.0));\n"
    "      for (int i = 1; i <= 64; i += 2) {\n"
    "        if (i > n) break;\n"
    "        float w1 = exp(float(i * i) * k), w2 = exp(float((i + 1) * (i + 1)) * k);\n"
    "        float w = w1 + w2;\n"
    "        float off = (float(i) * w1 + float(i + 1) * w2) / w;\n"
    "        o_sum += (texture(u_src, clamp(uv + dir * off, lo, hi)) + texture(u_src, clamp(uv - dir * off, lo, hi))) * w;\n"
    "        wsum += 2.0 * w;\n"
    "      }\n"
    "      cSharp = o_sum / wsum;\n"
    "    } else if (u_c[4].w > 0.001) {\n"
    "      vec2 uv = gl_FragCoord.xy * ts;\n"
    "      vec2 ox = vec2(u_c[4].w, 0.0) * ts;\n"
    "      vec2 oy = vec2(0.0, u_c[4].w) * ts;\n"
    "      cSharp = (texture(u_src, clamp(uv - ox - oy, lo, hi)) +\n"
    "                texture(u_src, clamp(uv + ox - oy, lo, hi)) +\n"
    "                texture(u_src, clamp(uv - ox + oy, lo, hi)) +\n"
    "                texture(u_src, clamp(uv + ox + oy, lo, hi))) * 0.25;\n"
    "    } else {\n"
    "      cSharp = texture(u_src, clamp(gl_FragCoord.xy * ts, lo, hi));\n"
    "    }\n"
    "    vec4 cWide = texture(u_lv3, clamp(gl_FragCoord.xy * u_c[5].xy, u_c[5].zw * 0.5, u_c[4].xy));\n"
    "    o = mix(cWide, cSharp, u_c[4].z);\n"
    "    return;\n"
    "  }\n"
    "  if (u_a.z > 3.5) {\n"
    "    vec2 p = u_c[0].xy + gl_FragCoord.xy * u_c[0].zw;\n"
    "    float ramp = 1.0;\n"
    "    if (u_c[1].y > 0.0) ramp *= clamp((p.y - u_c[1].x) / u_c[1].y, 0.0, 1.0);\n"
    "    if (u_c[1].w > 0.0) ramp *= clamp((u_c[1].z - p.y) / u_c[1].w, 0.0, 1.0);\n"
    "    if (u_c[2].y > 0.0) ramp *= clamp((p.x - u_c[2].x) / u_c[2].y, 0.0, 1.0);\n"
    "    if (u_c[2].w > 0.0) ramp *= clamp((u_c[2].z - p.x) / u_c[2].w, 0.0, 1.0);\n"
    "    float sp = max(u_c[3].x, 1.0);\n"
    "    float kf = u_c[3].y;\n"
    "    vec4 m0 = u_c[4], m1 = u_c[5], m2 = u_c[6], m3 = u_c[7];\n"
    "    if (kf < 0.5) { o = texture(u_src, clamp(gl_FragCoord.xy * m0.xy, m0.xy * 0.5, m0.zw)); return; }\n"
    "    float r = clamp(ramp, 0.0, 1.0);\n"
    "    if (r <= 0.0) { o = texture(u_src, clamp(gl_FragCoord.xy * m0.xy, m0.xy * 0.5, m0.zw)); return; }\n"
    "    if (r >= 1.0) {\n"
    "      int ki = int(kf + 0.5);\n"
    "      if (ki <= 1) o = texture(u_lv1, clamp(gl_FragCoord.xy * m1.xy, m1.xy * 1.0, m1.zw));\n"
    "      else if (ki == 2) o = texture(u_lv2, clamp(gl_FragCoord.xy * m2.xy, m2.xy * 2.0, m2.zw));\n"
    "      else o = texture(u_lv3, clamp(gl_FragCoord.xy * m3.xy, m3.xy * 4.0, m3.zw));\n"
    "      return;\n"
    "    }\n"
    "    float sig1 = pow(sp, 1.0 / kf);\n"
    "    float u_b = smoothstep(0.06, 1.0, r);\n"
    "    float rs = u_b * u_b;\n"
    "    float target_px = sp * rs;\n"
    "    if (target_px <= sig1 || kf <= 1.05) {\n"
    "      vec2 uv0_min = m0.xy * 0.5, uv0_max = m0.zw;\n"
    "      vec2 c_uv = gl_FragCoord.xy * m0.xy;\n"
    "      vec4 g_small;\n"
    "      if (target_px <= 0.01) {\n"
    "        g_small = texture(u_src, clamp(c_uv, uv0_min, uv0_max));\n"
    "      } else if (target_px <= 0.55) {\n"
    "        // Exact separable 3x3 Gaussian via 2x2 bilinear taps (center weight 0.5 split across +-o1)\n"
    "        float inv2 = -0.5 / (target_px * target_px);\n"
    "        float w1 = exp(inv2);\n"
    "        float o1 = w1 / (0.5 + w1);\n"
    "        vec2 ox = vec2(o1, 0.0) * m0.xy;\n"
    "        vec2 oy = vec2(0.0, o1) * m0.xy;\n"
    "        g_small = (texture(u_src, clamp(c_uv - ox - oy, uv0_min, uv0_max)) +\n"
    "                   texture(u_src, clamp(c_uv + ox - oy, uv0_min, uv0_max)) +\n"
    "                   texture(u_src, clamp(c_uv - ox + oy, uv0_min, uv0_max)) +\n"
    "                   texture(u_src, clamp(c_uv + ox + oy, uv0_min, uv0_max))) * 0.25;\n"
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
    "        float ox[4] = float[4](-o2, -o1, o1, o2);\n"
    "        float pw[4] = float[4](p2, p1, p1, p2);\n"
    "        g_small = vec4(0.0);\n"
    "        for (int iy = 0; iy < 4; iy++) {\n"
    "          vec4 row = vec4(0.0);\n"
    "          float uy = clamp(c_uv.y + ox[iy] * m0.y, uv0_min.y, uv0_max.y);\n"
    "          for (int ix = 0; ix < 4; ix++) {\n"
    "            float ux = clamp(c_uv.x + ox[ix] * m0.x, uv0_min.x, uv0_max.x);\n"
    "            row += texture(u_src, vec2(ux, uy)) * pw[ix];\n"
    "          }\n"
    "          g_small += row * pw[iy];\n"
    "        }\n"
    "      }\n"
    "      float blend_lo = min(1.1, sig1 * 0.45);\n"
    "      float bt = clamp((target_px - blend_lo) / max(sig1 - blend_lo, 1e-4), 0.0, 1.0);\n"
    "      float blend1 = bt > 0.0 ? pow(bt, 1.8) : 0.0;\n"
    "      if (blend1 > 0.0) {\n"
    "        vec4 c_lv1 = texture(u_lv1, clamp(gl_FragCoord.xy * m1.xy, m1.xy * 1.0, m1.zw));\n"
    "        o = mix(g_small, c_lv1, blend1);\n"
    "      } else {\n"
    "        o = g_small;\n"
    "      }\n"
    "      return;\n"
    "    }\n"
    "    float lv_u = clamp(1.0 + (kf - 1.0) * log(target_px / sig1) / max(log(sp / sig1), 1e-4), 1.0, kf);\n"
    "    int idx = min(int(floor(lv_u)), int(kf + 0.5) - 1);\n"
    "    if (idx < 0) idx = 0;\n"
    "    float f = lv_u - float(idx);\n"
    "    vec4 c0, c1;\n"
    "    if (idx == 0) {\n"
    "      c0 = texture(u_src, clamp(gl_FragCoord.xy * m0.xy, m0.xy * 0.5, m0.zw));\n"
    "      c1 = texture(u_lv1, clamp(gl_FragCoord.xy * m1.xy, m1.xy * 1.0, m1.zw));\n"
    "    } else if (idx == 1) {\n"
    "      c0 = texture(u_lv1, clamp(gl_FragCoord.xy * m1.xy, m1.xy * 1.0, m1.zw));\n"
    "      c1 = texture(u_lv2, clamp(gl_FragCoord.xy * m2.xy, m2.xy * 2.0, m2.zw));\n"
    "    } else {\n"
    "      c0 = texture(u_lv2, clamp(gl_FragCoord.xy * m2.xy, m2.xy * 2.0, m2.zw));\n"
    "      c1 = texture(u_lv3, clamp(gl_FragCoord.xy * m3.xy, m3.xy * 4.0, m3.zw));\n"
    "    }\n"
    "    o = mix(c0, c1, f);\n"
    "    return;\n"
    "  }\n"
    "  if (u_a.z > 2.5) {\n"
    "    vec2 p = u_c[0].xy + gl_FragCoord.xy * u_c[0].zw;\n"
    "    p = mb_contained_point(p, u_c[1], u_c[2], u_c[3], u_c[4], u_c[5], u_c[6], u_c[7], u_c[8], u_c[0].z);\n"
    "    o = texture(u_src, clamp((p - u_c[0].xy) / u_c[0].zw * ts, lo, hi));\n"
    "    return;\n"
    "  }\n"
    "  if (u_a.z < 1.5) {\n"
    "    vec2 uv = gl_FragCoord.xy * 2.0 * ts;\n"
    "    vec4 s = texture(u_src, clamp(uv, lo, hi)) * 4.0;\n"
    "    s += texture(u_src, clamp(uv - ts, lo, hi));\n"
    "    s += texture(u_src, clamp(uv + ts, lo, hi));\n"
    "    s += texture(u_src, clamp(uv + vec2(ts.x, -ts.y), lo, hi));\n"
    "    s += texture(u_src, clamp(uv + vec2(-ts.x, ts.y), lo, hi));\n"
    "    o = s / 8.0;\n"
    "    return;\n"
    "  }\n"
    "  vec2 uv = gl_FragCoord.xy * ts;\n"
    "  vec2 dir = u_b.zw * ts;\n"
    "  float sg = u_a.w;\n"
    "  float k = -0.5 / (sg * sg);\n"
    "  vec4 s = texture(u_src, clamp(uv, lo, hi));\n"
    "  float wsum = 1.0;\n"
    "  int n = int(ceil(sg * 3.0));\n"
    "  for (int i = 1; i <= 64; i += 2) {\n"
    "    if (i > n) break;\n"
    "    float w1 = exp(float(i * i) * k), w2 = exp(float((i + 1) * (i + 1)) * k);\n"
    "    float w = w1 + w2;\n"
    "    float off = (float(i) * w1 + float(i + 1) * w2) / w;\n"
    "    s += (texture(u_src, clamp(uv + dir * off, lo, hi)) + texture(u_src, clamp(uv - dir * off, lo, hi))) * w;\n"
    "    wsum += 2.0 * w;\n"
    "  }\n"
    "  o = s / wsum;\n"
    "}\n";

static const char *AD_FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D u_bd;\n"
    "uniform highp sampler2D u_prev;\n"
    "uniform vec4 u_map;\n"
    "uniform vec4 u_uvc;\n"
    "uniform vec4 u_box;\n"
    "uniform vec4 u_param;\n"
    "out vec4 o;\n"
    "float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "float mb_ink_luma(vec3 c) { return dot(c * c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "float mb_unpack16(vec2 rg) {\n"
    "  float hi = floor(clamp(rg.x, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  float lo = floor(clamp(rg.y, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  return (hi * 256.0 + lo) / 65535.0;\n"
    "}\n"
    "vec2 mb_pack16(float v) {\n"
    "  float q = floor(clamp(v, 0.0, 1.0) * 65535.0 + 0.5);\n"
    "  float hi = floor(q / 256.0);\n"
    "  float lo = q - hi * 256.0;\n"
    "  return vec2(hi / 255.0, lo / 255.0);\n"
    "}\n"
    "void main() {\n"
    "  float is_dark = u_param.x;\n"
    "  bool is_inverting = u_param.y > 3.5;\n"
    "  float p1 = u_param.y - (is_inverting ? 4.0 : 0.0);\n"
    "  bool is_through = p1 > 1.5;\n"
    "  float is_new = p1 - (is_through ? 2.0 : 0.0);\n"
    "  float dt = u_param.z;\n"
    "  float hole_y = (is_dark > 0.5) ? 0.0 : 1.0;\n"
    "  vec2 c = u_box.xy, h = u_box.zw;\n"
    "  float sum = 0.0, mx1 = -1.0, mx2 = -1.0, mn1 = 2.0, mn2 = 2.0;\n"
    "  for (int iy = -1; iy <= 1; iy++) {\n"
    "    float py = c.y + float(iy) * (h.y * 0.60);\n"
    "    for (int ix = -2; ix <= 2; ix++) {\n"
    "      float px = c.x + float(ix) * (h.x * 0.38);\n"
    "      vec2 uv = clamp(vec2(px, py) * u_map.xy + u_map.zw, u_uvc.xy, u_uvc.zw);\n"
    "      vec4 smp = texture(u_bd, uv);\n"
    "      float a = is_through ? clamp(smp.a, 0.0, 1.0) : 1.0;\n"
    "      float y = mb_ink_luma(clamp(smp.rgb + vec3((1.0 - a) * hole_y), 0.0, 1.0));\n"
    "      sum += y;\n"
    "      if (y > mx1) { mx2 = mx1; mx1 = y; } else if (y > mx2) { mx2 = y; }\n"
    "      if (y < mn1) { mn2 = mn1; mn1 = y; } else if (y < mn2) { mn2 = y; }\n"
    "    }\n"
    "  }\n"
    "  float bgL = (sum - mx1 - mx2 - mn1 - mn2) / 11.0;\n"
    "  int sid = int(u_param.w + 0.5);\n"
    "  vec4 prev = texelFetch(u_prev, ivec2(sid, 0), 0);\n"
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
    "  o = vec4(mb_pack16(nextT), latch, clamp(bgL, 0.0, 1.0));\n"
    "}\n";

static GLuint vao, vbo, atlas, catlas, post, ad_prog;
// how many pages each atlas has (the GL thread's: what its texture has, read by the app's thread too), what stands in a
// sampler2D unit nothing is bound to (an instance with no picture: 1 texel, clear), where pages fail to be made (the most
// there are then), and how many layers the context's arrays take (asked as it is made: gl_setup)
static _Atomic int gl_atlas_have[2] = {1, 1};
static GLuint stand_tex;
static _Atomic int gl_atlas_fail_most[2];
static _Atomic int gl_atlas_layers_most = 256;
// a ground's flat middle (bands.h mb_pieces), and where its window's size goes
static GLuint prog_flat;
static GLint uf_vp;
static GLuint prog_rect;
static GLint ur_vp = -1, ur_scale = -1, ur_top = -1;
static GLint pu_src, pu_lv1, pu_lv2, pu_lv3, pu_a, pu_b, pu_c;
static GLint adu_bd, adu_prev, adu_map, adu_uvc, adu_box, adu_param;

static int fast_rect_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("MBUI_FAST_RECT");
    enabled = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
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

// The kinds drawn by a program of their own: the shader with its kind fixed (ONE_KIND), so each is compiled to its
// kind's branch alone and a pixel of it is shaded with the registers that branch needs, not the most any kind's does
// (as Metal's: apple_render.m kind_pipe says what it saved there), what it draws the same to the byte. Rounded rects,
// glyphs, shadows, colour glyphs, images, lines, shapes' bands; what else there is (glass, a native view's picture)
// the program for any kind (progs[MB_KINDS]) draws. Each program's uniforms set as it is first used after they
// change (prog_gen); prog_now the one in use.
#define MB_KIND_LIQUID_SINGLE 13
#define MB_KINDS 14
static int has_own_prog(int kind) {
  return (kind >= 0 && kind <= 8) || kind == 10 || kind == 11 || kind == MB_KIND_LIQUID_SINGLE;
}
#ifndef MB_IMAGE_MIPS_DEFINED
static int image_mips(int id) {
  (void)id;
  return 1;
}
#endif

typedef struct {
  GLuint p;
  GLint vp, origin, scale, top, map, uvc, gb, shape, rad, n, through, glight, ad_box, ad_n, img_mips;
  unsigned gen;
} kprog;
static kprog progs[MB_KINDS + 1];
static kprog *prog_cur;
static unsigned prog_gen;
static GLuint prog_now;

static void set_img_mips(int mips) {
  if (prog_cur && prog_cur->img_mips >= 0) glUniform1i(prog_cur->img_mips, mips > 0 ? mips : 1);
}
static float *fbuf;
// the pieces of the frame's shapes (src/ui/shape.mbt: the instances of kind 12 after every one drawn), for the shader
// to read (u_segs): a float texture of SEGS_ROW fours a row (136 instances' 15), as many rows as they take
#define SEGS_ROW 2040
static GLuint segs_tex;
static int segs_rows;
// the pieces segs_tex holds (as they were uploaded): a frame whose pieces are the same (a part drawn again, a mark's
// look moving on) uploads none, the texture the GPU may still be drawing the last frame from not written again
static float *segs_had;
static size_t segs_had_n, segs_had_cap;
static size_t fcap;
static int fb_w, fb_h;
static double cur_w, cur_h, cur_scale;
// what is drawn into: the surface (0), or mbui-render's offscreen framebuffer (mb_render_init_offscreen)
static GLuint target_fb;

static void glass_lost(void);
static void canvas_lost(void);
static void blur_layers_lost(void);
static void blur_layers_free(void);
static void gl_layers_lost(void);
static void offscreen_lost(void);

// What the app does not paint left clear, what is under the surface showing (Android's over another app: OverActivity,
// Native.overOther).
static volatile int over_other;

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    LOG("shader: %s", log);
  }
  return s;
}

// The program that draws only instances of `kind` (-1 any) with the vertex shader `vs`, its textures' units set.
static kprog make_prog(GLuint vs, int kind) {
  char head[48];
  snprintf(head, sizeof head, "#version 300 es\n#define ONE_KIND %d\n", kind);
  const char *srcs[2] = {head, FS + strlen("#version 300 es\n")};
  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 2, srcs, NULL);
  glCompileShader(fs);
  GLint ok = 0;
  glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(fs, sizeof log, NULL, log);
    LOG("shader (kind %d): %s", kind, log);
  }
  kprog k = {glCreateProgram()};
  glAttachShader(k.p, vs);
  glAttachShader(k.p, fs);
  glLinkProgram(k.p);
  glDeleteShader(fs);
  k.vp = glGetUniformLocation(k.p, "u_vp");
  k.origin = glGetUniformLocation(k.p, "u_origin");
  k.scale = glGetUniformLocation(k.p, "u_scale");
  k.top = glGetUniformLocation(k.p, "u_top");
  k.map = glGetUniformLocation(k.p, "u_map");
  k.uvc = glGetUniformLocation(k.p, "u_uvc");
  k.gb = glGetUniformLocation(k.p, "u_gb");
  k.shape = glGetUniformLocation(k.p, "u_shape");
  k.rad = glGetUniformLocation(k.p, "u_rad");
  k.n = glGetUniformLocation(k.p, "u_n");
  k.through = glGetUniformLocation(k.p, "u_through");
  k.glight = glGetUniformLocation(k.p, "u_glight");
  k.ad_box = glGetUniformLocation(k.p, "u_ad_box");
  k.ad_n = glGetUniformLocation(k.p, "u_ad_n");
  k.img_mips = glGetUniformLocation(k.p, "u_img_mips");
  glUseProgram(k.p);
  GLint loc;
  if ((loc = glGetUniformLocation(k.p, "u_atlas")) >= 0) glUniform1i(loc, 0);
  if ((loc = glGetUniformLocation(k.p, "u_img")) >= 0) glUniform1i(loc, 1);
  if ((loc = glGetUniformLocation(k.p, "u_bd")) >= 0) glUniform1i(loc, 2);
  if ((loc = glGetUniformLocation(k.p, "u_segs")) >= 0) glUniform1i(loc, 3);
  if ((loc = glGetUniformLocation(k.p, "u_catlas")) >= 0) glUniform1i(loc, 4);
  if ((loc = glGetUniformLocation(k.p, "u_ad")) >= 0) glUniform1i(loc, 5);
  if (k.img_mips >= 0) glUniform1i(k.img_mips, 1);
  return k;
}

// An atlas of `pages` pages (`colour`: colour glyphs', RGBA, MB_ATLAS_COLOR square; else the words', a byte a texel,
// MB_ATLAS square): a texture array however many, as the shader takes it.
static GLuint gl_atlas_make(int colour, int pages) {
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D_ARRAY, t);
  int side = colour ? MB_ATLAS_COLOR : MB_ATLAS;
  glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, colour ? GL_RGBA8 : GL_R8, side, side, pages > 0 ? pages : 1);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return t;
}

// The most pages an atlas may have: atlas_pages.h's, as many as the context's arrays take, as many as were made where
// one more failed to be.
static int gl_atlas_most(int colour) {
  int device_most = gl_atlas_layers_most;
  int n = colour ? mb_atlas_pages_cap(MB_ATLAS_COLOR, 4, device_most) : mb_atlas_pages_cap(MB_ATLAS, 1, device_most);
  int fail = gl_atlas_fail_most[colour ? 1 : 0];
  return fail > 0 && fail < n ? fail : n;
}

// The atlas `colour` made `n` pages (the GL thread's, its context current): an array of n made, the pages both have
// copied into it on the GPU (each read through a framebuffer, glCopyTexSubImage3D: OpenGL ES 3.0's own), put in the
// old one's place. 0 where it cannot be made: the atlas as it was, and no more pages than it has from now on.
static int gl_atlas_pages(int colour, int n) {
  int c = colour ? 1 : 0;
  GLuint *tex = c ? &catlas : &atlas;
  if (n == gl_atlas_have[c]) return 1;
  if (n < 1) return 0;
  while (glGetError() != GL_NO_ERROR) {
  }
  GLuint t = gl_atlas_make(c, n);
  if (glGetError() != GL_NO_ERROR) {
    glDeleteTextures(1, &t);
    gl_atlas_fail_most[c] = gl_atlas_have[c];
    return 0;
  }
  int keep = n < gl_atlas_have[c] ? n : gl_atlas_have[c];
  int side = c ? MB_ATLAS_COLOR : MB_ATLAS;
  if (keep > 0 && *tex) {
    GLint read_was = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_was);
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, t);
    for (int i = 0; i < keep; i++) {
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, *tex, 0, i);
      glCopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, 0, 0, side, side);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_was);
    glDeleteFramebuffers(1, &fb);
  }
  if (glGetError() != GL_NO_ERROR) {
    glDeleteTextures(1, &t);
    gl_atlas_fail_most[c] = gl_atlas_have[c];
    return 0;
  }
  if (*tex) glDeleteTextures(1, tex);
  *tex = t;
  gl_atlas_have[c] = n;
  return 1;
}

// A glyph's pixels into its atlas's page (y: page * side + where on it), the texture bound to GL_TEXTURE_2D_ARRAY on
// the unit active; none past the pages the atlas has.
static void gl_atlas_sub(int colour, int x, int y, int w, int h, const void *px) {
  int c = colour ? 1 : 0, side = c ? MB_ATLAS_COLOR : MB_ATLAS;
  int page = y >= 0 ? y / side : -1, ly = y - page * side;
  if (page < 0 || page >= gl_atlas_have[c] || x < 0 || x + w > side || ly + h > side) return;
  glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, x, ly, page, w, h, 1, c ? GL_RGBA : GL_RED, GL_UNSIGNED_BYTE, px);
}

// The programs, the instances' buffer and the glyphs' atlases made in the context now current (a new one has none of
// the old: the host makes again what else it had in it).
static void gl_setup(void) {
  GLuint vs = compile(GL_VERTEX_SHADER, VS);
  for (int k = 0; k <= MB_KINDS; k++)
    progs[k] = k == MB_KINDS || has_own_prog(k) ? make_prog(vs, k == MB_KINDS ? -1 : k) : (kprog){0};
  prog_gen++;
  prog_flat = glCreateProgram();
  glAttachShader(prog_flat, vs);
  glAttachShader(prog_flat, compile(GL_FRAGMENT_SHADER, FS_FLAT));
  glLinkProgram(prog_flat);
  uf_vp = glGetUniformLocation(prog_flat, "u_vp");
  GLuint rfs = compile(GL_FRAGMENT_SHADER, FS_RECT);
  prog_rect = glCreateProgram();
  glAttachShader(prog_rect, vs);
  glAttachShader(prog_rect, rfs);
  glLinkProgram(prog_rect);
  glDeleteShader(rfs);
  GLint rok = 0;
  glGetProgramiv(prog_rect, GL_LINK_STATUS, &rok);
  if (rok) {
    ur_vp = glGetUniformLocation(prog_rect, "u_vp");
    ur_scale = glGetUniformLocation(prog_rect, "u_scale");
    ur_top = glGetUniformLocation(prog_rect, "u_top");
  } else {
    glDeleteProgram(prog_rect);
    prog_rect = 0;
    ur_vp = ur_scale = ur_top = -1;
  }
  glDeleteShader(vs);
  segs_tex = 0;
  segs_rows = 0;
  segs_had_n = 0;
  GLuint pvs = compile(GL_VERTEX_SHADER, PVS);
  post = glCreateProgram();
  glAttachShader(post, pvs);
  glAttachShader(post, compile(GL_FRAGMENT_SHADER, PFS));
  glLinkProgram(post);
  pu_src = glGetUniformLocation(post, "u_src");
  pu_lv1 = glGetUniformLocation(post, "u_lv1");
  pu_lv2 = glGetUniformLocation(post, "u_lv2");
  pu_lv3 = glGetUniformLocation(post, "u_lv3");
  pu_a = glGetUniformLocation(post, "u_a");
  pu_b = glGetUniformLocation(post, "u_b");
  pu_c = glGetUniformLocation(post, "u_c");
  ad_prog = glCreateProgram();
  glAttachShader(ad_prog, pvs);
  glAttachShader(ad_prog, compile(GL_FRAGMENT_SHADER, AD_FS));
  glLinkProgram(ad_prog);
  glDeleteShader(pvs);
  adu_bd = glGetUniformLocation(ad_prog, "u_bd");
  adu_prev = glGetUniformLocation(ad_prog, "u_prev");
  adu_map = glGetUniformLocation(ad_prog, "u_map");
  adu_uvc = glGetUniformLocation(ad_prog, "u_uvc");
  adu_box = glGetUniformLocation(ad_prog, "u_box");
  adu_param = glGetUniformLocation(ad_prog, "u_param");
  glass_lost();
  canvas_lost();
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  for (int i = 0; i < MB_INST / 4; i++) {
    glEnableVertexAttribArray(i);
    glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, MB_INST * 4, (void *)(intptr_t)(i * 16));
    glVertexAttribDivisor(i, 1);
  }
  // the words' atlas a byte a texel, colour glyphs' four (MB_ATLAS above): a context made anew has them of one page
  GLint layers = 0;
  glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &layers);
  gl_atlas_layers_most = layers > 0 ? layers : 256;
  atlas = gl_atlas_make(0, 1);
  catlas = gl_atlas_make(1, 1);
  gl_atlas_have[0] = gl_atlas_have[1] = 1;
  gl_atlas_fail_most[0] = gl_atlas_fail_most[1] = 0;
  static const uint32_t clear_texel = 0;
  glGenTextures(1, &stand_tex);
  glBindTexture(GL_TEXTURE_2D, stand_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &clear_texel);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

// ---------------------------------------------------------------------------
// Glass (src/ui/glass.mbt): when the instances reach a run of glass (slot 20 negative: the capture it shares), what
// is drawn so far under it (its rects with room for the blur round them) is copied out of the surface, halved down
// to where the blur is a few texels wide (dual Kawase's downsample, at most 1/8), blurred there by a Gaussian of what
// deviation is left, and drawn through. Level j is 1/2^j of the largest capture yet (the glass and the blur's margin
// round it, not the surface: a composer's strip, not the screen), [4] a Gaussian's first half. Level 0, the
// copy, has 8 bits a channel as the surface has; what the passes write keeps 10 (RGB10_A2, in as many bytes): at 8 each
// pass rounds the frost's slow slopes into flat steps, a texel of the last level 8 pixels wide on the screen (a blur
// under 4 pixels, too narrow to halve, is written back into the copy: it leaves no slope that wide).

typedef struct { GLuint t, fb; int w, h; GLenum format; } level;
static level lv[5];
static int lv_w, lv_h;
static level lens_lv[2];
static int lens_w, lens_h, cur_lens_ph;
static float cur_ox, cur_oy, cur_vpw, cur_vph;
static int lv_shrink_frames, lv_win_max_w, lv_win_max_h;
static int lv_frame_max_w, lv_frame_max_h;
static int levels_rebuilt;
static GLuint bd_tex;
static float gmap[4], guvc[4], ggb[4], gshape[32], grad[32];
static int gn;
static float gthrough;
// the app's part of the surface (MBView: the soft keyboard over the rest), in its pixels from the foot
static int vy0, vhp;

// the last capture's blur is kept to be shown again (capture, below)
static int kept_ok;
static void slots_invalidate(void);
static void slots_drop_textures(void);
static void glass_forget(void);

static level level_tex(GLenum format, int w, int h) {
  level l = {0, 0, w > 0 ? w : 1, h > 0 ? h : 1, format};
  glGenTextures(1, &l.t);
  glBindTexture(GL_TEXTURE_2D, l.t);
  glTexStorage2D(GL_TEXTURE_2D, 1, format, l.w, l.h);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &l.fb);
  glBindFramebuffer(GL_FRAMEBUFFER, l.fb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l.t, 0);
  return l;
}

static void ensure_levels(int w, int h) {
  if (lv[0].t && lv_w >= w && lv_h >= h) return;
  for (int j = 0; j < 5; j++)
    if (lv[j].t) {
      glDeleteTextures(1, &lv[j].t);
      glDeleteFramebuffers(1, &lv[j].fb);
    }
  int nw = (w + 255) & ~255;
  int nh = (h + 255) & ~255;
  if (fb_w > 0 && nw > fb_w) nw = fb_w;
  if (fb_h > 0 && nh > fb_h) nh = fb_h;
  if (nw < w) nw = w;
  if (nh < h) nh = h;
  lv_w = nw > lv_w ? nw : lv_w;
  lv_h = nh > lv_h ? nh : lv_h;
  levels_rebuilt++;
  lv_shrink_frames = 0;
  lv_win_max_w = lv_win_max_h = 0;
  lv_frame_max_w = lv_frame_max_h = 0;
  // Each cached slot owns its own texture (`bt` + `bw`/`bh`) and `show_blur` reads `bw`/`bh` directly,
  // so growing the shared scratch pyramid must not invalidate cached glass slots.
  for (int j = 0; j < 4; j++)
    lv[j] = level_tex(j == 0 ? GL_RGBA8 : GL_RGB10_A2, (lv_w + (1 << j) - 1) >> j, (lv_h + (1 << j) - 1) >> j);
  lv[4] = level_tex(GL_RGB10_A2, lv_w, lv_h);
  glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
}

static void ensure_lens_levels(int w, int h) {
  if (lens_lv[0].t && lens_w >= w && lens_h >= h) return;
  for (int j = 0; j < 2; j++)
    if (lens_lv[j].t) {
      glDeleteTextures(1, &lens_lv[j].t);
      glDeleteFramebuffers(1, &lens_lv[j].fb);
    }
  lens_w = w > lens_w ? w : lens_w;
  lens_h = h > lens_h ? h : lens_h;
  for (int j = 0; j < 2; j++) lens_lv[j] = level_tex(GL_RGBA8, lens_w, lens_h);
  glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
}

static void post_pass(level *src, int sw, int sh, level *dst, int w, int h, int mode, float sg, float dx, float dy) {
  glBindFramebuffer(GL_FRAMEBUFFER, dst->fb);
#ifdef GL_ES_VERSION_3_0
  {
    const GLenum att = GL_COLOR_ATTACHMENT0;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &att);
  }
#endif
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src->t);
  glUniform1i(pu_src, 0);
  glUniform4f(pu_a, 1.0f / src->w, 1.0f / src->h, (float)mode, sg);
  glUniform4f(pu_b, (sw - 0.5f) / src->w, (sh - 0.5f) / src->h, dx, dy);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

// The post program's other units (u_lv1..3) let go of the levels: a level still bound to an active sampler while a
// later pass draws into it is a feedback loop (GL_INVALID_OPERATION on WebGL2 and strict GLES 3, the draw dropped).
static void unbind_post_extra(void) {
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
}

// Partial redraw: the surface's frame is drawn into a canvas of its own (canvas_fb), kept
// across frames, and copied onto the surface (what the surface held is gone once shown); where the app says only some
// parts differ from the frame before (@hostapi.render_dirty), only those parts of the canvas are drawn again, each cut
// to it (the scissor, `cut`: x, y from the foot, w, h; while `cut_on`), the frame's ground over it first (the instance
// after the frame's, gl_present's), then every instance. Without the surface the offscreen framebuffer is the canvas.
static GLuint canvas_fb, canvas_tex;
static int canvas_w, canvas_h, offscreen;
// what the canvas (or the offscreen framebuffer) holds is the last frame given
static int canvas_ok;
// the parts of the next frame that differ (points: x, y, w, h), all of it (`dirty_whole`), and whether any was said
#define MB_PARTS 8
static double dirty[MB_PARTS][4];
static int dirty_n, dirty_whole, dirty_told;
// a picture the frame drawn last had none of yet (drawn without it): the next drawn whole
static int pictures_missing, encode_missing;
static int cut_on;
static int cut[4];

static void gl_dirty_add(double x, double y, double w, double h) {
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

static volatile float g_light_angle = 0.0f;
static void gl_set_glass_light_angle(double rad) { g_light_angle = (float)rad; }

#ifndef MB_HOST_THREADS
void mb_host_render_dirty(double x, double y, double w, double h) { gl_dirty_add(x, y, w, h); }
void mb_host_glass_light_angle(double rad) { gl_set_glass_light_angle(rad); }
// (each frame drawn as it is made, on the app's thread: none dropped. A host whose app runs on another thread counts
// those a newer one took the place of: android_host.c)
int32_t mb_host_frames_dropped(void) { return 0; }
#endif

// The parts of the frame the next one draws, from what was said of them (taken: a frame nothing was said for is drawn
// whole), into `r` (as `cut`): how many (0 none), -1 all of it. `kept`: the target holds the frame before. In the
// surface's pixels, the app's top the surface's.
static int take_parts(int kept, int r[MB_PARTS][4]) {
  int told = dirty_told, whole = dirty_whole, n = dirty_n;
  dirty_told = dirty_whole = dirty_n = 0;
  if (!told || whole || !kept || pictures_missing) return -1;
  int k = 0;
  for (int i = 0; i < n; i++) {
    // (whole pixels round it, and one more)
    int x0 = (int)floor(dirty[i][0] * cur_scale) - 1, y0 = (int)floor(dirty[i][1] * cur_scale) - 1;
    int x1 = (int)ceil((dirty[i][0] + dirty[i][2]) * cur_scale) + 1;
    int y1 = (int)ceil((dirty[i][1] + dirty[i][3]) * cur_scale) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_w) x1 = fb_w;
    if (y1 > fb_h) y1 = fb_h;
    if (x1 <= x0 || y1 <= y0) continue;
    r[k][0] = x0;
    r[k][1] = fb_h - y1;
    r[k][2] = x1 - x0;
    r[k][3] = y1 - y0;
    k++;
  }
  return k;
}

static void draw_into(GLuint fb, int part);

// The frame kept drawn into `fb`, only its parts that differ (take_parts) where it holds the frame before (`kept`),
// else whole.
static void draw_parts(GLuint fb, int kept) {
  int parts[MB_PARTS][4];
  int n = take_parts(kept, parts);
  int missing = 0;
  if (n < 0) {
    draw_into(fb, 0);
    missing = encode_missing;
  }
  for (int i = 0; i < n; i++) {
    memcpy(cut, parts[i], sizeof cut);
    draw_into(fb, 1);
    missing |= encode_missing;
  }
  if (n != 0) pictures_missing = missing;
}

// Offscreen blur layers (kind == 14 PUSH_BLUR_LAYER / kind == 15 POP_BLUR_LAYER): lazily allocated
// GL_RGBA8 layer targets and an RGBA8 blur pyramid (full 8-bit alpha for smooth premultiplied edges).
#define MB_BLUR_LAYERS 4
static level bl_lv[MB_BLUR_LAYERS];
static int bl_w, bl_h;
static level bl_py[5];
static int bl_py_w, bl_py_h;

static void blur_layers_lost(void) {
  memset(bl_lv, 0, sizeof bl_lv);
  bl_w = bl_h = 0;
  memset(bl_py, 0, sizeof bl_py);
  bl_py_w = bl_py_h = 0;
}

static void blur_layers_free(void) {
  for (int j = 0; j < MB_BLUR_LAYERS; j++)
    if (bl_lv[j].t) {
      glDeleteTextures(1, &bl_lv[j].t);
      glDeleteFramebuffers(1, &bl_lv[j].fb);
    }
  for (int j = 0; j < 5; j++)
    if (bl_py[j].t) {
      glDeleteTextures(1, &bl_py[j].t);
      glDeleteFramebuffers(1, &bl_py[j].fb);
    }
  blur_layers_lost();
}

static int ensure_blur_layer(int depth, int tw, int th) {
  if (depth < 0 || depth >= MB_BLUR_LAYERS || tw <= 0 || th <= 0) return 0;
  if (bl_w != tw || bl_h != th) {
    for (int j = 0; j < MB_BLUR_LAYERS; j++)
      if (bl_lv[j].t) {
        glDeleteTextures(1, &bl_lv[j].t);
        glDeleteFramebuffers(1, &bl_lv[j].fb);
      }
    memset(bl_lv, 0, sizeof bl_lv);
    bl_w = tw;
    bl_h = th;
  }
  if (!bl_lv[depth].t) {
    bl_lv[depth] = level_tex(GL_RGBA8, tw, th);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      glDeleteTextures(1, &bl_lv[depth].t);
      glDeleteFramebuffers(1, &bl_lv[depth].fb);
      bl_lv[depth] = (level){0};
    }
  }
  return bl_lv[depth].t != 0 && bl_lv[depth].fb != 0;
}

static int ensure_blur_py(int w, int h) {
  if (w <= 0 || h <= 0) return 0;
  if (bl_py[0].t && bl_py_w >= w && bl_py_h >= h) return 1;
  for (int j = 0; j < 5; j++)
    if (bl_py[j].t) {
      glDeleteTextures(1, &bl_py[j].t);
      glDeleteFramebuffers(1, &bl_py[j].fb);
    }
  memset(bl_py, 0, sizeof bl_py);
  bl_py_w = w > bl_py_w ? w : bl_py_w;
  bl_py_h = h > bl_py_h ? h : bl_py_h;
  for (int j = 0; j < 4; j++)
    bl_py[j] = level_tex(GL_RGBA8, (bl_py_w + (1 << j) - 1) >> j, (bl_py_h + (1 << j) - 1) >> j);
  bl_py[4] = level_tex(GL_RGBA8, bl_py_w, bl_py_h);
  return bl_py[0].t != 0 && bl_py[4].t != 0;
}

// The canvas gone with the context it was made in (a new one: gl_setup), nothing of it given back.
static void canvas_lost(void) {
  canvas_fb = canvas_tex = 0;
  canvas_w = canvas_h = 0;
  canvas_ok = 0;
  blur_layers_lost();
  gl_layers_lost();
  offscreen_lost();
}

// The canvas the surface's frames are drawn into, its size: made again at another size (or once the context is new:
// gl_setup), what it held lost.
static void ensure_canvas(void) {
  if (canvas_fb && canvas_w == fb_w && canvas_h == fb_h) return;
  if (canvas_fb) {
    glDeleteFramebuffers(1, &canvas_fb);
    glDeleteTextures(1, &canvas_tex);
  }
  canvas_fb = canvas_tex = 0;
  canvas_ok = 0;
  canvas_w = fb_w;
  canvas_h = fb_h;
  if (fb_w <= 0 || fb_h <= 0) return;
  glGenTextures(1, &canvas_tex);
  glBindTexture(GL_TEXTURE_2D, canvas_tex);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, fb_w, fb_h);
  glGenFramebuffers(1, &canvas_fb);
  glBindFramebuffer(GL_FRAMEBUFFER, canvas_fb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, canvas_tex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    glDeleteFramebuffers(1, &canvas_fb);
    glDeleteTextures(1, &canvas_tex);
    canvas_fb = canvas_tex = 0;
  }
}

// What a capture of glass is of, worked out before any of the GPU's work: its rectangle in the surface's pixels from its
// foot (px0, pb0, w × h; the app's top, `top`), how many times it is halved (k, at most 3) and the Gaussian left after
// (sg), whether its edge is contained, and the glass's own numbers (its bounds, liquid's shapes). Zeroed whole first,
// so two of the same compare the same (as Metal's: apple_render.m Grab).
typedef struct {
  int px0, pb0, w, h, top, k, contained, progressive, dual_scale, n, through;
  int vis_x, vis_y, vis_w, vis_h;
  float sg, sp, dual_w, dual_wsp;
  float fade[4], fadex[4];
  float gb[4], shape[32], rad[32];
  MBContained edge;
} Grab;

// The capture the run [start, end) of glass makes, frosted glass of the same capture further on (glass.mbt
// shares_capture) under it too; 0 when none of it shows, or it is outside the part drawn.
static int grab_of(Grab *m, const float *f, int32_t start, int32_t end, int32_t count) {
  memset(m, 0, sizeof *m);
  const float *a = f + start * MB_INST;
  // For frosted glass (kind 5), negative slot 7 is contained; for liquid glass (kind 6), negative slot 7 encodes clear-lens zoom.
  int kind = (int)(a[5] + 0.5f);
  int is_contained = (kind == 5 && a[7] < 0.0f);
  int is_progressive = (kind == 5 && a[22] < -0.5f);
  float raw_blur = (kind == 6 && a[7] < 0.0f) ? 0.0f : fabsf(a[7]);
  int is_clear_liq = (kind == 6 && a[7] >= -0.001f && a[7] <= 0.1f && a[11] < 0.12f);
  float col_ch = fmaxf(fmaxf(a[8], a[9]), a[10]) - fminf(fminf(a[8], a[9]), a[10]);
  float blur = (is_clear_liq && a[11] > 0.01f && raw_blur < 0.1f) ? 1.2f : raw_blur;
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
  float min_dim = fminf(a[2], a[3]);
  float dual_sz = fminf(fmaxf((min_dim - 16.0f) / 12.0f, 0.0f), 1.0f);
  dual_sz = dual_sz * dual_sz * (3.0f - 2.0f * dual_sz);
  float tall_t = fminf(fmaxf((min_dim - 44.0f) / 18.0f, 0.0f), 1.0f);
  tall_t = tall_t * tall_t * (3.0f - 2.0f * tall_t);
  float dual_base = 0.68f + 0.18f * tall_t;
  float dual_w = (kind == 6 && !is_clear_liq) ? (dual_base * dual_b * dual_hi * dual_sz * g_lo * fmaxf(g_neu, g_ch * g_ta)) : 0.0f;
  int is_dual = (dual_w > 0.005f);
  int merged_liquid = (kind == 6 && a[23] > 0.0f);
  int unmerged_liquid = (kind == 6 && !merged_liquid);
  float clear_refract_scale = (kind == 6 && fabsf(a[7]) < 0.01f && a[11] > 0.005f && a[11] < 0.31f) ? 2.20f : 1.0f;
  float margin = (is_contained || is_progressive) ? 0.0f : 3.0f * blur + (kind == 6 ? a[21] * clear_refract_scale * (1.0f + fabsf(a[22])) + fmaxf(a[23], 0.0f) : 0.0f) + 2.0f;
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (int32_t i = start; i < end; i++) {
    const float *b = f + i * MB_INST;
    float bx0 = b[0] > b[16] ? b[0] : b[16], by0 = b[1] > b[17] ? b[1] : b[17];
    float bx1 = b[0] + b[2] < b[18] ? b[0] + b[2] : b[18], by1 = b[1] + b[3] < b[19] ? b[1] + b[3] : b[19];
    if (kind == 6) {
      float m23 = fmaxf(unmerged_liquid ? b[23] : a[23], 0.0f);
      bx0 = b[0] - m23; by0 = b[1] - m23; bx1 = b[0] + b[2] + m23; by1 = b[1] + b[3] + m23;
      if (merged_liquid && m->n < 8) {
        for (int k = 0; k < 4; k++) m->shape[m->n * 4 + k] = b[k];
        if (fmaxf(fmaxf(b[36], b[37]), fmaxf(b[38], b[39])) > 0.0f) {
          for (int k = 0; k < 4; k++) m->rad[m->n * 4 + k] = b[36 + k];
        } else {
          for (int k = 0; k < 4; k++) m->rad[m->n * 4 + k] = b[4];
        }
        m->n++;
      }
      if (unmerged_liquid) {
        float bm = 3.0f * blur + b[21] * clear_refract_scale * (1.0f + fabsf(b[22])) + m23 + 2.0f;
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
        float bm = 3.0f * blur + b[21] * clear_refract_scale * (1.0f + fabsf(b[22])) + m23 + 2.0f;
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
  m->gb[0] = x0 - 2; m->gb[1] = y0 - 2; m->gb[2] = x1 + 2; m->gb[3] = y1 + 2;
  float sc = (float)cur_scale;
  // in the surface's pixels, from its foot (GL's way up)
  int top = vy0 + vhp;
  int px0 = (int)floorf((x0 - margin) * sc), px1 = (int)ceilf((x1 + margin) * sc);
  int pb0 = top - (int)ceilf((y1 + margin) * sc), pb1 = top - (int)floorf((y0 - margin) * sc);
  if (px0 < 0) px0 = 0;
  if (px1 > fb_w) px1 = fb_w;
  if (pb0 < vy0) pb0 = vy0;
  if (pb1 > top) pb1 = top;
  if (px1 <= px0 || pb1 <= pb0 || x1 <= x0 || y1 <= y0) return 0;
  // (a capture outside the part drawn: neither copied nor drawn. One that meets it is in it whole: @damage)
  if (cut_on && (px1 <= cut[0] || cut[0] + cut[2] <= px0 || pb1 <= cut[1] || cut[1] + cut[3] <= pb0)) return 0;
  m->px0 = px0; m->pb0 = pb0; m->w = px1 - px0; m->h = pb1 - pb0; m->top = top;
#ifdef MB_VIEWS_UNDER
  if (kind == 6) {
    float gx0 = x0 - margin, gy0 = y0 - margin, gx1 = x1 + margin, gy1 = y1 + margin;
    for (int32_t i = 0; i < start; i++) {
      const float *b = f + i * MB_INST;
      if ((int)(b[5] + 0.5f) != 9) continue;
      int btid = (int)b[20];
      if (btid <= MB_VIEW_TEX || btid > MB_LAYER_TEX || !view_under(btid)) continue;
      float hx0 = b[0] > b[16] ? b[0] : b[16], hy0 = b[1] > b[17] ? b[1] : b[17];
      float hx1 = b[0] + b[2] < b[18] ? b[0] + b[2] : b[18], hy1 = b[1] + b[3] < b[19] ? b[1] + b[3] : b[19];
      if (hx0 < hx1 && hy0 < hy1 && hx0 < gx1 && gx0 < hx1 && hy0 < gy1 && gy0 < hy1) {
        m->through = 1;
        break;
      }
    }
  }
#endif
  m->contained = is_contained ? mb_contained_make(&m->edge, a, px0 / sc, (top - pb0) / sc, 1.0f / sc, -1.0f / sc) : 0;
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
    int vy0_v = (top - (int)ceilf(y1 * sc)) - pb0 - 2;
    int vx1 = (int)ceilf(x1 * sc) - px0 + 2;
    int vy1_v = (top - (int)floorf(y0 * sc)) - pb0 + 2;
    if (vx0 < 0) vx0 = 0;
    if (vy0_v < 0) vy0_v = 0;
    if (vx1 > m->w) vx1 = m->w;
    if (vy1_v > m->h) vy1_v = m->h;
    if (vx1 <= vx0) { vx0 = 0; vx1 = m->w; }
    if (vy1_v <= vy0_v) { vy0_v = 0; vy1_v = m->h; }
    m->vis_x = vx0; m->vis_y = vy0_v; m->vis_w = vx1 - vx0; m->vis_h = vy1_v - vy0_v;
  }
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

// The size of a grab's level j (its passes write each level's foot left)
static void grab_levels(const Grab *m, int lw[4], int lh[4]) {
  lw[0] = m->w; lh[0] = m->h;
  for (int j = 1; j <= 3; j++) {
    lw[j] = (lw[j - 1] + 1) / 2;
    lh[j] = (lh[j - 1] + 1) / 2;
  }
}

static void grab_map_uvc(const Grab *m, int bw, int bh, float map[4], float uvc[4]) {
  int lw[4], lh[4];
  grab_levels(m, lw, lh);
  int sk = m->progressive ? 0 : m->k, px0 = m->px0, pb0 = m->pb0, top = m->top;
  float sc = (float)cur_scale;
  float d = (float)(1 << sk);
  map[0] = sc / d / bw; map[1] = -sc / d / bh;
  map[2] = -(float)px0 / d / bw; map[3] = (float)(top - pb0) / d / bh;
  if (m->progressive) {
    uvc[0] = ((float)m->vis_x + 0.5f) / bw; uvc[1] = ((float)m->vis_y + 0.5f) / bh;
    uvc[2] = ((float)(m->vis_x + m->vis_w) - 0.5f) / bw; uvc[3] = ((float)(m->vis_y + m->vis_h) - 0.5f) / bh;
  } else {
    uvc[0] = 0.5f / bw; uvc[1] = 0.5f / bh; uvc[2] = (lw[sk] - 0.5f) / bw; uvc[3] = (lh[sk] - 0.5f) / bh;
  }
}

// What the glass shows of a grab's blur in texture `bt` (bw × bh, level k's place in it): bd_tex and the uniforms
// that say where in it it is.
static void show_blur(const Grab *m, GLuint bt, int bw, int bh) {
  memcpy(ggb, m->gb, sizeof ggb);
  memcpy(gshape, m->shape, sizeof gshape);
  memcpy(grad, m->rad, sizeof grad);
  gn = m->n;
  gthrough = m->through ? 1.0f : 0.0f;
  bd_tex = bt;
  grab_map_uvc(m, bw, bh, gmap, guvc);
}

// The level a grab's blur is left in.
static level *grab_blurred(const Grab *m) {
  float dual_cut = m->k > 0 ? 0.55f : 0.3f;
  return (m->progressive || (m->dual_scale && m->sg <= dual_cut) || (m->contained && m->k == 0)) ? &lv[4] : &lv[m->k];
}

// A grab of target_fb blurred into the levels, with the final pass written directly into `out` (the chosen cache
// slot's framebuffer, eliminating the post-blur slot_fill blit).
// (how many: packages/host/tests/gles_render_test.c, a blur shown again as kept is none)
static int blurs_made;

static void blur_into(const Grab *m, level *out) {
  blurs_made++;
  int w = m->w, h = m->h, k = m->k, contained = m->contained;
  ensure_levels(w, h);
  glDisable(GL_BLEND);
  // (the copy and the passes whole, not cut to the part: a blit is cut by the scissor too)
  glDisable(GL_SCISSOR_TEST);
  int raw_copy = !m->dual_scale && !m->progressive && !contained && k == 0 && m->sg <= 0.3f;
  glBindFramebuffer(GL_READ_FRAMEBUFFER, target_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, raw_copy ? out->fb : lv[0].fb);
#ifdef GL_ES_VERSION_3_0
  {
    const GLenum att = GL_COLOR_ATTACHMENT0;
    glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &att);
  }
#endif
  glBlitFramebuffer(m->px0, m->pb0, m->px0 + w, m->pb0 + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  if (raw_copy) return;
  glUseProgram(post);
  unbind_post_extra();
  int lw[4], lh[4];
  grab_levels(m, lw, lh);
  if (m->dual_scale) {
    for (int j = 1; j <= 3; j++) {
      post_pass(&lv[j - 1], lw[j - 1], lh[j - 1], &lv[j], lw[j], lh[j], 1, 0, 0, 0);
    }
    float wsp = (m->dual_wsp > 0.0f ? m->dual_wsp : 14.0f) * (float)cur_scale;
    float wsk = wsp / 8.0f;
    float wpre = 0.75f * (1.0f - 1.0f / 64.0f) / 3.0f + 1.0f / 6.0f;
    float wsg = sqrtf(fmaxf(wsk * wsk - wpre, 0.0f));
    if (wsg > 0.3f) {
      post_pass(&lv[3], lw[3], lh[3], &lv[4], lw[3], lh[3], 6, wsg, 1, 0);
      post_pass(&lv[4], lw[3], lh[3], &lv[3], lw[3], lh[3], 6, wsg, 0, 1);
    }
    float dual[36] = {0};
    float d3 = (float)(1 << (3 - k));
    float tw3 = (float)lv[3].w, th3 = (float)lv[3].h;
    dual[16] = ((float)lw[3] - 0.5f) / tw3;
    dual[17] = ((float)lh[3] - 0.5f) / th3;
    dual[18] = 1.0f - m->dual_w;
    dual[19] = k > 0 ? 0.277f : 0.0f;
    dual[20] = 1.0f / (d3 * tw3); dual[21] = 1.0f / (d3 * th3);
    dual[22] = 1.0f / tw3;        dual[23] = 1.0f / th3;
    float dual_cut = k > 0 ? 0.55f : 0.3f;
    if (m->sg > dual_cut) {
      post_pass(&lv[k], lw[k], lh[k], &lv[4], lw[k], lh[k], 2, m->sg, 1, 0);
      glUniform4fv(pu_c, 9, dual);
      glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, lv[3].t); glUniform1i(pu_lv3, 3);
      post_pass(&lv[4], lw[k], lh[k], out, lw[k], lh[k], 5, m->sg, 0, 1);
    } else {
      glUniform4fv(pu_c, 9, dual);
      glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, lv[3].t); glUniform1i(pu_lv3, 3);
      post_pass(&lv[k], lw[k], lh[k], out, lw[k], lh[k], 5, 0.0f, 0, 0);
    }
    unbind_post_extra();
    return;
  }
  if (m->progressive) {
    float prev_var = 0.0f;
    for (int j = 1; j <= k; j++) {
      post_pass(&lv[j - 1], lw[j - 1], lh[j - 1], &lv[j], lw[j], lh[j], 1, 0, 0, 0);
      float target_px = powf(fmaxf(m->sp, 1.0f), (float)j / (float)k);
      float have_var = prev_var + 0.75f * (float)(1 << (2 * j - 2));
      float need_var = fmaxf(target_px * target_px - have_var, 0.0f);
      float sg_j = sqrtf(need_var) / (float)(1 << j);
      if (sg_j > 0.3f) {
        post_pass(&lv[j], lw[j], lh[j], &lv[4], lw[j], lh[j], 2, sg_j, 1, 0);
        post_pass(&lv[4], lw[j], lh[j], &lv[j], lw[j], lh[j], 2, sg_j, 0, 1);
      }
      prev_var = fmaxf(have_var, target_px * target_px);
    }
    float sc = (float)cur_scale;
    float prog[36] = {0};
    prog[0] = (float)m->px0 / sc; prog[1] = (float)(m->top - m->pb0) / sc;
    prog[2] = 1.0f / sc; prog[3] = -1.0f / sc;
    for (int i = 0; i < 4; i++) { prog[4 + i] = m->fade[i]; prog[8 + i] = m->fadex[i]; }
    prog[12] = m->sp; prog[13] = (float)k;
    for (int j = 0; j < 4; j++) {
      float dj = (float)(1 << j), tw = (float)lv[j].w, th = (float)lv[j].h;
      prog[16 + j * 4 + 0] = 1.0f / (dj * tw);
      prog[16 + j * 4 + 1] = 1.0f / (dj * th);
      prog[16 + j * 4 + 2] = ((float)lw[j] - 0.5f) / tw;
      prog[16 + j * 4 + 3] = ((float)lh[j] - 0.5f) / th;
    }
    glUniform4fv(pu_c, 9, prog);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, lv[1].t); glUniform1i(pu_lv1, 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, lv[2].t); glUniform1i(pu_lv2, 2);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, lv[3].t); glUniform1i(pu_lv3, 3);
    glEnable(GL_SCISSOR_TEST);
    glScissor(m->vis_x, m->vis_y, m->vis_w, m->vis_h);
    post_pass(&lv[0], w, h, out, w, h, 4, 0, 0, 0);
    glDisable(GL_SCISSOR_TEST);
    unbind_post_extra();
    return;
  }
  if (contained) {
    glUniform4fv(pu_c, 9, (const float *)&m->edge);
    if (k == 0 && m->sg <= 0.3f) {
      post_pass(&lv[0], w, h, out, w, h, 3, 0, 0, 0);
      return;
    }
    post_pass(&lv[0], w, h, &lv[4], w, h, 3, 0, 0, 0);
  }
  if (k > 0) {
    if (m->sg <= 0.3f) {
      for (int j = 1; j < k; j++)
        post_pass(j == 1 && contained ? &lv[4] : &lv[j - 1], lw[j - 1], lh[j - 1], &lv[j], lw[j], lh[j], 1, 0, 0, 0);
      post_pass(k == 1 && contained ? &lv[4] : &lv[k - 1], lw[k - 1], lh[k - 1], out, lw[k], lh[k], 1, 0, 0, 0);
      return;
    }
    for (int j = 1; j <= k; j++)
      post_pass(j == 1 && contained ? &lv[4] : &lv[j - 1], lw[j - 1], lh[j - 1], &lv[j], lw[j], lh[j], 1, 0, 0, 0);
    post_pass(&lv[k], lw[k], lh[k], &lv[4], lw[k], lh[k], 2, m->sg, 1, 0);
    post_pass(&lv[4], lw[k], lh[k], out, lw[k], lh[k], 2, m->sg, 0, 1);
  } else {
    level *from = contained ? &lv[4] : &lv[0];
    level *tmp = contained ? &lv[0] : &lv[4];
    post_pass(from, lw[0], lh[0], tmp, lw[0], lh[0], 2, m->sg, 1, 0);
    post_pass(tmp, lw[0], lh[0], out, lw[0], lh[0], 2, m->sg, 0, 1);
  }
}

// The blurs kept, a slot each, as many as a screen has groups of glass at once (a bar at the top, a composer and its
// ground at the foot, a sheet's, a page sliding over another: 2 to 6), the one least lately shown taken for a new one.
// A slot holds a capture's blur written directly into a compact 64-px-aligned texture (reused across frames when
// sized cleanly), and what it was of. What a capture copies is
// what the frame drew before it in the grab's rectangle (the instances up to the glass that reach into it, the pieces
// of the frame's shapes they are drawn by, the ground the frame cleared to) of a surface of that size: a frame that
// draws all that the same under the same glass (a caret blinking in a composer, a spinner turning, a button lit under
// the hand, over a list at rest) shows the blur as it is, with no copy and no passes, the framebuffer drawn into not
// left for them (apple_render.m's slots, Metal's); one where something moved under one group of glass makes that
// group's blur again, the others' kept. A stream or a native view's picture drawn into it (its pictures change under
// the same instances) is blurred anew each frame, as is a frame an image under it had no texture for yet.
#define MB_GLASS_SLOTS 32
#ifdef __ANDROID__
#define MB_GLASS_CACHE_MAX_BYTES ((size_t)32 * 1024 * 1024)
#else
#define MB_GLASS_CACHE_MAX_BYTES ((size_t)64 * 1024 * 1024)
#endif
typedef struct {
  GLuint bt, bfb;
  int bw, bh;
  GLenum bf;
  float *under;
  size_t under_cap, under_n;
  int32_t whole, picked;
  Grab grab;
  double key[9];
  int valid;
  unsigned used;
  unsigned gen;
} glass_slot;
static glass_slot slots[MB_GLASS_SLOTS];
static unsigned slots_clock;
static unsigned slots_gen;

static size_t glass_tex_bytes(int w, int h) {
  if (w <= 0 || h <= 0) return 0;
  return (size_t)w * (size_t)h * 4;
}

static size_t slots_total_bytes(void) {
  size_t total = 0;
  for (int i = 0; i < MB_GLASS_SLOTS; i++) {
    if (slots[i].bt) total += glass_tex_bytes(slots[i].bw, slots[i].bh);
  }
  return total;
}

static void slot_drop_tex(glass_slot *s) {
  if (s->bt) {
    glDeleteFramebuffers(1, &s->bfb);
    glDeleteTextures(1, &s->bt);
    s->bt = s->bfb = 0;
    s->bw = s->bh = 0;
    s->bf = 0;
  }
}

static void slots_drop_textures(void) {
  for (int i = 0; i < MB_GLASS_SLOTS; i++) slot_drop_tex(&slots[i]);
}
// what the capture now is of (under_now), as a slot keeps it
static float *under_now;
static size_t under_now_cap, under_now_n;
static int32_t under_whole, under_picked;

#define MB_ADAPTIVE_MAX 16
#define MB_AD_TAU 0.070
#define MB_AD_SETTLE_SEC 0.42
#define MB_AD_MATCH_DIST 64.0
#define MB_AD_RB_N 3

typedef struct {
  int active;
  float cx, cy, hx, hy;
  int is_dark;
  int is_inverting;
  int through;
  float tint_a;
  double settle_left;
  uint64_t settle_serial;
  uint64_t updated_serial;
  uint64_t seen_serial;
  int has_changed;
  uint64_t last_changed_serial;
} mb_ad_slot;

typedef struct {
  GLuint pbo;
  GLsync fence;
  uint32_t mask;
  uint64_t serial;
} gl_ad_rb;

static GLuint ad_tex[2], ad_fb[2];
static int ad_cur;
static gl_ad_rb ad_rb[MB_AD_RB_N];
static int ad_rb_i;
static int ad_rb_cur = -1;
static int ad_rb_prev_copied;
static uint32_t ad_track_mask;
static uint64_t ad_conv_serial[MB_ADAPTIVE_MAX];
static mb_ad_slot ad_slots[MB_ADAPTIVE_MAX];
static double ad_last_time, ad_fixed_dt;
static uint64_t ad_frame_serial;
static int ad_settling;
// 1 = update adaptive state, 0 = read-only, -1 = stateless
static int ad_mode = 1;
static double ad_cur_dt = 0.0;
static float gad_box[MB_ADAPTIVE_MAX * 4];
static int gad_n;

static void ad_forget(void) {
  memset(ad_slots, 0, sizeof ad_slots);
  memset(ad_conv_serial, 0, sizeof ad_conv_serial);
  for (int i = 0; i < MB_AD_RB_N; i++) ad_rb[i].mask = 0;
  ad_track_mask = 0;
  ad_rb_prev_copied = 0;
  ad_rb_cur = -1;
  ad_last_time = 0.0;
  __atomic_store_n(&ad_settling, 0, __ATOMIC_RELEASE);
  gad_n = 0;
}

static void ensure_ad_tex(void) {
  if (!ad_tex[0]) {
    for (int i = 0; i < 2; i++) {
      glGenTextures(1, &ad_tex[i]);
      glBindTexture(GL_TEXTURE_2D, ad_tex[i]);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, MB_ADAPTIVE_MAX, 1);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glGenFramebuffers(1, &ad_fb[i]);
      glBindFramebuffer(GL_FRAMEBUFFER, ad_fb[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ad_tex[i], 0);
    }
    ad_cur = 0;
  }
  if (!ad_rb[0].pbo) {
    for (int i = 0; i < MB_AD_RB_N; i++) {
      glGenBuffers(1, &ad_rb[i].pbo);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, ad_rb[i].pbo);
      glBufferData(GL_PIXEL_PACK_BUFFER, MB_ADAPTIVE_MAX * 4 * 2, NULL, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }
}

static void ad_poll_readbacks(void) {
  for (int i = 0; i < MB_AD_RB_N; i++) {
    if (!ad_rb[i].fence) continue;
    GLenum st = glClientWaitSync(ad_rb[i].fence, 0, 0);
    if (st == GL_ALREADY_SIGNALED || st == GL_CONDITION_SATISFIED) {
      if (ad_rb[i].pbo && ad_rb[i].mask) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ad_rb[i].pbo);
        const uint8_t *p = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, MB_ADAPTIVE_MAX * 4 * 2,
                                                             GL_MAP_READ_BIT);
        if (p) {
          const uint8_t *prev = p;
          const uint8_t *next = p + MB_ADAPTIVE_MAX * 4;
          for (int s = 0; s < MB_ADAPTIVE_MAX; s++) {
            if (!(ad_rb[i].mask & (1u << s))) continue;
            if (prev[s * 4 + 0] == next[s * 4 + 0] && prev[s * 4 + 1] == next[s * 4 + 1]) {
              if (ad_rb[i].serial > ad_conv_serial[s]) ad_conv_serial[s] = ad_rb[i].serial;
            }
          }
          glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      }
      glDeleteSync(ad_rb[i].fence);
      ad_rb[i].fence = NULL;
      ad_rb[i].mask = 0;
    } else if (st == GL_WAIT_FAILED) {
      glDeleteSync(ad_rb[i].fence);
      ad_rb[i].fence = NULL;
      ad_rb[i].mask = 0;
    }
  }
}

static void ad_fill_uniforms(void) {
  if (ad_mode < 0) {
    gad_n = 0;
    return;
  }
  int max_active = 0;
  for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
    if (ad_slots[i].active) {
      gad_box[i * 4 + 0] = ad_slots[i].cx;
      gad_box[i * 4 + 1] = ad_slots[i].cy;
      gad_box[i * 4 + 2] = ad_slots[i].hx;
      gad_box[i * 4 + 3] = ad_slots[i].hy;
      max_active = i + 1;
    } else {
      gad_box[i * 4 + 0] = -1e9f;
      gad_box[i * 4 + 1] = -1e9f;
      gad_box[i * 4 + 2] = -1e9f;
      gad_box[i * 4 + 3] = -1e9f;
    }
  }
  gad_n = max_active;
}

typedef struct {
  int sid, is_new, is_dark, is_inverting, through, track_rb;
  float cx, cy, hx, hy, tint_a;
} gl_ad_pend;

static int ad_collect_and_match(const float *f, int32_t start, int32_t end, int32_t count, const Grab *m,
                                int backdrop_changed, double dt, uint64_t serial, gl_ad_pend out[MB_ADAPTIVE_MAX]) {
  (void)end;
  if (ad_mode <= 0 || dt < 0.0) return 0;
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
      items[ni].cx = (m->gb[0] + m->gb[2]) * 0.5f;
      items[ni].cy = (m->gb[1] + m->gb[3]) * 0.5f;
      items[ni].hx = fmaxf((m->gb[2] - m->gb[0]) * 0.5f, 1.0f);
      items[ni].hy = fmaxf((m->gb[3] - m->gb[1]) * 0.5f, 1.0f);
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
  int cur_through = (m && m->through) ? 1 : 0;
  int n_upd = 0;
  for (int k = 0; k < ni; k++) {
    int sid = -1, is_new = 0, mode_changed = 0;
    for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
      mb_ad_slot *s = &ad_slots[i];
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
        mb_ad_slot *s = &ad_slots[i];
        if (!s->active || s->seen_serial == serial || ((s->is_dark > 0) != (items[k].is_dark > 0))) continue;
        float d = hypotf(items[k].cx - s->cx, items[k].cy - s->cy);
        if (d <= best_d) { best_d = d; sid = i; }
      }
      if (sid < 0) {
        for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
          if (!ad_slots[i].active) { sid = i; break; }
        }
        if (sid < 0) {
          uint64_t oldest = UINT64_MAX;
          for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
            if (ad_slots[i].seen_serial != serial && ad_slots[i].seen_serial < oldest) {
              oldest = ad_slots[i].seen_serial;
              sid = i;
            }
          }
          if (sid < 0) sid = 0;
        }
        is_new = 1;
        memset(&ad_slots[sid], 0, sizeof ad_slots[sid]);
        ad_slots[sid].active = 1;
        ad_slots[sid].is_dark = items[k].is_dark;
        ad_slots[sid].is_inverting = items[k].is_inverting;
        ad_slots[sid].through = cur_through;
        ad_slots[sid].tint_a = items[k].tint_a;
      } else if (ad_slots[sid].is_dark != items[k].is_dark ||
                 ad_slots[sid].is_inverting != items[k].is_inverting ||
                 fabsf(ad_slots[sid].tint_a - items[k].tint_a) > 0.002f ||
                 ad_slots[sid].through != cur_through) {
        ad_slots[sid].is_dark = items[k].is_dark;
        ad_slots[sid].is_inverting = items[k].is_inverting;
        ad_slots[sid].through = cur_through;
        ad_slots[sid].tint_a = items[k].tint_a;
        mode_changed = 1;
      }
      ad_slots[sid].cx = items[k].cx;
      ad_slots[sid].cy = items[k].cy;
      ad_slots[sid].hx = items[k].hx;
      ad_slots[sid].hy = items[k].hy;
      ad_slots[sid].seen_serial = serial;
    }
    mb_ad_slot *s = &ad_slots[sid];
    if (!is_new && !mode_changed && s->updated_serial == serial) continue;
    int slot_changed = backdrop_changed || mode_changed;
    if (!is_new && !slot_changed && s->settle_left > 0.0 && ad_conv_serial[sid] >= s->settle_serial) {
      s->settle_left = 0.0;
    }
    int need = is_new || slot_changed || s->settle_left > 0.0;
    if (need) {
      int consec_scroll = (slot_changed && !is_new && s->has_changed && s->last_changed_serial + 1 == serial);
      int track_rb = (!is_new && s->tint_a < 0.88f && dt > 0.0 && !consec_scroll);
      if (slot_changed && !is_new) {
        s->has_changed = 1;
        s->last_changed_serial = serial;
      }
      if (is_new || s->tint_a >= 0.88f) {
        s->settle_left = 0.0;
        s->settle_serial = serial;
      } else if (slot_changed) {
        s->settle_left = MB_AD_SETTLE_SEC;
        s->settle_serial = serial;
      } else {
        s->settle_left -= dt;
        if (s->settle_left <= 0.0) s->settle_left = 0.0;
      }
      s->updated_serial = serial;
      out[n_upd++] = (gl_ad_pend){sid, is_new, s->is_dark, s->is_inverting, s->through, track_rb, s->cx, s->cy, s->hx, s->hy, s->tint_a};
    }
  }
  return n_upd;
}

typedef struct {
  gl_ad_pend p;
  GLuint bt;
  float map[4];
  float uvc[4];
} gl_ad_batch;

static void ad_run_updates_batch(const gl_ad_batch *items, int n_upd, double dt) {
  if (n_upd <= 0 || !ad_prog) return;
  ensure_ad_tex();
  int cur = ad_cur, nxt = 1 - cur;
  uint32_t batch_mask = 0;
  for (int i = 0; i < n_upd; i++) {
    if (items[i].p.track_rb) batch_mask |= (1u << items[i].p.sid);
  }
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  if (batch_mask && !ad_rb_prev_copied) {
    int slot = -1;
    for (int k = 0; k < MB_AD_RB_N; k++) {
      int idx = (ad_rb_i + k) % MB_AD_RB_N;
      if (!ad_rb[idx].fence && ad_rb[idx].pbo) { slot = idx; break; }
    }
    if (slot >= 0) {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, ad_fb[cur]);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, ad_rb[slot].pbo);
      glReadPixels(0, 0, MB_ADAPTIVE_MAX, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void *)0);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      ad_rb_cur = slot;
      ad_rb_prev_copied = 1;
    }
  }
  ad_track_mask |= batch_mask;
  glBindFramebuffer(GL_READ_FRAMEBUFFER, ad_fb[cur]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ad_fb[nxt]);
  glBlitFramebuffer(0, 0, MB_ADAPTIVE_MAX, 1, 0, 0, MB_ADAPTIVE_MAX, 1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, ad_fb[nxt]);
  glViewport(0, 0, MB_ADAPTIVE_MAX, 1);
  glEnable(GL_SCISSOR_TEST);
  glUseProgram(ad_prog);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, ad_tex[cur]);
  glUniform1i(adu_prev, 1);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(adu_bd, 0);
  for (int i = 0; i < n_upd; i++) {
    if (!items[i].bt) continue;
    glBindTexture(GL_TEXTURE_2D, items[i].bt);
    glUniform4fv(adu_map, 1, items[i].map);
    glUniform4fv(adu_uvc, 1, items[i].uvc);
    glScissor(items[i].p.sid, 0, 1, 1);
    glUniform4f(adu_box, items[i].p.cx, items[i].p.cy, items[i].p.hx, items[i].p.hy);
    glUniform4f(adu_param, (items[i].p.is_dark > 0) ? ((float)items[i].p.is_dark + items[i].p.tint_a) : -items[i].p.tint_a, (float)items[i].p.is_new + (items[i].p.through ? 2.0f : 0.0f) + (items[i].p.is_inverting ? 4.0f : 0.0f), (float)dt, (float)items[i].p.sid);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  ad_cur = nxt;
}

static void ad_rb_finish(uint64_t serial) {
  if (ad_rb_prev_copied && ad_rb_cur >= 0 && ad_track_mask && ad_fb[ad_cur] && ad_rb[ad_rb_cur].pbo) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ad_fb[ad_cur]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, ad_rb[ad_rb_cur].pbo);
    glReadPixels(0, 0, MB_ADAPTIVE_MAX, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void *)(uintptr_t)(MB_ADAPTIVE_MAX * 4));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    ad_rb[ad_rb_cur].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    ad_rb[ad_rb_cur].mask = ad_track_mask;
    ad_rb[ad_rb_cur].serial = serial;
    ad_rb_i = (ad_rb_cur + 1) % MB_AD_RB_N;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target_fb);
  }
  ad_rb_prev_copied = 0;
  ad_track_mask = 0;
  ad_rb_cur = -1;
}

static void ad_run_updates(const gl_ad_pend *pend, int n_upd, double dt) {
  if (n_upd <= 0 || !bd_tex || !ad_prog) return;
  gl_ad_batch items[MB_ADAPTIVE_MAX];
  int m = n_upd > MB_ADAPTIVE_MAX ? MB_ADAPTIVE_MAX : n_upd;
  for (int i = 0; i < m; i++) {
    items[i].p = pend[i];
    items[i].bt = bd_tex;
    memcpy(items[i].map, gmap, sizeof items[i].map);
    memcpy(items[i].uvc, guvc, sizeof items[i].uvc);
  }
  ad_run_updates_batch(items, m, dt);
}

static double ad_next_dt(void) {
  if (ad_fixed_dt > 0.0) return ad_fixed_dt;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
  double dt = ad_last_time > 0.0 ? (now - ad_last_time) : (1.0 / 60.0);
  ad_last_time = now;
  if (dt < 0.0) dt = 0.0;
  if (dt > 0.1) dt = 0.1;
  return dt;
}

static void ad_finalize_frame(int whole_frame, uint64_t serial) {
  int settling = 0;
  for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
    mb_ad_slot *s = &ad_slots[i];
    if (!s->active) continue;
    if (whole_frame && s->seen_serial != serial) {
      s->active = 0;
      s->settle_left = 0.0;
      continue;
    }
    if (s->settle_left > 0.0 && ad_conv_serial[i] >= s->settle_serial) s->settle_left = 0.0;
    if (s->settle_left > 0.0) settling = 1;
  }
  __atomic_store_n(&ad_settling, settling, __ATOMIC_RELEASE);
}

static void gl_set_fixed_dt(double dt) { ad_fixed_dt = dt > 0.0 ? dt : 0.0; }
static void gl_adaptive_reset(void) { ad_forget(); }
static int gl_adaptive_settling(void) { return __atomic_load_n(&ad_settling, __ATOMIC_ACQUIRE); }

static void slots_invalidate(void) {
  for (int i = 0; i < MB_GLASS_SLOTS; i++) slots[i].valid = 0;
  kept_ok = 0;
}

static void glass_forget(void) {
  slots_invalidate();
  ad_forget();
}

// a new context has none of the old textures
static void glass_lost(void) {
  memset(lv, 0, sizeof lv);
  lv_w = lv_h = 0;
  memset(lens_lv, 0, sizeof lens_lv);
  lens_w = lens_h = 0;
  lv_shrink_frames = lv_win_max_w = lv_win_max_h = 0;
  lv_frame_max_w = lv_frame_max_h = 0;
  for (int i = 0; i < MB_GLASS_SLOTS; i++) {
    glass_slot *s = &slots[i];
    s->bt = s->bfb = 0;
    s->bw = s->bh = 0;
    s->bf = 0;
  }
  ad_tex[0] = ad_tex[1] = 0;
  ad_fb[0] = ad_fb[1] = 0;
  ad_cur = 0;
  memset(ad_rb, 0, sizeof ad_rb);
  ad_rb_i = 0;
  ad_rb_cur = -1;
  glass_forget();
}

static void shrink_levels(int w, int h) {
  for (int j = 0; j < 5; j++)
    if (lv[j].t) {
      glDeleteTextures(1, &lv[j].t);
      glDeleteFramebuffers(1, &lv[j].fb);
    }
  lv_w = w > 0 ? w : 1;
  lv_h = h > 0 ? h : 1;
  levels_rebuilt++;
  for (int j = 0; j < 4; j++)
    lv[j] = level_tex(j == 0 ? GL_RGBA8 : GL_RGB10_A2, (lv_w + (1 << j) - 1) >> j, (lv_h + (1 << j) - 1) >> j);
  lv[4] = level_tex(GL_RGB10_A2, lv_w, lv_h);
  glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
  for (int i = 0; i < MB_GLASS_SLOTS; i++) {
    glass_slot *s = &slots[i];
    if (!s->bt) continue;
    int sk = s->grab.progressive ? 0 : s->grab.k;
    int max_sw = (lv_w + (1 << sk) - 1) >> sk;
    int max_sh = (lv_h + (1 << sk) - 1) >> sk;
    if (!s->valid || s->bw > max_sw || s->bh > max_sh) {
      slot_drop_tex(s);
      s->valid = 0;
    }
  }
}

// The glass's own textures given back, in the context they were made in (current): the levels, the slots' copies and
// what each slot was of. The next capture makes what it needs again at its size (ensure_levels, slot_blur), its blur
// the same to the byte (a blur is of what is drawn under it, not of what was kept).
static void glass_free(void) {
  for (int j = 0; j < 5; j++)
    if (lv[j].t) {
      glDeleteTextures(1, &lv[j].t);
      glDeleteFramebuffers(1, &lv[j].fb);
    }
  for (int j = 0; j < 2; j++)
    if (lens_lv[j].t) {
      glDeleteTextures(1, &lens_lv[j].t);
      glDeleteFramebuffers(1, &lens_lv[j].fb);
    }
  for (int i = 0; i < MB_GLASS_SLOTS; i++) {
    glass_slot *s = &slots[i];
    if (s->bt) {
      glDeleteFramebuffers(1, &s->bfb);
      glDeleteTextures(1, &s->bt);
    }
    free(s->under);
    s->under = NULL;
    s->under_cap = s->under_n = 0;
  }
  for (int i = 0; i < 2; i++) {
    if (ad_fb[i]) glDeleteFramebuffers(1, &ad_fb[i]);
    if (ad_tex[i]) glDeleteTextures(1, &ad_tex[i]);
  }
  for (int i = 0; i < MB_AD_RB_N; i++) {
    if (ad_rb[i].fence) glDeleteSync(ad_rb[i].fence);
    if (ad_rb[i].pbo) glDeleteBuffers(1, &ad_rb[i].pbo);
  }
  bd_tex = 0;
  glass_lost();
}

// Glass's textures (its levels as big as the largest capture yet, a copy a slot) are given back once this many frames
// in a row drew no glass (a sheet closed, the composer gone): held on, they are tens of MB at a screen's size.
#define MB_GLASS_IDLE 180
static int glass_idle_frames;
static int glass_drawn;  // the frame drawing now reached glass (draw_into)

static void glass_idle_tick(void) {
  if (glass_drawn) {
    glass_idle_frames = 0;
    if (lv_w > 0 && lv_h > 0 && lv_frame_max_w > 0 && lv_frame_max_h > 0) {
      if (lv_frame_max_w > lv_win_max_w) lv_win_max_w = lv_frame_max_w;
      if (lv_frame_max_h > lv_win_max_h) lv_win_max_h = lv_frame_max_h;
      if (++lv_shrink_frames >= MB_GLASS_IDLE) {
        if ((size_t)lv_win_max_w * (size_t)lv_win_max_h * 2 < (size_t)lv_w * (size_t)lv_h) {
          shrink_levels(lv_win_max_w, lv_win_max_h);
        }
        lv_shrink_frames = 0;
        lv_win_max_w = lv_win_max_h = 0;
      }
    }
    lv_frame_max_w = lv_frame_max_h = 0;
  } else {
    lv_frame_max_w = lv_frame_max_h = 0;
    if (glass_idle_frames < MB_GLASS_IDLE && ++glass_idle_frames == MB_GLASS_IDLE && lv_w > 0) glass_free();
  }
  glass_drawn = 0;
}

// Whether instance i (its run of glass starting at `run`) draws anything in g (x0, y0, x1, y1, points): its quad as
// the vertex shader makes it (VS: grown round its rect, liquid glass over its group's bounds, rotated when turned).
static int reaches(const float *f, int32_t i, int32_t run, const float g[4]) {
  const float *b = f + (size_t)i * MB_INST;
  int kind = (int)(b[5] + 0.5f);
  if (b[32] != 0.0f && kind == 6 && (b[23] > 0.0f || f[(size_t)run * MB_INST + 23] > 0.0f)) return 1;
  float e = kind == 2 ? fabsf(b[7]) * 3.0f + 2.0f : 1.0f;
  if (kind == 14 || kind == 15) e = fabsf(b[7]) * 3.0f + 2.0f;
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

// Exact spatial dependency box of every instance before `start` whose pixels (directly, or transitively through an
// earlier glass capture or adaptive ink sample) reach into `m`'s grab rectangle.
static void under_dep_rect(const float *f, int32_t start, int32_t count, const Grab *m, float dep[4]) {
  float sc = (float)cur_scale;
  dep[0] = (float)m->px0 / sc - 1.0f;
  dep[1] = (float)(m->top - (m->pb0 + m->h)) / sc - 1.0f;
  dep[2] = (float)(m->px0 + m->w) / sc + 1.0f;
  dep[3] = (float)(m->top - m->pb0) / sc + 1.0f;
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
        float blur_r = (gkind == 6 && a[7] < 0.0f) ? 0.0f : fabsf(a[7]);
        int merged_liquid = (gkind == 6 && a[23] > 0.0f);
        int unmerged_liquid = (gkind == 6 && !merged_liquid);
        float margin = (is_contained || (gkind == 5 && a[22] < -0.5f)) ? 0.0f : 3.0f * blur_r + (gkind == 6 ? a[21] * (1.0f + fabsf(a[22])) + fmaxf(a[23], 0.0f) : 0.0f) + 2.0f;
        float gx0 = 1e9f, gy0 = 1e9f, gx1 = -1e9f, gy1 = -1e9f;
        for (int32_t j = first_i; j < count; j++) {
          const float *b = f + (size_t)j * MB_INST;
          if (b[20] != tid) continue;
          float bx0, by0, bx1, by1;
          if (gkind == 6) {
            float m23 = fmaxf(unmerged_liquid ? b[23] : a[23], 0.0f);
            bx0 = b[0] - m23; by0 = b[1] - m23; bx1 = b[0] + b[2] + m23; by1 = b[1] + b[3] + m23;
            if (unmerged_liquid) {
              float bm = 3.0f * blur_r + b[21] * (1.0f + fabsf(b[22])) + m23 + 2.0f;
              if (bm > margin) margin = bm;
            }
          } else {
            bx0 = b[0] > b[16] ? b[0] : b[16]; by0 = b[1] > b[17] ? b[1] : b[17];
            bx1 = b[0] + b[2] < b[18] ? b[0] + b[2] : b[18]; by1 = b[1] + b[3] < b[19] ? b[1] + b[3] : b[19];
          }
          if (bx0 < gx0) gx0 = bx0;
          if (by0 < gy0) gy0 = by0;
          if (bx1 > gx1) gx1 = bx1;
          if (by1 > gy1) gy1 = by1;
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
}

#define MB_SEG_BITS_WORDS 64
#define MB_SEG_BITS_MAX (MB_SEG_BITS_WORDS * 64)

// Records the exact kind-12 data instances (relative to `pieces`) read by a reaching kind-11 path band.
static void under_mark_shape_segs(const float *bi, int32_t seg_insts, uint64_t seg_bits[MB_SEG_BITS_WORDS],
                                  int *seg_overflow, int32_t *min_seg, int32_t *max_seg) {
  if (*seg_overflow || seg_insts <= 0) return;
  int32_t first = (int32_t)(bi[12] + 0.5f);
  int32_t sn = (int32_t)(bi[13] + 0.5f);
  if (first < 0 || sn <= 0) return;
  int32_t d0 = first / 6;
  int32_t d1 = (first + sn - 1) / 6;
  if (d0 >= seg_insts) return;
  if (d1 >= seg_insts) d1 = seg_insts - 1;
  if (d1 >= MB_SEG_BITS_MAX) {
    *seg_overflow = 1;
    return;
  }
  for (int32_t d = d0; d <= d1; d++) seg_bits[d >> 6] |= (1ULL << (d & 63));
  if (d0 < *min_seg) *min_seg = d0;
  if (d1 > *max_seg) *max_seg = d1;
}

// What the capture of the run at start is of (under_now): exact spatial dependency closure of every instance before
// start whose pixels (directly, or transitively through an earlier glass capture or adaptive ink sample) reach into
// the capture's rectangle, plus only the kind-12 segment data instances referenced by reaching kind-11 shapes.
// 0 when a stream or a native view's picture is among them.
static int under_take(const float *f, int32_t start, int32_t count, int32_t pieces, const Grab *m) {
  float dep[4];
  under_dep_rect(f, start, count, m, dep);
  int32_t seg_insts = count > pieces ? (count - pieces) : 0;
  uint64_t seg_bits[MB_SEG_BITS_WORDS];
  int seg_bits_init = 0, seg_overflow = 0;
  int32_t min_seg = seg_insts, max_seg = -1;
  size_t need_base = (size_t)start * MB_INST;
  if (under_now_cap < need_base) {
    under_now_cap = need_base;
    under_now = (float *)realloc(under_now, under_now_cap * sizeof(float));
  }
  float *o = under_now;
  int in_blur = 0, blur_hit = 0;
  int32_t picked = 0, run = 0;
  for (int32_t i = 0; i < start; i++) {
    float tid = f[(size_t)i * MB_INST + 20];
    if (i > 0 && tid != f[(size_t)(i - 1) * MB_INST + 20]) run = i;
    int k = (int)(f[(size_t)i * MB_INST + 5] + 0.5f);
    int take = 0;
    if (k == 14) {
      if (in_blur == 0) blur_hit = reaches(f, i, run, dep);
      in_blur++;
      take = blur_hit;
    } else if (k == 15) {
      take = blur_hit;
      if (in_blur > 0) {
        in_blur--;
        if (in_blur == 0) blur_hit = 0;
      }
    } else {
      take = (in_blur > 0 && blur_hit) || reaches(f, i, run, dep);
    }
    if (!take) continue;
    if (tid > (float)MB_STREAM_TEX) return 0;
    const float *bi = f + (size_t)i * MB_INST;
    if (k == 11 && seg_insts > 0) {
      if (!seg_bits_init) {
        memset(seg_bits, 0, sizeof seg_bits);
        seg_bits_init = 1;
      }
      under_mark_shape_segs(bi, seg_insts, seg_bits, &seg_overflow, &min_seg, &max_seg);
    }
    memcpy(o, bi, MB_INST * sizeof(float));
    o += MB_INST;
    picked++;
  }
  if (seg_bits_init && seg_insts > 0) {
    if (seg_overflow) {
      size_t total_need = ((size_t)picked + (size_t)seg_insts) * MB_INST;
      if (under_now_cap < total_need) {
        under_now_cap = total_need;
        under_now = (float *)realloc(under_now, under_now_cap * sizeof(float));
        o = under_now + (size_t)picked * MB_INST;
      }
      memcpy(o, f + (size_t)pieces * MB_INST, (size_t)seg_insts * MB_INST * sizeof(float));
      o += (size_t)seg_insts * MB_INST;
    } else if (max_seg >= min_seg) {
      int32_t seg_count = 0;
      for (int32_t w = min_seg >> 6; w <= (max_seg >> 6); w++) {
        uint64_t v = seg_bits[w];
        while (v) { v &= v - 1; seg_count++; }
      }
      size_t total_need = ((size_t)picked + (size_t)seg_count) * MB_INST;
      if (under_now_cap < total_need) {
        under_now_cap = total_need;
        under_now = (float *)realloc(under_now, under_now_cap * sizeof(float));
        o = under_now + (size_t)picked * MB_INST;
      }
      for (int32_t d = min_seg; d <= max_seg; d++) {
        if (seg_bits[d >> 6] & (1ULL << (d & 63))) {
          memcpy(o, f + (size_t)(pieces + d) * MB_INST, MB_INST * sizeof(float));
          o += MB_INST;
        }
      }
    }
  }
  under_now_n = (size_t)(o - under_now);
  under_whole = 0;
  under_picked = picked;
  return 1;
}

// Zero-copy fast-reject match of what is under [0, start) against existing valid slots with matching grab and key
// (and slot->gen > min_gen). Exits on the first differing reaching instance on scroll misses without copying into under_now.
static glass_slot *slot_match_under(const float *f, int32_t start, int32_t count, int32_t pieces, const Grab *m,
                                    const double key[9], unsigned min_gen) {
  glass_slot *cands[MB_GLASS_SLOTS];
  int cand_n = 0;
  for (int i = 0; i < MB_GLASS_SLOTS; i++) {
    glass_slot *s = &slots[i];
    if (s->valid && s->bt && s->whole == 0 && (min_gen == 0 || s->gen > min_gen) &&
        memcmp(&s->grab, m, sizeof *m) == 0 && memcmp(s->key, key, sizeof s->key) == 0) {
      cands[cand_n++] = s;
    }
  }
  if (cand_n == 0) return NULL;
  float dep[4];
  under_dep_rect(f, start, count, m, dep);
  int32_t seg_insts = count > pieces ? (count - pieces) : 0;
  uint64_t seg_bits[MB_SEG_BITS_WORDS];
  int seg_bits_init = 0, seg_overflow = 0;
  int32_t min_seg = seg_insts, max_seg = -1;
  uint32_t alive = (cand_n == 32) ? ~0u : ((1u << cand_n) - 1u);
  int in_blur = 0, blur_hit = 0;
  int32_t matched = 0, run = 0;
  for (int32_t i = 0; i < start; i++) {
    float tid = f[(size_t)i * MB_INST + 20];
    if (i > 0 && tid != f[(size_t)(i - 1) * MB_INST + 20]) run = i;
    int k = (int)(f[(size_t)i * MB_INST + 5] + 0.5f);
    int take = 0;
    if (k == 14) {
      if (in_blur == 0) blur_hit = reaches(f, i, run, dep);
      in_blur++;
      take = blur_hit;
    } else if (k == 15) {
      take = blur_hit;
      if (in_blur > 0) {
        in_blur--;
        if (in_blur == 0) blur_hit = 0;
      }
    } else {
      take = (in_blur > 0 && blur_hit) || reaches(f, i, run, dep);
    }
    if (!take) continue;
    if (tid > (float)MB_STREAM_TEX) return NULL;
    const float *bi = f + (size_t)i * MB_INST;
    for (int c = 0; c < cand_n; c++) {
      if (!(alive & (1u << c))) continue;
      glass_slot *s = cands[c];
      if (matched >= s->picked ||
          memcmp(s->under + (size_t)matched * MB_INST, bi, MB_INST * sizeof(float)) != 0) {
        alive &= ~(1u << c);
      }
    }
    if (!alive) return NULL;
    if (k == 11 && seg_insts > 0) {
      if (!seg_bits_init) {
        memset(seg_bits, 0, sizeof seg_bits);
        seg_bits_init = 1;
      }
      under_mark_shape_segs(bi, seg_insts, seg_bits, &seg_overflow, &min_seg, &max_seg);
    }
    matched++;
  }
  int32_t seg_count = 0;
  if (seg_bits_init && seg_insts > 0) {
    if (seg_overflow) {
      seg_count = seg_insts;
    } else if (max_seg >= min_seg) {
      for (int32_t w = min_seg >> 6; w <= (max_seg >> 6); w++) {
        uint64_t v = seg_bits[w];
        while (v) { v &= v - 1; seg_count++; }
      }
    }
  }
  size_t expect_n = ((size_t)matched + (size_t)seg_count) * MB_INST;
  for (int c = 0; c < cand_n; c++) {
    if (!(alive & (1u << c))) continue;
    glass_slot *s = cands[c];
    if (s->picked != matched || s->under_n != expect_n) alive &= ~(1u << c);
  }
  if (!alive) return NULL;
  if (seg_count > 0) {
    if (seg_overflow) {
      const float *src = f + (size_t)pieces * MB_INST;
      size_t seg_bytes = (size_t)seg_insts * MB_INST * sizeof(float);
      for (int c = 0; c < cand_n; c++) {
        if (!(alive & (1u << c))) continue;
        if (memcmp(cands[c]->under + (size_t)matched * MB_INST, src, seg_bytes) != 0) alive &= ~(1u << c);
      }
    } else {
      size_t off = (size_t)matched * MB_INST;
      for (int32_t d = min_seg; d <= max_seg && alive; d++) {
        if (!(seg_bits[d >> 6] & (1ULL << (d & 63)))) continue;
        const float *sd = f + (size_t)(pieces + d) * MB_INST;
        for (int c = 0; c < cand_n; c++) {
          if (!(alive & (1u << c))) continue;
          if (memcmp(cands[c]->under + off, sd, MB_INST * sizeof(float)) != 0) alive &= ~(1u << c);
        }
        off += MB_INST;
      }
    }
  }
  if (!alive) return NULL;
  for (int c = 0; c < cand_n; c++) {
    if (alive & (1u << c)) return cands[c];
  }
  return NULL;
}

static uint32_t lookahead_slot_mask;
static int lookahead_force_off;
static int lookahead_batched;

static void slots_evict_budget(int keep_idx, unsigned frame_clock, int allow_frame_slots) {
  while (slots_total_bytes() > MB_GLASS_CACHE_MAX_BYTES) {
    int victim = -1;
    for (int j = 0; j < MB_GLASS_SLOTS; j++) {
      if (j == keep_idx || !slots[j].bt) continue;
      if (lookahead_slot_mask & (1u << j)) continue;
      if (!allow_frame_slots && slots[j].used > frame_clock) continue;
      if (victim < 0 || (!slots[j].valid && slots[victim].valid) ||
          (slots[j].valid == slots[victim].valid && slots[j].used < slots[victim].used)) {
        victim = j;
      }
    }
    if (victim < 0) break;
    slot_drop_tex(&slots[victim]);
    slots[victim].valid = 0;
  }
}

// Picks and prepares the cache slot least lately shown (reusing a compact 64-px-aligned texture when sized cleanly)
// and blurs `m` directly into that slot's framebuffer (zero extra post-blur blit or level swap churn).
static glass_slot *slot_blur(const Grab *m, unsigned frame_clock) {
  ensure_levels(m->w, m->h);
  level *L = grab_blurred(m);
  int lw[4], lh[4];
  grab_levels(m, lw, lh);
  int sk = m->progressive ? 0 : m->k;
  int gw = lw[sk] > 0 ? lw[sk] : 1;
  int gh = lh[sk] > 0 ? lh[sk] : 1;
  int max_w = lv[sk].w, max_h = lv[sk].h;
  int bw = (gw + 63) & ~63;
  int bh = (gh + 63) & ~63;
  if (bw > max_w) bw = max_w;
  if (bh > max_h) bh = max_h;
  if (bw < 1) bw = 1;
  if (bh < 1) bh = 1;
  size_t b_area = (size_t)bw * (size_t)bh;
  GLenum fmt = L->format;
  int t0 = -1, t1 = -1, t2 = -1, t3 = -1, t4 = -1, fallback = -1;
  for (int i = 0; i < MB_GLASS_SLOTS; i++) {
    if (lookahead_slot_mask & (1u << i)) continue;
    glass_slot *s = &slots[i];
    if (fallback < 0 || s->used < slots[fallback].used) fallback = i;
    if (s->used > frame_clock) continue;
    int has_bt = s->bt != 0;
    int reuse_tex = has_bt && s->bf == fmt && s->bw >= gw && s->bh >= gh &&
                    (size_t)s->bw * (size_t)s->bh <= 2 * b_area;
    if (!s->valid) {
      if (reuse_tex) {
        if (t0 < 0) t0 = i;
      } else if (!has_bt) {
        if (t1 < 0) t1 = i;
      } else {
        if (t3 < 0) t3 = i;
      }
    } else if (reuse_tex) {
      if (t2 < 0 || s->used < slots[t2].used) t2 = i;
    }
    if (t4 < 0 || s->used < slots[t4].used) t4 = i;
  }
  int idx = t0 >= 0 ? t0 : t1 >= 0 ? t1 : t2 >= 0 ? t2 : t3 >= 0 ? t3 : t4 >= 0 ? t4 : fallback;
  if (idx < 0) {
    for (int i = 0; i < MB_GLASS_SLOTS; i++) {
      if (idx < 0 || slots[i].used < slots[idx].used) idx = i;
    }
    if (idx < 0) idx = 0;
  }
  glass_slot *s = &slots[idx];
  s->valid = 0;
  s->gen = ++slots_gen;
  if (!(s->bt != 0 && s->bf == fmt && s->bw >= gw && s->bh >= gh &&
        (size_t)s->bw * (size_t)s->bh <= 2 * b_area)) {
    slot_drop_tex(s);
    level nl = level_tex(fmt, bw, bh);
    s->bt = nl.t;
    s->bfb = nl.fb;
    s->bw = nl.w;
    s->bh = nl.h;
    s->bf = nl.format;
  }
  level out = {s->bt, s->bfb, s->bw, s->bh, s->bf};
  blur_into(m, &out);
  slots_evict_budget(idx, frame_clock, 0);
  return s;
}

static void slot_show(const Grab *m, glass_slot *s) {
  s->used = ++slots_clock;
  show_blur(m, s->bt, s->bw, s->bh);
}

static void slot_keep(glass_slot *s, const Grab *m, const double key[9]) {
  if (s->under_cap < under_now_n) {
    s->under = (float *)realloc(s->under, under_now_n * sizeof(float));
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

static void main_state(void);

#define MB_GLASS_GROUPS_MAX 32
#define MB_LOOKAHEAD_MAX 8

typedef struct {
  int32_t start, end;
  int tid, skind;
  int has_grab, in_cut_rect, pre_missed;
  Grab gm;
} gl_glass_group;

typedef struct {
  int tid;
  int32_t start;
  glass_slot *slot;
  unsigned gen;
  Grab grab;
  int ad_done;
  int reblurred;
} gl_pre_hit;

static int mb_popcount32(uint32_t x) {
  int n = 0;
  while (x) { x &= x - 1u; n++; }
  return n;
}

static int grab_in_cut(const Grab *m) {
  return !cut_on || (m->px0 + m->w > cut[0] && m->px0 < cut[0] + cut[2] &&
                     m->pb0 + m->h > cut[1] && m->pb0 < cut[1] + cut[3]);
}

// Conservative exact check: returns 1 iff no drawn instance in [from, to) can write any pixel inside `box` (points).
static int region_untouched_in_range(const float *f, int32_t from, int32_t to, int32_t count, const float box[4]) {
  for (int32_t j = from; j < to; ) {
    const float *b = f + (size_t)j * MB_INST;
    int kind = (int)(b[5] + 0.5f);
    if (kind == 12) { j++; continue; }
    if (kind == 14 || kind == 15) return 0;
    if (kind == 8) {
      int32_t cn = (int32_t)(b[12] + 0.5f);
      if (cn < 0) cn = 0;
      if (j + 1 + cn > count) cn = count - j - 1;
      if (b[32] != 0.0f) return 0;
      float lm = fmaxf(b[7], 0.0f) * 3.0f + fmaxf(b[14], 0.0f) + 5.0f;
      float lx0 = b[0], ly0 = b[1], lx1 = b[0] + b[2], ly1 = b[1] + b[3];
      for (int32_t ci = j + 1; ci < j + 1 + cn; ci++) {
        const float *cb = f + (size_t)ci * MB_INST;
        int ck = (int)(cb[5] + 0.5f);
        if (ck == 12 || cb[2] <= 0.0f || cb[3] <= 0.0f) continue;
        if (cb[0] < lx0) lx0 = cb[0];
        if (cb[1] < ly0) ly0 = cb[1];
        if (cb[0] + cb[2] > lx1) lx1 = cb[0] + cb[2];
        if (cb[1] + cb[3] > ly1) ly1 = cb[1] + cb[3];
      }
      if (lx1 + lm > box[0] && lx0 - lm < box[2] && ly1 + lm > box[1] && ly0 - lm < box[3]) return 0;
      j += 1 + cn;
      continue;
    }
    if (kind == 6 && b[23] > 0.0f) {
      float tid = b[20];
      int32_t run_end = j + 1;
      while (run_end < count && f[(size_t)run_end * MB_INST + 20] == tid &&
             (int)(f[(size_t)run_end * MB_INST + 5] + 0.5f) == 6) run_end++;
      float m23 = fmaxf(b[23], 0.0f) + 3.0f;
      float ux0 = 1e9f, uy0 = 1e9f, ux1 = -1e9f, uy1 = -1e9f;
      for (int32_t k = j; k < run_end; k++) {
        const float *bk = f + (size_t)k * MB_INST;
        if (bk[32] != 0.0f) return 0;
        float bx0 = fminf(bk[0], bk[0] + bk[2]) - m23, bx1 = fmaxf(bk[0], bk[0] + bk[2]) + m23;
        float by0 = fminf(bk[1], bk[1] + bk[3]) - m23, by1 = fmaxf(bk[1], bk[1] + bk[3]) + m23;
        if (bx0 < ux0) ux0 = bx0;
        if (by0 < uy0) uy0 = by0;
        if (bx1 > ux1) ux1 = bx1;
        if (by1 > uy1) uy1 = by1;
      }
      if (ux1 > box[0] && ux0 < box[2] && uy1 > box[1] && uy0 < box[3]) return 0;
      j = run_end < to ? run_end : to;
      continue;
    }
    int32_t run_j = j;
    if (b[20] < 0.0f) {
      while (run_j > 0 && f[(size_t)(run_j - 1) * MB_INST + 20] == b[20]) run_j--;
    }
    if (reaches(f, j, run_j, box)) return 0;
    j++;
  }
  return 1;
}

// What the run [start, end) of glass sees (`pieces`: where the frame's shapes' pieces start, `r`, `g`, `b` the ground it
// was cleared to), blurred anew or as a slot keeps it; 0 when none of it shows. On a cache miss that resolves target_fb,
// also lookahead-captures subsequent spatially-independent glass groups in the same offscreen phase (batching their
// adaptive updates) so tile-based GPUs avoid extra full-screen resolves and reloads. Leaves the main programs' state.
static int capture(const float *f, int32_t start, int32_t end, int32_t count, int32_t pieces, double r, double g,
                   double b, unsigned frame_clock, const gl_pre_hit *pre_hits, int pre_hits_n, gl_pre_hit *la_hits,
                   int *la_hits_n, const gl_glass_group *groups, int groups_n, unsigned pre_slots_gen) {
  Grab m;
  int tid = (int)f[start * MB_INST + 20];
  int shows = 0;
  glass_slot *s = NULL;
  int ad_done = 0, reblurred = 0;
  for (int h = 0; h < pre_hits_n; h++) {
    if (pre_hits[h].tid == tid && pre_hits[h].start == start) {
      glass_slot *hs = pre_hits[h].slot;
      if (hs && hs->valid && hs->bt && hs->gen == pre_hits[h].gen) {
        m = pre_hits[h].grab;
        shows = 1;
        s = hs;
      }
      break;
    }
  }
  if (!s && la_hits && la_hits_n && *la_hits_n > 0) {
    for (int h = 0; h < *la_hits_n; h++) {
      if (la_hits[h].tid == tid && la_hits[h].start == start) {
        glass_slot *hs = la_hits[h].slot;
        if (hs && hs->bt && hs->gen == la_hits[h].gen) {
          m = la_hits[h].grab;
          shows = 1;
          s = hs;
          ad_done = la_hits[h].ad_done;
          reblurred = la_hits[h].reblurred;
        }
        break;
      }
    }
  }
  int g_idx = -1;
  for (int gi = 0; gi < groups_n; gi++) {
    if (groups[gi].tid == tid && groups[gi].start == start) { g_idx = gi; break; }
  }
  double stretch_w = ((int)(cur_w * cur_scale + 0.5) != fb_w) ? cur_w : 0.0;
  double stretch_h = (vhp == fb_h && (int)(cur_h * cur_scale + 0.5) != fb_h) ? cur_h : 0.0;
  double key[9] = {fb_w, fb_h, cur_scale, r, g, b, over_other, stretch_w, stretch_h};
  int switched = 0;
  if (!s) {
    if (g_idx >= 0) {
      shows = groups[g_idx].has_grab;
      if (shows) m = groups[g_idx].gm;
    } else {
      shows = grab_of(&m, f, start, end, count);
    }
  }
  if (shows) {
    if (!s) {
      unsigned min_gen = (g_idx >= 0 && groups[g_idx].pre_missed) ? pre_slots_gen : 0u;
      s = slot_match_under(f, start, count, pieces, &m, key, min_gen);
      if (!s) {
        reblurred = 1;
        switched = 1;
        uint32_t pre_pins = 0u;
        for (int h = 0; h < pre_hits_n; h++) {
          if (pre_hits[h].start > start && pre_hits[h].slot) {
            int pi = (int)(pre_hits[h].slot - slots);
            if (pi >= 0 && pi < MB_GLASS_SLOTS) pre_pins |= (1u << pi);
          }
        }
        uint32_t prev_pre_mask = lookahead_slot_mask;
        lookahead_slot_mask |= pre_pins;
        s = slot_blur(&m, frame_clock);
        int known = !encode_missing ? under_take(f, start, count, pieces, &m) : 0;
        // (an image drawn under it that had no texture yet: it comes under the same instances, so this blur is not kept
        // to be shown again as theirs)
        if (known && !encode_missing) slot_keep(s, &m, key);
        s->used = ++slots_clock;
        // Lookahead-capture subsequent spatially-independent glass groups while target_fb is already resolved.
        gl_ad_batch la_ad_batch[MB_ADAPTIVE_MAX];
        int la_ad_n = 0;
        {
          float map0[4], uvc0[4];
          grab_map_uvc(&m, s->bw, s->bh, map0, uvc0);
          gl_ad_pend ad_pend[MB_ADAPTIVE_MAX];
          int nu = ad_collect_and_match(f, start, end, count, &m, 1, ad_cur_dt, ad_frame_serial, ad_pend);
          for (int k = 0; k < nu && la_ad_n < MB_ADAPTIVE_MAX; k++) {
            la_ad_batch[la_ad_n].p = ad_pend[k];
            la_ad_batch[la_ad_n].bt = s->bt;
            memcpy(la_ad_batch[la_ad_n].map, map0, sizeof map0);
            memcpy(la_ad_batch[la_ad_n].uvc, uvc0, sizeof uvc0);
            la_ad_n++;
          }
          ad_done = 1;
        }
        uint32_t new_la_pins = 0u;
        if (!lookahead_force_off && g_idx >= 0 && la_hits && la_hits_n && groups_n > g_idx + 1) {
          int s0_idx = (int)(s - slots);
          uint32_t s0_bit = (s0_idx >= 0 && s0_idx < MB_GLASS_SLOTS) ? (1u << s0_idx) : 0u;
          uint32_t prev_la_mask = lookahead_slot_mask;
          lookahead_slot_mask |= s0_bit;
          int ad_barrier = 0;
          int la_added = 0;
          float sc = (float)cur_scale;
          for (int gj = g_idx + 1; gj < groups_n; gj++) {
            const gl_glass_group *gg = &groups[gj];
            if (!gg->has_grab || (cut_on && !grab_in_cut(&gg->gm))) continue;
            int already = 0;
            for (int h = 0; h < pre_hits_n; h++) {
              if (pre_hits[h].tid == gg->tid && pre_hits[h].start == gg->start) { already = 1; break; }
            }
            if (!already) {
              for (int h = 0; h < *la_hits_n; h++) {
                if (la_hits[h].tid == gg->tid && la_hits[h].start == gg->start) {
                  already = 1;
                  if (gg->skind == 6 && !la_hits[h].ad_done) ad_barrier = 1;
                  break;
                }
              }
            }
            if (already) continue;
            if (*la_hits_n >= MB_GLASS_GROUPS_MAX || la_added >= MB_LOOKAHEAD_MAX ||
                mb_popcount32(lookahead_slot_mask) >= MB_GLASS_SLOTS - 2) break;
            float box[4] = {(float)gg->gm.px0 / sc - 1.0f, (float)(gg->gm.top - (gg->gm.pb0 + gg->gm.h)) / sc - 1.0f,
                            (float)(gg->gm.px0 + gg->gm.w) / sc + 1.0f, (float)(gg->gm.top - gg->gm.pb0) / sc + 1.0f};
            if (!region_untouched_in_range(f, start, gg->start, count, box)) {
              if (gg->skind == 6) ad_barrier = 1;
              continue;
            }
            unsigned mg = gg->pre_missed ? pre_slots_gen : 0u;
            glass_slot *sj = slot_match_under(f, gg->start, count, pieces, &gg->gm, key, mg);
            int sj_rebl = 0;
            if (!sj) {
              sj_rebl = 1;
              sj = slot_blur(&gg->gm, frame_clock);
              int knownj = !encode_missing ? under_take(f, gg->start, count, pieces, &gg->gm) : 0;
              if (knownj && !encode_missing) slot_keep(sj, &gg->gm, key);
              sj->used = ++slots_clock;
              lookahead_batched++;
            }
            int sj_idx = (int)(sj - slots);
            if (sj_idx >= 0 && sj_idx < MB_GLASS_SLOTS) {
              uint32_t sj_bit = (1u << sj_idx);
              lookahead_slot_mask |= sj_bit;
              new_la_pins |= sj_bit;
            }
            int sj_ad_done = 0;
            float mapj[4], uvcj[4];
            grab_map_uvc(&gg->gm, sj->bw, sj->bh, mapj, uvcj);
            if (gg->skind == 6 && !ad_barrier && ad_mode > 0 && ad_cur_dt >= 0.0) {
              int ad_used = 0;
              for (int ai = 0; ai < MB_ADAPTIVE_MAX; ai++) {
                if (ad_slots[ai].active && ad_slots[ai].seen_serial == ad_frame_serial) ad_used++;
              }
              const float *ga = f + (size_t)gg->start * MB_INST;
              int gg_ad_need = 0;
              if (ga[23] > 0.0f) {
                if (ga[14] < -0.5f && !(ga[7] <= 0.1f && ga[11] < 0.12f)) gg_ad_need = 1;
              } else {
                for (int32_t ii = gg->start; ii < count && gg_ad_need < MB_ADAPTIVE_MAX; ii++) {
                  const float *bi = f + (size_t)ii * MB_INST;
                  if (bi[20] != ga[20]) continue;
                  if ((int)(bi[5] + 0.5f) != 6 || bi[23] > 0.0f) continue;
                  if (bi[14] < -0.5f && !(bi[7] <= 0.1f && bi[11] < 0.12f)) gg_ad_need++;
                }
              }
              if (ad_used + gg_ad_need <= MB_ADAPTIVE_MAX) {
                gl_ad_pend tmp[MB_ADAPTIVE_MAX];
                int nu = ad_collect_and_match(f, gg->start, gg->end, count, &gg->gm, sj_rebl, ad_cur_dt, ad_frame_serial, tmp);
                if (nu > 0) {
                  int sid_clash = (la_ad_n + nu > MB_ADAPTIVE_MAX);
                  for (int k = 0; k < nu && !sid_clash; k++) {
                    for (int p = 0; p < la_ad_n; p++) {
                      if (la_ad_batch[p].p.sid == tmp[k].sid) { sid_clash = 1; break; }
                    }
                  }
                  if (sid_clash && la_ad_n > 0) {
                    ad_run_updates_batch(la_ad_batch, la_ad_n, ad_cur_dt);
                    la_ad_n = 0;
                  }
                  for (int k = 0; k < nu && la_ad_n < MB_ADAPTIVE_MAX; k++) {
                    la_ad_batch[la_ad_n].p = tmp[k];
                    la_ad_batch[la_ad_n].bt = sj->bt;
                    memcpy(la_ad_batch[la_ad_n].map, mapj, sizeof mapj);
                    memcpy(la_ad_batch[la_ad_n].uvc, uvcj, sizeof uvcj);
                    la_ad_n++;
                  }
                }
                sj_ad_done = 1;
              } else {
                ad_barrier = 1;
              }
            } else if (gg->skind == 5) {
              sj_ad_done = 1;
            }
            gl_pre_hit *lh = &la_hits[(*la_hits_n)++];
            lh->tid = gg->tid;
            lh->start = gg->start;
            lh->slot = sj;
            lh->gen = sj->gen;
            lh->grab = gg->gm;
            lh->ad_done = sj_ad_done;
            lh->reblurred = sj_rebl;
            la_added++;
          }
          // Release temporary pin on s0 (keep any pins that were already set + new lookahead pins).
          lookahead_slot_mask = (lookahead_slot_mask & ~s0_bit) | ((prev_la_mask | new_la_pins) & s0_bit);
        }
        lookahead_slot_mask = (lookahead_slot_mask & ~pre_pins) | ((prev_pre_mask | new_la_pins) & pre_pins);
        if (la_ad_n > 0) ad_run_updates_batch(la_ad_batch, la_ad_n, ad_cur_dt);
      }
    }
    kept_ok = s->valid;
    slot_show(&m, s);
    if (!ad_done) {
      gl_ad_pend ad_pend[MB_ADAPTIVE_MAX];
      int ad_n_upd = ad_collect_and_match(f, start, end, count, &m, reblurred, ad_cur_dt, ad_frame_serial, ad_pend);
      if (ad_n_upd > 0) {
        switched = 1;
        ad_run_updates(ad_pend, ad_n_upd, ad_cur_dt);
      }
    }
    ad_fill_uniforms();
  }
  if (switched) {
    main_state();
  } else if (shows) {
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, ad_tex[ad_cur] ? ad_tex[ad_cur] : stand_tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, bd_tex ? bd_tex : stand_tex);
    prog_gen++;
  }
  return shows;
}

// The program that draws instances of `kind` (-1 any) used, its uniforms set if they changed since it was last.
static void use_prog(int kind) {
  kprog *k = &progs[kind >= 0 && has_own_prog(kind) ? kind : MB_KINDS];
  prog_cur = k;
  prog_now = k->p;
  glUseProgram(k->p);
  if (k->gen == prog_gen) return;
  k->gen = prog_gen;
  if (k->vp >= 0) glUniform2f(k->vp, cur_vpw > 0.0f ? cur_vpw : (float)cur_w, cur_vph > 0.0f ? cur_vph : (float)cur_h);
  if (k->origin >= 0) glUniform2f(k->origin, cur_ox, cur_oy);
  if (k->scale >= 0) glUniform1f(k->scale, (float)cur_scale);
  if (k->top >= 0) glUniform1f(k->top, cur_lens_ph > 0 ? (float)cur_lens_ph : (float)(vy0 + vhp));
  if (k->map >= 0) glUniform4fv(k->map, 1, gmap);
  if (k->uvc >= 0) glUniform4fv(k->uvc, 1, guvc);
  if (k->gb >= 0) glUniform4fv(k->gb, 1, ggb);
  if (k->shape >= 0) glUniform4fv(k->shape, 8, gshape);
  if (k->rad >= 0) glUniform4fv(k->rad, 8, grad);
  if (k->n >= 0) glUniform1i(k->n, gn);
  if (k->through >= 0) glUniform1f(k->through, gthrough);
  if (k->glight >= 0) {
    float a = g_light_angle;
    glUniform2f(k->glight, cosf(a), sinf(a));
  }
  if (k->ad_box >= 0) glUniform4fv(k->ad_box, MB_ADAPTIVE_MAX, gad_box);
  if (k->ad_n >= 0) glUniform1i(k->ad_n, gad_n);
}

// the main programs' state (after the passes)
static void main_state(void) {
  glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
  glViewport(0, vy0, fb_w, vhp);
  if (cut_on) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(cut[0], cut[1], cut[2], cut[3]);
  } else {
    glDisable(GL_SCISSOR_TEST);
  }
  glEnable(GL_BLEND);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glActiveTexture(GL_TEXTURE5);
  glBindTexture(GL_TEXTURE_2D, ad_tex[ad_cur] ? ad_tex[ad_cur] : stand_tex);
  glActiveTexture(GL_TEXTURE4);
  glBindTexture(GL_TEXTURE_2D_ARRAY, catlas);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, atlas);
  // (the shapes' pieces before the backdrop: the unit left active as it was, 2, which a stream's SurfaceTexture is
  // latched on as its picture is asked for: Streams.show)
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, segs_tex ? segs_tex : stand_tex);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, bd_tex ? bd_tex : stand_tex);
  // (the programs' uniforms as they are now: each set again as it is next used)
  prog_gen++;
  use_prog(-1);
}

// The pieces of the frame's shapes (src/ui/shape.mbt: the instances of kind 12 after every one drawn) into segs_tex.

static void segs_upload(int32_t count) {
  int32_t d = count;
  while (d > 0 && (int)(fbuf[(d - 1) * MB_INST + 5] + 0.5f) == 12) d--;
  if (d == count) return;
  int rows = (int)(((size_t)(count - d) * (MB_INST / 4) + SEGS_ROW - 1) / SEGS_ROW);
  const float *pieces = fbuf + (size_t)d * MB_INST;
  size_t n = (size_t)(count - d) * MB_INST;
  // (the same as the texture holds: nothing to upload)
  if (segs_tex && rows <= segs_rows && n == segs_had_n && memcmp(segs_had, pieces, n * sizeof(float)) == 0) return;
  if (n > segs_had_cap) {
    segs_had_cap = n * 2;
    segs_had = realloc(segs_had, segs_had_cap * sizeof(float));
  }
  memcpy(segs_had, pieces, n * sizeof(float));
  segs_had_n = n;
  glActiveTexture(GL_TEXTURE3);
  if (!segs_tex || rows > segs_rows) {
    if (segs_tex) glDeleteTextures(1, &segs_tex);
    glGenTextures(1, &segs_tex);
    glBindTexture(GL_TEXTURE_2D, segs_tex);
    segs_rows = rows * 2;
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, SEGS_ROW, segs_rows);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }
  glBindTexture(GL_TEXTURE_2D, segs_tex);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SEGS_ROW, rows, GL_RGBA, GL_FLOAT, fbuf + (size_t)d * MB_INST);
}

// The frame presented last, kept (in fbuf): its instances and its ground, drawn again as a native view's picture
// changes (Android's Native.redraw), the app not asked.
static int32_t kept_count;
static double kept_r, kept_g, kept_b;
static void draw_kept(void);

static void canvas_shown(void);

// Whether the frame kept draws a native view's picture into the canvas (Android's views, Linux's players): what it
// shows changes under the same instances, so the frame is drawn whole.
static int views_drawn_in(void) {
  for (int32_t i = 0; i < kept_count; i++) {
    int tid = (int)fbuf[i * MB_INST + 20];
    if (tid <= MB_VIEW_TEX || tid > MB_LAYER_TEX) continue;
#ifdef MB_VIEWS_UNDER
    if (view_under(tid)) continue;
#endif
    return 1;
  }
  return 0;
}

// Physical-pixel offscreen tile layers (MB_LAYER_TEX + id): lazily allocated GL_RGBA8 texture + FBO.
typedef struct {
  int32_t id;
  GLuint tex;
  GLuint fb;
  int pw, ph;
} MBOffLayer;
static MBOffLayer *gl_layers;
static int gl_layers_n, gl_layers_cap;

static GLuint mb_layer_gl_tex(int id) {
  for (int i = 0; i < gl_layers_n; i++)
    if (gl_layers[i].id == id) return gl_layers[i].tex;
  return 0;
}

static void gl_layers_lost(void) {
  free(gl_layers);
  gl_layers = NULL;
  gl_layers_n = gl_layers_cap = 0;
}

static void gl_layer_destroy(int32_t id) {
  for (int i = 0; i < gl_layers_n; i++) {
    if (gl_layers[i].id != id) continue;
    if (gl_layers[i].fb) glDeleteFramebuffers(1, &gl_layers[i].fb);
    if (gl_layers[i].tex) glDeleteTextures(1, &gl_layers[i].tex);
    gl_layers[i] = gl_layers[--gl_layers_n];
    canvas_ok = 0;
    return;
  }
}

// GLES3 FBOs have Y=0 at the bottom, whereas callers supply top-down UV coordinates for kind == 4 layer samples.
static void flip_layer_uvs(float *buf, int32_t count) {
  for (int32_t i = 0; i < count; i++) {
    size_t b = (size_t)i * MB_INST;
    if ((int)(buf[b + 5] + 0.5f) == 4 && (int)buf[b + 20] > MB_LAYER_TEX) {
      buf[b + 13] = 1.0f - buf[b + 13];
      buf[b + 15] = -buf[b + 15];
    }
  }
}

// A frame presented (the host's mb_host_present, its glyphs in the atlases first): kept as floats, its groups' headers
// drawn out (groups.h), and drawn. A frame drawn whole (nothing said of its parts, all of it said, a picture missing
// from the last, a native view's picture in it) goes straight onto target_fb: through the canvas it would be drawn
// whole there and blitted again, every pixel read and written twice more (45 MB a frame of a fold's screen as a list
// scrolls), and a tiler's frame drawn whole on its chip is written out once. Glass reads what is drawn under it out of
// target_fb as well (capture: the surface's own framebuffer is read as GLES 3 reads any, its rows and the app's part,
// vy0 and vhp, the same). The canvas then holds no frame (canvas_ok): the first frame drawn in part after it is drawn
// whole into the canvas and shown, those after only the parts that differ.
static void gl_present_kept(int32_t count, double r, double g, double b);

// The floats a frame of `count` instances needs (`fbuf`'s room): its instances, its ground after them, and a row of
// the shapes' pieces more (segs_upload reads whole rows).
static size_t gl_frame_floats(int32_t count) {
  return (size_t)(count > 0 ? count : 0) * MB_INST + 2 * MB_INST + SEGS_ROW * 4;
}

static void gl_present(const double *buf, int32_t count, double r, double g, double b) {
  size_t n = gl_frame_floats(count);
  if (fcap < n) {
    fcap = n * 2;
    fbuf = (float *)realloc(fbuf, fcap * sizeof(float));
  }
  mb_groups_floats(fbuf, buf, count);
  flip_layer_uvs(fbuf, count);
  gl_present_kept(count, r, g, b);
}

static int32_t gl_layer_render(int32_t id, int32_t pw, int32_t ph, double scale, const double *inst, int32_t count) {
  if (id <= 0 || pw <= 0 || ph <= 0 || !(scale > 0) || count < 0 || (count > 0 && !inst)) return 0;
  MBOffLayer *L = NULL;
  for (int i = 0; i < gl_layers_n; i++) {
    if (gl_layers[i].id == id) {
      L = &gl_layers[i];
      break;
    }
  }
  if (!L) {
    if (gl_layers_n == gl_layers_cap) {
      int ncap = gl_layers_cap ? gl_layers_cap * 2 : 16;
      MBOffLayer *grown = (MBOffLayer *)realloc(gl_layers, (size_t)ncap * sizeof(MBOffLayer));
      if (!grown) return 0;
      gl_layers = grown;
      gl_layers_cap = ncap;
    }
    L = &gl_layers[gl_layers_n++];
    memset(L, 0, sizeof *L);
    L->id = id;
  }
  GLint prev_fb = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
  if (!L->fb || L->pw != pw || L->ph != ph) {
    if (L->fb) glDeleteFramebuffers(1, &L->fb);
    if (L->tex) glDeleteTextures(1, &L->tex);
    L->fb = L->tex = 0;
    L->pw = L->ph = 0;
    glGenTextures(1, &L->tex);
    glBindTexture(GL_TEXTURE_2D, L->tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, pw, ph);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &L->fb);
    glBindFramebuffer(GL_FRAMEBUFFER, L->fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, L->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      glDeleteFramebuffers(1, &L->fb);
      glDeleteTextures(1, &L->tex);
      L->fb = L->tex = 0;
      glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fb);
      return 0;
    }
    L->pw = pw;
    L->ph = ph;
  }
  size_t need = gl_frame_floats(count);
  float *lf = (float *)malloc(need * sizeof(float));
  if (!lf) {
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fb);
    return 0;
  }
  if (count > 0) {
    mb_groups_floats(lf, inst, count);
    flip_layer_uvs(lf, count);
  }
  float *kf = fbuf;
  size_t kc = fcap;
  int32_t kn = kept_count;
  double kr = kept_r, kg = kept_g, kb = kept_b;
  int kfw = fb_w, kfh = fb_h;
  double kw = cur_w, kh = cur_h, ks = cur_scale;
  int prev_over = over_other;
  fbuf = lf;
  fcap = need;
  kept_count = count;
  kept_r = kept_g = kept_b = 0.0;
  fb_w = pw;
  fb_h = ph;
  cur_scale = scale;
  cur_w = (double)pw / scale;
  cur_h = (double)ph / scale;
  over_other = 1;
  int prev_ad_mode = ad_mode;
  ad_mode = -1;
  draw_into(L->fb, 0);
  ad_mode = prev_ad_mode;
  over_other = prev_over;
  fbuf = kf;
  fcap = kc;
  kept_count = kn;
  kept_r = kr;
  kept_g = kg;
  kept_b = kb;
  fb_w = kfw;
  fb_h = kfh;
  cur_w = kw;
  cur_h = kh;
  cur_scale = ks;
  free(lf);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fb);
  canvas_ok = 0;
  return 1;
}

// The frame in fbuf (`count` instances as floats, its groups drawn out: gl_present's, or Android's made on the app's
// thread and handed over) kept and drawn, as gl_present says.
static void gl_present_drawn(int32_t count, double r, double g, double b);
static void gl_present_kept(int32_t count, double r, double g, double b) {
  ad_poll_readbacks();
  glass_drawn = 0;
  ad_mode = 1;
  ad_cur_dt = ad_next_dt();
  ad_frame_serial++;
  int whole = !dirty_told || dirty_whole || pictures_missing || views_drawn_in();
  if (!whole && gl_adaptive_settling()) {
    for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
      if (ad_slots[i].active && ad_slots[i].settle_left > 0.0)
        gl_dirty_add(ad_slots[i].cx - ad_slots[i].hx - 4.0, ad_slots[i].cy - ad_slots[i].hy - 4.0,
                     ad_slots[i].hx * 2.0 + 8.0, ad_slots[i].hy * 2.0 + 8.0);
    }
    if (!dirty_whole && !mb_glass_damage_expand(fbuf, count, dirty, &dirty_n, fb_w / cur_scale, fb_h / cur_scale, cur_scale))
      dirty_whole = 1;
  }
  gl_present_drawn(count, r, g, b);
  ad_rb_finish(ad_frame_serial);
  ad_finalize_frame(whole, ad_frame_serial);
  glass_idle_tick();
}

static int gl_adaptive_step(void) {
  if (!gl_adaptive_settling() || kept_count <= 0 || !fbuf) return 0;
  ad_poll_readbacks();
  int need = 0;
  for (int i = 0; i < MB_ADAPTIVE_MAX; i++) {
    mb_ad_slot *s = &ad_slots[i];
    if (!s->active || s->settle_left <= 0.0) continue;
    if (ad_conv_serial[i] >= s->settle_serial) {
      s->settle_left = 0.0;
    } else {
      need = 1;
    }
  }
  if (!need) {
    __atomic_store_n(&ad_settling, 0, __ATOMIC_RELEASE);
    if (!offscreen) {
      if (canvas_ok && canvas_fb) canvas_shown();
      else draw_kept();
    }
    return 0;
  }
  dirty_told = 1;
  dirty_whole = dirty_n = 0;
  gl_present_kept(kept_count, kept_r, kept_g, kept_b);
  return 1;
}

static void gl_present_drawn(int32_t count, double r, double g, double b) {
  kept_count = count;
  kept_r = r;
  kept_g = g;
  kept_b = b;
  if (views_drawn_in()) dirty_told = 0;
  if (offscreen) {
    draw_parts(target_fb, canvas_ok);
    canvas_ok = 1;
    return;
  }
  if (!dirty_told || dirty_whole || pictures_missing) {
    dirty_told = dirty_whole = dirty_n = 0;
    canvas_ok = 0;
    draw_into(target_fb, 0);
    pictures_missing = encode_missing;
    return;
  }
  ensure_canvas();
  if (!canvas_fb) {
    dirty_told = dirty_whole = dirty_n = 0;
    draw_into(target_fb, 0);
    return;
  }
  draw_parts(canvas_fb, canvas_ok);
  canvas_ok = 1;
  canvas_shown();
}

// The canvas onto the surface (target_fb), whole.
static void canvas_shown(void) {
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, canvas_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fb);
  glBlitFramebuffer(0, 0, fb_w, fb_h, 0, 0, fb_w, fb_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
}

// The frame presented last, drawn again into a framebuffer of its own the surface's size (as on the surface: what the
// surface holds is gone once it is shown): read_fb, bound as it returns.
static GLuint read_fb, read_tex;
static int read_w, read_h;

static void draw_kept_read(void) {
  if (!read_fb || read_w != fb_w || read_h != fb_h) {
    if (read_fb) {
      glDeleteFramebuffers(1, &read_fb);
      glDeleteTextures(1, &read_tex);
    }
    glGenTextures(1, &read_tex);
    glBindTexture(GL_TEXTURE_2D, read_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, fb_w, fb_h);
    glGenFramebuffers(1, &read_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, read_fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, read_tex, 0);
    read_w = fb_w;
    read_h = fb_h;
  }
  int prev_ad_mode = ad_mode;
  if (ad_mode > 0) ad_mode = 0;
  draw_into(read_fb, 0);
  ad_mode = prev_ad_mode;
  glBindFramebuffer(GL_FRAMEBUFFER, read_fb);
}

// What drawing holds beyond what frames need given back (the window hidden, the app put away; its context current):
// glass's textures and the read back's framebuffer. A frame after makes what it needs again as it draws.
void mb_render_trim(void) {
  glass_free();
  blur_layers_free();
  glass_idle_frames = 0;
  if (read_fb) {
    glDeleteFramebuffers(1, &read_fb);
    glDeleteTextures(1, &read_tex);
    read_fb = read_tex = 0;
    read_w = read_h = 0;
  }
}

// That frame's pixels in the rectangle x, y, w × h (from the top left) into `out` as RGBA, premultiplied, rows from
// the top (host_native.mbt drawn_pixels; on the thread MoonBit runs on, its context current): 1 when read, 0 where the
// rectangle is not all in the frame.
static int32_t gles_drawn_pixels(int32_t x, int32_t y, int32_t w, int32_t h, moonbit_bytes_t out) {
  if (kept_count <= 0 || fb_w <= 0 || fb_h <= 0 || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > fb_w || y + h > fb_h)
    return 0;
  if ((size_t)Moonbit_array_length(out) < (size_t)w * h * 4) return 0;
  GLuint was = target_fb;
  draw_kept_read();
  size_t row = (size_t)w * 4;
  unsigned char *px = malloc(row * h);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  // (GL's rows from the foot: the app's top is the surface's)
  glReadPixels(x, fb_h - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  for (int r = 0; r < h; r++) memcpy(out + row * r, px + row * (h - 1 - r), row);
  free(px);
  glBindFramebuffer(GL_FRAMEBUFFER, was);
  return 1;
}

// Instances never presented (a shot's, mbui/src/ui/shot.mbt) drawn as a frame is, but `scale` pixels a point into a
// picture of their own w × h (their 0, 0 its top left), on the ground r, g, b, and its pixels into `out` as
// mb_render_drawn_pixels gives the frame's: the frame kept, and the size frames are drawn at, set aside while they are
// drawn in its place, then put back (what a native view's picture redraws is the frame as it was). 1 when read.
static int32_t gles_pixels_of(const double *inst, int32_t count, double r, double g, double b, double scale,
                              int32_t w, int32_t h, moonbit_bytes_t out) {
  if (count <= 0 || !(scale > 0) || w <= 0 || h <= 0) return 0;
#ifdef GL_ATLAS_FLUSH
  GL_ATLAS_FLUSH();
#endif
  float *kf = fbuf;
  size_t kc = fcap;
  int32_t kn = kept_count;
  double kr = kept_r, kg = kept_g, kb = kept_b;
  int kfw = fb_w, kfh = fb_h;
  double kw = cur_w, kh = cur_h, ks = cur_scale;
  size_t n = (size_t)count * MB_INST;
  // (two extra slots for ground + blur-layer composite, plus a row of the shapes' pieces: segs_upload reads whole rows)
  fcap = n + 2 * MB_INST + SEGS_ROW * 4;
  fbuf = malloc(fcap * sizeof(float));
  for (size_t i = 0; i < n; i++) fbuf[i] = (float)inst[i];
  kept_count = count;
  kept_r = r;
  kept_g = g;
  kept_b = b;
  fb_w = w;
  fb_h = h;
  cur_scale = scale;
  cur_w = w / scale;
  cur_h = h / scale;
  int prev_ad_mode = ad_mode;
  ad_mode = -1;
  int32_t ok = gles_drawn_pixels(0, 0, w, h, out);
  ad_mode = prev_ad_mode;
  free(fbuf);
  fbuf = kf;
  fcap = kc;
  kept_count = kn;
  kept_r = kr;
  kept_g = kg;
  kept_b = kb;
  fb_w = kfw;
  fb_h = kfh;
  cur_w = kw;
  cur_h = kh;
  cur_scale = ks;
  return ok;
}

#ifndef MB_HOST_THREADS
int32_t mb_render_drawn_pixels(int32_t x, int32_t y, int32_t w, int32_t h, moonbit_bytes_t out) {
  return gles_drawn_pixels(x, y, w, h, out);
}

int32_t mb_render_pixels_of(const double *inst, int32_t count, double r, double g, double b, double scale, int32_t w,
                            int32_t h, moonbit_bytes_t out) {
  return gles_pixels_of(inst, count, r, g, b, scale, w, h, out);
}
#endif

// The frame kept drawn whole onto the surface again (a native view's picture changed: Android's Native.redraw),
// straight onto it as a frame presented whole is (gl_present): the canvas then holds no frame.
static void draw_kept(void) {
  draw_into(target_fb, 0);
  if (!offscreen) canvas_ok = 0;
}

// The frame kept drawn into framebuffer `fb` (the surface's size): `part` 1, only the part `cut` over what it holds
// (the frame's ground there first), else all of it, cleared to the ground. Whether a picture was missing in
// encode_missing.
// Instances first to first + n drawn (GLES has no base instance: the attributes start at the first).
static void draw_run(int32_t first, int32_t n) {
  for (int i = 0; i < MB_INST / 4; i++)
    glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, MB_INST * 4, (void *)(intptr_t)(first * MB_INST * 4 + i * 16));
  glDrawArraysInstanced(GL_TRIANGLES, 0, 6, n);
}

// Instances start to end drawn, each that pays for it in strips (bands.h), the
// rest as they come.
static void draw_banded(int32_t start, int32_t end, int32_t count) {
  int top = vy0 + vhp;
  MBStrip to = cut_on ? mb_strip(cut[0], top - (cut[1] + cut[3]), cut[0] + cut[2], top - cut[1]) : mb_strip(0, 0, fb_w, vhp);
  MBStrip st[MB_STRIPS];
  int32_t s = start;
  for (int32_t i = start; i < end; i++) {
    if (!mb_band_kind(fbuf, i)) continue;
    int n = mb_bands(fbuf, i, count, (float)cur_scale, to, st);
    if (n < 0) continue;
    if (i > s) draw_run(s, i - s);
    glEnable(GL_SCISSOR_TEST);
    for (int k = 0; k < n; k++) {
      if (k == 0 || st[k].flat != st[k - 1].flat) {
        glUseProgram(st[k].flat ? prog_flat : prog_now);
        if (st[k].flat) glUniform2f(uf_vp, (float)cur_w, (float)cur_h);
      }
      glScissor(st[k].x0, top - st[k].y1, st[k].x1 - st[k].x0, st[k].y1 - st[k].y0);
      draw_run(i, 1);
    }
    if (n > 0 && st[n - 1].flat) glUseProgram(prog_now);
    if (cut_on) glScissor(cut[0], cut[1], cut[2], cut[3]);
    else glDisable(GL_SCISSOR_TEST);
    s = i + 1;
  }
  if (end > s) draw_run(s, end - s);
}

static void draw_into(GLuint fb, int part) {
  int32_t count = kept_count;
  double r = kept_r, g = kept_g, b = kept_b;
  GLuint was = target_fb;
  target_fb = fb;
  encode_missing = 0;
  cut_on = part == 1;
  // the app drawn in the surface's top part, as tall as it was told (the soft keyboard over the rest)
  int vh = (int)(cur_h * cur_scale + 0.5);
  vy0 = 0;
  vhp = fb_h;
  if (vh > 0 && vh < fb_h) {
    vy0 = fb_h - vh;
    vhp = vh;
  }
  // (where the pieces of the frame's shapes start: what a capture is of, with what is before it)
  int32_t pieces = count;
  while (pieces > 0 && (int)(fbuf[(pieces - 1) * MB_INST + 5] + 0.5f) == 12) pieces--;
  // Pre-scan glass captures before binding target_fb: size blur levels once, record group grabs for lookahead
  // batching, and batch adaptive updates for blur-cache-hit liquid glass groups into a single pass.
  int levels_sized = 0, has_blur_layer = 0;
  gl_glass_group groups[MB_GLASS_GROUPS_MAX];
  int groups_n = 0;
  gl_pre_hit pre_hits[MB_GLASS_GROUPS_MAX];
  int pre_hits_n = 0;
  unsigned pre_slots_gen = slots_gen;
  if (pieces > 0) {
    int max_w = 0, max_h = 0, scan_bound = 0, any_glass = 0, scan_bd = 0;
    for (int32_t s0 = 0; s0 < pieces;) {
      int sk = (int)(fbuf[s0 * MB_INST + 5] + 0.5f);
      if (sk == 14) { scan_bd++; s0++; continue; }
      if (sk == 15) { has_blur_layer = 1; if (scan_bd > 0) scan_bd--; s0++; continue; }
      if (sk == 8) {
        int32_t cn = (int32_t)(fbuf[s0 * MB_INST + 12] + 0.5f);
        if (cn < 0) cn = 0;
        if (s0 + 1 + cn > pieces) cn = pieces - s0 - 1;
        s0 += 1 + cn;
        continue;
      }
      int stid = (int)fbuf[s0 * MB_INST + 20];
      int32_t e0 = s0 + 1;
      if (scan_bd == 0 && stid < 0) {
        any_glass = 1;
        while (e0 < pieces && (int)fbuf[e0 * MB_INST + 20] == stid) {
          int ek = (int)(fbuf[e0 * MB_INST + 5] + 0.5f);
          if (ek == 8 || ek == 14 || ek == 15) break;
          e0++;
        }
        if (stid != scan_bound) {
          scan_bound = stid;
          Grab gm;
          int has_g = grab_of(&gm, fbuf, s0, e0, count);
          int in_cut = has_g && grab_in_cut(&gm);
          if (has_g) {
            if (gm.w > max_w) max_w = gm.w;
            if (gm.h > max_h) max_h = gm.h;
          }
          if ((sk == 5 || sk == 6) && groups_n < MB_GLASS_GROUPS_MAX) {
            int dup = 0;
            for (int gi = 0; gi < groups_n; gi++) {
              if (groups[gi].tid == stid) { dup = 1; break; }
            }
            if (!dup) {
              gl_glass_group *gg = &groups[groups_n++];
              gg->start = s0;
              gg->end = e0;
              gg->tid = stid;
              gg->skind = sk;
              gg->has_grab = has_g;
              gg->in_cut_rect = in_cut;
              gg->pre_missed = 0;
              if (has_g) gg->gm = gm;
            }
          }
        }
      }
      s0 = e0;
    }
    if (any_glass) {
      levels_sized = 1;
      if (max_w > lv_w || max_h > lv_h) {
        ensure_levels(max_w, max_h);
      } else if (ad_mode > 0 && lv[0].t && max_w > 0 && max_h > 0) {
        if (max_w > lv_frame_max_w) lv_frame_max_w = max_w;
        if (max_h > lv_frame_max_h) lv_frame_max_h = max_h;
      }
      if (ad_mode > 0 && ad_cur_dt >= 0.0 && groups_n > 0) {
        gl_ad_batch pre_batch[MB_ADAPTIVE_MAX];
        int pre_n = 0;
        double stretch_w = ((int)(cur_w * cur_scale + 0.5) != fb_w) ? cur_w : 0.0;
        double stretch_h = (vhp == fb_h && (int)(cur_h * cur_scale + 0.5) != fb_h) ? cur_h : 0.0;
        double key[9] = {fb_w, fb_h, cur_scale, r, g, b, over_other, stretch_w, stretch_h};
        for (int gi = 0; gi < groups_n; gi++) {
          gl_glass_group *gg = &groups[gi];
          if (gg->skind != 6 || !gg->in_cut_rect) continue;
          glass_slot *sl = slot_match_under(fbuf, gg->start, count, pieces, &gg->gm, key, 0u);
          if (sl && sl->bt) {
            if (pre_hits_n < MB_GLASS_GROUPS_MAX) {
              gl_pre_hit *ph = &pre_hits[pre_hits_n++];
              ph->tid = gg->tid;
              ph->start = gg->start;
              ph->slot = sl;
              ph->gen = sl->gen;
              ph->grab = gg->gm;
              ph->ad_done = 0;
              ph->reblurred = 0;
            }
            if (pre_n < MB_ADAPTIVE_MAX) {
              gl_ad_pend tmp[MB_ADAPTIVE_MAX];
              int nu = ad_collect_and_match(fbuf, gg->start, gg->end, count, &gg->gm, 0, ad_cur_dt, ad_frame_serial, tmp);
              if (nu > 0) {
                float map[4], uvc[4];
                grab_map_uvc(&gg->gm, sl->bw, sl->bh, map, uvc);
                for (int i = 0; i < nu && pre_n < MB_ADAPTIVE_MAX; i++) {
                  pre_batch[pre_n].p = tmp[i];
                  pre_batch[pre_n].bt = sl->bt;
                  memcpy(pre_batch[pre_n].map, map, sizeof map);
                  memcpy(pre_batch[pre_n].uvc, uvc, sizeof uvc);
                  pre_n++;
                }
              }
            }
          } else {
            gg->pre_missed = 1;
          }
        }
        pre_slots_gen = slots_gen;
        if (pre_n > 0) {
          ad_run_updates_batch(pre_batch, pre_n, ad_cur_dt);
        }
      }
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
  glViewport(0, 0, fb_w, fb_h);
  if (!cut_on) {
    glDisable(GL_SCISSOR_TEST);
    if (over_other) glClearColor(0, 0, 0, 0);
    else glClearColor((float)r, (float)g, (float)b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glViewport(0, vy0, fb_w, vhp);
  if (count <= 0 && !cut_on) {
    target_fb = was;
    return;
  }
  // Upload shape segments (trailing kind-12 instances) to segs_tex, and only drawn instances [0, pieces) to the VBO.
  size_t n = (size_t)pieces * MB_INST;
  segs_upload(count);
  bd_tex = 0;
  memset(gmap, 0, sizeof gmap);
  memset(guvc, 0, sizeof guvc);
  memset(ggb, 0, sizeof ggb);
  gn = 0;
  gthrough = 0.0f;
  ad_fill_uniforms();
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  if (!cut_on && !has_blur_layer) {
    glBufferData(GL_ARRAY_BUFFER, n * sizeof(float), fbuf, GL_STREAM_DRAW);
  } else {
    glBufferData(GL_ARRAY_BUFFER, (n + 2 * MB_INST) * sizeof(float), NULL, GL_STREAM_DRAW);
    if (n > 0) glBufferSubData(GL_ARRAY_BUFFER, 0, n * sizeof(float), fbuf);
    if (cut_on) {
      float gr[MB_INST];
      memset(gr, 0, sizeof gr);
      gr[0] = -16.0f; gr[1] = -16.0f; gr[2] = (float)cur_w + 32.0f; gr[3] = (float)cur_h + 32.0f;
      if (!over_other) {
        gr[8] = (float)r; gr[9] = (float)g; gr[10] = (float)b; gr[11] = 1.0f;
      }
      gr[16] = -1e9f; gr[17] = -1e9f; gr[18] = 1e9f; gr[19] = 1e9f;
      glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(n * sizeof(float)), sizeof gr, gr);
    }
  }
  main_state();
  if (cut_on) {
    for (int i = 0; i < MB_INST / 4; i++)
      glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, MB_INST * 4, (void *)(intptr_t)(pieces * MB_INST * 4 + i * 16));
    glDisable(GL_BLEND);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 1);
    glEnable(GL_BLEND);
  }
  // one draw per run of instances showing the same image (slot 20; 0: none, the atlas stands in; a native view's
  // picture where it shows, kind 9: nothing until it has one) of one kind (its program: use_prog), or sharing one
  // capture of glass (negative); GLES has no base instance, so the attributes start at the run instead
  typedef struct {
    int px0, pb0, w, h, top, active;
    float sigma;
    GLuint prev_fb;
    int prev_cut_on;
    int prev_cut[4];
  } BlurLayer;
  BlurLayer bstack[MB_BLUR_LAYERS];
  int bdepth = 0, bover = 0;
  int32_t start = 0;
  int bound = 0, shows = 0;
  unsigned frame_clock = slots_clock;
  gl_pre_hit la_hits[MB_GLASS_GROUPS_MAX];
  int la_hits_n = 0;
  lookahead_slot_mask = 0;
  while (start < pieces) {
    int tid = (int)fbuf[start * MB_INST + 20];
    int kind = (int)(fbuf[start * MB_INST + 5] + 0.5f);
    if (kind == 14) {
      if (bdepth >= MB_BLUR_LAYERS) {
        bover++;
        start++;
        continue;
      }
      const float *a = fbuf + (size_t)start * MB_INST;
      float sc = (float)cur_scale;
      float sigma = fabsf(a[7]);
      float margin = sigma > 0.0f ? ceilf(3.0f * sigma) + 2.0f : 0.0f;
      int top = vy0 + vhp;
      int px0 = (int)floorf((a[0] - margin) * sc), px1 = (int)ceilf((a[0] + a[2] + margin) * sc);
      int pb0 = top - (int)ceilf((a[1] + a[3] + margin) * sc), pb1 = top - (int)floorf((a[1] - margin) * sc);
      if (px0 < 0) px0 = 0;
      if (px1 > fb_w) px1 = fb_w;
      if (pb0 < vy0) pb0 = vy0;
      if (pb1 > top) pb1 = top;
      int bw = px1 - px0, bh = pb1 - pb0;
      BlurLayer L = {px0, pb0, bw, bh, top, 0, sigma, target_fb, cut_on, {cut[0], cut[1], cut[2], cut[3]}};
      if (sigma > 0.0f && bw > 0 && bh > 0 && ensure_blur_layer(bdepth, fb_w, fb_h)) {
        L.active = 1;
        target_fb = bl_lv[bdepth].fb;
        cut_on = 1;
        cut[0] = px0; cut[1] = pb0; cut[2] = bw; cut[3] = bh;
        glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
        glViewport(0, vy0, fb_w, vhp);
        glEnable(GL_SCISSOR_TEST);
        glScissor(px0, pb0, bw, bh);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
      } else {
        cut_on = 1;
        cut[0] = 0; cut[1] = 0; cut[2] = 0; cut[3] = 0;
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 0, 0);
      }
      bstack[bdepth++] = L;
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
      BlurLayer L = bstack[--bdepth];
      if (!L.active) {
        target_fb = L.prev_fb;
        cut_on = L.prev_cut_on;
        memcpy(cut, L.prev_cut, sizeof cut);
        main_state();
        start++;
        continue;
      }
      if (ensure_blur_py(L.w, L.h)) {
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, bl_py[0].fb);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, bl_lv[bdepth].fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bl_py[0].fb);
        glBlitFramebuffer(L.px0, L.pb0, L.px0 + L.w, L.pb0 + L.h, 0, 0, L.w, L.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        float sc = (float)cur_scale;
        float sp = L.sigma * sc;
        int k = 0;
        while (k < 3 && sp / (float)(1 << (k + 1)) >= 2.0f) k++;
        float sk = sp / (float)(1 << k);
        float pre = k > 0 ? 0.75f * (1.0f - powf(0.25f, (float)k)) / 3.0f + 1.0f / 6.0f : 0.0f;
        float sg = sqrtf(sk * sk - pre > 0.0f ? sk * sk - pre : 0.0f);
        int lw[4] = {L.w, 0, 0, 0}, lh[4] = {L.h, 0, 0, 0};
        for (int j = 1; j <= 3; j++) {
          lw[j] = (lw[j - 1] + 1) / 2;
          lh[j] = (lh[j - 1] + 1) / 2;
        }
        glUseProgram(post);
        for (int j = 1; j <= k; j++) {
          glBindFramebuffer(GL_FRAMEBUFFER, bl_py[j].fb);
          glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
          glClear(GL_COLOR_BUFFER_BIT);
          post_pass(&bl_py[j - 1], lw[j - 1], lh[j - 1], &bl_py[j], lw[j], lh[j], 1, 0, 0, 0);
        }
        if (sg > 0.3f) {
          glBindFramebuffer(GL_FRAMEBUFFER, bl_py[4].fb);
          glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
          glClear(GL_COLOR_BUFFER_BIT);
          post_pass(&bl_py[k], lw[k], lh[k], &bl_py[4], lw[k], lh[k], 2, sg, 1, 0);
          glBindFramebuffer(GL_FRAMEBUFFER, bl_py[k].fb);
          glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
          glClear(GL_COLOR_BUFFER_BIT);
          post_pass(&bl_py[4], lw[k], lh[k], &bl_py[k], lw[k], lh[k], 2, sg, 0, 1);
        }
        target_fb = L.prev_fb;
        cut_on = L.prev_cut_on;
        memcpy(cut, L.prev_cut, sizeof cut);
        main_state();
        float comp[MB_INST];
        memcpy(comp, fbuf + (size_t)start * MB_INST, MB_INST * sizeof(float));
        float d = (float)(1 << k);
        comp[0] = (float)L.px0 / sc;
        comp[1] = (float)(L.top - (L.pb0 + L.h)) / sc;
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
        comp[13] = (float)L.h / (d * (float)bl_py[k].h);
        comp[14] = (float)L.w / (d * (float)bl_py[k].w);
        comp[15] = -(float)L.h / (d * (float)bl_py[k].h);
        comp[20] = 0.0f; comp[21] = 0.0f; comp[22] = 0.0f; comp[23] = 0.0f;
        comp[32] = 0.0f; comp[33] = 0.0f; comp[34] = 0.0f; comp[35] = 0.0f;
        comp[36] = 0.0f; comp[37] = 0.0f; comp[38] = 0.0f; comp[39] = 0.0f;
        use_prog(4);
        set_img_mips(1);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bl_py[k].t);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)((size_t)(pieces + 1) * MB_INST * sizeof(float)),
                        MB_INST * sizeof(float), comp);
        draw_run(pieces + 1, 1);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, stand_tex);
      } else {
        target_fb = L.prev_fb;
        cut_on = L.prev_cut_on;
        memcpy(cut, L.prev_cut, sizeof cut);
        main_state();
      }
      start++;
      continue;
    }
    if (kind == 8) {
      int32_t child_n = (int32_t)(fbuf[start * MB_INST + 12] + 0.5f);
      if (child_n < 0) child_n = 0;
      if (start + 1 + child_n > count) child_n = count - start - 1;
      const float *hb = fbuf + (size_t)start * MB_INST;
      if (child_n <= 0 || hb[11] <= 0.001f || hb[2] <= 0.0f || hb[3] <= 0.0f) {
        start += 1 + child_n;
        continue;
      }
      glass_drawn = 1;
      float sc_f = (float)cur_scale;
      float blur_pt = fmaxf(hb[7], 0.0f);
      float refr_pt = fmaxf(hb[14], 0.0f);
      float margin = blur_pt * 3.0f + refr_pt + 4.0f;
      float x0 = hb[0], y0 = hb[1], x1 = hb[0] + hb[2], y1 = hb[1] + hb[3];
      for (int32_t ci = start + 1; ci < start + 1 + child_n; ci++) {
        const float *cb_f = fbuf + (size_t)ci * MB_INST;
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
      int max_w = fb_w > 2048 ? fb_w : 2048;
      int max_h = fb_h > 2048 ? fb_h : 2048;
      if (pw < 1) pw = 1;
      if (ph < 1) ph = 1;
      if (pw > max_w) pw = max_w;
      if (ph > max_h) ph = max_h;
      ensure_lens_levels(pw, ph);
      GLuint prev_bd = bd_tex;
      // Pass 1: render children into lens_lv[0]
      glBindFramebuffer(GL_FRAMEBUFFER, lens_lv[0].fb);
      // (u_bd off any lens level: one bound there by a lens before is drawn into now)
      glActiveTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_2D, stand_tex);
      glViewport(0, 0, pw, ph);
      glDisable(GL_SCISSOR_TEST);
      glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glEnable(GL_BLEND);
      cur_ox = ox; cur_oy = oy; cur_vpw = (float)pw / sc_f; cur_vph = (float)ph / sc_f; cur_lens_ph = ph;
      prog_gen++;
      int32_t cs = start + 1, cend_all = start + 1 + child_n;
      while (cs < cend_all) {
        int ctid = (int)fbuf[cs * MB_INST + 20];
        int ck = (int)(fbuf[cs * MB_INST + 5] + 0.5f);
        if (ck == 5 || ck == 6 || ck == 8 || ck == 9 || ck == 12 || ck == 14 || ck == 15 || ctid < 0) {
          cs++;
          continue;
        }
        int cown = has_own_prog(ck);
        int32_t ce = cs + 1;
        while (ce < cend_all && (int)fbuf[ce * MB_INST + 20] == ctid) {
          int kk = (int)(fbuf[ce * MB_INST + 5] + 0.5f);
          if (kk == 5 || kk == 6 || kk == 8 || kk == 9 || kk == 12 || kk == 14 || kk == 15) break;
          if (cown ? kk != ck : has_own_prog(kk)) break;
          ce++;
        }
        GLuint ct = ctid ? image_tex(ctid) : stand_tex;
        if (ctid > MB_STREAM_TEX && !ct) { cs = ce; continue; }
        if (ctid > 0 && ctid <= MB_STREAM_TEX && !ct) encode_missing = 1;
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ct ? ct : stand_tex);
        use_prog(cown ? ck : -1);
        set_img_mips(ct ? image_mips(ctid) : 1);
        draw_run(cs, ce - cs);
        cs = ce;
      }
      // Pass 2: separable Gaussian blur in RGBA8
      float sp = blur_pt * sc_f;
      if (sp > 0.25f) {
        glDisable(GL_BLEND);
        glUseProgram(post);
        unbind_post_extra();
        post_pass(&lens_lv[0], pw, ph, &lens_lv[1], pw, ph, 2, sp, 1.0f, 0.0f);
        post_pass(&lens_lv[1], pw, ph, &lens_lv[0], pw, ph, 2, sp, 0.0f, 1.0f);
      }
      // Pass 3: composite refracted & SDF-clipped foreground lens onto target
      cur_ox = 0.0f; cur_oy = 0.0f; cur_vpw = 0.0f; cur_vph = 0.0f; cur_lens_ph = 0;
      bd_tex = lens_lv[0].t;
      float W = (float)lens_lv[0].w, H = (float)lens_lv[0].h;
      gmap[0] = sc_f / W; gmap[1] = -sc_f / H;
      gmap[2] = -(ox * sc_f) / W; gmap[3] = ((float)ph + oy * sc_f) / H;
      guvc[0] = 0.5f / W; guvc[1] = 0.5f / H;
      guvc[2] = ((float)pw - 0.5f) / W; guvc[3] = ((float)ph - 0.5f) / H;
      main_state();
      use_prog(8);
      draw_run(start, 1);
      bd_tex = prev_bd;
      glActiveTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_2D, bd_tex ? bd_tex : stand_tex);
      bound = 0;
      start += 1 + child_n;
      continue;
    }
    // (kinds without their own program one run, as before)
    // (liquid outside a capture keeps the program of any kind: the merged liquid's has no single shape or droplet)
    int own = tid >= 0 && kind != 6 && has_own_prog(kind);
    int fast_rect = tid == 0 && kind == 0 && prog_rect && fast_rect_enabled() && is_simple_rect(fbuf, start);
    int32_t end = start + 1;
    while (end < pieces && (int)fbuf[end * MB_INST + 20] == tid) {
      int k = (int)(fbuf[end * MB_INST + 5] + 0.5f);
      if (k == 8 || k == 14 || k == 15) break;
      if (own ? k != kind : tid >= 0 && has_own_prog(k)) break;
      if (tid == 0 && kind == 0 && prog_rect && fast_rect_enabled() && is_simple_rect(fbuf, end) != fast_rect) break;
      end++;
    }
    if (tid < 0) {
      glass_drawn = 1;
      if (!levels_sized) {
        levels_sized = 1;
        int max_w = 0, max_h = 0, scan_bound = 0;
        for (int32_t s0 = start; s0 < pieces;) {
          int stid = (int)fbuf[s0 * MB_INST + 20];
          int32_t e0 = s0 + 1;
          if (stid < 0) {
            while (e0 < pieces && (int)fbuf[e0 * MB_INST + 20] == stid) e0++;
            if (stid != scan_bound) {
              scan_bound = stid;
              Grab gm;
              if (grab_of(&gm, fbuf, s0, e0, count)) {
                if (gm.w > max_w) max_w = gm.w;
                if (gm.h > max_h) max_h = gm.h;
              }
            }
          }
          s0 = e0;
        }
        if (max_w > lv_w || max_h > lv_h) {
          ensure_levels(max_w, max_h);
          main_state();
        }
      }
      if (tid != bound) {
        int in_cut = 1;
        if (cut_on && bdepth == 0) {
          Grab gm;
          int found = 0;
          for (int h = 0; h < pre_hits_n; h++) {
            if (pre_hits[h].tid == tid && pre_hits[h].start == start) {
              gm = pre_hits[h].grab;
              found = 1;
              break;
            }
          }
          if (!found) {
            for (int h = 0; h < la_hits_n; h++) {
              if (la_hits[h].tid == tid && la_hits[h].start == start) {
                gm = la_hits[h].grab;
                found = 1;
                break;
              }
            }
          }
          if (!found) {
            for (int gi = 0; gi < groups_n; gi++) {
              if (groups[gi].tid == tid && groups[gi].start == start) {
                found = groups[gi].has_grab;
                if (found) gm = groups[gi].gm;
                break;
              }
            }
          }
          if (!found) found = grab_of(&gm, fbuf, start, end, count);
          in_cut = found && grab_in_cut(&gm);
        }
        shows = in_cut ? capture(fbuf, start, end, count, pieces, r, g, b, frame_clock,
                                 bdepth == 0 ? pre_hits : NULL, bdepth == 0 ? pre_hits_n : 0,
                                 bdepth == 0 ? la_hits : NULL, bdepth == 0 ? &la_hits_n : NULL,
                                 bdepth == 0 ? groups : NULL, bdepth == 0 ? groups_n : 0, pre_slots_gen)
                       : 0;
        bound = tid;
      }
      if (shows) {
        // merged liquid (merge > 0): the group as one shape, drawn once; unmerged liquid and frosted: each instance
        int merged_liquid = kind == 6 && fbuf[start * MB_INST + 23] > 0.0f;
        int gkind = kind == 5 ? 5 : (kind == 6 ? (merged_liquid ? 6 : MB_KIND_LIQUID_SINGLE) : -1);
        use_prog(gkind);
        for (int i = 0; i < MB_INST / 4; i++)
          glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, MB_INST * 4, (void *)(intptr_t)(start * MB_INST * 4 + i * 16));
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, merged_liquid ? 1 : end - start);
      }
      start = end;
      continue;
    }
    if (fast_rect) {
      if (prog_now != prog_rect) {
        prog_now = prog_rect;
        glUseProgram(prog_rect);
      }
      glUniform2f(ur_vp, (float)cur_w, (float)cur_h);
      glUniform1f(ur_scale, (float)cur_scale);
      glUniform1f(ur_top, (float)(vy0 + vhp));
    } else {
      use_prog(own ? kind : -1);
    }
    GLuint t = tid ? image_tex(tid) : stand_tex;
    if (!t && tid > 0 && tid <= MB_STREAM_TEX) encode_missing = 1;
    if (t) {  // 0: lost with the context, decoding again; a stream's or a view's with no picture yet
      if (tid > MB_VIEW_TEX && tid <= MB_LAYER_TEX) view_parts_in(tid, start, end);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, t);
      if (!fast_rect) set_img_mips(tid ? image_mips(tid) : 1);
      for (int i = 0; i < MB_INST / 4; i++)
        glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, MB_INST * 4, (void *)(intptr_t)(start * MB_INST * 4 + i * 16));
#ifdef MB_VIEWS_UNDER
      // (a view under the surface: its picture a white texel, so what kind 9 draws is how much of each pixel it covers,
      // and that much of the canvas is taken away there: a hole the view shows through, its round edge as soft as a
      // drawn one's; what is drawn after it draws over it)
      int under = tid > MB_VIEW_TEX && tid <= MB_LAYER_TEX && view_under(tid);
      if (under) glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
#endif
      if (tid == 0) draw_banded(start, end, count);
      else glDrawArraysInstanced(GL_TRIANGLES, 0, 6, end - start);
#ifdef MB_VIEWS_UNDER
      if (under) glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
#endif
    }
    start = end;
  }
  lookahead_slot_mask = 0;
  if (levels_sized && slots_total_bytes() > MB_GLASS_CACHE_MAX_BYTES) slots_evict_budget(-1, frame_clock, 1);
  if (cut_on) glDisable(GL_SCISSOR_TEST);
  cut_on = 0;
  target_fb = was;
}

// ---------------------------------------------------------------------------
// Without the surface (mbui-render, src/render: a frame drawn as on the surface, read back as a PNG): a framebuffer of
// its own the size the surface would be, drawn into.

static GLuint off_tex, off_fb;
static int off_pw, off_ph;

static void offscreen_lost(void) {
  off_tex = off_fb = 0;
  off_pw = off_ph = 0;
  read_fb = read_tex = 0;
  read_w = read_h = 0;
}

// Draw into a framebuffer of our own of w x h points at scale; 0 when the driver cannot make a complete one (nothing
// kept of it, so the next call at the same size tries again).
static int gl_offscreen(double w, double h, double scale) {
  offscreen = 1;
  cur_w = w;
  cur_h = h;
  cur_scale = scale;
  int pw = (int)floor(w * scale + 0.5);
  int ph = (int)floor(h * scale + 0.5);
  if (pw <= 0) pw = 1;
  if (ph <= 0) ph = 1;
  fb_w = pw;
  fb_h = ph;
  canvas_ok = 0;
  slots_invalidate();
  gl_adaptive_reset();
  if (!off_fb || pw != off_pw || ph != off_ph) {
    if (off_fb) {
      glDeleteFramebuffers(1, &off_fb);
      glDeleteTextures(1, &off_tex);
      off_fb = off_tex = 0;
    }
    glGenTextures(1, &off_tex);
    glBindTexture(GL_TEXTURE_2D, off_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, pw, ph);
    glGenFramebuffers(1, &off_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, off_fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, off_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      LOG("mbui-render: no framebuffer (%dx%d)", pw, ph);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDeleteFramebuffers(1, &off_fb);
      glDeleteTextures(1, &off_tex);
      off_fb = off_tex = 0;
      off_pw = off_ph = 0;
      target_fb = 0;
      return 0;
    }
    off_pw = pw;
    off_ph = ph;
  }
  target_fb = off_fb;
  glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
  return 1;
}

// The offscreen framebuffer as its frames left it (not the last frame drawn again: what drawing only the parts that
// differ made of it, mbui-render's MBUI_DL_FROM), as a PNG.
static int gl_png(const char *path, GLuint fb);

int mb_render_snapshot_drawn(const char *path) { return offscreen && target_fb ? gl_png(path, target_fb) : 0; }

static void png_chunk(FILE *f, const char *type, const unsigned char *data, uint32_t n) {
  unsigned char be[4] = {n >> 24, n >> 16, n >> 8, n};
  fwrite(be, 1, 4, f);
  fwrite(type, 1, 4, f);
  if (n) fwrite(data, 1, n, f);
  uLong c = crc32(0, (const Bytef *)type, 4);
  if (n) c = crc32(c, data, n);
  unsigned char cb[4] = {c >> 24, c >> 16, c >> 8, c};
  fwrite(cb, 1, 4, f);
}

// What framebuffer `fb` holds (the surface's size) as a PNG (8-bit RGBA, top row first, zlib's level 6): 1 when
// written.
static int gl_png(const char *path, GLuint fb) {
  if (fb_w <= 0 || fb_h <= 0) return 0;
  size_t row = (size_t)fb_w * 4;
  unsigned char *px = malloc(row * fb_h), *raw = malloc((row + 1) * fb_h);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  // GL's rows are from the foot up
  for (int y = 0; y < fb_h; y++) {
    raw[(row + 1) * y] = 0;
    memcpy(raw + (row + 1) * y + 1, px + row * (fb_h - 1 - y), row);
  }
  uLongf zn = compressBound((uLong)((row + 1) * fb_h));
  unsigned char *z = malloc(zn);
  int ok = compress2(z, &zn, raw, (uLong)((row + 1) * fb_h), 6) == Z_OK;
  FILE *f = ok ? fopen(path, "wb") : NULL;
  if (f) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    unsigned char ihdr[13] = {fb_w >> 24, fb_w >> 16, fb_w >> 8, fb_w, fb_h >> 24, fb_h >> 16, fb_h >> 8, fb_h,
                              8, 6, 0, 0, 0};
    fwrite(sig, 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", NULL, 0);
    ok = fclose(f) == 0;
  } else {
    ok = 0;
  }
  free(z);
  free(raw);
  free(px);
  return ok;
}
