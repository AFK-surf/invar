// packages/host/native/win_render.cpp's per-window vertex-buffer, allocation-failure and dropped-frame tests on
// Windows (MSVC Developer Command Prompt, D3D11 WARP):
//   cl /nologo /EHsc /W3 /WX /D_CRT_SECURE_NO_WARNINGS /Ipackages\host\native packages\host\tests\win_vbuf_test.cpp ^
//     /Fe:win_vbuf_test.exe /link d3d11.lib d3dcompiler.lib dcomp.lib dwmapi.lib gdi32.lib user32.lib ^
//     windowscodecs.lib ole32.lib
//   win_vbuf_test.exe
// Verifies that D3D11 dynamic vertex buffer growth handles UINT_MAX overflow and CreateBuffer failures without
// leaving a dangling/NULL `vbuf` dereferenced by `ctx->Map` (0xC0000005), recovers cleanly on the next frame,
// isolates per-window vertex buffers across partial redraws (such as `mb_render_adaptive_step`) and
// `mb_render_pixels_of`, keeps shared glass caches alive while another open window still renders glass, and that
// win_stats_step() / win_stats_on_present() count drops from PresentRefreshCount in frames of `beats` refreshes, and
// that frame_late.h breaks a window's chain of frames as soon as it asks for no next one.
#define MB_NO_MOONBIT_H 1
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static size_t test_out_bytes_len = 0;

static int fail_create_buffer_count = 0;
static HRESULT test_create_buffer(ID3D11Device *d, const D3D11_BUFFER_DESC *desc,
                                  const D3D11_SUBRESOURCE_DATA *init, ID3D11Buffer **out) {
  if (fail_create_buffer_count > 0) {
    fail_create_buffer_count--;
    *out = NULL;
    return E_OUTOFMEMORY;
  }
  return d->CreateBuffer(desc, init, out);
}
#define MB_WIN_CREATE_BUFFER(d, desc, init, out) test_create_buffer((d), (desc), (init), (out))

extern "C" {
size_t invar_test_array_length(const void *) { return test_out_bytes_len; }
const wchar_t *mb_win_env(const wchar_t *) { return NULL; }
const wchar_t *mb_win_app_id(void) { return L""; }
void mb_host_request_frame(void) {}
void *mb_image_srv(int) { return NULL; }
void *mb_stream_srv(int) { return NULL; }
void mb_text_set_scale(double) {}
int mb_views_count(void) { return 0; }
void mb_views_draw_under(uint8_t *, int, int, double) {}
double mb_app_scale(void) { return 1.0; }
}

#include "win_render.cpp"
#include "frame_late.h"

static int failed = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      printf("%s:%d: failed: %s\n", __FILE__, __LINE__, #c);             \
      fflush(stdout);                                                    \
      failed = 1;                                                        \
    }                                                                    \
  } while (0)

static void fill_rect_instances(double *inst, int count) {
  memset(inst, 0, (size_t)count * MB_INST * sizeof(double));
  for (int i = 0; i < count; i++) {
    double *p = inst + (size_t)i * MB_INST;
    p[0] = (double)(i % 32);
    p[1] = (double)(i / 32);
    p[2] = 8.0;
    p[3] = 8.0;
    p[8] = 0.2;
    p[9] = 0.4;
    p[10] = 0.8;
    p[11] = 1.0;
    p[16] = -1e9;
    p[17] = -1e9;
    p[18] = 1e9;
    p[19] = 1e9;
  }
}

