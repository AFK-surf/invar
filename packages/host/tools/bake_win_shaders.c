// bake_win_shaders <dir of .cso> <out .h>: the Windows host's compiled shaders (win_render.cpp's kept bytecode, one
// <key>.cso each, as a run with MBUI_SHADERS=<dir> leaves them) written as native/win_shaders_baked.h, so a program
// finds them in itself and compiles none as it starts (D3DCompile + D3DReflect of the glass shaders: ~4 s on a first
// start). A key no longer made by the sources (a shader changed) is simply never asked for: what is not baked is
// compiled and kept as before. Each shader is compressed with a deterministic, zero-dependency RFC 1951 raw-DEFLATE
// encoder and verified with ud_inflate() before writing. Build with any C99 compiler:
// cc -std=c99 -O2 bake_win_shaders.c -o bake_win_shaders
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UD_INFLATE_ONLY
#include "../native/update_delta.h"

// ---- deterministic RFC 1951 raw-DEFLATE encoder ----

enum {
  DF_WIN = 32768,
  DF_WMASK = DF_WIN - 1,
  DF_HASH_BITS = 15,
  DF_HASH_SIZE = 1 << DF_HASH_BITS,
  DF_HASH_MASK = DF_HASH_SIZE - 1,
  DF_MAX_CHAIN = 256,
  DF_MIN_MATCH = 3,
  DF_MAX_MATCH = 258,
  DF_BLK_TOKENS = 65536
};

typedef struct {
  unsigned char *buf;
  size_t len;
  size_t cap;
  uint64_t bits;
  int nbits;
  int oom;
} df_bitout;

static void df_push_byte(df_bitout *b, unsigned char v) {
  if (b->oom) return;
  if (b->len >= b->cap) {
    size_t ncap = b->cap ? b->cap * 2 : 4096;
    unsigned char *nb = (unsigned char *)realloc(b->buf, ncap);
    if (!nb) {
      b->oom = 1;
      return;
    }
    b->buf = nb;
    b->cap = ncap;
  }
  b->buf[b->len++] = v;
}

static void df_putbits(df_bitout *b, uint32_t val, int n) {
  b->bits |= (uint64_t)val << b->nbits;
  b->nbits += n;
  while (b->nbits >= 8) {
    df_push_byte(b, (unsigned char)(b->bits & 0xffu));
    b->bits >>= 8;
    b->nbits -= 8;
  }
}

static void df_align_byte(df_bitout *b) {
  if (b->nbits > 0) {
    df_push_byte(b, (unsigned char)(b->bits & 0xffu));
    b->bits = 0;
    b->nbits = 0;
  }
}

static uint16_t df_bitrev(uint32_t code, int bits) {
  uint32_t r = 0;
  for (int i = 0; i < bits; i++) {
    r = (r << 1) | (code & 1u);
    code >>= 1;
  }
  return (uint16_t)r;
}

