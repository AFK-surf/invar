// Lock-free, zero-RC, cross-OS-thread native Runloop wake bridge for invar/hostapi.
// Compatible with C11 (Clang/GCC on macOS, iOS, Linux) and MSVC (Windows).
#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <windows.h>

static void *volatile g_native_wake_fn = NULL;
static volatile LONG64 g_test_wake_hits = 0;

void invar_hostapi_set_native_wake(void *fn_ptr) {
  InterlockedExchangePointer(&g_native_wake_fn, fn_ptr);
}

void invar_hostapi_call_native_wake(void) {
  void *p = InterlockedCompareExchangePointer(&g_native_wake_fn, NULL, NULL);
  if (p) {
    void (*fn)(void) = (void (*)(void))(uintptr_t)p;
    fn();
  }
}

void invar_hostapi_test_wake_cb(void) {
  InterlockedIncrement64(&g_test_wake_hits);
}

void *invar_hostapi_test_wake_cb_ptr(void) {
  return (void *)(uintptr_t)invar_hostapi_test_wake_cb;
}

int64_t invar_hostapi_test_wake_hits(void) {
  return (int64_t)InterlockedCompareExchange64(&g_test_wake_hits, 0, 0);
}

void invar_hostapi_test_wake_reset(void) {
  InterlockedExchange64(&g_test_wake_hits, 0);
}

#else
#include <stdatomic.h>

static _Atomic(uintptr_t) g_native_wake_fn = 0;
static _Atomic(int64_t) g_test_wake_hits = 0;

void invar_hostapi_set_native_wake(void *fn_ptr) {
  atomic_store_explicit(&g_native_wake_fn, (uintptr_t)fn_ptr, memory_order_release);
}

void invar_hostapi_call_native_wake(void) {
  uintptr_t p = atomic_load_explicit(&g_native_wake_fn, memory_order_acquire);
  if (p != 0) {
    void (*fn)(void) = (void (*)(void))p;
    fn();
  }
}

void invar_hostapi_test_wake_cb(void) {
  atomic_fetch_add_explicit(&g_test_wake_hits, 1, memory_order_acq_rel);
}

void *invar_hostapi_test_wake_cb_ptr(void) {
  return (void *)(uintptr_t)invar_hostapi_test_wake_cb;
}

int64_t invar_hostapi_test_wake_hits(void) {
  return atomic_load_explicit(&g_test_wake_hits, memory_order_acquire);
}

void invar_hostapi_test_wake_reset(void) {
  atomic_store_explicit(&g_test_wake_hits, 0, memory_order_release);
}
#endif
