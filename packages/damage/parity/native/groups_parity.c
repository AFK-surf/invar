// groups.h for groups_parity_test.pion: a frame drawn out, each float's bits as a number.
#include "groups.h"

int32_t groups_parity_floats(double *bits, double *frame, int32_t count) {
  size_t n = (size_t)count * MB_GROUP_INST;
  float *f = (float *)malloc(sizeof(float) * (n ? n : 1));
  if (!f) return 0;
  mb_groups_floats(f, frame, count);
  for (size_t i = 0; i < n; i++) {
    int32_t b;
    memcpy(&b, &f[i], sizeof b);
    bits[i] = b;
  }
  free(f);
  return 1;
}
