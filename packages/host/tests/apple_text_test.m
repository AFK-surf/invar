// packages/host/native/apple_text.m's glyph prewarm tests, on Core Text itself: glyphs first drawn on the frames'
// thread queue their other quarter-pixel phases once, as one batch at the frame's end (mb_text_flush_ahead), drawn
// byte-identical to the frames' thread; a phase a queue is still drawing is drawn here at once rather than waited for
// (the skip mask), its queue's picture dropped and its entry reclaimed; a scale change mid-frame forgets what was
// pending; and frames racing the queues neither stall nor crash.
//   cc -Wall -Werror -fobjc-arc -Ipackages/host/compat -Ipackages/host/native packages/host/tests/apple_text_test.m \
//     -framework Foundation -framework AppKit -framework CoreText -framework CoreGraphics -framework ImageIO \
//     -o target/apple_text_test && target/apple_text_test
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#import <AppKit/AppKit.h>
#import "../native/apple_text.m"

// (what apple_text.m asks of the rest of the host: the Mac's system face, as mac_app.m gives it; an atlas and a
// snapshot it is not asked for here)
CTFontRef mb_platform_font(double size, int weight, int mono) {
  NSFontWeight w = weight >= 700 ? NSFontWeightBold
                   : weight >= 600 ? NSFontWeightSemibold
                   : weight >= 500 ? NSFontWeightMedium
                                   : NSFontWeightRegular;
  NSFont *f = mono ? [NSFont monospacedSystemFontOfSize:size weight:w] : [NSFont systemFontOfSize:size weight:w];
  return (CTFontRef)CFBridgingRetain(f);
}
void mb_atlas_upload(int color, int x, int y, int w, int h, const void *px) {
  (void)color, (void)x, (void)y, (void)w, (void)h, (void)px;
}
void mb_host_request_frame(void) {}
int mb_render_snapshot(const char *path) {
  (void)path;
  return 0;
}
moonbit_string_t moonbit_make_string_raw(int32_t n) {
  (void)n;
  return NULL;
}

static int failed;

