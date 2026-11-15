#include "core_internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * ASR 窗口切分器。
 *
 * M1：整窗模式。PCM 攒满 window_ms 后同步调用 port->transcribe()，
 *     结果包成 CA_EV_ASR_FINAL 发出。
 * M2：流式模式。port->stream_feed 非空时不再攒窗，每个小批直接喂给流式
 *     引擎；引擎返回 1 表示当前句的部分结果（CA_EV_ASR_PARTIAL，全量假设，
 *     时间戳为句子起点），返回 2 表示句末定稿（CA_EV_ASR_FINAL）。
 *
 * 线程约定：所有函数只允许在 ASR 工作线程（或单测主线程）调用，内部不加锁。
 */

#define CA_ASR_SAMPLE_RATE   16000
#define CA_ASR_MAX_WINDOW_MS 30000
#define CA_ASR_DEF_WINDOW_MS 4000
#define CA_ASR_TEXT_CAP      4096
#define CA_ASR_MIN_TAIL_MS   200   /* 停止时短于 200 ms 的尾巴不送识别 */

static const ca_asr_port* g_port;
static void*              g_engine;
static int16_t*           g_buf;        /* 整窗缓冲 */
static int32_t            g_win_frames;
static int32_t            g_step_frames;
static int32_t            g_len;
static int64_t            g_base_ts;
static int                g_has_base;
static uint64_t           g_consumed;   /* 已消费采样总数 */
static char*              g_text;
static int                g_attached;
static int                g_streaming;
static int64_t            g_utter_start_ms = -1;  /* 流式模式：当前句起点 */
static int64_t            g_last_ts_ms;           /* 已喂入音频的结束时刻 */
static int                g_utt_has_text;         /* 当前句是否已出过字 */

/* M2.9 断句腰斩修复：语义未完的 FINAL 先扣留，续句间隔 <1s 时合并后再定稿。
 * 落点在事件侧而非入库后：RollingSummarizer 按 id>lastSegId 增量取句，
 * 入库后 UPDATE 合并会让后半句永远进不了滚动摘要。完整句不受影响，即时发出。 */
static char               g_pend_text[CA_ASR_TEXT_CAP];
static int64_t            g_pend_start_ms = -1;
static int64_t            g_pend_end_ms = -1;
static int64_t            g_pend_since_ms = -1;  /* 首次扣留时刻：扣留总时长上限用 */
static int                g_pend_valid = 0;

/* 流式识别输出（供单测断言推理次数） */
static int32_t            g_stream_calls;
static int32_t            g_stream_finals;
static int32_t            g_stream_partials;

static int emit_text(ca_event_type type, int64_t ts, const char* text,
                     float score, int32_t code) {
  ca_event ev;
  ev.type = type;
  ev.ts_ms = ts;
  ev.text = text;
  ev.score = score;
  ev.code = code;
  ca_core_emit(&ev);
  return CA_OK;
}

int ca_asr_attach(const ca_asr_port* port, void* engine,
                  int32_t window_ms, int32_t step_ms) {
  if (g_attached) return CA_ERR_STATE;
  if (!port || (!port->transcribe && !port->stream_feed) || !engine) {
    return CA_ERR_INVALID;
  }

  if (window_ms <= 0) window_ms = CA_ASR_DEF_WINDOW_MS;
  if (window_ms > CA_ASR_MAX_WINDOW_MS) window_ms = CA_ASR_MAX_WINDOW_MS;
  if (step_ms <= 0 || step_ms > window_ms) step_ms = window_ms;

  g_win_frames  = window_ms * CA_ASR_SAMPLE_RATE / 1000;
  g_step_frames = step_ms * CA_ASR_SAMPLE_RATE / 1000;

  g_buf = (int16_t*)malloc((size_t)g_win_frames * sizeof(int16_t));
  g_text = (char*)malloc(CA_ASR_TEXT_CAP);
  if (!g_buf || !g_text) {
    free(g_buf);  g_buf = NULL;
    free(g_text); g_text = NULL;
    return CA_ERR_IO;
  }

  g_port = port;
  g_engine = engine;
  g_len = 0;
  g_has_base = 0;
  g_consumed = 0;
  g_attached = 1;
  g_streaming = (port->stream_feed != NULL);
  g_utter_start_ms = -1;
  g_last_ts_ms = 0;
  g_utt_has_text = 0;
  g_pend_valid = 0;
  g_stream_calls = g_stream_finals = g_stream_partials = 0;
  return CA_OK;
}

/* ---------- M1 整窗模式 ---------- */

