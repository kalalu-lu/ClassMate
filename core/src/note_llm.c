#include "note_internal.h"
#include "core_internal.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * M2.7 语义分章：递归二分 → 逐段校正/压缩 → 预算收敛 → 全局分章 → 硬校验。
 *
 * 约束（与 M2-操作清单 §10 一致）：
 *  - 切点只落在句子边界；在字数中点 ± 窗口内挑句间 gap 最大的边界；
 *    窗口内没有 gap 就退化为最近的句子边界硬切；每次切分都打日志。
 *  - 压缩稿只作为后续模型输入，不覆盖 segments；逐句校正若返回则另存 text_refined。
 *  - token 预算用真实 tokenizer（BPE）；没有 tokenizer 才退化为保守估算并标记。
 *  - 章节点只由模型给 seg_id，程序回查 segments.start_ms；查不到就拒绝该章，
 *    首章不合法/全部被拒则整体降级抽取式。模型永远不提供时间。
 *  - 调用次数有上限（免费额度护栏）；连续失败即停止，避免继续烧额度。
 */

/* M3.4 计时诊断：停录后出稿的 60–90 s 到底花在哪一段，必须能在 logcat 看到。
 * 只加日志，不改任何行为；非 Android 平台退回 printf。 */
#ifdef __ANDROID__
#include <android/log.h>
#include <time.h>
#define NL_TAG "ClassMate/Note"
#define NL_LOG(...) __android_log_print(ANDROID_LOG_INFO, NL_TAG, __VA_ARGS__)
static long nl_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
#else
#define NL_LOG(...) printf(__VA_ARGS__)
static long nl_ms(void) { return 0; }
#endif

#define NL_CTX_DEFAULT       2048
#define NL_ROUNDS_DEFAULT    2
#define NL_ROUNDS_CAP        3
#define NL_MAX_CH            8
#define NL_RESERVE_OUT       900
#define NL_MAX_CALLS         12
#define NL_TITLE_MAX_CP      18
#define NL_SUMMARY_LINE_CP   60
#define NL_DEPTH_MAX         12
#define NL_BATCH_CHUNKS      3
#define NL_BATCH_SEGS        60

typedef struct {
  int32_t ctx_len;
  int32_t max_chapters;
  int32_t reserve_out;
  int32_t compress_before;
  int32_t max_calls;
  int32_t rounds_max;
  char    model_id[128];
  char    prompt_ver[32];
  char    tokenizer_path[512];
  char    prompt_dir[512];
} nl_cfg;

typedef struct {
  char*  s;
  size_t len;
  size_t cap;
} sb;

typedef struct {
  int a;
  int b;
  int32_t tokens;
} nl_chunk;

typedef struct {
  nl_chunk* v;
  int n;
  int cap;
} nl_chunk_list;

typedef struct {
  char* text;
  int   rc;
  int   done;
} nl_result;

typedef struct {
  struct sqlite3*  db;
  int64_t          session;
  const note_seg*  segs;
  int              n;
  const ca_note_options* opt;
  const ca_llm_port* llm;
  void*            engine;
  ca_tokenizer*    tok;
  nl_cfg           cfg;
  int              estimated;
  int              llm_calls;
  int              fail_streak;
  int              dead;
  int              rounds;
  int              rejected;
  char             refine_tpl[4096];
  char             compress_tpl[4096];
  char             chapter_tpl[4096];
} nl_ctx;

/* ================= 迷你 JSON ================= */

static const char* j_ws(const char* p) {
  while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  return p;
}

