// packages/host/native/apple_text.m's glyph prewarm tests, on Core Text itself: glyphs first drawn on the frames'
// thread queue their other quarter-pixel phases once, as one batch at the frame's end (mb_text_flush_ahead), drawn
// byte-identical to the frames' thread; a phase a queue is still drawing is drawn here at once rather than waited for
// (the skip mask), its queue's picture dropped and its entry reclaimed; a scale change mid-frame forgets what was
// pending; and frames racing the queues neither stall nor crash. And shape_line's fast paths (single-unit clusters,
// carets taken from the shaped glyphs, the nominal ASCII advances) held to the line Core Text shapes: every cluster,
// advance (< 1e-6) and key the same as its unoptimized reference's (shape_parity), the nominal advances forgotten
// as a face changes (ascii_nominal); diagonal fractions shaped whole in a font of their own (fractions).
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
int32_t mb_render_atlas_lost_bumps(void) { return 0; }
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

// Face `face` set in the installed family `family` (as mb_host_face_add_family is called); whether it was found.
// (the name in a string laid out as the host's strings are: its length in the header just before its characters)
static int face_family(int face, NSString *family) {
  uint32_t mem[2 + 64] = {0};
  NSUInteger n = family.length < 128 ? family.length : 128;
  mem[0] = (uint32_t)n;
  [family getCharacters:(unichar *)(mem + 2) range:NSMakeRange(0, n)];
  return mb_host_face_add_family(face, (moonbit_string_t)(mem + 2));
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

// shape_line as it was before its fast paths (the reference: each cluster found by
// rangeOfComposedCharacterSequenceAtIndex, each caret asked of CTLineGetOffsetForStringIndex, every key interned
// from its string), which shape_line must match to within 1e-6 on every string, style and weight below.
static double ref_offset_at(CTLineRef line, NSString *s, NSUInteger len, NSUInteger i) {
  while (i < len && takes_no_room([s characterAtIndex:i])) i++;
  return CTLineGetOffsetForStringIndex(line, (CFIndex)i, NULL);
}

static int32_t shape_line_ref(const unichar *text, int32_t len, double size, int32_t weight, int32_t style,
                              double *out, int32_t cap) {
  NSString *s = [[NSString alloc] initWithCharacters:text length:(NSUInteger)len];
  CTFontRef f = font_for(size, weight, style);
  CTLineRef line = make_line(s, f, style);
  uint8_t *from = calloc((size_t)len + 1, 1);
  int any_rtl = 0;
  double *glyph_adv = calloc((size_t)len + 1, sizeof(double));
  double *glyph_x = malloc(sizeof(double) * ((size_t)len + 1));
  for (int32_t j = 0; j <= len; j++) glyph_x[j] = NAN;
  CFArrayRef runs = CTLineGetGlyphRuns(line);
  for (CFIndex r = 0; r < CFArrayGetCount(runs); r++) {
    CTRunRef run = CFArrayGetValueAtIndex(runs, r);
    CFIndex gc = CTRunGetGlyphCount(run);
    CFIndex *idx = malloc(sizeof(CFIndex) * (size_t)(gc > 0 ? gc : 1));
    CGPoint *pos = malloc(sizeof(CGPoint) * (size_t)(gc > 0 ? gc : 1));
    CTRunGetStringIndices(run, CFRangeMake(0, 0), idx);
    CTRunGetPositions(run, CFRangeMake(0, 0), pos);
    double w = CTRunGetTypographicBounds(run, CFRangeMake(0, 0), NULL, NULL, NULL);
    if (CTRunGetStatus(run) & kCTRunStatusRightToLeft) any_rtl = 1;
    for (CFIndex g = 0; g < gc; g++) {
      if (idx[g] < 0 || idx[g] >= len) continue;
      from[idx[g]] = 1;
      if (isnan(glyph_x[idx[g]]) || pos[g].x < glyph_x[idx[g]]) glyph_x[idx[g]] = pos[g].x;
      double next = g + 1 < gc ? pos[g + 1].x : pos[0].x + w;
      glyph_adv[idx[g]] += next - pos[g].x;
    }
    free(idx);
    free(pos);
  }
  int32_t k = 0;
  NSUInteger i = 0;
  double x0 = ref_offset_at(line, s, (NSUInteger)len, 0);
  while (i < (NSUInteger)len && k < cap) {
    NSRange r = [s rangeOfComposedCharacterSequenceAtIndex:i];
    unichar c = [s characterAtIndex:r.location];
    if (r.location != i) x0 = ref_offset_at(line, s, (NSUInteger)len, r.location);
    double start_x = x0;
    double x1 = ref_offset_at(line, s, (NSUInteger)len, r.location + r.length);
    double adv = x1 - x0;
    x0 = x1;
    if (any_rtl) {
      adv = 0;
      for (NSUInteger j = r.location; j < r.location + r.length; j++) adv += glyph_adv[j];
    }
    int key;
    if (c == '\n') {
      adv = 0;
      key = -1;
    } else if (c == ' ' || c == '\t') {
      key = -1;
    } else {
      int drawn = 0;
      for (NSUInteger j = r.location; j < r.location + r.length; j++) drawn |= from[j];
      double gx = NAN;
      for (NSUInteger j = r.location; j < r.location + r.length; j++)
        if (!isnan(glyph_x[j]) && (isnan(gx) || glyph_x[j] < gx)) gx = glyph_x[j];
      double dx = any_rtl || isnan(gx) ? 0 : gx - start_x;
      if (fabs(dx) < 0.01) dx = 0;
      BOOL after_wide = r.location > 0 && [s characterAtIndex:r.location - 1] >= 0x2E80;
      if (!drawn && k > 0)
        key = -2;
      else if (r.length == 1 && c < 128 && (dx == 0 || !after_wide))
        key = intern([s substringWithRange:r], size, weight, style);
      else
        key = intern_at([s substringWithRange:r], size, weight, style, dx);
    }
    out[k * 4] = r.location;
    out[k * 4 + 1] = r.length;
    out[k * 4 + 2] = adv < 0 ? 0 : adv;
    out[k * 4 + 3] = key;
    k++;
    i = r.location + r.length;
  }
  free(from);
  free(glyph_adv);
  free(glyph_x);
  CFRelease(line);
  return k;
}

// The strings shape_line is held to its reference on: Latin with kerned pairs and ligatures, digits, code, CJK with
// its punctuation squeezed and auto-spaced against Latin, kana, Hangul, emoji sequences, combining marks, characters
// that take no room, right-to-left and mixed lines, tabs and newlines.
static NSArray<NSString *> *parity_corpus(void) {
  static NSArray<NSString *> *corpus;
  if (!corpus) corpus = @[
    @"",
    @"a",
    @" ",
    @"\n",
    @"\t",
    @"Hello, world!",
    @"AVATAR WAVE To The Office official offline difficult",
    @"fi fl ffi ffl ff fj",
    @"0123456789 1111 8888 0:01 40:08",
    @"The quick brown fox jumps over the lazy dog.",
    @"const x = foo(bar, 123); // comment: a->b != c",
    @"https://example.com/path/to/file.m?a=1&b=2#L123",
    @"纯中文段落测试，不包含任何英文字符与数字。",
    @"汉字连续排版测量性能基准测试中文汉字简体繁体漢字",
    @"你是Owner，我在macOS 27上测试中英混排自动间距！",
    @"中文English中文123中文ABC中文",
    @"你好World世界Hello测试Test2026年11月22日",
    @"「（中文括号挤压测试）」，《书名号》【方括号】！？：；、。",
    @"“引号”‘单引号’——破折号……省略号·中点",
    @"（测试）、「测试」、『测试』、【测试】、《测试》",
    @"ＡＢＣ１２３！＠＃￥％……＆＊（）",
    @"日本語のひらがなとカタカナと漢字の混在テストです。",
    @"いろはにほへとちりぬるを カタカナ・ヴ・ヵ・ヶ",
    @"한국어 테스트입니다. 한글과 English 123 혼용.",
    @"完成任务 ✅ 下一步 → 延迟 ≤ 16ms 🎉 汉字缓存命中测试",
    @"👩🏽‍💻 👪 ❤️ 🏳️‍🌈 👍🏻 🧑‍🤝‍🧑",
    @"emoji💡中间穿插中文and English🚀",
    @"e\u0301 cafe\u0301 a\u0308 o\u0302 n\u0303",
    @"Z\u0300\u0301\u0302\u0303 combining stack",
    @"中文\u0301组合音标测试",
    @"zero\u200Bwidth\u200Cjoiner\u200Dword\u2060bom\uFEFFend",
    @"trailing zero width\u200B",
    @"\u200Bleading zero width",
    @"@[mention\u200B]\u200B",
    @"العربية مرحبا بالعالم",
    @"שלום עולם 123",
    @"Mixed العربية and English and 中文 together",
    @"RTL שלום + LTR hello + 数字 42",
    @"line1\nline2\n\nline4",
    @"col1\tcol2\tcol3\t中文\t123",
    @"CRLF\r\nline\r\nend",
    @"¥100 §1 20° ±0.5 · × ÷ – — ‘a’ “b” • … ‰ ′ ″ € ℃",
    @"αβγδε ΑΒΓΔΕ π ≈ 3.14159 ∑ ∫ √ ∞ ≠ ≤ ≥",
    @"Кириллица привет мир 12345",
    @"Tiếng Việt có dấu phức tạp hơn",
    @"ภาษาไทยทดสอบการตัดคำ",
    @"हिन्दी देवनागरी परीक्षण",
    @"fail ail hover ver AV To We",
    @"Hello…",
    @"中文 Hello…",
    @"真机性能剖析显示 tree words measured 是当前双端最大的 CPU 卡顿源。",
    @"perf(host): reduce Apple & Android text shaping hotpath overhead (500 rounds).",
    @"# Heading\n## 二级标题\n- 列表项 1: `code_span` 与 **粗体**",
    @"   leading and trailing spaces   ",
    @"\t\t\n\n  "
  ];
  return corpus;
}

// shape_line against shape_line_ref on every string of parity_corpus in each of `styles` and `weights` at 15pt: the
// same clusters, advances within 1e-6, the same keys (or keys of the same cluster and dx); how many strings matched.
static int parity_over(const int *styles, int n_styles, const int *weights, int n_weights, const char *what) {
  int checked = 0;
  double got[4 * 512], want[4 * 512];
  for (int si = 0; si < n_styles; si++) {
    for (int wi = 0; wi < n_weights; wi++) {
      int st = styles[si], wt = weights[wi];
      for (NSString *s in parity_corpus()) {
        unichar buf[512];
        NSUInteger n = s.length < 512 ? s.length : 512;
        [s getCharacters:buf range:NSMakeRange(0, n)];
        int32_t k_ref = shape_line_ref(buf, (int32_t)n, 15.0, wt, st, want, 512);
        int32_t k_got = shape_line((moonbit_string_t)buf, (int32_t)n, 15.0, wt, st, got, 512);
        CHECK(k_got == k_ref, "%s: cluster count mismatch (%d != %d) style=%d weight=%d in \"%s\"", what, k_got,
              k_ref, st, wt, s.UTF8String);
        if (k_got != k_ref) continue;
        BOOL ok = YES;
        for (int32_t c = 0; c < k_ref; c++) {
          BOOL range_ok = got[c * 4] == want[c * 4] && got[c * 4 + 1] == want[c * 4 + 1];
          CHECK(range_ok, "%s: range mismatch at %d style=%d weight=%d in \"%s\"", what, c, st, wt, s.UTF8String);
          double derr = fabs(got[c * 4 + 2] - want[c * 4 + 2]);
          CHECK(derr < 1e-6, "%s: adv mismatch at %d (%.6f != %.6f, err=%.6f) style=%d weight=%d in \"%s\"", what, c,
                got[c * 4 + 2], want[c * 4 + 2], derr, st, wt, s.UTF8String);
          int gk = (int)got[c * 4 + 3], wk = (int)want[c * 4 + 3];
          BOOL key_ok = gk == wk || (gk >= 0 && wk >= 0 && [texts[gk] isEqualToString:texts[wk]]
                                     && fabs(entries[gk].dx - entries[wk].dx) < 1e-6);
          CHECK(key_ok, "%s: key mismatch at %d (%d != %d) style=%d weight=%d in \"%s\"", what, c, gk, wk, st, wt,
                s.UTF8String);
          ok = ok && range_ok && derr < 1e-6 && key_ok;
        }
        if (ok) checked++;
      }
    }
  }
  return checked;
}

// Every string of the corpus in the system face, each style (slanted, code, serif, no ligatures, tabular, proportional,
// diagonal fractions alone and with tabular digits) at two weights: shape_line's clusters, advances and keys are its
// reference's.
static void shape_parity(void) {
  static const int styles[] = {0, 1, 2, 4, 8, 32, 64, 128, 32 | 128};
  static const int weights[] = {400, 700};
  int n_styles = (int)(sizeof(styles) / sizeof(styles[0]));
  int total = n_styles * (int)(sizeof(weights) / sizeof(weights[0])) * (int)parity_corpus().count;
  int checked = parity_over(styles, n_styles, weights, 2, "system");
  CHECK(checked == total, "%d of %d cases matched", checked, total);
  printf("shape_parity: %d cases matched reference (< 1e-6)\n", checked);
}

static double nominal_advance(CTFontRef f, unichar c) {
  CGGlyph g = 0;
  CGSize adv = CGSizeZero;
  if (!CTFontGetGlyphsForCharacters(f, &c, &g, 1) || g == 0) return NAN;
  CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, &g, &adv, 1);
  return adv.width;
}

