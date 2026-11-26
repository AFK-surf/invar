// packages/host/native/apple_grab.h and packages/host/native/ios_nav_state.h deterministic tests:
//   cc -Wall -Werror -fobjc-arc packages/host/tests/apple_grab_nav_test.m \
//     -framework Foundation -framework CoreGraphics -framework CoreVideo -framework ImageIO \
//     -o target/apple_grab_nav_test && target/apple_grab_nav_test
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <math.h>
#include <stdio.h>
#import "../native/apple_grab.h"
#import "../native/ios_nav_state.h"

static int failed;

#define CHECK(c)                                                      \
  do {                                                                \
    if (!(c)) {                                                       \
      fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); \
      failed = 1;                                                     \
    }                                                                 \
  } while (0)

static CVPixelBufferRef make_bgra_pixel_buffer(size_t w, size_t h) {
  CVPixelBufferRef pb = NULL;
  NSDictionary *attrs = @{
    (__bridge NSString *)kCVPixelBufferCGImageCompatibilityKey : @YES,
    (__bridge NSString *)kCVPixelBufferCGBitmapContextCompatibilityKey : @YES,
  };
  CVReturn r = CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA,
                                   (__bridge CFDictionaryRef)attrs, &pb);
  if (r != kCVReturnSuccess || !pb) return NULL;
  CVPixelBufferLockBaseAddress(pb, 0);
  uint8_t *base = (uint8_t *)CVPixelBufferGetBaseAddress(pb);
  size_t bpr = CVPixelBufferGetBytesPerRow(pb);
  for (size_t y = 0; y < h; y++) {
    uint8_t *row = base + y * bpr;
    for (size_t x = 0; x < w; x++) {
      row[x * 4 + 0] = (uint8_t)((x * 255) / (w > 1 ? w - 1 : 1));
      row[x * 4 + 1] = (uint8_t)((y * 255) / (h > 1 ? h - 1 : 1));
      row[x * 4 + 2] = (uint8_t)((x + y) & 0xFF);
      row[x * 4 + 3] = 0xFF;
    }
  }
  CVPixelBufferUnlockBaseAddress(pb, 0);
  return pb;
}

static CGImageRef make_cg_image(size_t w, size_t h) {
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
  CGColorSpaceRelease(cs);
  if (!c) return NULL;
  CGContextSetRGBFillColor(c, 0.25, 0.55, 0.85, 1.0);
  CGContextFillRect(c, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h));
  CGImageRef img = CGBitmapContextCreateImage(c);
  CGContextRelease(c);
  return img;
}

static BOOL verify_jpeg_size(NSData *data, size_t expectedW, size_t expectedH) {
  if (!data || data.length < 4) return NO;
  const uint8_t *bytes = (const uint8_t *)data.bytes;
  if (bytes[0] != 0xFF || bytes[1] != 0xD8) return NO;
  if (bytes[data.length - 2] != 0xFF || bytes[data.length - 1] != 0xD9) return NO;
  CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
  if (!src) return NO;
  CGImageRef decoded = CGImageSourceCreateImageAtIndex(src, 0, NULL);
  CFRelease(src);
  if (!decoded) return NO;
  BOOL ok = CGImageGetWidth(decoded) == expectedW && CGImageGetHeight(decoded) == expectedH;
  CGImageRelease(decoded);
  return ok;
}

static void test_apple_grab(void) {
  // 1. 1920x1080 32BGRA CVPixelBuffer scaled to 640 -> valid 640x360 JPEG.
  CVPixelBufferRef pb1080 = make_bgra_pixel_buffer(1920, 1080);
  CHECK(pb1080 != NULL);
  if (pb1080) {
    NSData *jpeg = mb_apple_encode_frame_jpeg(NULL, pb1080, 640.0);
    CHECK(verify_jpeg_size(jpeg, 640, 360));
    CVPixelBufferRelease(pb1080);
  }

  // 2. Generic CGImageRef path 800x600 -> 400x300.
  CGImageRef cg800 = make_cg_image(800, 600);
  CHECK(cg800 != NULL);
  if (cg800) {
    NSData *jpeg = mb_apple_encode_frame_jpeg(cg800, NULL, 400.0);
    CHECK(verify_jpeg_size(jpeg, 400, 300));
    CGImageRelease(cg800);
  }

  // 3. No-upscale path: 320x240 with side 640 stays 320x240.
  CVPixelBufferRef pbSmall = make_bgra_pixel_buffer(320, 240);
  CHECK(pbSmall != NULL);
  if (pbSmall) {
    NSData *jpeg = mb_apple_encode_frame_jpeg(NULL, pbSmall, 640.0);
    CHECK(verify_jpeg_size(jpeg, 320, 240));

    // 4. Invalid side boundaries (0, negative, NaN, Inf) fall back safely to 1:1.
    CHECK(verify_jpeg_size(mb_apple_encode_frame_jpeg(NULL, pbSmall, 0.0), 320, 240));
    CHECK(verify_jpeg_size(mb_apple_encode_frame_jpeg(NULL, pbSmall, -120.0), 320, 240));
    CHECK(verify_jpeg_size(mb_apple_encode_frame_jpeg(NULL, pbSmall, NAN), 320, 240));
    CHECK(verify_jpeg_size(mb_apple_encode_frame_jpeg(NULL, pbSmall, INFINITY), 320, 240));
    CVPixelBufferRelease(pbSmall);
  }

  // 5. NULL inputs and non-32BGRA / planar pixel buffers return nil safely.
  CHECK(mb_apple_encode_frame_jpeg(NULL, NULL, 640.0) == nil);
  CVPixelBufferRef pbYuv = NULL;
  CVReturn yuvRes = CVPixelBufferCreate(kCFAllocatorDefault, 64, 64, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                        NULL, &pbYuv);
  CHECK(yuvRes == kCVReturnSuccess && pbYuv != NULL);
  if (pbYuv) {
    CHECK(mb_apple_encode_frame_jpeg(NULL, pbYuv, 640.0) == nil);
    CVPixelBufferRelease(pbYuv);
  }
  // Packed (non-planar) but not 32BGRA: 32ARGB.
  CVPixelBufferRef pbArgb = NULL;
  CVReturn argbRes = CVPixelBufferCreate(kCFAllocatorDefault, 64, 64, kCVPixelFormatType_32ARGB, NULL, &pbArgb);
  CHECK(argbRes == kCVReturnSuccess && pbArgb != NULL);
  if (pbArgb) {
    CHECK(!CVPixelBufferIsPlanar(pbArgb));
    CHECK(mb_apple_encode_frame_jpeg(NULL, pbArgb, 640.0) == nil);
    CVPixelBufferRelease(pbArgb);
  }

  // 6. Encoding runs safely on concurrent background queues: the same buffer encoded off the caller's thread at once.
  CVPixelBufferRef pbAsync = make_bgra_pixel_buffer(640, 360);
  CHECK(pbAsync != NULL);
  if (pbAsync) {
    enum { N = 8 };
    __block int ok = 0;
    __block int offMain = 0;
    NSLock *lock = [NSLock new];
    dispatch_group_t group = dispatch_group_create();
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    for (int i = 0; i < N; i++) {
      CVPixelBufferRetain(pbAsync);
      dispatch_group_async(group, q, ^{
        NSData *jpeg = mb_apple_encode_frame_jpeg(NULL, pbAsync, 320.0);
        CVPixelBufferRelease(pbAsync);
        BOOL good = verify_jpeg_size(jpeg, 320, 180);
        BOOL bg = ![NSThread isMainThread];
        [lock lock];
        if (good) ok++;
        if (bg) offMain++;
        [lock unlock];
      });
    }
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
    CHECK(ok == N);
    CHECK(offMain == N);
    CVPixelBufferRelease(pbAsync);
  }
}