static void utf8_put(char* out, int cap, int* n, uint32_t cp) {
  if (*n + 4 >= cap) return;
  if (cp < 0x80) {
    out[(*n)++] = (char)cp;
  } else if (cp < 0x800) {
    out[(*n)++] = (char)(0xC0 | (cp >> 6));
    out[(*n)++] = (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out[(*n)++] = (char)(0xE0 | (cp >> 12));
    out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[(*n)++] = (char)(0x80 | (cp & 0x3F));
  } else {
    out[(*n)++] = (char)(0xF0 | (cp >> 18));
    out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[(*n)++] = (char)(0x80 | (cp & 0x3F));
  }
}

static int j_hex4(const char* p, uint32_t* out) {
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

/* p 指向 '"'；解码到 out，返回结束后指针 */
static const char* json_string_dec(const char* p, char* out, int cap, int* out_len) {
  int n = 0;

  if (!p || *p != '"') return NULL;
  p++;
  while (*p && *p != '"') {
    if (*p == '\\') {
      p++;
      switch (*p) {
        case '"':  if (n < cap) out[n] = '"';  n++; p++; break;
        case '\\': if (n < cap) out[n] = '\\'; n++; p++; break;
        case '/':  if (n < cap) out[n] = '/';  n++; p++; break;
        case 'n':  if (n < cap) out[n] = '\n'; n++; p++; break;
        case 'r':  if (n < cap) out[n] = '\r'; n++; p++; break;
        case 't':  if (n < cap) out[n] = '\t'; n++; p++; break;
        case 'b':  if (n < cap) out[n] = '\b'; n++; p++; break;
        case 'f':  if (n < cap) out[n] = '\f'; n++; p++; break;
        case 'u': {
          uint32_t cp = 0, lo = 0;
          if (!j_hex4(p + 1, &cp)) return NULL;
          p += 5;
          if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u' &&
              j_hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
            p += 6;
          }
          {
            char tmp[8];
            int tn = 0;
            utf8_put(tmp, (int)sizeof(tmp), &tn, cp);
            if (n < cap) {
              int i;
              for (i = 0; i < tn; ++i) { if (n < cap) out[n] = tmp[i]; n++; }
            } else {
              n += tn;
            }
          }
          break;
        }
        default: return NULL;
      }
    } else {
      if (n < cap) out[n] = *p;
      n++;
      p++;
    }
  }
  if (*p != '"') return NULL;
  p++;
  if (out_len) *out_len = n;
  if (cap > 0) {
    int idx = n < cap ? n : cap - 1;
    if (idx > 0) out[idx] = '\0';
  }
  return p;
}

static const char* j_skip_string(const char* p) {
  if (!p || *p != '"') return NULL;
  p++;
  while (*p) {
    if (*p == '\\') { p += 2; continue; }
    if (*p == '"') return p + 1;
    p++;
  }
  return NULL;
}

static const char* j_skip_value(const char* p) {
  p = j_ws(p);
  if (!p) return NULL;
  if (*p == '"') return j_skip_string(p);
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (*p) {
      if (*p == '"') {
        p = j_skip_string(p);
        if (!p) return NULL;
        continue;
      }
      if (*p == '{' || *p == '[') depth++;
      else if (*p == '}' || *p == ']') {
        depth--;
        if (depth == 0) return p + 1;
      }
      p++;
    }
    return NULL;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']' &&
         *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
  return p;
}

static const char* j_obj_get(const char* obj, const char* key) {
  const char* p = j_ws(obj);
  size_t klen = strlen(key);

  if (!p || *p != '{') return NULL;
  p++;
  for (;;) {
    const char* ke;
    const char* ks;
    p = j_ws(p);
    if (*p == '}' || !*p) return NULL;
    if (*p != '"') return NULL;
    ks = p + 1;
    ke = j_skip_string(p);
    if (!ke) return NULL;
    p = j_ws(ke);
    if (*p != ':') return NULL;
    p = j_ws(p + 1);
    if ((size_t)(ke - 1 - ks) == klen && memcmp(ks, key, klen) == 0) return p;
    p = j_skip_value(p);
    if (!p) return NULL;
    p = j_ws(p);
    if (*p == ',') { p++; continue; }
    return NULL;
  }
}

static char* j_str_dup(const char* val) {
  char buf[8192];
  int n = 0;
  const char* after;

  if (!val || *val != '"') return NULL;
  after = json_string_dec(val, buf, (int)sizeof(buf), &n);
  if (!after || n < 0) return NULL;
  {
    char* out = (char*)malloc((size_t)n + 1);
    if (!out) return NULL;
    memcpy(out, buf, (size_t)n);
    out[n] = '\0';
    return out;
  }
}

static int j_int(const char* val, int32_t* out) {
  char tmp[32];
  int i = 0;
  const char* p = j_ws(val);

  if (!p) return 0;
  if (strncmp(p, "true", 4) == 0) { *out = 1; return 1; }
  if (strncmp(p, "false", 5) == 0) { *out = 0; return 1; }
  while (*p && i < (int)sizeof(tmp) - 1 &&
         ((*p >= '0' && *p <= '9') || *p == '-')) {
    tmp[i++] = *p++;
  }
  tmp[i] = '\0';
  if (i == 0) return 0;
  *out = (int32_t)atoi(tmp);
  return 1;
}

static const char* j_first_json(const char* text) {
  const char* p = text;
  if (!p) return NULL;
  while (*p) {
    if (*p == '[' || *p == '{') return p;
    p++;
  }
  return NULL;
}

/* ================= 字符串/文本工具 ================= */

static int sb_add(sb* b, const char* add) {
  size_t n;
  char* p;

  if (!add) return 0;
  n = strlen(add);
  if (b->len + n + 1 > b->cap) {
    size_t ncap = b->cap ? b->cap * 2 : 4096;
    while (ncap < b->len + n + 1) ncap *= 2;
    p = (char*)realloc(b->s, ncap);
    if (!p) return 0;
    b->s = p;
    b->cap = ncap;
  }
  memcpy(b->s + b->len, add, n);
  b->len += n;
  b->s[b->len] = '\0';
  return 1;
}

static int sb_addi(sb* b, const char* label, long long v) {
  char tmp[128];
  snprintf(tmp, sizeof(tmp), label, v);
  return sb_add(b, tmp);
}

static void sb_free(sb* b) {
  free(b->s);
  b->s = NULL;
  b->len = 0;
  b->cap = 0;
}

static char* dup_str(const char* s) {
  size_t n;
  char* p;
  if (!s) return NULL;
  n = strlen(s) + 1;
  p = (char*)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

static int cp_count(const char* s) {
  int n = 0;
  size_t i = 0;
  if (!s) return 0;
  while (s[i]) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) i += 1;
    else if (c < 0xE0) i += 2;
    else if (c < 0xF0) i += 3;
    else i += 4;
    n++;
  }
  return n;
}

/* 按码点截断字符串（避免从 UTF-8 中间切断） */
static void utf8_trim_cp(char* s, int max_cp) {
  size_t i = 0;
  int n = 0;
  while (s[i] && n < max_cp) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) i += 1;
    else if (c < 0xE0) i += 2;
    else if (c < 0xF0) i += 3;
    else i += 4;
    n++;
  }
  s[i] = '\0';
}

static void norm_trim(char* s) {
  static const char* punct = " \t\r\n。，、；：！？…．,;:!?\"'“”‘’（）()";
  size_t n;
  size_t i = 0;

  if (!s) return;
  while (s[i] && strchr(punct, s[i])) i++;
  if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
  n = strlen(s);
  while (n > 0 && strchr(punct, s[n - 1])) n--;
  s[n] = '\0';
}

/* ================= 配置 ================= */

static int allow_llm_from_options(const ca_note_options* opt) {
  return opt ? opt->allow_llm : 1;
}

static void cfg_defaults(nl_cfg* c) {
  memset(c, 0, sizeof(*c));
  c->ctx_len = NL_CTX_DEFAULT;
  c->max_chapters = NL_MAX_CH;
  c->reserve_out = NL_RESERVE_OUT;
  c->compress_before = 1;
  c->max_calls = NL_MAX_CALLS;
  c->rounds_max = NL_ROUNDS_DEFAULT;
  snprintf(c->model_id, sizeof(c->model_id), "%s", "qwen1.5b-q4");
  snprintf(c->prompt_ver, sizeof(c->prompt_ver), "%s", "m27-1");
}

static char* read_text_file(const char* path) {
  FILE* f;
  long size;
  char* data;

  if (!path || !path[0]) return NULL;
  f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  size = ftell(f);
  if (size <= 0 || size > 1024 * 1024L) { fclose(f); return NULL; }
  rewind(f);
  data = (char*)malloc((size_t)size + 1);
  if (!data) { fclose(f); return NULL; }
  if (fread(data, 1, (size_t)size, f) != (size_t)size) {
    free(data); fclose(f); return NULL;
  }
  data[size] = '\0';
  fclose(f);
  return data;
}

static void cfg_read_backend(nl_cfg* c, const char* cfg_json, const char* backend) {
  const char* backends;
  const char* b;
  const char* v;
  int32_t iv;

  if (!cfg_json) return;
  backends = j_obj_get(cfg_json, "backends");
  if (!backends) return;
  b = j_obj_get(backends, backend && backend[0] ? backend : "local");
  if (!b) return;

  if ((v = j_obj_get(b, "ctx_len")) && j_int(v, &iv) && iv > 0) c->ctx_len = iv;
  if ((v = j_obj_get(b, "max_chapters")) && j_int(v, &iv) && iv > 0) c->max_chapters = iv;
  if ((v = j_obj_get(b, "reserve_out")) && j_int(v, &iv) && iv > 0) c->reserve_out = iv;
  if ((v = j_obj_get(b, "compress_before_chapters")) && j_int(v, &iv)) c->compress_before = iv;
  if ((v = j_obj_get(b, "max_llm_calls")) && j_int(v, &iv) && iv > 0) c->max_calls = iv;
  if ((v = j_obj_get(b, "model_id")) && v && *v == '"') {
    char* s = j_str_dup(v);
    if (s) { snprintf(c->model_id, sizeof(c->model_id), "%s", s); free(s); }
  }
}

