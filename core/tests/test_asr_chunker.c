#include <stdio.h>
#include <string.h>

#include "classmate.h"

/*
 * asr_chunker 单测：不碰真实引擎，用桩 port 验证窗口策略。
 * 覆盖：窗口触发时机、步进重叠、事件文本/时间戳、空结果跳过、
 *       未挂引擎时的状态码、默认参数、参数钳制。
 * 用法：桌面 gcc 直接编译运行（见 run_tests.bat）。
 */

#define CHECK(cond) do { \
  g_checks++; \
  if (!(cond)) { g_fails++; printf("FAIL %d: %s\n", __LINE__, #cond); } \
} while (0)

static int g_checks;
static int g_fails;

/* ---- 桩引擎 ---- */
static int g_stub_calls;
static int g_stub_frames_total;
static int g_stub_empty;     /* 非 0 时返回空串 */

static int stub_transcribe(void* engine, const int16_t* pcm, int32_t frames,
                           int32_t sample_rate, char* out, int32_t out_cap) {
  (void)engine; (void)pcm;
  g_stub_calls++;
  g_stub_frames_total += frames;
  if (sample_rate != 16000) return CA_ERR_INVALID;
  if (g_stub_empty) {
    if (out_cap > 0) out[0] = '\0';
    return CA_OK;
  }
  snprintf(out, (size_t)out_cap, "win%d", g_stub_calls);
  return CA_OK;
}

static ca_asr_port g_stub_port = { stub_transcribe, NULL };
static int g_stub_engine_token;

/* ---- 桩流式引擎：非静音出 PARTIAL，首个静音块出 FINAL ---- */
static int g_stream_engine_token;
static int g_stream_calls_stub;
static int g_stream_finalized;

static int stub_stream_feed(void* engine, const int16_t* pcm, int32_t frames,
                            int32_t sample_rate, char* out, int32_t out_cap,
                            int32_t* is_final) {
  int is_silence = 1;
  (void)engine;
  if (sample_rate != 16000) return CA_ERR_INVALID;
  for (int32_t i = 0; i < frames; ++i) {
    if (pcm[i] != 0) { is_silence = 0; break; }
  }
  g_stream_calls_stub++;
  if (is_silence) {
    if (g_stream_finalized) {          /* 已收句后继续的静音：不再重复出句 */
      *is_final = 0;
      if (out_cap > 0) out[0] = '\0';
      return CA_OK;
    }
    g_stream_finalized = 1;
    *is_final = 1;
    snprintf(out, (size_t)out_cap, "尾句");
    return CA_OK;
  }
  g_stream_finalized = 0;
  *is_final = 0;
  snprintf(out, (size_t)out_cap, "第%d次", g_stream_calls_stub);
  return CA_OK;
}

static ca_asr_port g_stream_port = { NULL, stub_stream_feed };

/* ---- 事件捕获 ---- */
#define MAX_EV 128
static ca_event g_evs[MAX_EV];
static char     g_ev_text[MAX_EV][64];
static int      g_ev_count;

static void on_ev(const ca_event* ev, void* user) {
  (void)user;
  if (g_ev_count >= MAX_EV) return;
  g_evs[g_ev_count] = *ev;
  if (ev->text) {
    snprintf(g_ev_text[g_ev_count], sizeof(g_ev_text[0]), "%s", ev->text);
    g_evs[g_ev_count].text = g_ev_text[g_ev_count];
  }
  g_ev_count++;
}

static void reset_world(void) {
  g_checks = 0; g_fails = 0;
  g_stub_calls = 0; g_stub_frames_total = 0; g_stub_empty = 0;
  g_stream_calls_stub = 0; g_stream_finalized = 0;
  g_ev_count = 0;
  memset(g_evs, 0, sizeof(g_evs));
  ca_shutdown();
}

static void make_cfg(ca_config* cfg, const char* wav) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->wav_path = wav;
  cfg->asr_port = &g_stub_port;
  cfg->asr_engine = &g_stub_engine_token;
}

static void make_stream_cfg(ca_config* cfg, const char* wav) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->wav_path = wav;
  cfg->asr_port = &g_stream_port;
  cfg->asr_engine = &g_stream_engine_token;
}