static int df_len_code(int len) {
  int lo = 0, hi = 28;
  while (lo < hi) {
    int mid = (lo + hi + 1) >> 1;
    if ((int)ud_len_base[mid] <= len) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

static int df_dist_code(int dist) {
  int lo = 0, hi = 29;
  while (lo < hi) {
    int mid = (lo + hi + 1) >> 1;
    if ((int)ud_dist_base[mid] <= dist) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

typedef struct {
  uint64_t w;
  int left;
  int right;
  int sym;
} df_hnode;

static void df_build_lengths(const uint32_t *freq, int nsym, int max_bits, unsigned char *lens) {
  df_hnode nodes[576];
  int depth[576];
  int leaf_sym[288];
  int na = 0;

  memset(lens, 0, (size_t)nsym);
  for (int i = 0; i < nsym; i++) {
    if (freq[i] > 0) leaf_sym[na++] = i;
  }
  if (na == 0) return;
  if (na == 1) {
    lens[leaf_sym[0]] = 1;
    return;
  }

  // Deterministic insertion sort by (freq ascending, sym ascending)
  for (int i = 1; i < na; i++) {
    int s = leaf_sym[i];
    uint32_t f = freq[s];
    int j = i - 1;
    while (j >= 0 && (freq[leaf_sym[j]] > f || (freq[leaf_sym[j]] == f && leaf_sym[j] > s))) {
      leaf_sym[j + 1] = leaf_sym[j];
      j--;
    }
    leaf_sym[j + 1] = s;
  }

  for (int i = 0; i < na; i++) {
    nodes[i].w = freq[leaf_sym[i]];
    nodes[i].left = -1;
    nodes[i].right = -1;
    nodes[i].sym = leaf_sym[i];
  }

  int lq = 0, iq = na, total = na;
  while ((na - lq) + (total - iq) > 1) {
    int pair[2];
    for (int k = 0; k < 2; k++) {
      if (lq < na && (iq >= total || nodes[lq].w <= nodes[iq].w)) {
        pair[k] = lq++;
      } else {
        pair[k] = iq++;
      }
    }
    nodes[total].w = nodes[pair[0]].w + nodes[pair[1]].w;
    nodes[total].left = pair[0];
    nodes[total].right = pair[1];
    nodes[total].sym = -1;
    total++;
  }

  depth[total - 1] = 0;
  for (int i = total - 1; i >= na; i--) {
    depth[nodes[i].left] = depth[i] + 1;
    depth[nodes[i].right] = depth[i] + 1;
  }

  uint32_t max_k = 1u << max_bits;
  uint32_t kraft = 0;
  for (int i = 0; i < na; i++) {
    int d = depth[i];
    if (d > max_bits) d = max_bits;
    lens[leaf_sym[i]] = (unsigned char)d;
    kraft += 1u << (max_bits - d);
  }

  // Rebalance Kraft sum if depth clamping oversubscribed the tree
  while (kraft > max_k) {
    int best = -1;
    for (int i = 0; i < na; i++) {
      unsigned char l = lens[leaf_sym[i]];
      if (l < (unsigned char)max_bits) {
        if (best < 0 || l > lens[leaf_sym[best]]) best = i;
      }
    }
    lens[leaf_sym[best]]++;
    kraft -= 1u << (max_bits - lens[leaf_sym[best]]);
  }
  while (kraft < max_k) {
    int best = -1;
    for (int i = na - 1; i >= 0; i--) {
      unsigned char l = lens[leaf_sym[i]];
      if (l > 1 && (1u << (max_bits - l)) <= max_k - kraft) {
        best = i;
        break;
      }
    }
    if (best < 0) break;
    kraft += 1u << (max_bits - lens[leaf_sym[best]]);
    lens[leaf_sym[best]]--;
  }
}

static void df_make_codes(const unsigned char *lens, int nsym, uint16_t *codes) {
  uint32_t bl_count[16];
  uint32_t next_code[16];
  memset(bl_count, 0, sizeof bl_count);
  memset(codes, 0, (size_t)nsym * sizeof(uint16_t));
  for (int i = 0; i < nsym; i++) {
    if (lens[i] > 0) bl_count[lens[i]]++;
  }
  uint32_t code = 0;
  for (int bits = 1; bits <= 15; bits++) {
    code = (code + bl_count[bits - 1]) << 1;
    next_code[bits] = code;
  }
  for (int i = 0; i < nsym; i++) {
    int l = lens[i];
    if (l > 0) codes[i] = df_bitrev(next_code[l]++, l);
  }
}

static void df_emit_tokens(df_bitout *b, const uint16_t *tok_litlen, const uint16_t *tok_dist, int ntok,
                           const unsigned char *lit_len, const uint16_t *lit_code,
                           const unsigned char *dist_len, const uint16_t *dist_code) {
  for (int i = 0; i < ntok; i++) {
    if (tok_dist[i] == 0) {
      int sym = tok_litlen[i];
      df_putbits(b, lit_code[sym], lit_len[sym]);
    } else {
      int lc = df_len_code((int)tok_litlen[i]);
      int lsym = 257 + lc;
      df_putbits(b, lit_code[lsym], lit_len[lsym]);
      if (ud_len_extra[lc] > 0) {
        df_putbits(b, (uint32_t)(tok_litlen[i] - ud_len_base[lc]), ud_len_extra[lc]);
      }
      int dc = df_dist_code((int)tok_dist[i]);
      df_putbits(b, dist_code[dc], dist_len[dc]);
      if (ud_dist_extra[dc] > 0) {
        df_putbits(b, (uint32_t)(tok_dist[i] - ud_dist_base[dc]), ud_dist_extra[dc]);
      }
    }
  }
  df_putbits(b, lit_code[256], lit_len[256]);
}

static void df_emit_stored(df_bitout *b, const unsigned char *raw, size_t raw_len, int is_final) {
  size_t off = 0;
  do {
    size_t chunk = raw_len - off;
    if (chunk > 65535) chunk = 65535;
    int last = is_final && (off + chunk == raw_len);
    df_putbits(b, (uint32_t)(last ? 1u : 0u), 3);  // BFINAL=last, BTYPE=00
    df_align_byte(b);
    uint16_t len16 = (uint16_t)chunk;
    uint16_t nlen16 = (uint16_t)(~len16);
    df_push_byte(b, (unsigned char)(len16 & 0xffu));
    df_push_byte(b, (unsigned char)(len16 >> 8));
    df_push_byte(b, (unsigned char)(nlen16 & 0xffu));
    df_push_byte(b, (unsigned char)(nlen16 >> 8));
    for (size_t i = 0; i < chunk; i++) df_push_byte(b, raw[off + i]);
    off += chunk;
  } while (off < raw_len);
}

static void df_flush_block(df_bitout *b, const unsigned char *raw, size_t raw_len,
                           const uint16_t *tok_litlen, const uint16_t *tok_dist, int ntok, int is_final) {
  static const unsigned char cl_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

  uint32_t lit_freq[286];
  uint32_t dist_freq[30];
  memset(lit_freq, 0, sizeof lit_freq);
  memset(dist_freq, 0, sizeof dist_freq);

  uint64_t extra_bits = 0;
  for (int i = 0; i < ntok; i++) {
    if (tok_dist[i] == 0) {
      lit_freq[tok_litlen[i]]++;
    } else {
      int lc = df_len_code((int)tok_litlen[i]);
      int dc = df_dist_code((int)tok_dist[i]);
      lit_freq[257 + lc]++;
      dist_freq[dc]++;
      extra_bits += ud_len_extra[lc] + ud_dist_extra[dc];
    }
  }
  lit_freq[256] = 1;  // EOB

  // Fixed Huffman tables & cost
  unsigned char fix_lit_len[288], fix_dist_len[30];
  uint16_t fix_lit_code[288], fix_dist_code[30];
  for (int i = 0; i < 144; i++) fix_lit_len[i] = 8;
  for (int i = 144; i < 256; i++) fix_lit_len[i] = 9;
  for (int i = 256; i < 280; i++) fix_lit_len[i] = 7;
  for (int i = 280; i < 288; i++) fix_lit_len[i] = 8;
  for (int i = 0; i < 30; i++) fix_dist_len[i] = 5;

  uint64_t fix_bits = 3 + extra_bits;
  for (int i = 0; i < 286; i++) fix_bits += (uint64_t)lit_freq[i] * fix_lit_len[i];
  for (int i = 0; i < 30; i++) fix_bits += (uint64_t)dist_freq[i] * fix_dist_len[i];

  // Dynamic Huffman tables & cost
  unsigned char dyn_lit_len[286], dyn_dist_len[30];
  uint16_t dyn_lit_code[286], dyn_dist_code[30];
  df_build_lengths(lit_freq, 286, 15, dyn_lit_len);
  df_build_lengths(dist_freq, 30, 15, dyn_dist_len);

  int hlit = 286;
  while (hlit > 257 && dyn_lit_len[hlit - 1] == 0) hlit--;
  int hdist = 30;
  while (hdist > 1 && dyn_dist_len[hdist - 1] == 0) hdist--;

  unsigned char all_lens[286 + 30];
  memcpy(all_lens, dyn_lit_len, (size_t)hlit);
  memcpy(all_lens + hlit, dyn_dist_len, (size_t)hdist);
  int nall = hlit + hdist;

  unsigned char rle_sym[286 + 30];
  unsigned char rle_ext[286 + 30];
  int nrle = 0;
  uint32_t cl_freq[19];
  memset(cl_freq, 0, sizeof cl_freq);

  for (int i = 0; i < nall;) {
    unsigned char v = all_lens[i];
    int run = 1;
    while (i + run < nall && all_lens[i + run] == v) run++;
    i += run;
    if (v == 0) {
      while (run >= 11) {
        int c = run > 138 ? 138 : run;
        rle_sym[nrle] = 18;
        rle_ext[nrle++] = (unsigned char)(c - 11);
        cl_freq[18]++;
        run -= c;
      }
      if (run >= 3) {
        rle_sym[nrle] = 17;
        rle_ext[nrle++] = (unsigned char)(run - 3);
        cl_freq[17]++;
        run = 0;
      }
      while (run-- > 0) {
        rle_sym[nrle] = 0;
        rle_ext[nrle++] = 0;
        cl_freq[0]++;
      }
    } else {
      rle_sym[nrle] = v;
      rle_ext[nrle++] = 0;
      cl_freq[v]++;
      run--;
      while (run >= 3) {
        int c = run > 6 ? 6 : run;
        rle_sym[nrle] = 16;
        rle_ext[nrle++] = (unsigned char)(c - 3);
        cl_freq[16]++;
        run -= c;
      }
      while (run-- > 0) {
        rle_sym[nrle] = v;
        rle_ext[nrle++] = 0;
        cl_freq[v]++;
      }
    }
  }

  unsigned char cl_len[19];
  uint16_t cl_code[19];
  df_build_lengths(cl_freq, 19, 7, cl_len);
  int hclen = 19;
  while (hclen > 4 && cl_len[cl_order[hclen - 1]] == 0) hclen--;

  uint64_t dyn_bits = 3 + 14 + (uint64_t)hclen * 3 + extra_bits;
  for (int i = 0; i < nrle; i++) {
    int s = rle_sym[i];
    dyn_bits += cl_len[s];
    if (s == 16) dyn_bits += 2;
    else if (s == 17) dyn_bits += 3;
    else if (s == 18) dyn_bits += 7;
  }
  for (int i = 0; i < 286; i++) dyn_bits += (uint64_t)lit_freq[i] * dyn_lit_len[i];
  for (int i = 0; i < 30; i++) dyn_bits += (uint64_t)dist_freq[i] * dyn_dist_len[i];

  // Stored block cost
  size_t nstored = raw_len ? (raw_len + 65534) / 65535 : 1;
  uint64_t pad_bits = (uint64_t)((8 - ((b->nbits + 3) & 7)) & 7);
  uint64_t stored_bits = pad_bits + (uint64_t)nstored * 40 + (uint64_t)raw_len * 8;

  if (stored_bits <= fix_bits && stored_bits <= dyn_bits) {
    df_emit_stored(b, raw, raw_len, is_final);
  } else if (fix_bits <= dyn_bits) {
    df_make_codes(fix_lit_len, 288, fix_lit_code);
    df_make_codes(fix_dist_len, 30, fix_dist_code);
    df_putbits(b, (uint32_t)((is_final ? 1u : 0u) | (1u << 1)), 3);  // BTYPE=01
    df_emit_tokens(b, tok_litlen, tok_dist, ntok, fix_lit_len, fix_lit_code, fix_dist_len, fix_dist_code);
  } else {
    df_make_codes(cl_len, 19, cl_code);
    df_make_codes(dyn_lit_len, 286, dyn_lit_code);
    df_make_codes(dyn_dist_len, 30, dyn_dist_code);
    df_putbits(b, (uint32_t)((is_final ? 1u : 0u) | (2u << 1)), 3);  // BTYPE=10
    df_putbits(b, (uint32_t)(hlit - 257), 5);
    df_putbits(b, (uint32_t)(hdist - 1), 5);
    df_putbits(b, (uint32_t)(hclen - 4), 4);
    for (int i = 0; i < hclen; i++) df_putbits(b, cl_len[cl_order[i]], 3);
    for (int i = 0; i < nrle; i++) {
      int s = rle_sym[i];
      df_putbits(b, cl_code[s], cl_len[s]);
      if (s == 16) df_putbits(b, rle_ext[i], 2);
      else if (s == 17) df_putbits(b, rle_ext[i], 3);
      else if (s == 18) df_putbits(b, rle_ext[i], 7);
    }
    df_emit_tokens(b, tok_litlen, tok_dist, ntok, dyn_lit_len, dyn_lit_code, dyn_dist_len, dyn_dist_code);
  }
}

static uint32_t df_hash3(const unsigned char *p) {
  uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
  return (v * 2654435761u) >> (32 - DF_HASH_BITS);
}

static void df_insert(int32_t *head, int32_t *prev, const unsigned char *in, size_t pos) {
  uint32_t h = df_hash3(in + pos) & DF_HASH_MASK;
  prev[pos & DF_WMASK] = head[h];
  head[h] = (int32_t)pos;
}

static int df_find_match(const int32_t *head, const int32_t *prev, const unsigned char *in, size_t n, size_t pos,
                         int *out_dist) {
  if (pos + DF_MIN_MATCH > n) return 0;
  uint32_t h = df_hash3(in + pos) & DF_HASH_MASK;
  int32_t cur = head[h];
  int max_len = (int)(n - pos < DF_MAX_MATCH ? n - pos : DF_MAX_MATCH);
  int best_len = DF_MIN_MATCH - 1;
  int best_dist = 0;
  int chain = DF_MAX_CHAIN;

  while (cur >= 0 && chain-- > 0) {
    size_t dist = pos - (size_t)cur;
    if (dist == 0 || dist > DF_WIN) break;
    if (in[(size_t)cur + best_len] == in[pos + best_len] && in[cur] == in[pos] && in[cur + 1] == in[pos + 1] &&
        in[cur + 2] == in[pos + 2]) {
      int len = 3;
      while (len < max_len && in[(size_t)cur + (size_t)len] == in[pos + (size_t)len]) len++;
      if (len > best_len) {
        best_len = len;
        best_dist = (int)dist;
        if (best_len >= max_len) break;
      }
    }
    int32_t nxt = prev[(size_t)cur & DF_WMASK];
    if (nxt >= cur) break;
    cur = nxt;
  }
  if (best_len >= DF_MIN_MATCH) {
    *out_dist = best_dist;
    return best_len;
  }
  return 0;
}

static unsigned char *df_deflate(const unsigned char *in, size_t n, size_t *out_len) {
  df_bitout b;
  memset(&b, 0, sizeof b);
  if (n == 0) {
    df_emit_stored(&b, in, 0, 1);
    df_align_byte(&b);
    if (b.oom) {
      free(b.buf);
      return NULL;
    }
    *out_len = b.len;
    return b.buf;
  }

  int32_t *head = (int32_t *)malloc(DF_HASH_SIZE * sizeof(int32_t));
  int32_t *prev = (int32_t *)malloc(DF_WIN * sizeof(int32_t));
  uint16_t *tok_litlen = (uint16_t *)malloc(DF_BLK_TOKENS * sizeof(uint16_t));
  uint16_t *tok_dist = (uint16_t *)malloc(DF_BLK_TOKENS * sizeof(uint16_t));
  if (!head || !prev || !tok_litlen || !tok_dist) {
    free(head);
    free(prev);
    free(tok_litlen);
    free(tok_dist);
    return NULL;
  }
  for (int i = 0; i < DF_HASH_SIZE; i++) head[i] = -1;
  for (int i = 0; i < DF_WIN; i++) prev[i] = -1;

  int ntok = 0;
  size_t blk_start = 0;
  size_t pos = 0;
  while (pos < n) {
    int mdist = 0;
    int mlen = df_find_match(head, prev, in, n, pos, &mdist);
    if (mlen >= DF_MIN_MATCH) {
      // Lazy match: check if pos + 1 yields a strictly better match
      if (pos + DF_MIN_MATCH <= n) df_insert(head, prev, in, pos);
      int ndist = 0;
      int nlen = (mlen < 128 && pos + 1 + DF_MIN_MATCH <= n) ? df_find_match(head, prev, in, n, pos + 1, &ndist) : 0;
      if (nlen > mlen || (nlen == mlen && ndist < mdist / 4)) {
        tok_litlen[ntok] = in[pos];
        tok_dist[ntok] = 0;
        ntok++;
        pos++;
      } else {
        tok_litlen[ntok] = (uint16_t)mlen;
        tok_dist[ntok] = (uint16_t)mdist;
        ntok++;
        for (int k = 1; k < mlen; k++) {
          if (pos + (size_t)k + DF_MIN_MATCH <= n) df_insert(head, prev, in, pos + (size_t)k);
        }
        pos += (size_t)mlen;
      }
    } else {
      if (pos + DF_MIN_MATCH <= n) df_insert(head, prev, in, pos);
      tok_litlen[ntok] = in[pos];
      tok_dist[ntok] = 0;
      ntok++;
      pos++;
    }

    if (ntok >= DF_BLK_TOKENS || pos >= n) {
      df_flush_block(&b, in + blk_start, pos - blk_start, tok_litlen, tok_dist, ntok, pos >= n ? 1 : 0);
      blk_start = pos;
      ntok = 0;
    }
  }

  df_align_byte(&b);
  free(head);
  free(prev);
  free(tok_litlen);
  free(tok_dist);
  if (b.oom) {
    free(b.buf);
    return NULL;
  }
  *out_len = b.len;
  return b.buf;
}

static char *dup_str(const char *s) {
  size_t len = strlen(s) + 1;
  char *out = (char *)malloc(len);
  if (out) memcpy(out, s, len);
  return out;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: bake_win_shaders <dir> <out.h>\n");
    return 2;
  }
  char *names[256];
  uint32_t raw_sizes[256];
  int n = 0;
#ifdef _WIN32
  char pattern[4096];
  snprintf(pattern, sizeof pattern, "%s\\*.cso", argv[1]);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "bake_win_shaders: cannot open %s\n", argv[1]);
    return 1;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    size_t l = strlen(fd.cFileName);
    if (l == 20 && !strcmp(fd.cFileName + 16, ".cso") && n < 256) {
      names[n++] = dup_str(fd.cFileName);
    }
  } while (FindNextFileA(h, &fd) && n < 256);
  FindClose(h);
#else
  DIR *d = opendir(argv[1]);
  if (!d) {
    perror(argv[1]);
    return 1;
  }
  struct dirent *e;
  while ((e = readdir(d)) && n < 256) {
    size_t l = strlen(e->d_name);
    if (l == 20 && !strcmp(e->d_name + 16, ".cso")) names[n++] = dup_str(e->d_name);
  }
  closedir(d);
#endif
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (strcmp(names[i], names[j]) > 0) {
        char *t = names[i];
        names[i] = names[j];
        names[j] = t;
      }
  FILE *o = fopen(argv[2], "wb");
  if (!o) {
    perror(argv[2]);
    return 1;
  }
  fprintf(o, "// Generated by tools/bake_win_shaders.c from a run's kept shaders: do not edit. win_render.cpp's compile()\n");
  fprintf(o, "// takes a shader from here by its key before compiling it.\n#pragma once\n\n");
  int baked = 0;
  size_t total_raw = 0, total_z = 0;
  for (int i = 0; i < n; i++) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", argv[1], names[i]);
    FILE *f = fopen(path, "rb");
    if (!f) {
      free(names[i]);
      continue;
    }
    unsigned char head[16];
    if (fread(head, 1, 16, f) != 16 || memcmp(head, "MBSH", 4) != 0) {
      fclose(f);
      free(names[i]);
      continue;
    }
    uint32_t size = (uint32_t)head[4] | (uint32_t)head[5] << 8 | (uint32_t)head[6] << 16 | (uint32_t)head[7] << 24;
    unsigned char *code = (unsigned char *)malloc(size ? size : 1);
    if (!code || fread(code, 1, size, f) != size) {
      fclose(f);
      free(code);
      free(names[i]);
      continue;
    }
    fclose(f);

    size_t zsize = 0;
    unsigned char *zcode = df_deflate(code, size, &zsize);
    if (!zcode) {
      fprintf(stderr, "bake_win_shaders: deflate failed for %s\n", names[i]);
      free(code);
      fclose(o);
      return 1;
    }

    // Round-trip self-check using ud_inflate()
    unsigned char *inflated = (unsigned char *)malloc(size ? size : 1);
    if (!inflated) {
      fprintf(stderr, "bake_win_shaders: out of memory verifying %s\n", names[i]);
      free(zcode);
      free(code);
      fclose(o);
      return 1;
    }
    size_t made = 0, took = 0;
    const char *why = ud_inflate(zcode, zsize, inflated, size, &made, &took);
    if (why != NULL || made != size || took != zsize || (size > 0 && memcmp(inflated, code, size) != 0)) {
      fprintf(stderr, "bake_win_shaders: inflate self-check failed for %s (why=%s, made=%zu/%u, took=%zu/%zu)\n",
              names[i], why ? why : "mismatch", made, size, took, zsize);
      free(inflated);
      free(zcode);
      free(code);
      fclose(o);
      return 1;
    }
    free(inflated);

    fprintf(o, "static const unsigned char baked_shader_%d[%u] = {", baked, (unsigned)zsize);
    for (size_t k = 0; k < zsize; k++) fprintf(o, "%s%u,", k % 24 ? "" : "\n  ", zcode[k]);
    fprintf(o, "\n};\n");
    free(zcode);
    free(code);
    raw_sizes[baked] = size;
    names[baked] = names[i];
    total_raw += size;
    total_z += zsize;
    baked++;
  }
  fprintf(o, "\nstatic const struct {\n  unsigned long long key;\n  unsigned zsize;\n  unsigned size;\n  const unsigned char *code;\n} baked_shaders[] = {\n");
  for (int i = 0; i < baked; i++)
    fprintf(o, "  {0x%.16sULL, sizeof baked_shader_%d, %u, baked_shader_%d},\n", names[i], i, raw_sizes[i], i);
  fprintf(o, "  {0, 0, 0, 0},\n};\n");
  fclose(o);
  fprintf(stderr, "bake_win_shaders: %d shaders (%zu -> %zu bytes, %.1f%% saved) -> %s\n", baked, total_raw, total_z,
          total_raw ? 100.0 * (double)(total_raw - total_z) / (double)total_raw : 0.0, argv[2]);
  return 0;
}