static void build_cfg(nl_ctx* c, const ca_note_options* opt) {
  char* cfg_json;
  const char* v;
  int32_t iv;

  cfg_defaults(&c->cfg);
  cfg_json = read_text_file(opt ? opt->config_path : NULL);
  if (cfg_json) {
    if ((v = j_obj_get(cfg_json, "compress_rounds_max")) && j_int(v, &iv) && iv > 0) {
      c->cfg.rounds_max = iv > NL_ROUNDS_CAP ? NL_ROUNDS_CAP : iv;
    }
    if ((v = j_obj_get(cfg_json, "prompt_ver")) && *v == '"') {
      char* s = j_str_dup(v);
      if (s) { snprintf(c->cfg.prompt_ver, sizeof(c->cfg.prompt_ver), "%s", s); free(s); }
    }
    if ((v = j_obj_get(cfg_json, "prompt_dir")) && *v == '"') {
      char* s = j_str_dup(v);
      if (s) { snprintf(c->cfg.prompt_dir, sizeof(c->cfg.prompt_dir), "%s", s); free(s); }
    }
    if ((v = j_obj_get(cfg_json, "tokenizer")) && *v == '"') {
      char* s = j_str_dup(v);
      if (s) { snprintf(c->cfg.tokenizer_path, sizeof(c->cfg.tokenizer_path), "%s", s); free(s); }
    }
    cfg_read_backend(&c->cfg, cfg_json, opt ? opt->backend : NULL);
    free(cfg_json);
  }

  if (opt) {
    if (opt->target_ctx > 0) c->cfg.ctx_len = opt->target_ctx;
    if (opt->compress_rounds_max > 0) {
      c->cfg.rounds_max = opt->compress_rounds_max > NL_ROUNDS_CAP
                         ? NL_ROUNDS_CAP : opt->compress_rounds_max;
    }
    if (opt->max_chapters > 0) c->cfg.max_chapters = opt->max_chapters;
    if (opt->model_id && opt->model_id[0]) {
      snprintf(c->cfg.model_id, sizeof(c->cfg.model_id), "%s", opt->model_id);
    }
    if (opt->prompt_ver && opt->prompt_ver[0]) {
      snprintf(c->cfg.prompt_ver, sizeof(c->cfg.prompt_ver), "%s", opt->prompt_ver);
    }
    if (opt->prompt_dir && opt->prompt_dir[0]) {
      snprintf(c->cfg.prompt_dir, sizeof(c->cfg.prompt_dir), "%s", opt->prompt_dir);
    }
  }
  if (c->llm && c->llm->ctx_len > 0) c->cfg.ctx_len = c->llm->ctx_len;
  if (c->llm && c->llm->model_id && c->llm->model_id[0]) {
    snprintf(c->cfg.model_id, sizeof(c->cfg.model_id), "%s", c->llm->model_id);
  }
  if (c->llm && c->llm->prompt_ver && c->llm->prompt_ver[0]) {
    snprintf(c->cfg.prompt_ver, sizeof(c->cfg.prompt_ver), "%s", c->llm->prompt_ver);
  }
  if (c->cfg.rounds_max > NL_ROUNDS_CAP) c->cfg.rounds_max = NL_ROUNDS_CAP;
  if (c->cfg.rounds_max < 1) c->cfg.rounds_max = 1;
  if (c->cfg.max_chapters < 2) c->cfg.max_chapters = 2;
}

/* ================= 提示词 ================= */

static const char* TPL_REFINE =
  "你是课堂录音整理助手。下面按段给出课堂转写（每行 [句id] 文本）。\n"
  "对每一段完成两件事：\n"
  "1) 修正明显的同音字/错别字，不改变原意，不补充原文没有的内容；\n"
  "2) 概括压缩为授课要点，保留定义、结论、数字、公式与例子。\n"
  "只输出 JSON 数组，每段一项，格式：\n"
  "[{\"seg_id_start\":12,\"seg_id_end\":20,\"text\":\"该段压缩稿\","
  "\"refined\":[{\"seg_id\":12,\"text\":\"校正后句子\"}]}]\n"
  "要求：\n"
  "- seg_id_start / seg_id_end 必须是该段第一句/最后一句的 id，不得编造；\n"
  "- text 控制在原文的 40% 以内，不要分章，不要输出时间戳；\n"
  "- refined 可为空数组；只输出 JSON，不要解释。\n\n输入：\n";

static const char* TPL_COMPRESS =
  "把下面的课堂要点稿再压缩一半，保留话题切换处与关键结论。\n"
  "只输出 JSON 数组，每段一项：\n"
  "[{\"seg_id_start\":12,\"seg_id_end\":20,\"text\":\"更短的稿\"}]\n"
  "seg_id 范围必须与输入一致，不得编造；不要输出时间戳；只输出 JSON。\n\n输入：\n";

static const char* TPL_CHAPTERS =
  "根据下面的课堂要点稿做全局语义分章。按话题/知识块划分，不要按固定时间间隔。\n"
  "只输出 JSON：\n"
  "{\"chapters\":[{\"seg_id\":12,\"title\":\"不超过18字的概括短语\","
  "\"summary\":\"第一行提要\\n第二行提要\"}]}\n"
  "要求：\n"
  "- 第一个章节的 seg_id 必须是输入第一段的起始句 id；\n"
  "- 各章 seg_id 递增且必须来自输入；\n"
  "- title 是概括短语，不得照抄任何一句原文，不得出现“首先/接下来/然后”等过渡语；\n"
  "- summary 恰好两行，每行不超过 60 字；\n"
  "- 不要输出任何时间戳，时间由程序回查 segments。\n\n输入：\n";

