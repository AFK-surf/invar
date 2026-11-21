// Minimal macOS platform symbols needed by upstream apple_render.m / apple_text.m / apple_image.m
// when linked standalone without mac_app.m / apple_views.m / apple_stream.m.
#import "apple.h"
#import <AppKit/AppKit.h>

mb_callbacks mb_cb;

double mb_now_ms(void) {
  return CACurrentMediaTime() * 1000.0;
}

CTFontRef mb_platform_font(double size, int weight, int mono) {
  NSFontWeight w = weight >= 700 ? NSFontWeightBold
                   : weight >= 600 ? NSFontWeightSemibold
                   : weight >= 500 ? NSFontWeightMedium
                                   : NSFontWeightRegular;
  NSFont *f = mono ? [NSFont monospacedSystemFontOfSize:size weight:w]
                   : [NSFont systemFontOfSize:size weight:w];
  return (CTFontRef)CFBridgingRetain(f);
}

void mb_host_outside(int32_t kind, int32_t id, int32_t state) {
  (void)kind;
  (void)id;
  (void)state;
}

void mb_host_request_frame(void) {}

id<MTLTexture> mb_stream_texture(int sid) {
  (void)sid;
  return nil;
}

int mb_views_count(void) { return 0; }
void mb_views_draw_under(CGContextRef ctx, double scale) { (void)ctx; (void)scale; }
int mb_views_take_moved(void) { return 0; }
int mb_views_frost(int n, const double *frosts) { (void)n; (void)frosts; return 0; }