#define CHECK(c, ...)                                                 \
  do {                                                                \
    if (!(c)) {                                                       \
      fprintf(stderr, "%s:%d: failed: %s: ", __FILE__, __LINE__, #c); \
      fprintf(stderr, __VA_ARGS__);                                   \
      fprintf(stderr, "\n");                                          \
      failed = 1;                                                     \
    }                                                                 \
  } while (0)

// `s` shaped as mb_host_shape shapes it: its clusters (start, length, advance, key), how many
static int shape(NSString *s, double size, int style, double *out, int cap) {
  unichar buf[512];
  NSUInteger n = s.length < 512 ? s.length : 512;
  [s getCharacters:buf range:NSMakeRange(0, n)];
  return shape_line((moonbit_string_t)buf, (int32_t)n, size, 400, style, out, cap);
}

static int key_of(NSString *c, double size, int style) {
  double out[4 * 8];
  int k = shape(c, size, style, out, 8);
  return k > 0 ? (int)out[3] : -1;
}

static void drain_ahead_queues(void) {
  dispatch_sync(ahead_q, ^{
                });
  dispatch_sync(ahead_rest_q, ^{
                });
}

static BOOL drawn_eq(MBDrawn *d, MBDrawn *r) {
  if (!d || !r) return d == r;
  if (d.pw != r.pw || d.ph != r.ph || d.color != r.color || !CGRectEqualToRect(d.b, r.b)) return NO;
  if (!d.px || !r.px) return d.px == r.px;
  return memcmp(d.px, r.px, (size_t)d.pw * d.ph * (d.color ? 4 : 1)) == 0;
}

// What the frames' thread draws of glyph `k` at phase `sub` itself.
static MBDrawn *drawn_here(int k, int sub) {
  glyph_entry *e = &entries[k];
  return draw_cluster(texts[k], font_for(e->size, e->weight, e->style), e->style, e->dx, sub_dx(sub), text_scale);
}

// Glyphs first drawn on the frames' thread ask nothing of the queues while the frame draws; at its end
// (mb_text_flush_ahead, also from mb_host_atlas_generation and mb_host_glyphs_ahead) their other phases are asked
// in one batch, drawn as the frames' thread draws them, to the byte.
static void flush_batched(void) {
  mb_host_atlas_generation();
  ahead_ensure();
  drain_ahead_queues();
  int k1 = key_of(@"批", 19, 0);
  int k2 = key_of(@"刷", 19, 0);
  CHECK(k1 >= 0 && k2 >= 0, "no keys");
  double out[8] = {0};
  mb_host_glyph(k1 | (0 << 26), out);
  mb_host_glyph(k2 | (2 << 26), out);
  mb_host_glyph(k1 | (0 << 26), out);  // (drawn twice in a frame: pending once)
  drain_ahead_queues();
  CHECK(n_fg_pending == 2, "pending %d, not 2", n_fg_pending);
  CHECK(entries[k1].subs_queued == 0x1 && entries[k1].fg_queued == 1, "k1 mid-frame (subs=%u fg=%u)",
        entries[k1].subs_queued, entries[k1].fg_queued);
  CHECK(entries[k2].subs_queued == 0x4 && entries[k2].fg_queued == 1, "k2 mid-frame (subs=%u fg=%u)",
        entries[k2].subs_queued, entries[k2].fg_queued);
  CHECK(ahead_of[@(k1)] == nil && ahead_of[@(k2)] == nil, "asked of the queues mid-frame");

  mb_text_flush_ahead();
  CHECK(n_fg_pending == 0, "pending left after flush: %d", n_fg_pending);
  mb_text_flush_ahead();  // (nothing pending: nothing asked)
  drain_ahead_queues();
  CHECK(entries[k1].subs_queued == SUB_ALL && entries[k1].fg_queued == 0, "k1 not flushed");
  CHECK(entries[k2].subs_queued == SUB_ALL && entries[k2].fg_queued == 0, "k2 not flushed");
  for (int s = 1; s < SUB_COUNT; s++) {
    MBDrawn *d = drawn_ahead(k1, s);
    CHECK(d != nil, "k1 phase %d missing", s);
    CHECK(drawn_eq(d, drawn_here(k1, s)), "k1 phase %d not as the frames' thread draws it", s);
  }
  CHECK(drawn_ahead(k1, 0) == nil, "k1 phase 0 drawn again");
  for (int s = 0; s < SUB_COUNT; s++) {
    if (s == 2) continue;
    MBDrawn *d = drawn_ahead(k2, s);
    CHECK(d != nil && drawn_eq(d, drawn_here(k2, s)), "k2 phase %d missing or unlike", s);
  }
  CHECK(drawn_ahead(k2, 2) == nil, "k2 phase 2 drawn again");
  CHECK(ahead_of[@(k1)] == nil && ahead_of[@(k2)] == nil, "ahead_of not cleared");

  // the frame's end in mb_host_atlas_generation, as the next frame starts
  int k3 = key_of(@"帧", 19, 0);
  mb_host_glyph(k3 | (1 << 26), out);
  CHECK(n_fg_pending == 1 && ahead_of[@(k3)] == nil, "k3 asked mid-frame");
  mb_host_atlas_generation();
  CHECK(n_fg_pending == 0 && ahead_of[@(k3)] != nil, "mb_host_atlas_generation did not flush");
  drain_ahead_queues();
  for (int s = 0; s < SUB_COUNT; s++) {
    if (s != 1) CHECK(drawn_ahead(k3, s) != nil, "k3 phase %d missing", s);
  }
  printf("flush_batched: ok\n");
}

// A phase a queue is drawing when the frames' thread comes to it: not waited for, marked skipped (asked again: kept);
// the queue's picture of it dropped when it is done and the entry, empty, reclaimed.
static void skip_mask(void) {
  ahead_ensure();
  drain_ahead_queues();
  int k = key_of(@"跳", 19, 0);
  CHECK(k >= 0, "no key");
  MBAhead *a = [MBAhead new];
  a.key = k;
  a.cluster = texts[k];
  a.font = (__bridge id)font_for(entries[k].size, entries[k].weight, entries[k].style);
  a.style = entries[k].style;
  a.dx = entries[k].dx;
  a.scale = text_scale;
  a.drawing = (uint8_t)(1 << 1);
  a.drawn = [NSMutableArray arrayWithObjects:NSNull.null, NSNull.null, NSNull.null, NSNull.null, nil];
  ahead_of[@(k)] = a;
  [ahead_order addObject:a];

  // (the old drawn_ahead waited here for the queue, which never finishes: a deadlock the watchdog would catch)
  MBDrawn *d = drawn_ahead(k, 1);
  CHECK(d == nil, "expected nil while drawing");
  CHECK((a.skip & (1 << 1)) != 0, "skip bit not set");
  CHECK(ahead_of[@(k)] == a, "entry let go while drawing");
  double out[8] = {0};
  mb_host_glyph(k | (1 << 26), out);
  CHECK(out[2] > 0 && out[3] > 0, "not drawn here at once (%g x %g)", out[2], out[3]);
  CHECK(n_fg_pending == 0, "glyph with an entry pending again");

  int32_t req[1] = {k | (1 << 26)};
  mb_host_glyphs_ahead(req, 1);
  CHECK((a.skip & (1 << 1)) == 0, "asked again and still skipped");

  // a queue's draw of a skipped phase: its picture dropped, nothing left to take
  a.drawing = 0;
  a.want = (uint8_t)(1 << 1);
  a.first = (uint8_t)(1 << 1);
  a.skip = (uint8_t)(1 << 1);
  dispatch_sync(ahead_q, ^{
    draw_ahead(a, YES);
  });
  CHECK(a.drawing == 0 && a.want == 0 && a.skip == 0 && a.have == 0,
        "skipped phase kept (drawing=%u want=%u skip=%u have=%u)", a.drawing, a.want, a.skip, a.have);
  CHECK(a.drawn[1] == NSNull.null, "skipped picture kept");
  CHECK(ahead_of[@(k)] == a, "empty entry let go before reclaimed");
  while (ahead_order.firstObject != a && drop_oldest_drawn()) {
  }
  CHECK(drop_oldest_drawn(), "empty entry not reclaimed");
  CHECK(ahead_of[@(k)] == nil, "reclaimed entry still asked");
  entries[k].subs_queued = 0;
  printf("skip_mask: ok\n");
}

// The scale changed with glyphs pending and phases being drawn: all forgotten, nothing drawn at the old scale taken.
static void scale_change(void) {
  ahead_ensure();
  drain_ahead_queues();
  NSString *page = @"缩放切换时帧末预热的字形全部作废Scale change drops pending glyphs";
  double out[4 * 128];
  int n = shape(page, 17, 0, out, 128);
  CHECK(n > 10, "page shaped to %d", n);
  double g[8];
  for (int i = 0; i < n; i++) mb_host_glyph((int)out[4 * i + 3], g);
  CHECK(n_fg_pending > 0, "nothing pending");
  int32_t keys[128];
  for (int i = 0; i < n; i++) keys[i] = (int)out[4 * i + 3] | (1 << 26);
  int32_t many[128];
  double out2[4 * 128];
  int n2 = shape(@"另一页尚未画过的字形在队列里画着", 21, 0, out2, 128);
  for (int i = 0; i < n2; i++) many[i] = (int)out2[4 * i + 3];
  mb_host_glyphs_ahead(many, n2);  // (also flushes what is pending)
  int gen = generation;
  mb_text_set_scale(3.0);
  CHECK(generation == gen + 1, "generation not moved on");
  CHECK(n_fg_pending == 0 && ahead_of.count == 0 && ahead_order.count == 0, "not forgotten (pending %d, asked %lu)",
        n_fg_pending, (unsigned long)ahead_of.count);
  for (int i = 0; i < n_entries; i++) {
    if (entries[i].fg_queued || entries[i].subs_queued) {
      CHECK(0, "entry %d still queued", i);
      break;
    }
  }
  mb_text_flush_ahead();
  drain_ahead_queues();
  CHECK(ahead_of.count == 0, "flushed after the scale changed");
  // drawn again at the new scale: ahead as here
  for (int i = 0; i < n; i++) mb_host_glyph(keys[i] & ~(3 << 26), g);
  mb_text_flush_ahead();
  drain_ahead_queues();
  int k = (int)out[3];
  MBDrawn *d = drawn_ahead(k, 1);
  CHECK(d != nil && drawn_eq(d, drawn_here(k, 1)), "not drawn at the new scale");
  mb_text_set_scale(2.0);
  printf("scale_change: ok\n");
}

// Frames racing the queues: pages asked ahead and drawn at once at every phase (taking, skipping, drawing here),
// flushed, the scale changed between some; each glyph drawn as the frames' thread draws it, no stall, no crash.
static void racing(void) {
  ahead_ensure();
  drain_ahead_queues();
  NSString *page = @"文字渲染横向推入页面每个字形的四分之一像素相位Rendering every glyph's phases ahead 0123456789";
  int sizes[] = {13, 15, 17, 19, 23};
  for (int round = 0; round < 60; round++) {
    @autoreleasepool {
      double out[4 * 256];
      int n = shape(page, sizes[round % 5], round & 1, out, 256);
      int32_t keys[256];
      for (int i = 0; i < n; i++) keys[i] = (int)out[4 * i + 3] | ((round % SUB_COUNT) << 26);
      mb_host_glyphs_ahead(keys, n);
      double g[8];
      for (int s = 0; s < SUB_COUNT; s++) {
        for (int i = 0; i < n; i++) {
          int k = key_base(keys[i]);
          mb_host_glyph(k | (s << 26), g);
          if (round % 7 == 0 && s == 1 && i % 9 == 0) {
            MBDrawn *h = drawn_here(k, s);
            CHECK((h.px ? h.pw / text_scale : 0) == entries[k].w, "round %d glyph %d phase %d drawn unlike", round,
                  i, s);
          }
        }
        mb_text_flush_ahead();
      }
      if (round % 10 == 9) mb_text_set_scale(round % 20 == 9 ? 3.0 : 2.0);
      if (round % 15 == 14) mb_host_atlas_generation();
    }
  }
  drain_ahead_queues();
  mb_text_set_scale(2.0);
  drain_ahead_queues();
  CHECK(ahead_order.count <= AHEAD_MOST, "more asked than AHEAD_MOST: %lu", (unsigned long)ahead_order.count);
  printf("racing: ok\n");
}

static void stalled(int sig) {
  (void)sig;
  static const char msg[] = "apple_text_test: stalled (deadlock?)\n";
  write(2, msg, sizeof msg - 1);
  _exit(2);
}

int main(void) {
  signal(SIGALRM, stalled);
  alarm(120);
  @autoreleasepool {
    text_scale = 2.0;
    flush_batched();
    skip_mask();
    scale_change();
    racing();
  }
  if (failed) return 1;
  printf("apple_text_test: ok\n");
  return 0;
}