static void load_templates(nl_ctx* c) {
  const char* dir = c->cfg.prompt_dir;
  char path[600];
  char* text;

  snprintf(c->refine_tpl, sizeof(c->refine_tpl), "%s", TPL_REFINE);
  snprintf(c->compress_tpl, sizeof(c->compress_tpl), "%s", TPL_COMPRESS);
  snprintf(c->chapter_tpl, sizeof(c->chapter_tpl), "%s", TPL_CHAPTERS);
  if (!dir[0]) return;

  snprintf(path, sizeof(path), "%s/refine_compress.txt", dir);
  text = read_text_file(path);
  if (text) { snprintf(c->refine_tpl, sizeof(c->refine_tpl), "%s", text); free(text); }
  snprintf(path, sizeof(path), "%s/compress_more.txt", dir);
  text = read_text_file(path);
  if (text) { snprintf(c->compress_tpl, sizeof(c->compress_tpl), "%s", text); free(text); }
  snprintf(path, sizeof(path), "%s/chapters.txt", dir);
  text = read_text_file(path);
  if (text) { snprintf(c->chapter_tpl, sizeof(c->chapter_tpl), "%s", text); free(text); }
}

/* ================= token 预算 ================= */

static int32_t nl_tokens(nl_ctx* c, const char* text) {
  int32_t v = -1;

  if (c->llm && c->llm->count_tokens) {
    v = c->llm->count_tokens(c->engine, text);
  }
  if (v < 0 && c->tok) v = ca_tokenizer_count(c->tok, text);
  if (v < 0) {
    v = ca_token_estimate(text);
    c->estimated = 1;
  }
  return v;
}

static char* seg_lines(nl_ctx* c, int a, int b) {
  sb out;
  int i;

  memset(&out, 0, sizeof(out));
  for (i = a; i < b; ++i) {
    sb_add(&out, "[");
    sb_addi(&out, "%lld", (long long)c->segs[i].id);
    sb_add(&out, "] ");
    sb_add(&out, c->segs[i].text ? c->segs[i].text : "");
    sb_add(&out, "\n");
  }
  return out.s ? out.s : dup_str("");
}

/* ================= LLM 调用 ================= */

static void nl_on_done(int rc, const char* full, int32_t tokens, void* user) {
  nl_result* r = (nl_result*)user;
  (void)tokens;
  r->rc = rc;
  r->done = 1;
  if (rc == CA_OK && full) r->text = dup_str(full);
}

static int nl_call(nl_ctx* c, const char* prompt, char** out_text) {
  nl_result* r;
  int rc;

  *out_text = NULL;
  if (c->dead) return CA_ERR_STATE;
  if (!c->llm || !c->llm->generate) { c->dead = 1; return CA_ERR_STATE; }
  if (c->llm_calls >= c->cfg.max_calls) {
    c->dead = 1;
    printf("[M2.7][llm] 调用次数达到上限 %d，停止调用并降级\n", c->cfg.max_calls);
    return CA_ERR_STATE;
  }

  c->llm_calls++;
  r = (nl_result*)calloc(1, sizeof(nl_result));
  if (!r) return CA_ERR_IO;
  long nl_call_t0 = nl_ms();
  NL_LOG("llm call #%d start prompt_chars=%d", c->llm_calls,
         prompt ? (int)strlen(prompt) : 0);
  rc = c->llm->generate(c->engine, prompt, NULL, nl_on_done, r);
  NL_LOG("llm call #%d end rc=%d %ldms out_chars=%d", c->llm_calls, rc,
         nl_ms() - nl_call_t0, r->text ? (int)strlen(r->text) : 0);
  if (rc != CA_OK || !r->done || !r->text) {
    c->fail_streak++;
    if (c->fail_streak >= 2) c->dead = 1;
    printf("[M2.7][llm] 调用失败 rc=%d streak=%d\n", rc, c->fail_streak);
    free(r->text);
    free(r);
    return CA_ERR_STATE;
  }
  c->fail_streak = 0;
  *out_text = r->text;
  free(r);
  return CA_OK;
}

/* ================= 数组遍历 ================= */

typedef int (*nl_item_cb)(nl_ctx* c, const char* obj, void* user);

static int walk_array_objects(nl_ctx* c, const char* json, nl_item_cb cb,
                              void* user) {
  const char* p = j_first_json(json);
  int count = 0;

  if (!p) return -1;
  if (*p == '{') {
    const char* arr = NULL;
    const char* q = p + 1;
    for (;;) {
      q = j_ws(q);
      if (*q == '}' || !*q || *q != '"') break;
      {
        const char* ke = j_skip_string(q);
        if (!ke) break;
        q = j_ws(ke);
        if (*q != ':') break;
        q = j_ws(q + 1);
        if (*q == '[') { arr = q; break; }
        q = j_skip_value(q);
        if (!q) break;
        q = j_ws(q);
        if (*q == ',') { q++; continue; }
        break;
      }
    }
    if (!arr) return -1;
    p = arr;
  }
  if (*p != '[') return -1;
  p++;
  for (;;) {
    const char* end;
    p = j_ws(p);
    if (*p == ']' || !*p) break;
    end = j_skip_value(p);
    if (!end) return -1;
    if (cb(c, p, user) != 0) return -1;
    count++;
    p = j_ws(end);
    if (*p == ',') { p++; continue; }
    if (*p == ']') break;
    return -1;
  }
  return count;
}

/* ================= 递归二分 ================= */

static int chunk_push(nl_chunk_list* l, int a, int b, int32_t tokens) {
  if (l->n >= l->cap) {
    int ncap = l->cap ? l->cap * 2 : 16;
    nl_chunk* p = (nl_chunk*)realloc(l->v, (size_t)ncap * sizeof(nl_chunk));
    if (!p) return 0;
    l->v = p;
    l->cap = ncap;
  }
  l->v[l->n].a = a;
  l->v[l->n].b = b;
  l->v[l->n].tokens = tokens;
  l->n++;
  return 1;
}

