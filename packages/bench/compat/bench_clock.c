// Monotonic wall-clock timer (ms) and peak RSS (KB) for Invar CPU/GPU benchmarks.
// Zero GPU dependencies; portable across macOS, Linux, and Windows.

#include <stdint.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

double invar_bench_now_ms(void) {
  static LARGE_INTEGER freq = {0};
  if (freq.QuadPart == 0) {
    QueryPerformanceFrequency(&freq);
  }
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}

int64_t invar_bench_rss_kb(void) {
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return (int64_t)(pmc.PeakWorkingSetSize / 1024);
  }
  return 0;
}

#else
#include <sys/resource.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

double invar_bench_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1.0e-6;
}

int64_t invar_bench_rss_kb(void) {
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
#endif