/* 喂 seconds 秒静音，按 chunk_ms 一块块喂 */
static void feed_seconds(double seconds, int chunk_ms, int64_t* ts) {
  static int16_t pcm[16000];
  int32_t chunk = 16000 * chunk_ms / 1000;
  int64_t total = (int64_t)(seconds * 16000.0);
  memset(pcm, 0, sizeof(pcm));
  while (total > 0) {
    int32_t n = (total > chunk) ? chunk : (int32_t)total;
    ca_asr_feed(pcm, n, *ts);
    *ts += n * 1000 / 16000;
    total -= n;
  }
}

/* 喂 seconds 秒“人声”（非零采样），按 chunk_ms 一块块喂 */
static void feed_speech(double seconds, int chunk_ms, int64_t* ts) {
  static int16_t pcm[16000];
  int32_t chunk = 16000 * chunk_ms / 1000;
  int64_t total = (int64_t)(seconds * 16000.0);
  for (int i = 0; i < 16000; ++i) pcm[i] = 800;
  while (total > 0) {
    int32_t n = (total > chunk) ? chunk : (int32_t)total;
    ca_asr_feed(pcm, n, *ts);
    *ts += n * 1000 / 16000;
    total -= n;
  }
}

int main(void) {
  /* 用例 1：默认 4s 窗 / 4s 步进，喂 10s 应触发 2 次识别 */
  reset_world();
  {
    ca_config cfg;
    int64_t ts = 1000;
    make_cfg(&cfg, "test_m1_a.wav");
    CHECK(ca_init(&cfg, on_ev, NULL) == CA_OK);
    feed_seconds(10.0, 500, &ts);
    CHECK(g_stub_calls == 2);
    CHECK(g_stub_frames_total == 2 * 64000);
    CHECK(g_ev_count == 2);
    CHECK(g_evs[0].type == CA_EV_ASR_FINAL);
    CHECK(g_evs[0].ts_ms == 1000);          /* 第一窗起点 = 首个喂入 ts */
    CHECK(g_evs[1].ts_ms == 1000 + 4000);   /* 第二窗起点 */
    CHECK(strcmp(g_evs[0].text, "win1") == 0);
    CHECK(strcmp(g_evs[1].text, "win2") == 0);
    ca_shutdown();
  }
  printf("case1 default window: %d checks\n", g_checks);

  /* 用例 2：自定义 2s 窗 / 1s 步进，喂 5s 应在 t=2,3,4,5s 触发 4 次 */
  reset_world();
  {
    ca_config cfg;
    int64_t ts = 0;
    make_cfg(&cfg, "test_m1_b.wav");
    cfg.asr_window_ms = 2000;
    cfg.asr_step_ms = 1000;
    CHECK(ca_init(&cfg, on_ev, NULL) == CA_OK);
    feed_seconds(5.0, 250, &ts);
    CHECK(g_stub_calls == 4);
    CHECK(g_ev_count == 4);
    CHECK(g_evs[0].ts_ms == 0);
    CHECK(g_evs[1].ts_ms == 1000);
    CHECK(g_evs[2].ts_ms == 2000);
    CHECK(g_evs[3].ts_ms == 3000);
    ca_shutdown();
  }
  printf("case2 overlap window: %d checks\n", g_checks);

  /* 用例 3：引擎返回空串 -> 不发事件；错误码 -> 发 CA_EV_ERROR */
  reset_world();
  {
    ca_config cfg;
    int64_t ts = 0;
    make_cfg(&cfg, "test_m1_c.wav");
    CHECK(ca_init(&cfg, on_ev, NULL) == CA_OK);
    g_stub_empty = 1;
    feed_seconds(4.0, 1000, &ts);
    CHECK(g_ev_count == 0);     /* 空结果被跳过 */
    ca_shutdown();
  }
  printf("case3 empty result skipped: %d checks\n", g_checks);

  /* 用例 4：不挂引擎时 ca_asr_feed 返回 CA_ERR_STATE 且不崩 */
  reset_world();
  {
    ca_config cfg;
    int16_t pcm[1600];
    memset(&cfg, 0, sizeof(cfg));
    memset(pcm, 0, sizeof(pcm));
    cfg.wav_path = "test_m1_d.wav";
    CHECK(ca_init(&cfg, on_ev, NULL) == CA_OK);
    CHECK(ca_asr_feed(pcm, 1600, 0) == CA_ERR_STATE);
    ca_shutdown();
    CHECK(ca_asr_feed(pcm, 1600, 0) == CA_ERR_STATE);  /* shutdown 后同样安全 */
  }
  printf("case4 no engine: %d checks\n", g_checks);

  /* 用例 5：窗口参数钳制（超上限 30s / 步进大于窗口）不崩且仍出事件 */
  reset_world();
  {
    ca_config cfg;
    int64_t ts = 0;
    make_cfg(&cfg, "test_m1_e.wav");
    cfg.asr_window_ms = 60000;  /* 应被钳到 30000 */
    cfg.asr_step_ms = 99999;    /* 应被钳到窗口 */
    CHECK(ca_init(&cfg, on_ev, NULL) == CA_OK);
    feed_seconds(10.0, 1000, &ts);  /* 30s 窗攒不满 -> 无事件但不崩 */
    CHECK(g_stub_calls == 0);
    CHECK(g_ev_count == 0);
    ca_shutdown();
  }
  printf("case5 clamp: %d checks\n", g_checks);

  /* 用例 6：流式模式 —— 讲话出 PARTIAL，静音收 FINAL，句时间戳=首块 ts */
  reset_world();
  {
    ca_config cfg;
    int64_t ts = 0;
    make_stream_cfg(&cfg, "test_m2_a.wav");
    CHECK(ca_init(&cfg, on_ev, NULL) == CA_OK);
    feed_speech(1.0, 100, &ts);          /* 10 块人声 -> 10 个 PARTIAL */
    CHECK(g_ev_count == 10);
    CHECK(g_evs[0].type == CA_EV_ASR_PARTIAL);
    CHECK(g_evs[0].ts_ms == 0);          /* 句起点 = 第一块人声 */
    feed_seconds(0.5, 100, &ts);         /* 首个静音块收句 */
    CHECK(g_ev_count == 11);
    CHECK(g_evs[10].type == CA_EV_ASR_FINAL);
    CHECK(g_evs[10].ts_ms == 0);         /* FINAL 仍记句起点，不是收句时刻 */
    CHECK(g_evs[10].code == 1100);       /* 时长 = 1.0s 人声 + 0.1s 静音块 */
    CHECK(strcmp(g_evs[10].text, "尾句") == 0);
    ca_shutdown();
  }
  printf("case6 stream partial/final: %d checks\n", g_checks);

  /* 用例 7：整窗模式 detach 前补跑尾部不足一窗的残余（修 M1 问题②） */
  reset_world();
  {
    ca_config cfg;
    int64_t ts = 0;
    make_cfg(&cfg, "test_m2_b.wav");
    CHECK(ca_init(&cfg, on_ev, NULL) == CA_OK);
    feed_seconds(2.5, 250, &ts);         /* 4s 窗攒不满 -> 平时无事件 */
    CHECK(g_stub_calls == 0);
    CHECK(g_ev_count == 0);
    ca_shutdown();                       /* drain 补跑 2.5s 尾窗 */
    CHECK(g_stub_calls == 1);
    CHECK(g_stub_frames_total == 40000);
    CHECK(g_ev_count == 1);
    CHECK(g_evs[0].type == CA_EV_ASR_FINAL);
    CHECK(g_evs[0].ts_ms == 0);
    CHECK(g_evs[0].code == 2500);
  }
  printf("case7 windowed drain tail: %d checks\n", g_checks);

  /* 用例 8：流式模式 detach 前补喂静音，未收句的尾句也要落定 */
  reset_world();
  {
    ca_config cfg;
    int64_t ts = 0;
    make_stream_cfg(&cfg, "test_m2_c.wav");
    CHECK(ca_init(&cfg, on_ev, NULL) == CA_OK);
    feed_speech(1.5, 100, &ts);          /* 全是人声，尚未静音收句 */
    CHECK(g_ev_count == 15);
    CHECK(g_evs[14].type == CA_EV_ASR_PARTIAL);
    ca_shutdown();                       /* 补 160ms 静音 -> 收句 */
    CHECK(g_ev_count == 16);
    CHECK(g_evs[15].type == CA_EV_ASR_FINAL);
    CHECK(g_evs[15].ts_ms == 0);
    CHECK(g_evs[15].code == 1520);       /* 1500ms 人声 + 20ms 静音 */
  }
  printf("case8 stream drain tail: %d checks\n", g_checks);

  remove("test_m1_a.wav"); remove("test_m1_b.wav"); remove("test_m1_c.wav");
  remove("test_m1_d.wav"); remove("test_m1_e.wav");
  remove("test_m2_a.wav"); remove("test_m2_b.wav"); remove("test_m2_c.wav");

  if (g_fails == 0) {
    printf("RESULT: PASS - asr chunker windowing works\n");
    return 0;
  }
  printf("RESULT: FAIL - %d checks failed\n", g_fails);
  return 1;
}