static void recursive_split(nl_ctx* c, const int32_t* tok_prefix,
                            const int64_t* char_prefix,
                            int a, int b, int depth, int32_t budget,
                            nl_chunk_list* out) {
  int32_t tokens = tok_prefix[b] - tok_prefix[a];
  int64_t chars = char_prefix[b] - char_prefix[a];
  int64_t mid_char;
  int64_t window;
  int cut = -1;
  int64_t best_gap = -1;
  int i;

  if (tokens <= budget) {
    chunk_push(out, a, b, tokens);
    return;
  }
  if (depth >= NL_DEPTH_MAX || b - a <= 1) {
    printf("[M2.7][split] depth=%d seg=[%d,%d) tokens=%d 超过预算 %d，"
           "已到句边界无法再分，整段送入\n",
           depth, a, b, tokens, budget);
    chunk_push(out, a, b, tokens);
    return;
  }

  mid_char = (char_prefix[a] + char_prefix[b]) / 2;
  window = chars / 8;
  if (window < 40) window = 40;
  if (window > chars / 2) window = chars / 2;

  for (i = a + 1; i < b; ++i) {
    int64_t delta = char_prefix[i] - mid_char;
    if (delta < 0) delta = -delta;
    if (delta > window) continue;
    {
      int64_t gap = c->segs[i].start_ms - c->segs[i - 1].end_ms;
      if (gap > best_gap) {
        best_gap = gap;
        cut = i;
      }
    }
  }

  if (cut <= a || cut >= b) {
    int64_t best_delta = -1;
    for (i = a + 1; i < b; ++i) {
      int64_t delta = char_prefix[i] - mid_char;
      if (delta < 0) delta = -delta;
      if (best_delta < 0 || delta < best_delta) {
        best_delta = delta;
        cut = i;
      }
    }
    if (cut <= a || cut >= b) {
      chunk_push(out, a, b, tokens);
      return;
    }
    best_gap = c->segs[cut].start_ms - c->segs[cut - 1].end_ms;
    printf("[M2.7][split] depth=%d seg=[%d,%d) tokens=%d chars=%lld mid=%lld "
           "cut=%d(%lld) gap=%lldms 窗口内无停顿，退化句边界硬切\n",
           depth, a, b, tokens, (long long)chars, (long long)mid_char,
           cut, (long long)c->segs[cut].id, (long long)best_gap);
  } else {
    printf("[M2.7][split] depth=%d seg=[%d,%d) tokens=%d chars=%lld mid=%lld "
           "cut=%d(%lld) gap=%lldms window=%lld\n",
           depth, a, b, tokens, (long long)chars, (long long)mid_char,
           cut, (long long)c->segs[cut].id, (long long)best_gap,
           (long long)window);
  }

  recursive_split(c, tok_prefix, char_prefix, a, cut, depth + 1, budget, out);
  recursive_split(c, tok_prefix, char_prefix, cut, b, depth + 1, budget, out);
}

/* ================= 逐段压缩/校正 ================= */

typedef struct {
  nl_ctx*  c;
  nl_chunk* chunks;
  int       chunk_count;
  char**    comp;
  const char* model_id;
  const char* prompt_ver;
} map_user;

static int seg_in_range(nl_ctx* c, int a, int b, int32_t seg_id) {
  int i;
  for (i = a; i < b; ++i) if (c->segs[i].id == seg_id) return 1;
  return 0;
}

static void save_refined(nl_ctx* c, nl_chunk* ch, const char* refined_ptr,
                         const char* model_id, const char* prompt_ver) {
  const char* p = j_ws(refined_ptr);
  sqlite3_stmt* st = NULL;

  if (!p || *p != '[') return;
  if (sqlite3_prepare_v2(c->db,
      "INSERT OR REPLACE INTO text_refined(session_id, seg_id, text,"
      " model_id, prompt_ver) VALUES(?, ?, ?, ?, ?)", -1, &st, NULL)
      != SQLITE_OK) {
    return;
  }
  p++;
  for (;;) {
    const char* end;
    const char* v;
    int32_t sid = -1;
    char* text;

    p = j_ws(p);
    if (*p == ']' || !*p) break;
    end = j_skip_value(p);
    if (!end) break;
    if (ch) {
      if ((v = j_obj_get(p, "seg_id")) && j_int(v, &sid)) {
        if (sid >= 0 && seg_in_range(c, ch->a, ch->b, sid)) {
          if ((v = j_obj_get(p, "text")) && *v == '"') {
            text = j_str_dup(v);
            if (text) {
              sqlite3_bind_int64(st, 1, (sqlite3_int64)c->session);
              sqlite3_bind_int64(st, 2, (sqlite3_int64)sid);
              sqlite3_bind_text(st, 3, text, -1, SQLITE_TRANSIENT);
              sqlite3_bind_text(st, 4, model_id ? model_id : "", -1, SQLITE_TRANSIENT);
              sqlite3_bind_text(st, 5, prompt_ver ? prompt_ver : "", -1, SQLITE_TRANSIENT);
              sqlite3_step(st);
              sqlite3_reset(st);
              free(text);
            }
          }
        }
      }
    }
    p = j_ws(end);
    if (*p == ',') { p++; continue; }
    break;
  }
  sqlite3_finalize(st);
}

static int on_map_item(nl_ctx* c, const char* obj, void* user) {
  map_user* m = (map_user*)user;
  const char* v;
  int32_t sid_start = -1, sid_end = -1;
  char* text = NULL;
  int i;

  if ((v = j_obj_get(obj, "seg_id_start")) && j_int(v, &sid_start)) {
    if ((v = j_obj_get(obj, "seg_id_end")) && j_int(v, &sid_end)) {
      if ((v = j_obj_get(obj, "text")) && *v == '"') text = j_str_dup(v);
    }
  }

  for (i = 0; i < m->chunk_count; ++i) {
    nl_chunk* ch = &m->chunks[i];
    if (m->comp[i]) continue;
    if (sid_start == c->segs[ch->a].id && sid_end == c->segs[ch->b - 1].id) {
      if (text && text[0]) m->comp[i] = text;
      else m->comp[i] = seg_lines(c, ch->a, ch->b);
      if ((v = j_obj_get(obj, "refined")) && *v == '[') {
        save_refined(c, ch, v, m->model_id, m->prompt_ver);
      }
      return 0;
    }
  }
  free(text);
  return 0;
}

static char* build_chunks_prompt(nl_ctx* c, nl_chunk_list* batch,
                                 const char* tpl) {
  sb out;
  int i;

  memset(&out, 0, sizeof(out));
  sb_add(&out, tpl);
  for (i = 0; i < batch->n; ++i) {
    nl_chunk* ch = &batch->v[i];
    sb_add(&out, "第");
    sb_addi(&out, "%lld", (long long)(i + 1));
    sb_add(&out, "段（句 ");
    sb_addi(&out, "%lld", (long long)c->segs[ch->a].id);
    sb_add(&out, " 至 ");
    sb_addi(&out, "%lld", (long long)c->segs[ch->b - 1].id);
    sb_add(&out, "）：\n");
    {
      char* body = seg_lines(c, ch->a, ch->b);
      if (body) { sb_add(&out, body); free(body); }
    }
    sb_add(&out, "\n");
  }
  return out.s ? out.s : dup_str("");
}