// WinDropStats: drops during continuous presentation only (PresentRefreshCount deltas within 2.5 * period), counted in
// frames of `beats` refreshes, 32-bit wrap safe, none across a gap in a window's frames, monotonic across its windows.
static void test_dropped_step_logic(void) {
  const int64_t freq = 10000000;  // 10 MHz QPC (10000 ticks = 1 ms)
  const double p60 = 1000.0 / 60.0;
  const double p120 = 1000.0 / 120.0;

  // 1. Continuous 60Hz, drops, idle >= 2.5*period, clamp, non-monotonic QPC, missing time.
  WinDropStats st = {};
  CHECK(win_stats_step(&st, 1, 0, 10, 100, 100000, freq, p60, 1) == 0);  // first sample
  CHECK(win_stats_step(&st, 1, 0, 11, 101, 266700, freq, p60, 1) == 0);  // normal 60Hz
  CHECK(win_stats_step(&st, 1, 0, 12, 104, 599700, freq, p60, 1) == 2);  // 2 refreshes missed
  CHECK(win_stats_step(&st, 1, 0, 13, 108, 1099700, freq, p60, 1) == 0); // idle 50ms >= 2.5*16.67ms
  CHECK(win_stats_step(&st, 1, 0, 14, 158, 1299700, freq, p60, 1) == MB_WIN_DROPPED_CLAMP);
  CHECK(win_stats_step(&st, 1, 0, 15, 162, 1200000, freq, p60, 1) == 0); // non-monotonic QPC
  CHECK(win_stats_step(&st, 1, 0, 15, 165, 1400000, freq, p60, 1) == 0); // duplicate PresentCount

  // 2. 120Hz / 240Hz (beats=2) / 360Hz (beats=3).
  memset(&st, 0, sizeof st);
  CHECK(win_stats_step(&st, 1, 0, 1, 10, 100000, freq, p120, 2) == 0);
  CHECK(win_stats_step(&st, 1, 0, 2, 12, 183300, freq, p120, 2) == 0);   // normal 240Hz->120fps
  CHECK(win_stats_step(&st, 1, 0, 3, 15, 225000, freq, p120, 2) == 0);   // 1-beat phase jitter
  CHECK(win_stats_step(&st, 1, 0, 4, 19, 391700, freq, p120, 2) == 1);   // 1 frame dropped
  CHECK(win_stats_step(&st, 1, 0, 5, 25, 558400, freq, p120, 2) == 2);   // 2 frames dropped
  memset(&st, 0, sizeof st);
  CHECK(win_stats_step(&st, 1, 0, 1, 10, 100000, freq, p120, 3) == 0);
  CHECK(win_stats_step(&st, 1, 0, 2, 13, 183300, freq, p120, 3) == 0);
  CHECK(win_stats_step(&st, 1, 0, 3, 19, 350000, freq, p120, 3) == 1);

  // 3. Old-segment lagging sample preserved (101->102 counts!), cross-gap (102->103) suppressed, new-segment (103->104) counts.
  memset(&st, 0, sizeof st);
  CHECK(win_stats_step(&st, 1, 0, 101, 500, 100000, freq, p60, 1) == 0);
  st.gap_owed = 1;  // idle gap after submitting 102
  win_stats_on_present(&st, 1, 1, 103);  // new segment starts at 103
  CHECK(win_stats_step(&st, 1, 0, 102, 503, 433000, freq, p60, 1) == 2);  // lagging 101->102 real drop NOT lost!
  CHECK(win_stats_step(&st, 1, 0, 103, 508, 633000, freq, p60, 1) == 0);  // cross-gap 102->103 suppressed!
  CHECK(win_stats_step(&st, 1, 0, 104, 510, 966000, freq, p60, 1) == 1);  // new segment 103->104 real drop counts!

  // 4. Lagging jump sequence 97 -> 97 -> 99 -> 101 -> 102 with gap boundary 100.
  memset(&st, 0, sizeof st);
  st.gap_owed = 1;
  win_stats_on_present(&st, 1, 1, 100);
  CHECK(win_stats_step(&st, 1, 0, 97, 500, 100000, freq, p60, 1) == 0);
  CHECK(win_stats_step(&st, 1, 0, 97, 500, 100000, freq, p60, 1) == 0);
  CHECK(win_stats_step(&st, 1, 0, 99, 502, 300000, freq, p60, 1) == 0);
  CHECK(win_stats_step(&st, 1, 0, 101, 508, 500000, freq, p60, 1) == 0);  // crosses 100 -> suppressed
  CHECK(win_stats_step(&st, 1, 0, 102, 511, 833000, freq, p60, 1) == 2);  // post-gap 101->102 counts 2

  // 5. 32-bit wrap-around through production win_stats_step: 0xFFFFFFFF -> 0 must NOT be dropped as invalid.
  memset(&st, 0, sizeof st);
  CHECK(win_stats_step(&st, 1, 0, 0xFFFFFFFFu, 0xFFFFFFFEu, 100000, freq, p60, 1) == 0);
  CHECK(win_stats_step(&st, 1, 0, 0x00000000u, 0x00000001u, 300000, freq, p60, 1) == 2);

  // 6. DISJOINT, OCCLUDED, GetLastPresentCount failure -> unknown_until sealing, and gap_starts overflow recovery.
  memset(&st, 0, sizeof st);
  CHECK(win_stats_step(&st, 1, 0, 50, 100, 100000, freq, p60, 1) == 0);
  st.gap_owed = 1;
  win_stats_on_present(&st, 1, 0, 0);  // GetLastPresentCount failed -> gap_unknown_open=1
  // Next present seals unknown gap at pc=52: lagging 50->51 and 51->52 must be suppressed, then 52->53 counts!
  win_stats_on_present(&st, 1, 1, 52);
  CHECK(win_stats_step(&st, 1, 0, 51, 104, 300000, freq, p60, 1) == 0);
  CHECK(win_stats_step(&st, 1, 0, 52, 108, 500000, freq, p60, 1) == 0);
  CHECK(win_stats_step(&st, 1, 0, 53, 111, 800000, freq, p60, 1) == 2);
  // OCCLUDED & DISJOINT reset history and open unknown gap.
  win_stats_on_present(&st, 0, 0, 0);
  CHECK(st.has_frame_stats == 0 && st.gap_unknown_open == 1);
  win_stats_on_present(&st, 1, 1, 54);
  CHECK(win_stats_step(&st, 1, 0, 54, 112, 900000, freq, p60, 1) == 0);
  CHECK(win_stats_step(&st, 1, 1, 55, 116, 1100000, freq, p60, 1) == 0);  // DISJOINT
  CHECK(st.has_frame_stats == 0 && st.gap_unknown_open == 1);

  // 6b. 5 consecutive gap_owed calls overflow gap_starts (last pc=120): suppressed up to 120, then 120->121 recovers!
  memset(&st, 0, sizeof st);
  CHECK(win_stats_step(&st, 1, 0, 100, 200, 100000, freq, p60, 1) == 0);
  UINT gap_pcs[5] = {104, 108, 112, 116, 120};
  for (int i = 0; i < 5; i++) {
    st.gap_owed = 1;
    win_stats_on_present(&st, 1, 1, gap_pcs[i]);
  }
  CHECK(win_stats_step(&st, 1, 0, 120, 230, 300000, freq, p60, 1) == 0);  // crosses unknown_until=120 -> 0
  CHECK(win_stats_step(&st, 1, 0, 121, 233, 600000, freq, p60, 1) == 2);  // post-overflow 120->121 recovers!

  // 6c. DISJOINT recovery landing right on 32-bit wrap pc == 0: cold sample 0 is accepted and 0->1 counts drops!
  memset(&st, 0, sizeof st);
  CHECK(win_stats_step(&st, 1, 1, 0xFFFFFFFFu, 100, 100000, freq, p60, 1) == 0);  // DISJOINT
  win_stats_on_present(&st, 1, 1, 0u);  // seals unknown_until == 0
  CHECK(win_stats_step(&st, 1, 0, 0u, 105, 200000, freq, p60, 1) == 0);
  CHECK(st.has_frame_stats == 1 && st.has_unknown_until == 0);
  CHECK(win_stats_step(&st, 1, 0, 1u, 108, 500000, freq, p60, 1) == 2);

  // 7. Dual-window interleaved reporting + secondary window close/reopen: mb_host_frames_dropped() stays monotonic.
  present_dropped_total = 0;
  render_win_fresh(&render_wins[0]);
  render_win_fresh(&render_wins[1]);
  win_stats_on_present(&render_wins[0].drop_stats, 1, 1, 1);
  win_stats_on_present(&render_wins[1].drop_stats, 1, 1, 1);
  present_dropped_total += win_stats_step(&render_wins[0].drop_stats, 1, 0, 1, 10, 100000, freq, p60, 1);
  present_dropped_total += win_stats_step(&render_wins[1].drop_stats, 1, 0, 1, 20, 100000, freq, p60, 1);
  present_dropped_total += win_stats_step(&render_wins[0].drop_stats, 1, 0, 2, 12, 300000, freq, p60, 1);  // +1
  present_dropped_total += win_stats_step(&render_wins[1].drop_stats, 1, 0, 2, 23, 300000, freq, p60, 1);  // +2
  CHECK(mb_host_frames_dropped() == 3);
  // Close & reopen window 1: total must NOT regress
  render_win_fresh(&render_wins[1]);
  CHECK(mb_host_frames_dropped() == 3);
  present_dropped_total = 0;
  render_win_fresh(&render_wins[0]);
  render_win_fresh(&render_wins[1]);
}

