#include "core_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* M3.4 分段计时：6.8 MB tokenizer.json 在真机上曾耗 75 s，必须能看到卡在哪一步 */
#ifdef __ANDROID__
#include <android/log.h>
#include <time.h>
#define TK_LOG(...) __android_log_print(ANDROID_LOG_INFO, "ClassMate/Tok", __VA_ARGS__)
static long tk_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
#else
#define TK_LOG(...) printf(__VA_ARGS__)
static long tk_ms(void) { return 0; }
#endif

/*
 * M2.7：随包 tokenizer.json（HuggingFace tokenizers 格式）的最小 BPE 计数器。
 *
 * 为什么不用字数估：本地 Qwen1.5B 的 ctx 只有 2048，中文一句 30 字可能是 20
 * 个 token，也可能是 35 个；按字数硬估会把 prompt 顶出窗口。这里真正解析
 * model.vocab / model.merges，按 merge rank 做 BPE，只算 token 数（不生成文本）。
 *
 * 只覆盖 Qwen / GPT-2 系 byte-level BPE 的公共子集：vocab 的 key 就是
 * byte-to-unicode 映射后的字符串，merges 是 "A B" 形式。解析失败返回 -1，
 * 由调用方退化到 ca_token_estimate（并打日志、标 estimated）。
 */

typedef struct {
  char*   key;
  int     len;
  int32_t id;
  int     next;   /* 同桶链表下标，-1 结尾 */
} tok_vocab;

typedef struct {
  uint64_t key;      /* (idA << 32) | idB */
  int32_t  rank;
  int32_t  result;   /* merge 后合并 token 的 vocab id */
  int      used;
} tok_pair;

struct ca_tokenizer {
  tok_vocab* v;
  int        vn;
  int        vcap;
  int*       vb;     /* vocab 桶 */
  int        nb;     /* 2 的幂 */
  tok_pair*  pm;
  int        pn;     /* 2 的幂 */
  int        pn_used;
  unsigned char byte_repr[256][5];
  int           byte_repr_len[256];
};

/* ---------- 基础工具 ---------- */

static uint32_t hash_bytes(const char* s, int len) {
  uint32_t h = 2166136261u;
  int i;
  for (i = 0; i < len; ++i) {
    h ^= (uint32_t)(unsigned char)s[i];
    h *= 16777619u;
  }
  return h;
}

static int is_high_surrogate(uint32_t cp) { return cp >= 0xD800 && cp <= 0xDBFF; }
static int is_low_surrogate(uint32_t cp)  { return cp >= 0xDC00 && cp <= 0xDFFF; }

static void append_utf8(char* out, int cap, int* len, uint32_t cp) {
  if (*len + 4 >= cap) return;
  if (cp < 0x80) {
    out[(*len)++] = (char)cp;
  } else if (cp < 0x800) {
    out[(*len)++] = (char)(0xC0 | (cp >> 6));
    out[(*len)++] = (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out[(*len)++] = (char)(0xE0 | (cp >> 12));
    out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[(*len)++] = (char)(0x80 | (cp & 0x3F));
  } else {
    out[(*len)++] = (char)(0xF0 | (cp >> 18));
    out[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[(*len)++] = (char)(0x80 | (cp & 0x3F));
  }
}

static int hex4(const char* p, uint32_t* out) {
  int i;
  uint32_t v = 0;
  for (i = 0; i < 4; ++i) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    else return 0;
  }
  *out = v;
  return 1;
}

/* p 指向 '"'；把字符串解码进 out（容量 cap），返回结束后的指针。
 * 失败返回 NULL。out 允许为 NULL（只求长度/跳过）。 */
static const char* json_string(const char* p, char* out, int cap, int* out_len) {
  int n = 0;

  if (!p || *p != '"') return NULL;
  p++;
  while (*p && *p != '"') {
    if (*p == '\\') {
      p++;
      switch (*p) {
        case '"':  if (out && n < cap) out[n] = '"';  n++; p++; break;
        case '\\': if (out && n < cap) out[n] = '\\'; n++; p++; break;
        case '/':  if (out && n < cap) out[n] = '/';  n++; p++; break;
        case 'b':  if (out) { if (n < cap) out[n] = '\b'; } n++; p++; break;
        case 'f':  if (out) { if (n < cap) out[n] = '\f'; } n++; p++; break;
        case 'n':  if (out) { if (n < cap) out[n] = '\n'; } n++; p++; break;
        case 'r':  if (out) { if (n < cap) out[n] = '\r'; } n++; p++; break;
        case 't':  if (out) { if (n < cap) out[n] = '\t'; } n++; p++; break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(p + 1, &cp)) return NULL;
          p += 5;
          if (is_high_surrogate(cp) && p[0] == '\\' && p[1] == 'u') {
            uint32_t lo = 0;
            if (hex4(p + 2, &lo) && is_low_surrogate(lo)) {
              cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
              p += 6;
            }
          }
          {
            char tmp[8];
            int tn = 0;
            append_utf8(tmp, (int)sizeof(tmp), &tn, cp);
            if (out) {
              int i;
              for (i = 0; i < tn; ++i) {
                if (n < cap) out[n] = tmp[i];
                n++;
              }
            } else {
              n += tn;
            }
          }
          break;
        }
        default:
          return NULL;
      }
    } else {
      if (out && n < cap) out[n] = *p;
      n++;
      p++;
    }
  }
  if (*p != '"') return NULL;
  p++;
  if (out_len) *out_len = n;
  if (out && cap > 0) {
    int idx = n < cap ? n : cap - 1;
    out[idx] = '\0';
  }
  return p;
}