/* 对 n 个采样做一次整窗识别；n 可以小于窗口（停止时的尾巴） */
static int flush_window(int32_t n) {
  int64_t win_start;
  int rc;

  if (n <= 0) return CA_OK;

  g_text[0] = '\0';
  rc = g_port->transcribe(g_engine, g_buf, n, CA_ASR_SAMPLE_RATE,
                          g_text, CA_ASR_TEXT_CAP);
  if (rc != CA_OK) {
    emit_text(CA_EV_ERROR, g_base_ts, "asr transcribe failed", 0.0f, rc);
    return rc;
  }

  win_start = g_base_ts + (int64_t)(g_consumed * 1000ull / CA_ASR_SAMPLE_RATE);

  if (g_text[0] != '\0') {
    /* code = 本次识别覆盖的音频长度（ms），平台层据此打印「音频/耗时」比 */
    emit_text(CA_EV_ASR_FINAL, win_start, g_text, 0.0f,
              (int32_t)(n * 1000 / CA_ASR_SAMPLE_RATE));
  }
  return CA_OK;
}

/* ---------- M2.9 语义未完护栏（与 platform/asr_sherpa.cpp 规则一致） ---------- */

/* 剥句尾标点/空白，返回有效长度 */
static size_t strip_tail_punct(const char* s, size_t n) {
  static const char* punct[] = {
    "。", "，", "、", "；", "：", "！", "？", "…", ".", ",", ";", ":", "!", "?"
  };
  int changed = 1;

  while (changed && n > 0) {
    size_t i;
    changed = 0;
    for (i = 0; i < sizeof(punct) / sizeof(punct[0]); ++i) {
      size_t m = strlen(punct[i]);
      if (n >= m && memcmp(s + n - m, punct[i], m) == 0) { n -= m; changed = 1; break; }
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) { n--; changed = 1; }
  }
  return n;
}

/* 末两个 codepoint 的起始偏移（UTF-8 自同步：回退跳过 10xxxxxx 续字节） */
static size_t last_cp_off(const char* s, size_t end, size_t* prev) {
  size_t i = end;

  do { i--; } while (i > 0 && (((unsigned char)s[i]) & 0xC0) == 0x80);
  if (prev) {
    if (i == 0) {
      *prev = (size_t)-1;
    } else {
      size_t j = i - 1;
      while (j > 0 && (((unsigned char)s[j]) & 0xC0) == 0x80) j--;
      *prev = j;
    }
  }
  return i;
}

/* cp 是否命中字集（集内均为等长 3 字节 CJK 字，按等长步进比较） */
static int cp_in_set(const char* s, size_t off, size_t len, const char* set) {
  size_t i;

  for (i = 0; set[i]; i += len) {
    if (memcmp(set + i, s + off, len) == 0) return 1;
  }
  return 0;
}

/* 实时严表：只含高置信度项（连词/助词尾、数字尾、数字+量词尾、单字重复），
 * 供扣留/合并判定；动词表与 不/没/未/什/各 误判面大，不进实时路径。 */
int ca_text_looks_incomplete_strict(const char* s) {
  static const char* tails[] = {
    "因为", "所以", "但是", "如果", "虽然", "而且", "然后", "以及",
    "或者", "还有", "就是", "并且", "不过", "于是", "的话",
    "的", "和", "跟", "与", "或", "把", "被", "让", "对", "向"
  };
  static const char* digits = "一二三四五六七八九十百千万亿两";
  /* 量词集：排除年/月/日/号（「2026年。」是完整句） */
  static const char* measure = "个种条次点位本张部块件项类段节层句行页章篇题步轮遍组套倍成";
  size_t n, i, last, prev, llen, plen;
  unsigned char c;

  if (!s) return 0;
  n = strip_tail_punct(s, strlen(s));
  if (n == 0) return 0;

  for (i = 0; i < sizeof(tails) / sizeof(tails[0]); ++i) {
    size_t m = strlen(tails[i]);
    if (n >= m && memcmp(s + n - m, tails[i], m) == 0) return 1;
  }

  last = last_cp_off(s, n, &prev);
  llen = n - last;
  c = (unsigned char)s[last];

  if (llen == 1 && c >= '0' && c <= '9') return 1;          /* 数字尾 */
  if (llen == 3 && cp_in_set(s, last, 3, digits)) return 1; /* 中文数字尾 */
  if (prev != (size_t)-1) {
    plen = last - prev;
    if (llen == 3 && plen == 3 &&
        memcmp(s + last, s + prev, 3) == 0) return 1;       /* 单字重复 */
    if (llen == 3 && cp_in_set(s, last, 3, measure)) {      /* 数字+量词尾 */
      unsigned char pc = (unsigned char)s[prev];
      if ((plen == 1 && pc >= '0' && pc <= '9') ||
          (plen == 3 && cp_in_set(s, prev, 3, digits))) return 1;
    }
  }
  return 0;
}