static void test_frame_late_logic(void) {
  const double p60 = 1000.0 / 60.0;
  // Multi-window independent last_begin: window A continuous late by 1 beat, window B fresh/idle.
  double win_a_last = -1.0, win_b_last = -1.0;
  CHECK(mb_frame_late_from_begin(win_a_last, 100.0, p60, 0) == 0.0);
  win_a_last = mb_frame_late_next(1, 100.0);
  // Window B draws while idle (want_next = 0) -> does not affect window A.
  CHECK(mb_frame_late_from_begin(win_b_last, 105.0, p60, 0) == 0.0);
  win_b_last = mb_frame_late_next(0, 105.0);
  CHECK(win_b_last == -1.0);
  // Window A's next continuous frame arrives 33.33ms later (1 beat late) -> late > 0.
  double late_a = mb_frame_late_from_begin(win_a_last, 133.333, p60, 0);
  CHECK(late_a > 15.0 && late_a < 18.0);
  // Settle gap: window A stops asking for next app frame (want_next = 0), 100ms later next app frame has frame_late == 0.
  win_a_last = mb_frame_late_next(0, 133.333);
  CHECK(win_a_last == -1.0);
  CHECK(mb_frame_late_from_begin(win_a_last, 233.333, p60, 0) == 0.0);
  // Scripted / away always 0.
  CHECK(mb_frame_late_from_begin(100.0, 200.0, p60, 1) == 0.0);
  // Target-based helper (iOS).
  CHECK(mb_frame_late_from_target(-1.0, 200.0) == 0.0);
  CHECK(mb_frame_late_from_target(100.0, 100.0) == 0.0);
  CHECK(mb_frame_late_from_target(100.0, 116.67) > 16.0);
}