/* ---------- vocab ---------- */

static void vocab_rehash(ca_tokenizer* t, int nb) {
  int i;
  free(t->vb);
  t->nb = nb;
  t->vb = (int*)malloc((size_t)nb * sizeof(int));
  if (!t->vb) { t->nb = 0; return; }
  for (i = 0; i < nb; ++i) t->vb[i] = -1;
  for (i = 0; i < t->vn; ++i) {
    uint32_t h = hash_bytes(t->v[i].key, t->v[i].len);
    int b = (int)(h & (uint32_t)(nb - 1));
    t->v[i].next = t->vb[b];
    t->vb[b] = i;
  }
}

static int vocab_add(ca_tokenizer* t, char* key, int len, int32_t id) {
  if (!t->vb) {
    vocab_rehash(t, 1 << 18);
    if (!t->vb) return 0;
  }
  if (t->vn >= t->nb / 2) {
    vocab_rehash(t, t->nb * 2);
    if (!t->vb) return 0;
  }
  if (t->vn >= t->vcap) {
    int ncap = t->vcap ? t->vcap * 2 : 1024;
    tok_vocab* p = (tok_vocab*)realloc(t->v, (size_t)ncap * sizeof(tok_vocab));
    if (!p) return 0;
    t->v = p;
    t->vcap = ncap;
  }
  t->v[t->vn].key = key;
  t->v[t->vn].len = len;
  t->v[t->vn].id = id;
  t->v[t->vn].next = -1;
  {
    uint32_t h = hash_bytes(key, len);
    int b = (int)(h & (uint32_t)(t->nb - 1));
    t->v[t->vn].next = t->vb[b];
    t->vb[b] = t->vn;
  }
  t->vn++;
  return 1;
}

static int32_t vocab_find(const ca_tokenizer* t, const char* s, int len) {
  uint32_t h;
  int b;
  int idx;

  if (!t || !t->vb) return -1;
  h = hash_bytes(s, len);
  b = (int)(h & (uint32_t)(t->nb - 1));
  for (idx = t->vb[b]; idx >= 0; idx = t->v[idx].next) {
    if (t->v[idx].len == len && memcmp(t->v[idx].key, s, (size_t)len) == 0) {
      return t->v[idx].id;
    }
  }
  return -1;
}

/* ---------- merges（(idA,idB) -> rank/result） ---------- */

static uint64_t pair_key(int32_t a, int32_t b) {
  return ((uint64_t)(uint32_t)a << 32) | (uint64_t)(uint32_t)b;
}

/*
 * M3.4 修复：原来 pair 哈希写成 (uint32_t)(key ^ (key >> 32))，等价于 a XOR b。
 * token id 集中在 0..15 万，a^b 只有 17 位熵且大量撞值 —— 15 万条 merges
 * 在真机上解析要 80 s（logcat: vocab 55 ms / merges 80349 ms），
 * 停录后出稿的 60–90 s 全部耗在这里。这里换成 murmur3 finalizer 混匀。
 */
static uint32_t pair_hash(uint64_t key) {
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdULL;
  key ^= key >> 33;
  key *= 0xc4ceb9fe1a85ec53ULL;
  key ^= key >> 33;
  return (uint32_t)key;
}

