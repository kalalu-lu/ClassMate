#include <vector>
#include <string>
#include <cstring>
#include <chrono>

#include <android/log.h>

#include "whisper.h"
#include "classmate.h"

/*
 * M1 whisper.cpp 引擎实现：把 vendored 的 whisper.cpp 包成 ca_asr_port。
 * 线程安全约定：一个引擎实例只被一条 ASR 工作线程调用（jni_bridge 保证）。
 */

#define LOG_TAG "ClassMate/ASR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

struct WhisperEngine {
  whisper_context* ctx = nullptr;
  int32_t threads = 4;
  std::vector<float> f32;   // 复用的 int16->float 转换缓冲
};

int whisper_transcribe(void* engine, const int16_t* pcm, int32_t frames,
                       int32_t sample_rate, char* out, int32_t out_cap) {
  if (!engine || !pcm || frames <= 0 || !out || out_cap <= 1) return CA_ERR_INVALID;
  if (sample_rate != 16000) return CA_ERR_INVALID;

  auto* e = static_cast<WhisperEngine*>(engine);
  out[0] = '\0';

  // int16 -> float [-1, 1]
  if (e->f32.size() < (size_t)frames) e->f32.resize((size_t)frames);
  for (int32_t i = 0; i < frames; ++i) {
    e->f32[i] = (float)pcm[i] / 32768.0f;
  }

  whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.language        = "zh";          // 课堂场景锁定中文，省自动检测时间
  params.translate       = false;
  params.n_threads       = e->threads;
  params.no_timestamps   = true;          // 窗口级时间戳由核心 chunker 给
  params.single_segment  = true;          // 整窗一段文本，M1 不做段内对齐
  params.print_progress  = false;
  params.print_realtime  = false;
  params.print_special   = false;
  params.print_timestamps = false;
  /* 修 M1 遗留问题①：whisper 无引导时会简繁混杂（如把「再见」写成「再見」）。
   * initial_prompt 强制简体书面语，实测能把繁输出压到接近零。 */
  params.initial_prompt  = "以下是普通话课堂的句子，请使用简体中文输出。";

  auto t0 = std::chrono::steady_clock::now();
  if (whisper_full(e->ctx, params, e->f32.data(), frames) != 0) {
    LOGE("whisper_full failed");
    return CA_ERR_IO;
  }
  auto t1 = std::chrono::steady_clock::now();
  double cost_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  double audio_ms = frames * 1000.0 / sample_rate;
  /* 可观测性（复核要求③）：每个整窗的耗时与实时倍率 */
  LOGI("window %.0fms audio -> %.0fms infer (%.2fx realtime)",
       audio_ms, cost_ms, cost_ms / audio_ms);

  const int nseg = whisper_full_n_segments(e->ctx);
  std::string text;
  for (int i = 0; i < nseg; ++i) {
    const char* seg = whisper_full_get_segment_text(e->ctx, i);
    if (seg) text += seg;
  }
  // 去掉 whisper 输出常见的首尾空格
  size_t b = text.find_first_not_of(" \t\r\n");
  size_t t = text.find_last_not_of(" \t\r\n");
  std::string trimmed = (b == std::string::npos) ? "" : text.substr(b, t - b + 1);

  // 过滤纯标点/语气噪声窗（whisper 对静音常输出 "。" 或 "[BLANK_AUDIO]"）
  if (trimmed.empty() || trimmed == "[BLANK_AUDIO]" ||
      trimmed.find_first_not_of(",.!?;:'-") == std::string::npos) {
    return CA_OK;   // out 保持空串，核心会跳过该窗
  }

  std::strncpy(out, trimmed.c_str(), (size_t)out_cap - 1);
  out[out_cap - 1] = '\0';
  return CA_OK;
}

ca_asr_port g_port = { whisper_transcribe };

}  // namespace

extern "C" {

void* ca_whisper_create(const char* model_path, int32_t threads) {
  if (!model_path || !model_path[0]) return nullptr;

  whisper_context_params cparams = whisper_context_default_params();
  cparams.use_gpu = false;   // M1 纯 CPU；GGML 的 GPU 后端留给 M5 调优

  whisper_context* ctx = whisper_init_from_file_with_params(model_path, cparams);
  if (!ctx) {
    LOGE("whisper init failed: %s", model_path);
    return nullptr;
  }

  auto* e = new WhisperEngine();
  e->ctx = ctx;
  e->threads = (threads > 0) ? threads : 4;
  LOGI("whisper ready: %s, threads=%d", model_path, e->threads);
  return e;
}

void ca_whisper_destroy(void* engine) {
  if (!engine) return;
  auto* e = static_cast<WhisperEngine*>(engine);
  if (e->ctx) whisper_free(e->ctx);
  delete e;
}

const ca_asr_port* ca_whisper_port(void) {
  return &g_port;
}

}  // extern "C"