// The nominal ASCII advances (bmp_ascii_nom): measured once per font where the unkerned ASCII path may be taken (not
// in the code face, nor with tabular or proportional figures, nor diagonal fractions), equal to the font's own; Latin runs in the primary font
// tagged for it; forgotten the moment a face changes (before keys_sweep), measured again in the new face, where
// shape_line still matches its reference; and the paged keys let go at keys_sweep.
static void ascii_nominal(void) {
  mb_host_atlas_generation();
  double out[4 * 64];
  shape(@"Hello, world", 15, 0, out, 64);
  bmp_keys *bk = bmp_for(15, 400, 0);
  CHECK(bk->ascii_nom_ready, "nominal advances not measured");
  CTFontRef sys = font_for(15, 400, 0);
  for (unichar c = 0x20; c <= 0x7E; c++) {
    double want = nominal_advance(sys, c);
    CHECK((isnan(want) && isnan(bk->ascii_nom[c])) || bk->ascii_nom[c] == want, "nominal advance of '%c': %f != %f",
          (char)c, bk->ascii_nom[c], want);
  }
  CHECK(isnan(bk->ascii_nom[0x1F]) && isnan(bk->ascii_nom[0x7F]), "nominal advance outside printable ASCII");
  CHECK(shape_simple[0] >= 2 && shape_simple[0] == shape_simple[4], "Latin run in the primary font not tagged: %d %d",
        shape_simple[0], shape_simple[4]);
  shape(@"国国国", 15, 0, out, 64);
  CHECK(shape_simple[0] == 1, "a Han run tagged for the ASCII path: %d", shape_simple[0]);
  static const int skipped[] = {2, 32, 64, 128};
  for (int i = 0; i < 4; i++) {
    shape(@"Hello, world", 15, skipped[i], out, 64);
    CHECK(!bmp_for(15, 400, skipped[i])->ascii_nom_ready, "nominal advances measured in style %d", skipped[i]);
  }
  double sys_w = bmp_for(15, 400, 0)->ascii_nom['W'];
  CHECK(face_family(1, @"Times New Roman") || face_family(1, @"Helvetica"), "no face for slot 1");
  for (int i = 0; i < n_bmp; i++) CHECK(!bmp_cache[i].ascii_nom_ready, "nominal advances kept as the face changed");
  shape(@"Hello, world", 15, 0, out, 64);
  bk = bmp_for(15, 400, 0);
  CHECK(bk->ascii_nom_ready, "nominal advances not measured in the new face");
  CHECK(bk->ascii_nom['W'] == nominal_advance(font_for(15, 400, 0), 'W') && bk->ascii_nom['W'] != sys_w,
        "nominal advance of W in the new face: %f (system's %f)", bk->ascii_nom['W'], sys_w);
  static const int styles[] = {0, 8};
  static const int weights[] = {400, 700};
  int total = 4 * (int)parity_corpus().count;
  int checked = parity_over(styles, 2, weights, 2, "face");
  CHECK(checked == total, "%d of %d cases matched in the given face", checked, total);
  printf("shape_parity (given face): %d cases matched reference (< 1e-6)\n", checked);
  mb_host_face_clear(1);
  for (int i = 0; i < n_bmp; i++) CHECK(!bmp_cache[i].ascii_nom_ready, "nominal advances kept as the face was cleared");
  CHECK(n_bmp > 0, "paged keys let go before keys_sweep");
  mb_host_atlas_generation();
  CHECK(n_bmp == 0 && bmp_cache == NULL && last_bmp == -1, "paged keys kept past keys_sweep: %d", n_bmp);
  printf("ascii_nominal: ok\n");
}