static int refine_chunks(nl_ctx* c, nl_chunk_list* chunks,
                         char** comp,
                         const char* model_id, const char* prompt_ver) {
  int i = 0;
  int gi;

  while (i < chunks->n) {
    nl_chunk_list batch;
    int32_t batch_tokens = 0;
    int batch_segs = 0;
    char* prompt;
    char* response = NULL;
    map_user m;
    int j;

    memset(&batch, 0, sizeof(batch));
    for (j = i; j < chunks->n; ++j) {
      nl_chunk* ch = &chunks->v[j];
      int32_t add = ch->tokens + 8;
      if (batch.n > 0 &&
          (batch.n >= NL_BATCH_CHUNKS ||
           batch_segs + (ch->b - ch->a) > NL_BATCH_SEGS ||
           batch_tokens + add > c->cfg.ctx_len / 2)) {
        break;
      }
      chunk_push(&batch, ch->a, ch->b, ch->tokens);
      batch_tokens += add;
      batch_segs += ch->b - ch->a;
    }

    prompt = build_chunks_prompt(c, &batch, c->refine_tpl);
    if (!prompt) { free(batch.v); return CA_ERR_IO; }
    printf("[M2.7][refine] batch segs=%d tokens~%d prompt_tokens=%d\n",
           batch_segs, batch_tokens, nl_tokens(c, prompt));

    if (nl_call(c, prompt, &response) == CA_OK) {
      memset(&m, 0, sizeof(m));
      m.c = c;
      m.chunks = chunks->v + i;
      m.chunk_count = batch.n;
      m.comp = comp + i;
      m.model_id = model_id;
      m.prompt_ver = prompt_ver;
      walk_array_objects(c, response, on_map_item, &m);
      free(response);
    }

    for (j = 0; j < batch.n; ++j) {
      if (!comp[i + j]) comp[i + j] = seg_lines(c, batch.v[j].a, batch.v[j].b);
    }

    gi = i + batch.n;
    i = (gi > i) ? gi : i + 1;
    free(batch.v);
  }
  return CA_OK;
}

static char* join_comp(nl_ctx* c, nl_chunk_list* chunks, char** comp) {
  sb out;
  int i;

  memset(&out, 0, sizeof(out));
  for (i = 0; i < chunks->n; ++i) {
    nl_chunk* ch = &chunks->v[i];
    sb_add(&out, "[seg ");
    sb_addi(&out, "%lld", (long long)c->segs[ch->a].id);
    sb_add(&out, "-");
    sb_addi(&out, "%lld", (long long)c->segs[ch->b - 1].id);
    sb_add(&out, "] ");
    sb_add(&out, comp[i] ? comp[i] : "");
    sb_add(&out, "\n");
  }
  return out.s ? out.s : dup_str("");
}

/* 把合并稿再压一轮；成功则替换 *text */
static int compress_more(nl_ctx* c, char** text, int32_t budget) {
  char* prompt;
  char* response = NULL;
  sb out;
  const char* p;
  char target[64];

  snprintf(target, sizeof(target), "%d", (int)(budget * 3 / 5));
  prompt = (char*)malloc(strlen(c->compress_tpl) + strlen(*text) + 160);
  if (!prompt) return CA_ERR_IO;
  snprintf(prompt, strlen(c->compress_tpl) + strlen(*text) + 160,
           "%s（目标不超过约 %s 字）\n%s", c->compress_tpl, target, *text);

  if (nl_call(c, prompt, &response) != CA_OK) { free(prompt); return CA_ERR_STATE; }
  free(prompt);

  memset(&out, 0, sizeof(out));
  p = j_first_json(response);
  if (!p) { free(response); return CA_ERR_STATE; }
  if (*p == '{') {
    const char* arr = NULL;
    const char* q = p + 1;
    for (;;) {
      q = j_ws(q);
      if (*q == '}' || !*q || *q != '"') break;
      {
        const char* ke = j_skip_string(q);
        if (!ke) break;
        q = j_ws(ke);
        if (*q != ':') break;
        q = j_ws(q + 1);
        if (*q == '[') { arr = q; break; }
        q = j_skip_value(q);
        if (!q) break;
        q = j_ws(q);
        if (*q == ',') { q++; continue; }
        break;
      }
    }
    if (!arr) { free(response); return CA_ERR_STATE; }
    p = arr;
  }
  if (*p != '[') { free(response); return CA_ERR_STATE; }

  {
    const char* q = p + 1;
    for (;;) {
      const char* end;
      const char* v;
      int32_t a = -1, b = -1;
      char* t = NULL;

      q = j_ws(q);
      if (*q == ']' || !*q) break;
      end = j_skip_value(q);
      if (!end) { free(response); sb_free(&out); return CA_ERR_STATE; }
      if ((v = j_obj_get(q, "seg_id_start")) && j_int(v, &a)) {
        if ((v = j_obj_get(q, "seg_id_end")) && j_int(v, &b)) {
          if ((v = j_obj_get(q, "text")) && *v == '"') t = j_str_dup(v);
        }
      }
      if (a >= 0 && t) {
        sb_add(&out, "[seg ");
        sb_addi(&out, "%lld", (long long)a);
        sb_add(&out, "-");
        sb_addi(&out, "%lld", (long long)b);
        sb_add(&out, "] ");
        sb_add(&out, t);
        sb_add(&out, "\n");
      }
      free(t);
      q = j_ws(end);
      if (*q == ',') { q++; continue; }
      break;
    }
  }
  free(response);
  if (!out.s) { sb_free(&out); return CA_ERR_STATE; }

  free(*text);
  *text = out.s;
  return CA_OK;
}

/* ================= 章节校验 ================= */

typedef struct {
  nl_ctx* c;
  note_chapter* arr;
  int n;
  int cap;
  int64_t last_seg;
} chap_user;

static int title_is_original(nl_ctx* c, const char* title) {
  char* norm = dup_str(title);
  int i;
  int hit = 0;

  if (!norm) return 0;
  norm_trim(norm);
  for (i = 0; i < c->n; ++i) {
    char* t = dup_str(c->segs[i].text);
    if (!t) continue;
    norm_trim(t);
    if (strcmp(norm, t) == 0) { hit = 1; free(t); break; }
    free(t);
  }
  free(norm);
  return hit;
}

static int title_ok(nl_ctx* c, const char* title) {
  int cp;

  if (!title || !title[0]) return 0;
  cp = cp_count(title);
  if (cp < 2 || cp > NL_TITLE_MAX_CP) return 0;
  if (strchr(title, '[') || strchr(title, ']')) return 0;
  if (title_is_original(c, title)) return 0;
  return 1;
}