static void pair_grow(ca_tokenizer* t) {
  int oldn = t->pn;
  tok_pair* old = t->pm;
  int i;

  t->pn = oldn ? oldn * 2 : 1024;
  t->pm = (tok_pair*)calloc((size_t)t->pn, sizeof(tok_pair));
  if (!t->pm) { t->pm = old; t->pn = oldn; return; }
  t->pn_used = 0;
  for (i = 0; i < oldn; ++i) {
    if (!old[i].used) continue;
    {
      uint32_t h = pair_hash(old[i].key);
      int b = (int)(h & (uint32_t)(t->pn - 1));
      while (t->pm[b].used) b = (b + 1) & (t->pn - 1);
      t->pm[b] = old[i];
    }
    t->pn_used++;
  }
  free(old);
}

static void pair_add(ca_tokenizer* t, int32_t a, int32_t b, int32_t rank, int32_t result) {
  uint64_t key = pair_key(a, b);
  if (!t->pm) pair_grow(t);
  if (!t->pm) return;
  if (t->pn_used * 10 >= t->pn * 7) pair_grow(t);
  {
    uint32_t h = pair_hash(key);
    int i = (int)(h & (uint32_t)(t->pn - 1));
    while (t->pm[i].used) {
      if (t->pm[i].key == key) return;  /* 重复 merge 以先出现的 rank 为准 */
      i = (i + 1) & (t->pn - 1);
    }
    t->pm[i].key = key;
    t->pm[i].rank = rank;
    t->pm[i].result = result;
    t->pm[i].used = 1;
    t->pn_used++;
  }
}

static int pair_find(const ca_tokenizer* t, int32_t a, int32_t b,
                     int32_t* rank, int32_t* result) {
  uint64_t key = pair_key(a, b);
  uint32_t h;
  int i;

  if (!t || !t->pm) return 0;
  h = pair_hash(key);
  i = (int)(h & (uint32_t)(t->pn - 1));
  while (t->pm[i].used) {
    if (t->pm[i].key == key) {
      if (rank) *rank = t->pm[i].rank;
      if (result) *result = t->pm[i].result;
      return 1;
    }
    i = (i + 1) & (t->pn - 1);
  }
  return 0;
}

/* ---------- 解析 tokenizer.json ---------- */

static void build_byte_repr(ca_tokenizer* t) {
  int b, n = 0;

  for (b = 0; b < 256; ++b) {
    uint32_t cp;
    if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255)) {
      cp = (uint32_t)b;
    } else {
      cp = (uint32_t)(256 + n);
      n++;
    }
    t->byte_repr_len[b] = 0;
    append_utf8((char*)t->byte_repr[b], 5, &t->byte_repr_len[b], cp);
  }
}

static const char* skip_ws(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  return p;
}

static int parse_vocab(ca_tokenizer* t, const char* p) {
  p = skip_ws(p);
  if (*p != '{') return 0;
  p++;
  for (;;) {
    char* key;
    int klen = 0;
    const char* after;
    long id;

    p = skip_ws(p);
    if (*p == '}') return 1;
    if (*p != '"') return 0;

    after = json_string(p, NULL, 0, &klen);
    if (!after) return 0;
    key = (char*)malloc((size_t)klen + 1);
    if (!key) return 0;
    if (!json_string(p, key, klen + 1, &klen)) { free(key); return 0; }

    p = skip_ws(after);
    if (*p != ':') { free(key); return 0; }
    p = skip_ws(p + 1);
    id = strtol(p, (char**)&p, 10);
    if (!vocab_add(t, key, klen, (int32_t)id)) { free(key); return 0; }

    p = skip_ws(p);
    if (*p == ',') { p++; continue; }
    if (*p == '}') return 1;
    return 0;
  }
}