// The width of `s` shaped as mb_host_shape shapes it (its clusters' advances summed).
static double shaped_width(NSString *s, int style) {
  double out[4 * 64];
  int k = shape(s, 15, style, out, 64);
  double w = 0;
  for (int i = 0; i < k; i++) w += out[i * 4 + 2];
  return w;
}

// Diagonal fractions (style & 128, hostapi style_diagonal_fractions): a font of their own, apart from the plain one's and
// the tabular one's; "1/2" and "24/7" (printable ASCII the nominal-advance path would take) shaped whole by Core Text with
// frac, narrower than unset and equal to the line Core Text sets; their keys and paged keys apart from style 0's.
static void fractions(void) {
  CTFontRef plain = font_for(15, 400, 0), tab = font_for(15, 400, 32);
  CTFontRef frac = font_for(15, 400, 128), both = font_for(15, 400, 32 | 128);
  CHECK(frac != plain && frac != tab && both != frac && both != tab, "fraction fonts shared with another style's");
  NSArray *feats = CFBridgingRelease(CTFontCopyFeatureSettings(font_for(15, 400, 128)));
  BOOL has_frac = NO;
  for (NSDictionary *f in feats) {
    if ([f[(__bridge id)kCTFontOpenTypeFeatureTag] isEqual:@"frac"]
        || ([f[(__bridge id)kCTFontFeatureTypeIdentifierKey] intValue] == kFractionsType
            && [f[(__bridge id)kCTFontFeatureSelectorIdentifierKey] intValue] == kDiagonalFractionsSelector))
      has_frac = YES;
  }
  CHECK(has_frac, "style 128's font has no frac feature: %s", [feats description].UTF8String);
  for (NSString *s in @[ @"1/2", @"24/7", @"3/4 cup" ]) {
    double w0 = shaped_width(s, 0), w1 = shaped_width(s, 128);
    CHECK(w1 < w0 - 1, "\"%s\" not set as a fraction: %f vs %f unset", s.UTF8String, w1, w0);
    CTLineRef line = make_line(s, font_for(15, 400, 128), 128);
    double want = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line);
    CHECK(fabs(w1 - want) < 1e-6, "\"%s\" shaped %f, Core Text's line %f", s.UTF8String, w1, want);
  }
  CHECK(key_of(@"1", 15, 128) != key_of(@"1", 15, 0) && key_of(@"1", 15, 128) != key_of(@"1", 15, 32),
        "a digit's key shared across fraction and plain styles");
  CHECK(bmp_for(15, 400, 128) != bmp_for(15, 400, 0), "paged keys shared with style 0");
  CHECK(!bmp_for(15, 400, 128)->ascii_nom_ready, "nominal advances measured with fractions");
  printf("fractions: ok\n");
}