static void test_ios_nav_state(void) {
  // 1. Corner cache matrix: 8 queries on same window/bounds => 1 miss + 7 hits.
  MBCornerCache cache = {0};
  uintptr_t win1 = 0x1001, win2 = 0x2002;
  CGRect b1 = CGRectMake(0, 0, 669, 951);
  CGRect b2 = CGRectMake(0, 0, 951, 669);
  double radii1[4] = {8.0, 59.0, 59.0, 8.0};
  int probes = 0;
  for (int i = 0; i < 8; i++) {
    double r = -1;
    if (!mb_corner_cache_hit(&cache, win1, b1, i & 3, &r)) {
      probes++;
      mb_corner_cache_store(&cache, win1, b1, radii1);
      r = radii1[i & 3];
    }
    CHECK(r == radii1[i & 3]);
  }
  CHECK(probes == 1);

  // Changed bounds misses.
  double outR = -1;
  CHECK(!mb_corner_cache_hit(&cache, win1, b2, 0, &outR));
  // Changed window misses.
  CHECK(!mb_corner_cache_hit(&cache, win2, b1, 0, &outR));
  // Explicit invalidation misses.
  CHECK(mb_corner_cache_hit(&cache, win1, b1, 1, &outR) && outR == 59.0);
  mb_corner_cache_invalidate(&cache);
  CHECK(!mb_corner_cache_hit(&cache, win1, b1, 1, &outR));
  // Zero winTag never hits.
  mb_corner_cache_store(&cache, 0, b1, radii1);
  CHECK(!mb_corner_cache_hit(&cache, 0, b1, 0, &outR));

  // 2. Lazy slot appliedActsKey matrix.
  CHECK(mb_ios_nav_slot_needs_bar_items(nil, @"acts-v1", YES));
  CHECK(!mb_ios_nav_slot_needs_bar_items(@"acts-v1", @"acts-v1", YES));
  CHECK(mb_ios_nav_slot_needs_bar_items(@"acts-v1", @"acts-v2", YES));
  CHECK(!mb_ios_nav_slot_needs_bar_items(nil, @"acts-v1", NO));
  CHECK(!mb_ios_nav_slot_needs_bar_items(nil, nil, YES));
  CHECK(!mb_ios_nav_slot_needs_bar_items(nil, @"", YES));

  // 3. Icon cache key uniqueness and determinism.
  NSString *k1 = mb_ios_nav_icon_cache_key('i', 24.0, 3.0, @"0 2 2 1 1 23 23");
  NSString *k1b = mb_ios_nav_icon_cache_key('i', 24.0, 3.0, @"0 2 2 1 1 23 23");
  NSString *kKind = mb_ios_nav_icon_cache_key('b', 24.0, 3.0, @"0 2 2 1 1 23 23");
  NSString *kSize = mb_ios_nav_icon_cache_key('i', 26.0, 3.0, @"0 2 2 1 1 23 23");
  NSString *kScale = mb_ios_nav_icon_cache_key('i', 24.0, 2.0, @"0 2 2 1 1 23 23");
  NSString *kSpec = mb_ios_nav_icon_cache_key('i', 24.0, 3.0, @"0 2 2 2 2 22 22");
  CHECK([k1 isEqualToString:k1b]);
  CHECK(![k1 isEqualToString:kKind]);
  CHECK(![k1 isEqualToString:kSize]);
  CHECK(![k1 isEqualToString:kScale]);
  CHECK(![k1 isEqualToString:kSpec]);
}

int main(void) {
  @autoreleasepool {
    test_apple_grab();
    test_ios_nav_state();
  }
  if (!failed) printf("apple_grab_nav_test: ok\n");
  return failed;
}