static int parse_merges(ca_tokenizer* t, const char* p) {
  int rank = 0;

  p = skip_ws(p);
  if (*p != '[') return 0;
  p++;
  for (;;) {
    char buf[512];
    int blen = 0;
    const char* after;
    char* space;
    int32_t a, b, result;
    char merged[512];
    int mlen;

    p = skip_ws(p);
    if (*p == ']') return 1;
    if (*p != '"') return 0;

    after = json_string(p, buf, (int)sizeof(buf), &blen);
    if (!after) return 0;

    /* merge 串是 "左 右"，左右 token 本身不含空格（空格已编码成 Ġ） */
    space = strchr(buf, ' ');
    if (space) {
      *space = '\0';
      a = vocab_find(t, buf, (int)(space - buf));
      b = vocab_find(t, space + 1, blen - (int)(space - buf) - 1);
      if (a >= 0 && b >= 0) {
        mlen = 0;
        if (blen < (int)sizeof(merged) - 1) {
          memcpy(merged, buf, (size_t)(space - buf));
          mlen = (int)(space - buf);
          memcpy(merged + mlen, space + 1,
                 (size_t)(blen - (int)(space - buf) - 1));
          mlen += blen - (int)(space - buf) - 1;
          merged[mlen] = '\0';
        }
        result = vocab_find(t, merged, mlen);
        if (result < 0) result = -1;
        pair_add(t, a, b, rank, result);
      }
    }
    rank++;
    p = skip_ws(after);
    if (*p == ',') { p++; continue; }
    if (*p == ']') return 1;
    return 0;
  }
}

ca_tokenizer* ca_tokenizer_load(const char* path) {
  FILE* f;
  long size;
  char* data;
  ca_tokenizer* t;
  const char* model;
  const char* p;

  if (!path || !path[0]) return NULL;
  f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  size = ftell(f);
  if (size <= 0 || size > 64 * 1024 * 1024L) { fclose(f); return NULL; }
  rewind(f);
  data = (char*)malloc((size_t)size + 1);
  if (!data) { fclose(f); return NULL; }
  if (fread(data, 1, (size_t)size, f) != (size_t)size) {
    free(data); fclose(f); return NULL;
  }
  data[size] = '\0';
  fclose(f);

  t = (ca_tokenizer*)calloc(1, sizeof(ca_tokenizer));
  if (!t) { free(data); return NULL; }
  build_byte_repr(t);
  long tk_t0 = tk_ms();

  model = strstr(data, "\"model\"");
  if (!model) model = data;

  p = strstr(model, "\"vocab\"");
  if (!p || !parse_vocab(t, strchr(p, ':') ? strchr(p, ':') + 1 : p)) goto fail;
  TK_LOG("vocab %d entries %ldms", t->vn, tk_ms() - tk_t0);

  p = strstr(model, "\"merges\"");
  if (!p || !parse_merges(t, strchr(p, ':') ? strchr(p, ':') + 1 : p)) goto fail;
  TK_LOG("merges %d pairs %ldms", t->pn_used, tk_ms() - tk_t0);

  free(data);
  return t;

fail:
  free(data);
  ca_tokenizer_free(t);
  return NULL;
}

void ca_tokenizer_free(ca_tokenizer* t) {
  int i;
  if (!t) return;
  for (i = 0; i < t->vn; ++i) free(t->v[i].key);
  free(t->v);
  free(t->vb);
  free(t->pm);
  free(t);
}

/* ---------- BPE 计数 ---------- */

typedef struct {
  int32_t rank;
  int32_t li;
  int32_t ri;
} heap_item;

typedef struct {
  heap_item* a;
  int n;
  int cap;
} heap_t;

static void heap_push(heap_t* h, int32_t rank, int32_t li, int32_t ri) {
  int i;
  if (h->n >= h->cap) {
    int ncap = h->cap ? h->cap * 2 : 64;
    heap_item* p = (heap_item*)realloc(h->a, (size_t)ncap * sizeof(heap_item));
    if (!p) return;
    h->a = p;
    h->cap = ncap;
  }
  i = h->n++;
  while (i > 0) {
    int parent = (i - 1) / 2;
    if (h->a[parent].rank <= rank) break;
    h->a[i] = h->a[parent];
    i = parent;
  }
  h->a[i].rank = rank;
  h->a[i].li = li;
  h->a[i].ri = ri;
}

static int heap_pop(heap_t* h, heap_item* out) {
  heap_item top;
  int i = 0;

  if (h->n <= 0) return 0;
  top = h->a[0];
  h->n--;
  if (h->n > 0) {
    heap_item v = h->a[h->n];
    for (;;) {
      int c = i * 2 + 1;
      if (c >= h->n) break;
      if (c + 1 < h->n && h->a[c + 1].rank < h->a[c].rank) c++;
      if (h->a[c].rank > v.rank) break;
      h->a[i] = h->a[c];
      i = c;
    }
    h->a[i] = v;
  }
  *out = top;
  return 1;
}