int main(void) {
  CHECK(mb_host_frames_dropped() == 0);
  test_dropped_step_logic();
  test_frame_late_logic();
  mb_render_init_offscreen(128.0, 128.0, 1.0);
  CHECK(dev != NULL && ctx != NULL && rtv != NULL);
  if (!dev || !ctx || !rtv) return 1;

  static double inst[128 * MB_INST];
  static float finst[128 * MB_INST];
  fill_rect_instances(inst, 128);
  for (int i = 0; i < 128 * MB_INST; i++) finst[i] = (float)inst[i];

  // 1. Normal frame succeeds and allocates vbuf.
  CHECK(submit(inst, 16, 1.0, 1.0, 1.0) == 1);
  CHECK(vbuf != NULL && vbuf_cap >= 17u * MB_INST * sizeof(float));

  // 2. Overflow count (> UINT_MAX bytes) fails cleanly without crashing.
  uint8_t px[16 * 16 * 4];
  test_out_bytes_len = sizeof(px);
  CHECK(submit(inst, 20000000, 1.0, 1.0, 1.0) == 0);
  CHECK(fill_vbuf(finst, 20000000) == 0);
  CHECK(mb_render_pixels_of(inst, 20000000, 1.0, 1.0, 1.0, 1.0, 16, 16, px) == 0);

  // 3. When 2x CreateBuffer fails with E_OUTOFMEMORY, ensure_vbuf falls back to exact `room` and succeeds.
  fail_create_buffer_count = 1;
  CHECK(submit(inst, 64, 1.0, 1.0, 1.0) == 1);
  CHECK(fail_create_buffer_count == 0);
  CHECK(vbuf != NULL && vbuf_cap == 65u * MB_INST * sizeof(float));

  // 4. When both 2x and exact-size CreateBuffer fail, submit() returns 0 with vbuf cleared (no Map(NULL) crash).
  fail_create_buffer_count = 2;
  CHECK(submit(inst, 128, 1.0, 1.0, 1.0) == 0);
  CHECK(fail_create_buffer_count == 0);
  CHECK(vbuf == NULL && vbuf_cap == 0);

  // 5. Same for fill_vbuf() when CreateBuffer fails.
  fail_create_buffer_count = 2;
  CHECK(fill_vbuf(finst, 128) == 0);
  CHECK(fail_create_buffer_count == 0);
  CHECK(vbuf == NULL && vbuf_cap == 0);

  // 6. Next normal frame and offscreen pixel read recover cleanly after the failure.
  CHECK(submit(inst, 16, 1.0, 1.0, 1.0) == 1);
  CHECK(vbuf != NULL && vbuf_cap >= 17u * MB_INST * sizeof(float));
  CHECK(mb_render_pixels_of(inst, 16, 1.0, 1.0, 1.0, 1.0, 16, 16, px) == 1);

  // Helper to read a single BGRA pixel from `off_target`.
  auto read_off_bgra = [](int x, int y, uint8_t out[4]) -> int {
    D3D11_TEXTURE2D_DESC td = {};
    off_target->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = NULL;
    if (FAILED(dev->CreateTexture2D(&td, NULL, &st)) || !st) return 0;
    ctx->CopyResource(st, off_target);
    D3D11_MAPPED_SUBRESOURCE m;
    int ok = 0;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
      const uint8_t *p = (const uint8_t *)m.pData + (size_t)y * m.RowPitch + (size_t)x * 4;
      memcpy(out, p, 4);
      ctx->Unmap(st, 0);
      ok = 1;
    }
    st->Release();
    return ok;
  };

  // 7. Multi-window vertex-buffer isolation: window 1's submit() must not clobber window 0's vbuf or ground slot
  //    used by subsequent partial redraws (mb_render_adaptive_step).
  static double win0_inst[32 * MB_INST];
  memset(win0_inst, 0, sizeof win0_inst);
  for (int i = 0; i < 32; i++) {
    double *p = win0_inst + (size_t)i * MB_INST;
    p[16] = -1e9; p[17] = -1e9; p[18] = 1e9; p[19] = 1e9;
  }
  {
    // Place a bright green quad at slot 24 (well past window 1's instance count).
    double *g = win0_inst + (size_t)24 * MB_INST;
    g[0] = 20.0; g[1] = 20.0; g[2] = 40.0; g[3] = 40.0;
    g[8] = 0.0; g[9] = 1.0; g[10] = 0.0; g[11] = 1.0;
  }
  mb_render_use(0);
  CHECK(submit(win0_inst, 32, 1.0, 0.0, 0.0) == 1);
  ID3D11Buffer *win0_vb = render_wins[0].win_vbuf;
  CHECK(win0_vb != NULL);

  // Set up an offscreen render target on window 1 and submit a small 2-instance blue frame.
  D3D11_TEXTURE2D_DESC td1 = {};
  td1.Width = 128; td1.Height = 128; td1.MipLevels = td1.ArraySize = 1;
  td1.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td1.SampleDesc.Count = 1;
  td1.Usage = D3D11_USAGE_DEFAULT; td1.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *win1_tex = NULL;
  ID3D11RenderTargetView *win1_rtv = NULL;
  CHECK(SUCCEEDED(dev->CreateTexture2D(&td1, NULL, &win1_tex)) && win1_tex != NULL);
  CHECK(SUCCEEDED(dev->CreateRenderTargetView(win1_tex, NULL, &win1_rtv)) && win1_rtv != NULL);
  mb_render_use(1);
  render_win_fresh(rw);
  rtv = win1_rtv;
  fb_w = 128; fb_h = 128; vw = 128.0; vh = 128.0; vscale = 1.0;
  static double win1_inst[2 * MB_INST];
  memset(win1_inst, 0, sizeof win1_inst);
  for (int i = 0; i < 2; i++) {
    double *p = win1_inst + (size_t)i * MB_INST;
    p[0] = 0.0; p[1] = 0.0; p[2] = 128.0; p[3] = 128.0;
    p[8] = 0.0; p[9] = 0.0; p[10] = 1.0; p[11] = 1.0;
    p[16] = -1e9; p[17] = -1e9; p[18] = 1e9; p[19] = 1e9;
  }
  CHECK(submit(win1_inst, 2, 0.0, 0.0, 0.0) == 1);
  CHECK(render_wins[1].win_vbuf != NULL && render_wins[1].win_vbuf != win0_vb);
  CHECK(render_wins[0].win_vbuf == win0_vb);

  // Switch back to window 0, overwrite off_target with blue, then run partial encode() without re-uploading vbuf.
  mb_render_use(0);
  const float blue_clear[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  ctx->ClearRenderTargetView(rtv, blue_clear);
  const D3D11_RECT part = {10, 10, 70, 70};
  encode(rtv, fb_w, fb_h, last_count, last_bg, last_inst, &part);
  uint8_t px_ground[4] = {}, px_green[4] = {}, px_outside[4] = {};
  CHECK(read_off_bgra(12, 12, px_ground) == 1);
  CHECK(read_off_bgra(40, 40, px_green) == 1);
  CHECK(read_off_bgra(2, 2, px_outside) == 1);
  CHECK(px_ground[2] > 250 && px_ground[1] < 5 && px_ground[0] < 5);
  CHECK(px_green[1] > 250 && px_green[2] < 5 && px_green[0] < 5);
  CHECK(px_outside[0] > 250 && px_outside[1] < 5 && px_outside[2] < 5);

  // 8. mb_render_pixels_of() must restore both last_inst and the partial-redraw ground slot in vbuf.
  CHECK(mb_render_pixels_of(win1_inst, 2, 0.0, 0.0, 1.0, 1.0, 16, 16, px) == 1);
  ctx->ClearRenderTargetView(rtv, blue_clear);
  encode(rtv, fb_w, fb_h, last_count, last_bg, last_inst, &part);
  CHECK(read_off_bgra(12, 12, px_ground) == 1);
  CHECK(read_off_bgra(40, 40, px_green) == 1);
  CHECK(px_ground[2] > 250 && px_ground[1] < 5 && px_ground[0] < 5);
  CHECK(px_green[1] > 250 && px_green[2] < 5 && px_green[0] < 5);

  // 8b. On a transparent window (mb_render_set_clear) the restored ground slot stays clear, as submit() writes it.
  auto ground_alpha = [](int32_t slot) -> float {
    D3D11_BUFFER_DESC bd = {};
    vbuf->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer *st = NULL;
    float a = -1.0f;
    if (FAILED(dev->CreateBuffer(&bd, NULL, &st)) || !st) return a;
    ctx->CopyResource(st, vbuf);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
      a = ((const float *)m.pData)[(size_t)slot * MB_INST + 11];
      ctx->Unmap(st, 0);
    }
    st->Release();
    return a;
  };
  CHECK(ground_alpha(last_count) == 1.0f);
  mb_render_set_clear(0, 1);
  CHECK(fill_vbuf(last_inst, last_count, last_bg) == 1);
  CHECK(ground_alpha(last_count) == 0.0f);
  mb_render_set_clear(0, 0);
  CHECK(fill_vbuf(last_inst, last_count, last_bg) == 1);
  CHECK(ground_alpha(last_count) == 1.0f);

  // Closing window 1 releases its vbuf without affecting window 0.
  mb_render_forget(1);
  if (win1_tex) win1_tex->Release();
  CHECK(render_wins[1].win_vbuf == NULL && render_wins[1].win_vbuf_cap == 0);
  CHECK(render_wins[0].win_vbuf == win0_vb);

  // 9. Multi-window glass_idle_tick: another window drawing without glass must not free shared glass levels
  //    while window 0 still has glass on screen; once no open window has glass, idle eviction still frees them.
  CHECK(ensure_levels(16, 16, 16, 16) == 1 && lv_w >= 16);
  render_wins[0].has_glass = 1;
  glass_idle_frames = 0;
  for (int i = 0; i < MB_GLASS_IDLE + 10; i++) {
    glass_drawn = 0;
    glass_idle_tick();
  }
  CHECK(lv_w >= 16);
  render_wins[0].has_glass = 0;
  for (int i = 0; i < MB_GLASS_IDLE; i++) {
    glass_drawn = 0;
    glass_idle_tick();
  }
  CHECK(lv_w == 0);

  if (failed) return 1;
  printf("win_vbuf: ok\n");
  return 0;
}
