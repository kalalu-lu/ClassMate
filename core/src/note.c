#include "core_internal.h"
#include "note_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * M4 课后整理：只依赖 segments 文本（text_corrected 优先），不读音频。
 *
 *  - chapters：语义切块（最小块 block_ms，默认 45 s；章节间隔 ≥2 min），
 *    优先切在句间长停顿 / 话题信号词 / 句末标点处，标题取块内完整首句，
 *    过滤过短短语与悬空词
 *  - 语义/固定是**整场级**开关：只要有一句 text_corrected 就走语义切块；
 *    整场都没有（模型未下载 / 关闭课后校正）则退回 45 s 纯固定分块——
 *    此时完全不看停顿/话题词，与升级前行为一致
 *  - 标题取块内第一句完整话（有句末标点、非语义未完），≤24 字在标点处收口；
 *    全块找不到合格句退化「本段要点」——不把半句原样当标题，不出 40 字长标题
 *  - 笔记 Markdown：章节标题 + 逐句 [mm:ss] 锚点，UI 可点锚点跳到对应字幕行
 *
 * 章节 start_ms 一律等于某条 segment 的 start_ms，保证点击跳转与 seekTo 对齐。
 * 时间戳一律是相对课堂开始的毫秒数；绝对钟点由 UI 用 sessions.started_at 换算。
 */

#define CA_NOTE_MAX_SEGMENTS 5000
#define CA_NOTE_DEF_BLOCK_MS 45000
#define CA_NOTE_TAIL_MIN_MS 45000       /* 末章短于 45 s 时并入上一章 */
#define CA_NOTE_MIN_CHAPTER_MS 120000   /* 章节最小间隔 2 min */
#define CA_NOTE_CUT_WINDOW_MS 150000    /* 2 min 后最晚到 2.5 min 必切 */
#define CA_NOTE_PAUSE_MS 1200           /* 句间 ≥1.2 s 视为长停顿 */
#define CA_NOTE_TITLE_MIN_CP 6
#define CA_NOTE_TITLE_MAX_CP 24          /* 所有章节标题统一 ≤24 字（语义/固定两条路径一致） */
#define CA_NOTE_SUMMARY_CP 60

static char* note_strdup(const char* s) {
  size_t n;
  char* p;

  if (!s) return NULL;
  n = strlen(s) + 1;
  p = (char*)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

static size_t utf8_prefix_bytes(const char* s, int codepoints) {
  size_t i = 0;
  int n = 0;

  if (!s) return 0;
  while (s[i] && n < codepoints) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) i += 1;
    else if (c < 0xE0) i += 2;
    else if (c < 0xF0) i += 3;
    else i += 4;
    n++;
  }
  return i;
}

static char* utf8_truncate(const char* s, int codepoints) {
  size_t n;
  char* out;

  if (!s) return NULL;
  n = utf8_prefix_bytes(s, codepoints);
  out = (char*)malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

static void format_mmss(int64_t ms, char* buf, size_t cap) {
  int64_t sec = ms / 1000;
  if (sec < 0) sec = 0;
  if (sec >= 3600) {
    snprintf(buf, cap, "%lld:%02lld:%02lld",
             (long long)(sec / 3600), (long long)((sec / 60) % 60),
             (long long)(sec % 60));
  } else {
    snprintf(buf, cap, "%02lld:%02lld",
             (long long)(sec / 60), (long long)(sec % 60));
  }
}

/* 取一句的字幕文本，优先级：会中增量纠错 > 课后声学校正 > 原始 ASR。
 * M4 起增量摘要每批都会回写 refined_segments（实测能把「市场价格是指政府为了
 * 维维持一一品品的产产」纠成「支持价格是指政府为了维持某种产品的生产…」），
 * 出稿必须用它，否则纠错白做。列序见 load_segments 的 SELECT。 */
static const char* seg_text(sqlite3_stmt* st) {
  const unsigned char* refined = sqlite3_column_text(st, 5);
  const unsigned char* corrected = sqlite3_column_text(st, 4);
  const unsigned char* raw = sqlite3_column_text(st, 3);

  if (refined && refined[0]) return (const char*)refined;
  if (corrected && corrected[0]) return (const char*)corrected;
  return raw ? (const char*)raw : "";
}

static int load_segments(sqlite3* db, int64_t session_id,
                         note_seg** out, int* out_n) {
  sqlite3_stmt* st;
  note_seg* arr = NULL;
  int n = 0;
  int cap = 0;

  if (sqlite3_prepare_v2(db,
      "SELECT s.id, s.start_ms, s.end_ms, s.text, s.text_corrected, r.text"
      " FROM segments s LEFT JOIN text_refined r"
      " ON r.session_id = s.session_id AND r.seg_id = s.id"
      " WHERE s.session_id=? ORDER BY s.start_ms, s.id", -1, &st, NULL) != SQLITE_OK) {
    return CA_ERR_IO;
  }
  sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    if (n >= CA_NOTE_MAX_SEGMENTS) break;
    if (n >= cap) {
      int ncap = cap ? cap * 2 : 64;
      note_seg* p = (note_seg*)realloc(arr, (size_t)ncap * sizeof(note_seg));
      if (!p) break;
      arr = p;
      cap = ncap;
    }
    arr[n].id = (int64_t)sqlite3_column_int64(st, 0);
    arr[n].start_ms = (int64_t)sqlite3_column_int64(st, 1);
    arr[n].end_ms = (int64_t)sqlite3_column_int64(st, 2);
    arr[n].text = note_strdup(seg_text(st));
    arr[n].corrected = (sqlite3_column_text(st, 4) &&
                        sqlite3_column_text(st, 4)[0]) ? 1 : 0;
    n++;
  }
  sqlite3_finalize(st);
  *out = arr;
  *out_n = n;
  return CA_OK;
}