static int32_t bpe_count(ca_tokenizer* t, const char* text) {
  size_t blen = strlen(text);
  int32_t* ids;
  int32_t* prev;
  int32_t* next;
  unsigned char* alive;
  int n, i;
  heap_t heap;
  int count;

  if (blen == 0) return 0;
  if (blen > 200000) return -1;   /* 单段异常长：交给估算，避免爆内存 */

  n = (int)blen;
  ids = (int32_t*)malloc((size_t)n * sizeof(int32_t));
  prev = (int32_t*)malloc((size_t)n * sizeof(int32_t));
  next = (int32_t*)malloc((size_t)n * sizeof(int32_t));
  alive = (unsigned char*)malloc((size_t)n);
  if (!ids || !prev || !next || !alive) {
    free(ids); free(prev); free(next); free(alive);
    return -1;
  }

  for (i = 0; i < n; ++i) {
    int b = (int)(unsigned char)text[i];
    int32_t id = vocab_find(t, (const char*)t->byte_repr[b], t->byte_repr_len[b]);
    if (id < 0) {
      free(ids); free(prev); free(next); free(alive);
      return -1;
    }
    ids[i] = id;
    prev[i] = i - 1;
    next[i] = (i + 1 < n) ? i + 1 : -1;
    alive[i] = 1;
  }

  memset(&heap, 0, sizeof(heap));
  for (i = 0; i + 1 < n; ++i) {
    int32_t rank, result;
    if (pair_find(t, ids[i], ids[i + 1], &rank, &result)) {
      heap_push(&heap, rank, i, i + 1);
    }
  }

  for (;;) {
    heap_item it;
    int32_t rank2, result2;
    int32_t p, q;

    if (!heap_pop(&heap, &it)) break;
    if (!alive[it.li] || !alive[it.ri] || next[it.li] != it.ri) continue;
    if (!pair_find(t, ids[it.li], ids[it.ri], &rank2, &result2)) continue;
    if (rank2 != it.rank) continue;
    if (result2 < 0) continue;

    ids[it.li] = result2;
    alive[it.ri] = 0;
    next[it.li] = next[it.ri];
    if (next[it.ri] >= 0) prev[next[it.ri]] = it.li;

    p = prev[it.li];
    q = next[it.li];
    if (p >= 0 && alive[p]) {
      int32_t r3, res3;
      if (pair_find(t, ids[p], ids[it.li], &r3, &res3)) heap_push(&heap, r3, p, it.li);
    }
    if (q >= 0 && alive[q]) {
      int32_t r3, res3;
      if (pair_find(t, ids[it.li], ids[q], &r3, &res3)) heap_push(&heap, r3, it.li, q);
    }
  }

  count = 0;
  for (i = 0; i < n; ++i) if (alive[i]) count++;

  free(ids);
  free(prev);
  free(next);
  free(alive);
  free(heap.a);
  return count;
}

int32_t ca_tokenizer_count(ca_tokenizer* t, const char* text) {
  if (!t || !text) return -1;
  return bpe_count(t, text);
}

/* 保守估算（仅在 tokenizer.json 缺失/解析失败时使用）：
 * 中日韩字符与标点按 1 token，连续 ASCII 字母/数字按 4 字符 1 token，
 * 最后整体乘 1.15 留余量（宁可压得狠，也不让 prompt 顶出 ctx）。 */
int32_t ca_token_estimate(const char* text) {
  size_t i = 0;
  double tokens = 0.0;
  int ascii_run = 0;

  if (!text) return 0;
  while (text[i]) {
    unsigned char c = (unsigned char)text[i];
    if (c >= 0x20 && c < 0x7F) {
      if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z')) {
        ascii_run++;
        i++;
        continue;
      }
      tokens += 1.0;
      i++;
    } else if (c < 0x80) {
      i++;
    } else {
      /* 一个 UTF-8 多字节字符 ≈ 1 token */
      if (c < 0xE0) i += 2;
      else if (c < 0xF0) i += 3;
      else i += 4;
      tokens += 1.0;
    }
    if (ascii_run > 0) {
      tokens += (double)(ascii_run + 3) / 4.0;
      ascii_run = 0;
    }
  }
  if (ascii_run > 0) tokens += (double)(ascii_run + 3) / 4.0;
  tokens = tokens * 1.15 + 1.0;
  return (int32_t)(tokens + 0.999);
}
