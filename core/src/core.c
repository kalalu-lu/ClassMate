#include "core_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * M1 核心主流程：ca_init -> (录音线程 ca_feed_pcm | ASR 线程 ca_asr_feed)
 *               -> ca_flush -> ca_shutdown
 * 本文件不含任何 Android 依赖，可在 Windows / Linux / macOS 上直接编译并单测。
 */

#define CA_M0_SAMPLE_RATE 16000
#define CA_M0_CHANNELS    1

static ca_cb  g_cb;
static void*  g_user;
static char*  g_wav_path;
static int    g_save_wav;   /* M2.5：本次会话是否把 PCM 落成 WAV */
static int    g_inited;
static int64_t g_frames;

static char* ca_strdup(const char* s) {
  size_t n;
  char* p;
  if (!s) return NULL;
  n = strlen(s) + 1;
  p = (char*)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

void ca_core_emit(const ca_event* ev) {
  if (!g_cb || !ev) return;
  /* M2：句级定稿先落 SQLite，再派发给上层 UI。只有 ASR_FINAL 会真正写库。 */
  ca_storage_on_event(ev);
  g_cb(ev, g_user);
}

static void emit_error(int32_t code, const char* text, int64_t ts_ms) {
  ca_event ev;
  ev.type = CA_EV_ERROR;
  ev.ts_ms = ts_ms;
  ev.text = text;
  ev.score = 0.0f;
  ev.code = code;
  ca_core_emit(&ev);
}

int ca_init(const ca_config* cfg, ca_cb cb, void* user) {
  int rc;

  if (g_inited) return CA_ERR_STATE;
  if (!cfg) {
    emit_error(CA_ERR_INVALID, "config is required", 0);
    return CA_ERR_INVALID;
  }

  /* M2.5：save_wav=1 时 wav_path 必填；=0 时跳过 ca_wav_open/ca_wav_write，
   * 只做 ASR + SQLite，wav_path 仅用于推导库名标题（可为空）。 */
  g_save_wav = (cfg->save_wav != 0);
  if (g_save_wav && (!cfg->wav_path || !cfg->wav_path[0])) {
    emit_error(CA_ERR_INVALID, "wav_path is required when save_wav=1", 0);
    return CA_ERR_INVALID;
  }

  if (g_save_wav) {
    g_wav_path = ca_strdup(cfg->wav_path);
    if (!g_wav_path) {
      emit_error(CA_ERR_IO, "out of memory", 0);
      return CA_ERR_IO;
    }

    rc = ca_wav_open(g_wav_path, CA_M0_SAMPLE_RATE, CA_M0_CHANNELS);
    if (rc != CA_OK) {
      emit_error(rc, "cannot create wav file", 0);
      free(g_wav_path);
      g_wav_path = NULL;
      return rc;
    }
  }

  g_cb = cb;
  g_user = user;
  g_frames = 0;
  g_inited = 1;

  /* M2：SQLite 落库（sessions/segments）。失败不致命，字幕照常出，只是不留档。
   * M2.6：标题取自调用方给的 wav_path（classmate_yyyymmdd_HHMMSS），
   * 不能用 g_wav_path —— save_wav=0 时它是 NULL，标题会退化成「课堂录音」。 */
  rc = ca_storage_open(cfg->db_path, cfg->started_at_ms, cfg->wav_path);
  if (rc != CA_OK) {
    emit_error(rc, "sqlite open failed, transcript will not be stored", 0);
  }

  /* M1：配置了引擎就挂 ASR 窗口切分器；挂不上不致命，仅报错事件，
   * 录音与 WAV 落盘继续可用（降级为纯 M0 行为）。 */
  if (cfg->asr_port && cfg->asr_engine) {
    rc = ca_asr_attach(cfg->asr_port, cfg->asr_engine,
                       cfg->asr_window_ms, cfg->asr_step_ms);
    if (rc != CA_OK) {
      emit_error(rc, "asr attach failed, recording without transcription", 0);
    }
  }
  return CA_OK;
}

int ca_feed_pcm(const int16_t* pcm, int32_t frames, int64_t ts_ms) {
  int rc;
  (void)ts_ms;   /* M0 不写时间戳索引，M2 落 SQLite 时启用 */

  if (!g_inited) return CA_ERR_STATE;
  if (!pcm || frames <= 0) return CA_ERR_INVALID;

  /* 注：M0 直接写磁盘。文档 §6.1 要求本调用不阻塞超过 5 ms，
   * M1 会把落盘移到工作线程 + 环形缓冲，此处先保证数据不丢。
   * M2.5：save_wav=0 时只累计帧数，不碰磁盘。 */
  if (g_save_wav) {
    rc = ca_wav_write(pcm, frames);
    if (rc != CA_OK) {
      emit_error(rc, "wav write failed", ts_ms);
      return rc;
    }
  }

  g_frames += frames;
  return CA_OK;
}

int ca_flush(void) {
  if (!g_inited) return CA_ERR_STATE;
  return g_save_wav ? ca_wav_flush() : CA_OK;
}

void ca_shutdown(void) {
  if (!g_inited) return;
  ca_asr_detach();              /* 内含 drain：尾部短窗/未收句先落定、写库 */
  ca_storage_close((int64_t)time(NULL) * 1000);   /* 先收尾会话再关库 */
  if (g_save_wav) ca_wav_close();
  free(g_wav_path);
  g_wav_path = NULL;
  g_cb = NULL;
  g_user = NULL;
  g_frames = 0;
  g_inited = 0;
}

int ca_set_lecture_context(const char* text) {
  (void)text;
  return CA_ERR_NOT_IMPL;   /* M3：导入课件上下文 */
}

int ca_generate_note(const char* session_id) {
  (void)session_id;
  return CA_ERR_NOT_IMPL;   /* M4：分块摘要 + Markdown 导出 */
}

const char* ca_version(void) {
  return "ClassMate core 0.5.0-M2.8";
}