static void chap_push(chap_user* u, note_chapter* ch) {
  if (u->n >= u->cap) {
    int ncap = u->cap ? u->cap * 2 : 8;
    note_chapter* p = (note_chapter*)realloc(u->arr,
        (size_t)ncap * sizeof(note_chapter));
    if (!p) {
      free(ch->title); free(ch->summary); free(ch->source);
      free(ch->model_id); free(ch->prompt_ver);
      return;
    }
    u->arr = p;
    u->cap = ncap;
  }
  u->arr[u->n++] = *ch;
}

static int find_seg_index(nl_ctx* c, int64_t seg_id) {
  int i;
  for (i = 0; i < c->n; ++i) if (c->segs[i].id == seg_id) return i;
  return -1;
}

static int on_chapter_item(nl_ctx* c, const char* obj, void* user) {
  chap_user* u = (chap_user*)user;
  const char* v;
  int32_t sid = -1;
  char* title = NULL;
  char* summary = NULL;
  int idx;
  note_chapter ch;

  if ((v = j_obj_get(obj, "seg_id")) && j_int(v, &sid)) {
    if ((v = j_obj_get(obj, "title")) && *v == '"') title = j_str_dup(v);
    if ((v = j_obj_get(obj, "summary")) && *v == '"') summary = j_str_dup(v);
  }
  if (sid < 0 || !title) { free(title); free(summary); return 0; }

  idx = find_seg_index(c, (int64_t)sid);
  if (idx < 0) {
    printf("[M2.7][chapter] 拒绝：seg_id=%d 回查 segments 失败\n", sid);
    c->rejected++;
    free(title); free(summary);
    return 0;
  }
  if ((int64_t)sid <= u->last_seg) {
    printf("[M2.7][chapter] 拒绝：seg_id=%d 非递增\n", sid);
    c->rejected++;
    free(title); free(summary);
    return 0;
  }
  if (!title_ok(c, title)) {
    printf("[M2.7][chapter] 拒绝：标题非法/等于原文：%s\n", title);
    c->rejected++;
    free(title); free(summary);
    return 0;
  }

  if (!summary) summary = dup_str("");
  {
    char* nl1 = strchr(summary, '\n');
    if (nl1) {
      char* nl2 = strchr(nl1 + 1, '\n');
      if (nl2) nl2[0] = '\0';
    }
    utf8_trim_cp(summary, NL_SUMMARY_LINE_CP * 2);
  }

  memset(&ch, 0, sizeof(ch));
  ch.start_ms = c->segs[idx].start_ms;
  ch.seg_id = (int64_t)sid;
  ch.title = title;
  ch.summary = summary;
  ch.source = dup_str("llm");
  ch.model_id = dup_str(c->cfg.model_id);
  ch.prompt_ver = dup_str(c->cfg.prompt_ver);
  chap_push(u, &ch);
  u->last_seg = (int64_t)sid;
  return 0;
}

/* ================= 对外入口 ================= */

void ca_note_free_chapters(note_chapter* arr, int n) {
  int i;
  if (!arr) return;
  for (i = 0; i < n; ++i) {
    free(arr[i].title);
    free(arr[i].summary);
    free(arr[i].source);
    free(arr[i].model_id);
    free(arr[i].prompt_ver);
  }
  free(arr);
}