/* 宽表 = 严表 + 动词/介词掐断尾 + 否定掐断 + 什/各：仅供 note.c 标题筛选，
 * 不进实时收句/合并路径。 */
int ca_text_looks_incomplete(const char* s) {
  static const char* wide_tails[] = {
    /* 动词/介词掐断：后面缺宾语 */
    "取出", "拿出", "提出", "看到", "讲到", "说到", "包括", "分为",
    "称为", "叫做", "变成", "得到", "形成", "产生", "进行", "通过",
    "采用", "根据", "按照", "针对", "由于", "为了", "关于", "对于",
    /* 否定掐断 / 「什么」「各个」被掐 */
    "没有", "不", "没", "未", "什", "各"
  };
  size_t n, i;

  if (!s) return 0;
  if (ca_text_looks_incomplete_strict(s)) return 1;
  n = strip_tail_punct(s, strlen(s));
  if (n == 0) return 0;
  for (i = 0; i < sizeof(wide_tails) / sizeof(wide_tails[0]); ++i) {
    size_t m = strlen(wide_tails[i]);
    if (n >= m && memcmp(s + n - m, wide_tails[i], m) == 0) return 1;
  }
  return 0;
}

/* 扣留的未完句定稿发出（code = 覆盖音频时长，与既有语义一致） */
static void flush_pending(void) {
  if (!g_pend_valid) return;
  emit_text(CA_EV_ASR_FINAL, g_pend_start_ms, g_pend_text, 0.0f,
            (int32_t)(g_pend_end_ms - g_pend_start_ms));
  g_pend_valid = 0;
}

/* ---------- M2 流式模式 ---------- */

static int feed_stream(const int16_t* pcm, int32_t frames, int64_t ts_ms) {
  int32_t is_final = 0;
  int rc;

  /* 扣留句等续句：已隔 ≥1s 仍未接上 → 先定稿发出。
   * 续句进行中（g_utter_start_ms 已指向新句起点）时按其起点判断，
   * 避免长句讲到一半时扣留句被墙钟误冲掉、错过后续合并。
   * 扣留总时长到 1.8s 上限也强制定稿：屏幕不能停住。 */
  if (g_pend_valid) {
    int64_t ref = (g_utter_start_ms >= 0) ? g_utter_start_ms : ts_ms;
    if (ref - g_pend_end_ms >= 1000 ||
        ts_ms - g_pend_since_ms >= 1800) flush_pending();
  }

  if (g_utter_start_ms < 0) g_utter_start_ms = ts_ms;

  g_text[0] = '\0';
  g_stream_calls++;
  rc = g_port->stream_feed(g_engine, pcm, frames, CA_ASR_SAMPLE_RATE,
                           g_text, CA_ASR_TEXT_CAP, &is_final);
  if (rc != CA_OK) {
    emit_text(CA_EV_ERROR, ts_ms, "stream asr failed", 0.0f, rc);
    return rc;
  }
  /* 还没出过字的纯静音块：把句起点回滚，避免把句子起点记早 0.2-0.4 s */
  if (g_text[0] == '\0') {
    if (!g_utt_has_text) g_utter_start_ms = -1;
    return CA_OK;
  }
  g_utt_has_text = 1;

  if (is_final) {
    int64_t final_end = ts_ms + (int64_t)frames * 1000 / CA_ASR_SAMPLE_RATE;
    g_stream_finals++;
    if (g_pend_valid && g_utter_start_ms - g_pend_end_ms < 1000 &&
        ts_ms - g_pend_since_ms < 1800 &&
        ca_text_looks_incomplete_strict(g_pend_text)) {
      /* 续句间隔 <1s、前条语义未完（严表）、扣留未超上限：
       * 剥前条尾标点后拼接，继续扣住（可链式） */
      size_t cur;
      g_pend_text[strip_tail_punct(g_pend_text, strlen(g_pend_text))] = '\0';
      cur = strlen(g_pend_text);
      strncat(g_pend_text, g_text, CA_ASR_TEXT_CAP - cur - 1);
      g_pend_end_ms = final_end;
    } else {
      flush_pending();
      if (ca_text_looks_incomplete_strict(g_text)) {
        /* 语义未完：扣留等续句，但立刻发 PARTIAL 保持屏幕实时，
         * 后续合并或定稿时由 FINAL 替换（FINAL 被扣留时零事件是
         * 「录音中不出字幕」的直接原因，必须避免） */
        strncpy(g_pend_text, g_text, CA_ASR_TEXT_CAP - 1);
        g_pend_text[CA_ASR_TEXT_CAP - 1] = '\0';
        g_pend_start_ms = g_utter_start_ms;
        g_pend_end_ms = final_end;
        g_pend_since_ms = ts_ms;
        g_pend_valid = 1;
        emit_text(CA_EV_ASR_PARTIAL, g_pend_start_ms, g_pend_text, 0.0f,
                  (int32_t)(g_pend_end_ms - g_pend_start_ms));
      } else {
        emit_text(CA_EV_ASR_FINAL, g_utter_start_ms, g_text, 0.0f,
                  (int32_t)(final_end - g_utter_start_ms));
      }
    }
    g_utter_start_ms = -1;
    g_utt_has_text = 0;
  } else {
    g_stream_partials++;
    if (g_pend_valid) {
      /* 扣留期间：以「扣留句 + 当前识别中」合成 PARTIAL，屏幕不停 */
      char comb[CA_ASR_TEXT_CAP];
      size_t pl;
      strncpy(comb, g_pend_text, CA_ASR_TEXT_CAP - 1);
      comb[CA_ASR_TEXT_CAP - 1] = '\0';
      pl = strlen(comb);
      strncat(comb, g_text, CA_ASR_TEXT_CAP - pl - 1);
      emit_text(CA_EV_ASR_PARTIAL, g_pend_start_ms, comb, 0.0f, 0);
    } else {
      emit_text(CA_EV_ASR_PARTIAL, g_utter_start_ms, g_text, 0.0f, 0);
    }
  }
  return CA_OK;
}