/* ---------- 文本小工具 ---------- */

static char* note_append(char* buf, size_t* len, const char* add) {
  size_t a;
  char* p;

  if (!add) return buf;
  a = strlen(add);
  if (a == 0) return buf;
  p = (char*)realloc(buf, *len + a + 1);
  if (!p) return buf;
  memcpy(p + *len, add, a);
  *len += a;
  p[*len] = '\0';
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

static int ends_with(const char* s, const char* suffix) {
  size_t n, m;

  if (!s || !suffix) return 0;
  n = strlen(s);
  m = strlen(suffix);
  return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

static int ends_sentence(const char* s) {
  return ends_with(s, "。") || ends_with(s, "！") || ends_with(s, "？") ||
         ends_with(s, "!") || ends_with(s, "?") || ends_with(s, "…");
}

static int starts_with_at(const char* s, const char* head) {
  size_t i = 0, m;

  if (!s) return 0;
  while (s[i] == ' ' || s[i] == '\t') i++;
  m = strlen(head);
  return strncmp(s + i, head, m) == 0;
}

/* 话题信号词：出现在句首时视为新话题的起点 */
static int is_topic_cue(const char* s) {
  static const char* cues[] = {
    "接下来", "下面", "首先", "其次", "再次", "然后", "最后", "另外",
    "此外", "我们来看", "我们再看", "这一节", "这节课", "本章", "这一章",
    "第一个", "第二个", "第三个", "总结一下", "综上所述", "总之"
  };
  size_t i;

  for (i = 0; i < sizeof(cues) / sizeof(cues[0]); ++i) {
    if (starts_with_at(s, cues[i])) return 1;
  }
  return 0;
}

/* 句间停顿（s 时刻与上一句结束的间隔）与话题信号，分值越高越适合当切点 */
static int signal_score(note_seg* segs, int i) {
  int score = 0;
  int64_t prev_end;
  int64_t pause;

  if (i <= 0) return 0;
  prev_end = segs[i - 1].end_ms > segs[i - 1].start_ms
               ? segs[i - 1].end_ms : segs[i - 1].start_ms;
  pause = segs[i].start_ms - prev_end;
  if (pause >= CA_NOTE_PAUSE_MS) score += 2;
  if (is_topic_cue(segs[i].text)) score += 2;
  if (ends_sentence(segs[i - 1].text)) score += 1;
  return score;
}

/* 去掉标题尾部的标点/悬空词/孤立数字，避免「…不超过二」「已参加」这类残句。
 * aggressive=0 时只清尾部标点：整句保留（未校正的无标点长句）不得从中间截断。 */
static void trim_title(char* s, int aggressive) {
  static const char* punct[] = {
    "。", "，", "、", "；", "：", "！", "？", "…", ".", ",", ";", ":", "!", "?"
  };
  static const char* dangling[] = {
    "的", "地", "得", "和", "与", "及", "或", "而", "但", "因为", "如果",
    "所以", "但是", "而且", "虽然", "由于", "并且", "不超过", "不少于", "已经"
  };
  size_t n;
  int changed = 1;

  if (!s) return;
  n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                   s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
  while (changed && n > 0) {
    size_t i;
    changed = 0;
    for (i = 0; i < sizeof(punct) / sizeof(punct[0]); ++i) {
      size_t m = strlen(punct[i]);
      if (n >= m && memcmp(s + n - m, punct[i], m) == 0) { n -= m; changed = 1; break; }
    }
    if (!changed && aggressive) {
      for (i = 0; i < sizeof(dangling) / sizeof(dangling[0]); ++i) {
        size_t m = strlen(dangling[i]);
        if (n >= m && memcmp(s + n - m, dangling[i], m) == 0) { n -= m; changed = 1; break; }
      }
    }
    if (!changed && aggressive && n > 0 && cp_count(s) > 8) {
      unsigned char c = (unsigned char)s[n - 1];
      if (c >= '0' && c <= '9') { n--; changed = 1; }
      else if (n >= 3) {
        static const char* digits = "一二三四五六七八九十百千零两";
        char last3[4];
        last3[0] = s[n - 3]; last3[1] = s[n - 2]; last3[2] = s[n - 1]; last3[3] = 0;
        if (strstr(digits, last3)) { n -= 3; changed = 1; }
      }
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
  }
  s[n] = '\0';
}

/* 标题上限内最靠后的标点位置（字节偏移），没有则返回 0 */
static size_t last_punct_within(const char* s, size_t limit) {
  static const char* punct[] = {"。", "，", "、", "；", "：", "！", "？", "…", ".", ",", ";"};
  size_t best = 0;
  size_t i, p;

  for (i = 0; i < sizeof(punct) / sizeof(punct[0]); ++i) {
    size_t m = strlen(punct[i]);
    for (p = 0; p + m <= limit && s[p]; ++p) {
      if (memcmp(s + p, punct[i], m) == 0 && p + m > best) best = p + m;
    }
  }
  return best;
}

/* 标题 = 块内第一句「完整话」：≥6 字、有句末标点、且非语义未完（否则跳下一句）。
 * ≤24 字整句直接用；>24 字在 24 字内最后标点处收口；24 字内无标点也跳过。
 * 全块找不到合格句 → 「本段要点」：不把半句原样当标题，不出 40 字长标题。 */
static char* build_title(note_seg* segs, int from, int to) {
  int i;

  for (i = from; i < to; ++i) {
    const char* t = segs[i].text;
    size_t limit, cut;
    char* buf;

    if (!t || !t[0]) continue;
    if (cp_count(t) < CA_NOTE_TITLE_MIN_CP) continue;    /* 「初始。」式残片 */
    if (!ends_sentence(t)) continue;                     /* 无句末标点：未完 */
    if (ca_text_looks_incomplete(t)) continue;           /* 动词/量词等掐断 */

    if (cp_count(t) <= CA_NOTE_TITLE_MAX_CP) {
      buf = note_strdup(t);
    } else {
      limit = utf8_prefix_bytes(t, CA_NOTE_TITLE_MAX_CP);
      cut = last_punct_within(t, limit);
      if (cut == 0) continue;        /* 24 字内无标点：硬切即腰斩，跳下一句 */
      buf = note_strdup(t);
      if (buf) buf[cut] = '\0';
    }
    if (!buf) return NULL;
    trim_title(buf, 0);              /* 只清尾标点，不做激进去词 */
    if (cp_count(buf) >= 4) return buf;
    free(buf);                       /* 收口后不足 4 字：跳下一句 */
  }
  return note_strdup("本段要点");
}

/* 摘要 = 从块首句开始累积到 60 字（不臆造） */
static char* build_summary(note_seg* segs, int from, int to) {
  char* buf = NULL;
  char* out;
  size_t len = 0;
  int i;

  for (i = from; i < to; ++i) {
    if (!segs[i].text || !segs[i].text[0]) continue;
    buf = note_append(buf, &len, segs[i].text);
    if (!buf) return note_strdup("");
    if (cp_count(buf) >= CA_NOTE_SUMMARY_CP) break;
  }
  if (!buf) return note_strdup("");
  out = utf8_truncate(buf, CA_NOTE_SUMMARY_CP);
  free(buf);
  return out ? out : note_strdup("");
}

/* 从 first 起找下一章切点：≥2 min 后在 2.5 min 窗口内挑信号最强的一句 */
static int find_cut(note_seg* segs, int n, int first) {
  int64_t base = segs[first].start_ms;
  int best = -1;
  int best_score = -1;
  int i;

  for (i = first + 1; i < n; ++i) {
    int64_t dt = segs[i].start_ms - base;
    int score;
    if (dt < CA_NOTE_MIN_CHAPTER_MS) continue;
    if (dt > CA_NOTE_CUT_WINDOW_MS) break;
    score = signal_score(segs, i);
    if (score > best_score) {
      best = i;
      best_score = score;
      if (score >= 3) break;    /* 停顿 + 话题词，足够明确 */
    }
  }
  if (best < 0) {
    /* 窗口内没有信号：到 2.5 min 强制切，避免整堂课只有一章 */
    for (i = first + 1; i < n; ++i) {
      if (segs[i].start_ms - base >= CA_NOTE_CUT_WINDOW_MS) { best = i; break; }
    }
  }
  return best;
}

static void clear_session_note(sqlite3* db, int64_t session_id) {
  sqlite3_stmt* st;

  if (sqlite3_prepare_v2(db, "DELETE FROM chapters WHERE session_id=?",
                         -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  /* text_refined 与章节同批次重建，避免上一次模型的逐句校正残留 */
  if (sqlite3_prepare_v2(db, "DELETE FROM text_refined WHERE session_id=?",
                         -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
}

static int write_chapter_rows(sqlite3* db, int64_t session_id,
                              note_chapter* arr, int n) {
  int i;

  for (i = 0; i < n; ++i) {
    sqlite3_stmt* st = NULL;

    if (sqlite3_prepare_v2(db,
        "INSERT INTO chapters(session_id, start_ms, title, summary,"
        " seg_id, model_id, prompt_ver, source) VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &st, NULL) != SQLITE_OK) {
      return CA_ERR_IO;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)arr[i].start_ms);
    sqlite3_bind_text(st, 3, arr[i].title ? arr[i].title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, arr[i].summary ? arr[i].summary : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)arr[i].seg_id);
    sqlite3_bind_text(st, 6, arr[i].model_id ? arr[i].model_id : "",
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, arr[i].prompt_ver ? arr[i].prompt_ver : "",
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, arr[i].source ? arr[i].source : "extractive",
                      -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  return CA_OK;
}

static void write_meta(sqlite3* db, int64_t session_id,
                       const ca_note_meta* meta) {
  sqlite3_stmt* st = NULL;

  if (sqlite3_prepare_v2(db,
      "INSERT OR REPLACE INTO note_meta(session_id, source, model_id,"
      " prompt_ver, reason, llm_calls, compress_rounds, rejected_chapters,"
      " estimated) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)", -1, &st, NULL)
      != SQLITE_OK) {
    return;
  }
  sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
  sqlite3_bind_text(st, 2, meta->source == 0 ? "llm" : "extractive",
                    -1, SQLITE_TRANSIENT);
  /* model_id/prompt_ver 由 chapters 行携带；UI 从 chapters 读 */
  sqlite3_bind_text(st, 3, "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, meta->reason, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 6, meta->llm_calls);
  sqlite3_bind_int(st, 7, meta->compress_rounds);
  sqlite3_bind_int(st, 8, meta->rejected_chapters);
  sqlite3_bind_int(st, 9, meta->estimated);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

/* M2.6 抽取式：最小块 45 s，≥2 min 间隔，优先停顿/话题词/句末标点 */
static int build_chapters_extractive(note_seg* segs, int n, int32_t block_ms,
                                     note_chapter** out, int* out_n) {
  note_chapter* arr;
  int* starts;
  int semantic = 0;
  int chapters = 0;
  int k = 0;
  int i;

  if (n <= 0) {
    *out = NULL;
    *out_n = 0;
    return CA_OK;
  }
  arr = (note_chapter*)calloc((size_t)n, sizeof(note_chapter));
  starts = (int*)calloc((size_t)n, sizeof(int));
  if (!arr || !starts) {
    free(arr);
    free(starts);
    return CA_ERR_IO;
  }

  /* 有校正文本才走语义切块；没有（模型未下载/关闭校正）退回固定分块 */
  for (i = 0; i < n; ++i) {
    if (segs[i].corrected) { semantic = 1; break; }
  }

  if (semantic) {
    int first = 0;
    starts[k++] = 0;
    while (first < n) {
      int cut = find_cut(segs, n, first);
      if (cut <= first || cut >= n) break;
      starts[k++] = cut;
      first = cut;
    }
    /* 末章过短并回上一章 */
    while (k > 1) {
      int last = starts[k - 1];
      int64_t end = segs[n - 1].end_ms > segs[last].start_ms
                      ? segs[n - 1].end_ms : segs[n - 1].start_ms;
      if (end - segs[last].start_ms < CA_NOTE_TAIL_MIN_MS) k--;
      else break;
    }
  } else {
    int idx = 0;
    while (idx < n) {
      int64_t block_start = segs[idx].start_ms;
      starts[k++] = idx;
      idx++;
      while (idx < n && segs[idx].start_ms - block_start < block_ms) idx++;
    }
  }

  for (i = 0; i < k; ++i) {
    int first = starts[i];
    int end = (i + 1 < k) ? starts[i + 1] : n;
    int64_t block_start = segs[first].start_ms;
    char* title = build_title(segs, first, end);
    char* summary = build_summary(segs, first, end);

    if (!title) title = note_strdup("课堂片段");
    if (!summary) summary = note_strdup("");
    if (!title || !summary) { free(title); free(summary); break; }

    arr[chapters].start_ms = block_start;
    arr[chapters].seg_id = segs[first].id;
    arr[chapters].title = title;
    arr[chapters].summary = summary;
    arr[chapters].source = note_strdup("extractive");
    arr[chapters].model_id = note_strdup("");
    arr[chapters].prompt_ver = note_strdup("");
    chapters++;
  }

  free(starts);
  *out = arr;
  *out_n = chapters;
  return CA_OK;
}

static int build_chapters(sqlite3* db, int64_t session_id, note_seg* segs,
                          int n, int32_t block_ms,
                          const ca_note_options* opt,
                          const ca_llm_port* llm, void* llm_engine,
                          note_chapter** out, int* out_n,
                          ca_note_meta* meta) {
  memset(meta, 0, sizeof(*meta));
  meta->source = 1;
  if (opt && !opt->allow_llm) {
    snprintf(meta->reason, sizeof(meta->reason),
             "未启用 LLM（未填 Key 或本地模型未就绪），已降级抽取式");
  } else {
    snprintf(meta->reason, sizeof(meta->reason), "抽取式章节");
  }

  clear_session_note(db, session_id);
  if (n <= 0) { *out = NULL; *out_n = 0; return CA_OK; }

  if (opt && opt->allow_llm && llm) {
    int rc = ca_note_llm_chapters(db, session_id, segs, n, opt, llm,
                                  llm_engine, out, out_n, meta);
    if (rc == CA_OK && *out_n > 0) {
      if (write_chapter_rows(db, session_id, *out, *out_n) == CA_OK) {
        write_meta(db, session_id, meta);
        return CA_OK;
      }
      ca_note_free_chapters(*out, *out_n);
      *out = NULL;
      *out_n = 0;
      snprintf(meta->reason, sizeof(meta->reason),
               "写入 chapters 失败，已降级抽取式");
      meta->source = 1;
    }
    /* LLM 失败：meta 里已写具体原因，继续走抽取式 */
  }

  if (build_chapters_extractive(segs, n, block_ms, out, out_n) != CA_OK) {
    return CA_ERR_IO;
  }
  write_chapter_rows(db, session_id, *out, *out_n);
  write_meta(db, session_id, meta);
  return CA_OK;
}

static void write_note(const char* path, int64_t session_id, int64_t started_at,
                       const char* title, note_chapter* chapters, int cn,
                       note_seg* segs, int n, const ca_note_meta* meta) {
  FILE* f;
  int ci = 0;
  char ts[24];

  if (!path || !path[0]) return;
  f = fopen(path, "wb");
  if (!f) return;

  fprintf(f, "# 课堂笔记\n\n");
  fprintf(f, "- 会话：#%lld", (long long)session_id);
  if (title && title[0]) fprintf(f, " %s", title);
  fprintf(f, "\n");
  if (started_at > 0) {
    fprintf(f, "- 开始：%lld（epoch ms，界面可切换课堂钟点）\n",
            (long long)started_at);
  }
  fprintf(f, "- 说明：自动整理自转写文本，无需音频；点 [mm:ss] 可跳转到对应字幕\n");
  if (meta) {
    fprintf(f, "- 生成：%s（%s）\n",
            meta->source == 0 ? "LLM 语义分章" : "抽取式章节（降级）",
            meta->reason);
  }
  fprintf(f, "\n");

  for (int s = 0; s < n; ++s) {
    /* 每个章节起点都等于某句的起点：在该句前输出章节标题 */
    while (ci < cn && segs[s].start_ms >= chapters[ci].start_ms) {
      format_mmss(chapters[ci].start_ms, ts, sizeof(ts));
      fprintf(f, "## [%s] %s\n\n", ts,
              chapters[ci].title ? chapters[ci].title : "课堂片段");
      if (chapters[ci].summary && chapters[ci].summary[0]) {
        fprintf(f, "> %s\n\n", chapters[ci].summary);
      }
      ci++;
    }
    format_mmss(segs[s].start_ms, ts, sizeof(ts));
    fprintf(f, "- [%s] %s\n", ts, segs[s].text ? segs[s].text : "");
  }

  fclose(f);
}

int64_t ca_note_build_ex(const char* db_path, const char* note_path,
                         const ca_note_options* opt,
                         const ca_llm_port* llm, void* llm_engine) {
  sqlite3* db = NULL;
  sqlite3_stmt* st = NULL;
  note_seg* segs = NULL;
  note_chapter* chapters = NULL;
  ca_note_meta meta;
  int n = 0, cn = 0;
  int64_t session_id = -1;
  int64_t started_at = 0;
  int32_t block_ms = CA_NOTE_DEF_BLOCK_MS;
  char title[128];

  if (!db_path || !db_path[0]) return -1;
  title[0] = '\0';
  memset(&meta, 0, sizeof(meta));

  if (sqlite3_open(db_path, &db) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return -1;
  }
  if (ca_storage_init_schema(db) != CA_OK) {
    sqlite3_close(db);
    return -1;
  }

  /* 优先取最近一场正常结束的会话；没有就退回最新一行，便于中断后也能整理 */
  if (sqlite3_prepare_v2(db,
      "SELECT id, started_at, title FROM sessions"
      " WHERE status='done' ORDER BY ended_at DESC, id DESC LIMIT 1",
      -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) {
      session_id = (int64_t)sqlite3_column_int64(st, 0);
      started_at = (int64_t)sqlite3_column_int64(st, 1);
      const unsigned char* t = sqlite3_column_text(st, 2);
      if (t) snprintf(title, sizeof(title), "%s", (const char*)t);
    }
    sqlite3_finalize(st);
  }
  if (session_id < 0 &&
      sqlite3_prepare_v2(db,
        "SELECT id, started_at, title FROM sessions ORDER BY id DESC LIMIT 1",
        -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) {
      session_id = (int64_t)sqlite3_column_int64(st, 0);
      started_at = (int64_t)sqlite3_column_int64(st, 1);
      const unsigned char* t = sqlite3_column_text(st, 2);
      if (t) snprintf(title, sizeof(title), "%s", (const char*)t);
    }
    sqlite3_finalize(st);
  }

  if (session_id < 0) {
    sqlite3_close(db);
    return -1;
  }

  if (load_segments(db, session_id, &segs, &n) != CA_OK) {
    sqlite3_close(db);
    return -1;
  }
  if (build_chapters(db, session_id, segs, n, block_ms, opt, llm, llm_engine,
                     &chapters, &cn, &meta) != CA_OK) {
    cn = 0;
  }
  write_note(note_path, session_id, started_at,
             title[0] ? title : NULL, chapters, cn, segs, n, &meta);

  ca_note_free_chapters(chapters, cn);
  for (int i = 0; i < n; ++i) free(segs[i].text);
  free(segs);
  sqlite3_close(db);
  return session_id;
}

int64_t ca_note_build(const char* db_path, const char* note_path,
                      int32_t block_ms) {
  (void)block_ms;   /* M2.6 的 45 s 固定块是唯一降级口径，不再由外部覆盖 */
  return ca_note_build_ex(db_path, note_path, NULL, NULL, NULL);
}

int ca_note_ensure_schema(const char* db_path) {
  sqlite3* db = NULL;
  int rc;

  if (!db_path || !db_path[0]) return CA_ERR_INVALID;
  if (sqlite3_open(db_path, &db) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return CA_ERR_IO;
  }
  rc = ca_storage_init_schema(db);
  sqlite3_close(db);
  return rc;
}