int ca_note_llm_chapters(struct sqlite3* db, int64_t session_id,
                         const note_seg* segs, int n,
                         const ca_note_options* opt,
                         const ca_llm_port* llm, void* llm_engine,
                         note_chapter** out, int* out_n,
                         ca_note_meta* meta) {
  nl_ctx c;
  int32_t* seg_tok = NULL;
  int32_t* tok_prefix = NULL;
  int64_t* char_prefix = NULL;
  nl_chunk_list chunks;
  char** comp = NULL;
  char* merged = NULL;
  int32_t merged_tokens = 0;
  int32_t chapter_budget;
  int i;

  memset(&c, 0, sizeof(c));
  memset(&chunks, 0, sizeof(chunks));
  c.db = db;
  c.session = session_id;
  c.segs = segs;
  c.n = n;
  c.opt = opt;
  c.llm = llm;
  c.engine = llm_engine;

  *out = NULL;
  *out_n = 0;
  if (meta) {
    memset(meta, 0, sizeof(*meta));
    meta->source = 1;
    snprintf(meta->reason, sizeof(meta->reason), "%s", "未启用 LLM（抽取式）");
  }
  if (!allow_llm_from_options(opt) || !llm || !llm->generate) return CA_ERR_STATE;
  if (n <= 0) return CA_ERR_STATE;

  build_cfg(&c, opt);
  long nl_t0 = nl_ms();
  printf("[M2.7][cfg] backend=%s ctx=%d rounds=%d max_chapters=%d "
         "reserve_out=%d max_calls=%d model=%s prompt_ver=%s\n",
         opt && opt->backend ? opt->backend : "local", c.cfg.ctx_len,
         c.cfg.rounds_max, c.cfg.max_chapters, c.cfg.reserve_out,
         c.cfg.max_calls, c.cfg.model_id, c.cfg.prompt_ver);

  if (opt && opt->tokenizer_path && opt->tokenizer_path[0]) {
    c.tok = ca_tokenizer_load(opt->tokenizer_path);
  }
  if (!c.tok && c.cfg.tokenizer_path[0]) c.tok = ca_tokenizer_load(c.cfg.tokenizer_path);
  if (!c.tok) {
    printf("[M2.7][tokenizer] tokenizer.json 缺失/解析失败，改用保守估算（已标记）\n");
  }
  load_templates(&c);
  NL_LOG("t=+%ldms tokenizer=%s segs=%d", nl_ms() - nl_t0,
         c.tok ? "ok" : "null", n);

  seg_tok = (int32_t*)calloc((size_t)n, sizeof(int32_t));
  tok_prefix = (int32_t*)calloc((size_t)n + 1, sizeof(int32_t));
  char_prefix = (int64_t*)calloc((size_t)n + 1, sizeof(int64_t));
  if (!seg_tok || !tok_prefix || !char_prefix) goto fail_io;

  for (i = 0; i < n; ++i) {
    seg_tok[i] = nl_tokens(&c, segs[i].text ? segs[i].text : "") + 3;
    tok_prefix[i + 1] = tok_prefix[i] + seg_tok[i];
    char_prefix[i + 1] = char_prefix[i] + cp_count(segs[i].text);
  }
  if (meta) meta->estimated = c.estimated;
  NL_LOG("t=+%ldms counted total_tokens=%d", nl_ms() - nl_t0, tok_prefix[n]);

  chapter_budget = c.cfg.ctx_len - c.cfg.reserve_out -
                   nl_tokens(&c, c.chapter_tpl) - 32;
  if (chapter_budget < 256) chapter_budget = 256;

  if (!c.cfg.compress_before || tok_prefix[n] <= chapter_budget) {
    printf("[M2.7][skip] total_tokens=%d <= chapter_budget=%d，跳过分段压缩\n",
           tok_prefix[n], chapter_budget);
    merged = seg_lines(&c, 0, n);
    if (!merged) goto fail_io;
  } else {
    int32_t chunk_budget = c.cfg.ctx_len - c.cfg.reserve_out -
                           nl_tokens(&c, c.refine_tpl) - 32;
    if (chunk_budget < 200) chunk_budget = 200;
    printf("[M2.7][split] total_tokens=%d chunk_budget=%d 开始递归二分\n",
           tok_prefix[n], chunk_budget);
    recursive_split(&c, tok_prefix, char_prefix, 0, n, 0, chunk_budget, &chunks);
    printf("[M2.7][split] 得到 %d 段\n", chunks.n);

    comp = (char**)calloc((size_t)chunks.n, sizeof(char*));
    if (!comp) goto fail_io;
    refine_chunks(&c, &chunks, comp, c.cfg.model_id, c.cfg.prompt_ver);
    merged = join_comp(&c, &chunks, comp);
    if (!merged) goto fail_io;
    merged_tokens = nl_tokens(&c, merged);
    printf("[M2.7][budget] merged_tokens=%d chapter_budget=%d\n",
           merged_tokens, chapter_budget);

    while (merged_tokens > chapter_budget && c.rounds < c.cfg.rounds_max) {
      c.rounds++;
      printf("[M2.7][compress] round=%d in_tokens=%d\n", c.rounds, merged_tokens);
      if (compress_more(&c, &merged, chapter_budget) != CA_OK) {
        printf("[M2.7][compress] round=%d 失败\n", c.rounds);
        break;
      }
      merged_tokens = nl_tokens(&c, merged);
      printf("[M2.7][compress] round=%d out_tokens=%d\n", c.rounds, merged_tokens);
    }
    if (merged_tokens > chapter_budget) {
      if (meta) {
        meta->llm_calls = c.llm_calls;
        meta->compress_rounds = c.rounds;
        meta->estimated = c.estimated;
        snprintf(meta->reason, sizeof(meta->reason),
                 "压缩 %d 轮后仍超 ctx（%d > %d），已降级抽取式",
                 c.rounds, merged_tokens, chapter_budget);
      }
      goto fail_state;
    }
  }

  {
    char* prompt;
    char* response = NULL;
    chap_user cu;
    int items;

    prompt = (char*)malloc(strlen(c.chapter_tpl) + strlen(merged) + 64);
    if (!prompt) goto fail_io;
    sprintf(prompt, "%s%s", c.chapter_tpl, merged);
    printf("[M2.7][chapter] prompt_tokens=%d\n", nl_tokens(&c, prompt));

    if (nl_call(&c, prompt, &response) != CA_OK) {
      free(prompt);
      if (meta) {
        meta->llm_calls = c.llm_calls;
        meta->compress_rounds = c.rounds;
        meta->estimated = c.estimated;
        snprintf(meta->reason, sizeof(meta->reason),
                 "云端/本地调用失败（超时/429/余额不足/离线），已降级抽取式");
      }
      goto fail_state;
    }
    free(prompt);

    memset(&cu, 0, sizeof(cu));
    cu.c = &c;
    cu.last_seg = -1;
    items = walk_array_objects(&c, response, on_chapter_item, &cu);
    free(response);

    if (items <= 0 || cu.n <= 0) {
      ca_note_free_chapters(cu.arr, cu.n);
      if (meta) {
        meta->llm_calls = c.llm_calls;
        meta->compress_rounds = c.rounds;
        meta->estimated = c.estimated;
        snprintf(meta->reason, sizeof(meta->reason),
                 "章节 JSON 解析失败，已降级抽取式");
      }
      goto fail_state;
    }
    if (cu.arr[0].seg_id != segs[0].id) {
      printf("[M2.7][chapter] 首章 seg_id=%lld 不等于首句 %lld，整体降级\n",
             (long long)cu.arr[0].seg_id, (long long)segs[0].id);
      ca_note_free_chapters(cu.arr, cu.n);
      if (meta) {
        meta->llm_calls = c.llm_calls;
        meta->compress_rounds = c.rounds;
        meta->estimated = c.estimated;
        snprintf(meta->reason, sizeof(meta->reason),
                 "首章未对齐首句 start_ms，已降级抽取式");
      }
      goto fail_state;
    }
    if (cu.n > c.cfg.max_chapters) {
      int k;
      for (k = c.cfg.max_chapters; k < cu.n; ++k) {
        free(cu.arr[k].title);
        free(cu.arr[k].summary);
        free(cu.arr[k].source);
        free(cu.arr[k].model_id);
        free(cu.arr[k].prompt_ver);
      }
      cu.n = c.cfg.max_chapters;
    }

    *out = cu.arr;
    *out_n = cu.n;
    if (meta) {
      meta->source = 0;
      meta->llm_calls = c.llm_calls;
      meta->compress_rounds = c.rounds;
      meta->rejected_chapters = c.rejected;
      meta->estimated = c.estimated;
      snprintf(meta->reason, sizeof(meta->reason),
               "LLM 语义分章：%d 章，%d 次调用，压缩 %d 轮，拒绝 %d 章",
               cu.n, c.llm_calls, c.rounds, c.rejected);
    }
    printf("[M2.7][done] chapters=%d calls=%d rounds=%d rejected=%d estimated=%d\n",
           cu.n, c.llm_calls, c.rounds, c.rejected, c.estimated);
  }

  for (i = 0; i < chunks.n; ++i) free(comp ? comp[i] : NULL);
  free(comp);
  free(merged);
  free(chunks.v);
  free(seg_tok);
  free(tok_prefix);
  free(char_prefix);
  ca_tokenizer_free(c.tok);
  return CA_OK;

fail_io:
  if (meta) {
    meta->llm_calls = c.llm_calls;
    meta->compress_rounds = c.rounds;
    meta->estimated = c.estimated;
    snprintf(meta->reason, sizeof(meta->reason), "内存/IO 失败，已降级抽取式");
  }
fail_state:
  for (i = 0; i < chunks.n; ++i) free(comp ? comp[i] : NULL);
  free(comp);
  free(merged);
  free(chunks.v);
  free(seg_tok);
  free(tok_prefix);
  free(char_prefix);
  ca_tokenizer_free(c.tok);
  return CA_ERR_STATE;
}