int ca_asr_feed(const int16_t* pcm, int32_t frames, int64_t ts_ms) {
  if (!g_attached) return CA_ERR_STATE;
  if (!pcm || frames <= 0) return CA_ERR_INVALID;

  if (!g_has_base) {
    g_base_ts = ts_ms;
    g_has_base = 1;
  }
  g_last_ts_ms = ts_ms + (int64_t)frames * 1000 / CA_ASR_SAMPLE_RATE;

  if (g_streaming) return feed_stream(pcm, frames, ts_ms);

  while (frames > 0) {
    int32_t space = g_win_frames - g_len;
    int32_t take = (frames < space) ? frames : space;
    if (take > 0) {
      memcpy(g_buf + g_len, pcm, (size_t)take * sizeof(int16_t));
      g_len += take;
      pcm += take;
      frames -= take;
    }
    if (g_len >= g_win_frames) {
      int rc = flush_window(g_win_frames);
      if (rc != CA_OK) return rc;
      /* 滑出一步，余下（重叠部分）移到头部 */
      g_len -= g_step_frames;
      if (g_len > 0) {
        memmove(g_buf, g_buf + g_step_frames, (size_t)g_len * sizeof(int16_t));
      }
      g_consumed += (uint64_t)g_step_frames;
    }
  }
  return CA_OK;
}

/*
 * 停止时调用：把尾部不足一窗的残余采样补跑一次，避免最后一句被吞。
 * 修 M1 遗留问题②：原实现在 detach 里直接 free(g_buf)，尾巴永久丢失。
 */
void ca_asr_drain(void) {
  if (!g_attached) return;

  if (g_streaming) {
    /* 流式引擎自己会在静音处收句；这里补喂 0 让未定稿的句子有机会落定。
     * 时间戳从上一次喂入的真实结束时刻继续走，保证 FINAL 的时长正确。 */
    if (g_utter_start_ms >= 0 && g_port->stream_feed) {
      int16_t silence[320];
      int64_t t = g_last_ts_ms;
      memset(silence, 0, sizeof(silence));
      for (int i = 0; i < 8; ++i) {   /* 160 ms 静音，触发端点判定 */
        feed_stream(silence, 320, t);
        t += 320 * 1000 / CA_ASR_SAMPLE_RATE;
      }
      g_last_ts_ms = t;
    }
  } else if (g_len > 0 && g_port->transcribe) {
    int32_t tail_ms = g_len * 1000 / CA_ASR_SAMPLE_RATE;
    if (tail_ms >= CA_ASR_MIN_TAIL_MS) {
      flush_window(g_len);
      g_consumed += (uint64_t)g_len;
    }
    g_len = 0;
  }

  flush_pending();   /* M2.9：停止时补发扣留的未完句，不丢尾 */
}

void ca_asr_detach(void) {
  if (!g_attached) return;
  ca_asr_drain();
  free(g_buf);  g_buf = NULL;
  free(g_text); g_text = NULL;
  g_port = NULL;
  g_engine = NULL;
  g_len = 0;
  g_consumed = 0;
  g_has_base = 0;
  g_utter_start_ms = -1;
  g_last_ts_ms = 0;
  g_utt_has_text = 0;
  g_streaming = 0;
  g_attached = 0;
}