// The single-unit fast path's technical, math, box-drawing, check/cross and Latin-1 symbols (is_unit_clusters): taken,
// each unit a cluster of its own as Foundation cuts them, and shaped as shape_line_ref shapes them; ↔, ©, ®, the soft
// hyphen and an emoji with a modifier still kept off it, and shaped as the reference too.
static void unit_fastpath(void) {
  NSString *fast = @"延迟 145ms → 125ms ← ↑ ↓ ≤ 16ms ≥ 0ms ≠ 1 ≈ 99% −5 ∞ ✓ ✗ ┌─┬─┐ │ └─┴─┘ café «ok» ¹²³ ½ ¿ × ÷";
  NSArray<NSString *> *slow = @[ @"a ↔ b", @"© 2026", @"Brand®", @"soft\u00ADhyphen", @"ok 👍🏻" ];
  unichar buf[512];
  NSUInteger n = fast.length;
  [fast getCharacters:buf range:NSMakeRange(0, n)];
  CHECK(is_unit_clusters(buf, (int32_t)n), "fast path not taken for \"%s\"", fast.UTF8String);
  for (NSUInteger i = 0; i < n; i++) {
    NSRange r = [fast rangeOfComposedCharacterSequenceAtIndex:i];
    CHECK(r.location == i && r.length == 1, "U+%04X not a cluster of its own", buf[i]);
  }
  for (NSString *s in slow) {
    NSUInteger m = s.length;
    [s getCharacters:buf range:NSMakeRange(0, m)];
    CHECK(!is_unit_clusters(buf, (int32_t)m), "fast path taken for \"%s\"", s.UTF8String);
  }
  double got[4 * 512], want[4 * 512];
  int cases = 0;
  for (NSString *s in [@[ fast ] arrayByAddingObjectsFromArray:slow]) {
    NSUInteger m = s.length;
    [s getCharacters:buf range:NSMakeRange(0, m)];
    for (int wt = 400; wt <= 700; wt += 300) {
      int32_t k_ref = shape_line_ref(buf, (int32_t)m, 15.0, wt, 0, want, 512);
      int32_t k_got = shape_line((moonbit_string_t)buf, (int32_t)m, 15.0, wt, 0, got, 512);
      CHECK(k_got == k_ref, "cluster count %d != %d weight=%d in \"%s\"", k_got, k_ref, wt, s.UTF8String);
      if (k_got != k_ref) continue;
      for (int32_t c = 0; c < k_ref; c++) {
        CHECK(got[c * 4] == want[c * 4] && got[c * 4 + 1] == want[c * 4 + 1], "range mismatch at %d in \"%s\"", c,
              s.UTF8String);
        CHECK(fabs(got[c * 4 + 2] - want[c * 4 + 2]) < 1e-6, "adv mismatch at %d (%.6f != %.6f) in \"%s\"", c,
              got[c * 4 + 2], want[c * 4 + 2], s.UTF8String);
        int gk = (int)got[c * 4 + 3], wk = (int)want[c * 4 + 3];
        CHECK(gk == wk || (gk >= 0 && wk >= 0 && [texts[gk] isEqualToString:texts[wk]]
                           && fabs(entries[gk].dx - entries[wk].dx) < 1e-6),
              "key mismatch at %d (%d != %d) in \"%s\"", c, gk, wk, s.UTF8String);
      }
      cases++;
    }
  }
  printf("unit_fastpath: %d cases matched reference\n", cases);
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
    shape_parity();
    unit_fastpath();
    fractions();
    ascii_nominal();
  }
  if (failed) return 1;
  printf("apple_text_test: ok\n");
  return 0;
}
