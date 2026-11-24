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
// win_count_dropped_step() counts drops from the samples' own sync times in frames of `beats` refreshes.
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

// win_count_dropped_step(): drops during continuous animation only (the samples' sync times within 2.5 * period),
// counted in frames of `beats` refreshes, clamped to MB_WIN_DROPPED_CLAMP, 0 on non-monotonic counters or no times.
static void test_dropped_step_logic(void) {
  const int64_t freq = 10000000;  // 10 MHz QPC (10000 ticks = 1 ms)
  const double p60 = 1000.0 / 60.0;
  // First sample (has_prev = 0): never counts drops.
  CHECK(win_count_dropped_step(0, 0, 0, 0, 10, 20, 100000, freq, p60, 1) == 0);
  // Continuous 60Hz (dt = 16.67ms < 41.67ms), no drops (d_sync == d_present == 1).
  CHECK(win_count_dropped_step(10, 100, 100000, 1, 11, 101, 266700, freq, p60, 1) == 0);
  // Continuous animation (dt = 33.3ms < 41.67ms), 2 sync refreshes missed (d_sync=3, d_present=1).
  CHECK(win_count_dropped_step(10, 100, 100000, 1, 11, 103, 433000, freq, p60, 1) == 2);
  // Idle pause (dt = 50ms >= 2.5 * 16.67ms): ignored even if d_sync > d_present.
  CHECK(win_count_dropped_step(10, 100, 100000, 1, 11, 104, 600000, freq, p60, 1) == 0);
  // Huge spike within continuous window: clamped to MB_WIN_DROPPED_CLAMP (10).
  CHECK(win_count_dropped_step(10, 100, 100000, 1, 11, 150, 300000, freq, p60, 1) == MB_WIN_DROPPED_CLAMP);
  // Non-monotonic QPC or PresentCount: 0.
  CHECK(win_count_dropped_step(10, 100, 200000, 1, 11, 103, 100000, freq, p60, 1) == 0);
  CHECK(win_count_dropped_step(10, 100, 100000, 1, 10, 103, 200000, freq, p60, 1) == 0);
  // 120Hz continuous (period = 8.33ms, 2.5x = 20.83ms): dt = 16.67ms counts 1 drop, dt = 22ms ignored.
  const double p120 = 1000.0 / 120.0;
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 12, 266700, freq, p120, 1) == 1);
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 13, 320000, freq, p120, 1) == 0);
  // 240Hz paced to 120fps (beats = 2, period = 8.33ms per frame, 2.5x = 20.83ms): each present expects 2 syncs.
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 12, 183300, freq, p120, 2) == 0);   // normal frame
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 13, 225000, freq, p120, 2) == 0);   // one-beat phase jitter
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 14, 266700, freq, p120, 2) == 1);   // one 120fps frame dropped
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 16, 300000, freq, p120, 2) == 2);   // two dropped
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 3, 16, 300000, freq, p120, 2) == 1);   // two presents, one dropped
  // 360Hz paced to 120fps (beats = 3).
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 13, 183300, freq, p120, 3) == 0);   // normal frame
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 16, 266700, freq, p120, 3) == 1);   // one dropped
  // Waking from a 500ms idle: the samples' own sync times are 500ms apart, so the long sync gap is not a drop.
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 130, 5100000, freq, p120, 2) == 0);
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 130, 5100000, freq, p60, 1) == 0);
  // A sample without a sync time, or no beats: 0.
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 14, 0, freq, p120, 2) == 0);
  CHECK(win_count_dropped_step(1, 10, 0, 1, 2, 14, 166700, freq, p120, 2) == 0);
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 14, 266700, freq, p120, 0) == 0);
  CHECK(win_count_dropped_step(1, 10, 100000, 1, 2, 14, 266700, freq, p120, -1) == 0);
  // Nothing presented through a window's swap chain here: none counted.
  CHECK(mb_host_frames_dropped() == 0);
}

int main(void) {
  test_dropped_step_logic();
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
