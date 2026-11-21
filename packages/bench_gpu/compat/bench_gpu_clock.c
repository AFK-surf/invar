// Monotonic wall-clock timer (ms), peak RSS (KB), and platform GPU renderer tag for bench_gpu.

#include <stdint.h>
#include <sys/resource.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

double invar_bench_gpu_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1.0e-6;
}

int64_t invar_bench_gpu_rss_kb(void) {
  int64_t peak_kb = 0;
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
#if defined(__APPLE__)
    peak_kb = (int64_t)(ru.ru_maxrss / 1024);
#else
    peak_kb = (int64_t)ru.ru_maxrss;
#endif
  }
#if defined(__APPLE__)
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
    int64_t cur_kb = (int64_t)(info.resident_size / 1024);
    if (cur_kb > peak_kb) {
      peak_kb = cur_kb;
    }
  }
#endif
  return peak_kb;
}

extern void mb_render_draw_timed(const double *inst, int32_t count, double r, double g, double b, double *out);

void invar_bench_gpu_draw_timed(const double *inst, int32_t count, double r, double g, double b, double *out) {
  mb_render_draw_timed(inst, count, r, g, b, out);
}

int32_t invar_bench_gpu_platform_id(void) {
#if defined(__APPLE__)
  return 1; // Apple Metal
#elif defined(__linux__)
  return 2; // EGL/GLES3
#else
  return 0;
#endif
}
