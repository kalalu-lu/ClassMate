#include <string>
#include <vector>
#include <cstring>
#include <chrono>
#include <cmath>

#include <android/log.h>

#include "c-api.h"
#include "classmate.h"

/*
 * M2 流式识别引擎：sherpa-onnx OnlineRecognizer（Paraformer 中英双语 int8）
 * + Silero VAD 切句，包成 ca_asr_port。
 *
 * 工作方式：
 *  - 每 20 ms 一小批 PCM；VAD 判定是否在说话
 *  - 说话中（或还有未落定的文本）才送进 recognizer 解码，静音时省 CPU
 *  - 解析结果里的 text 作为「当前句到目前为止的完整假设」回给核心，
 *    核心发 CA_EV_ASR_PARTIAL，1 秒内即可上屏
 *  - 端点（VAD 静音 ≥0.4 s 或 recognizer 端点检测）触发时发 FINAL，并
 *    reset 流，下一句从零开始 —— 这就是「句级」的边界与时间戳来源
 *
 * 简体：Paraformer 的中文词表本身是简体，不需要后处理转换。
 */

#define LOG_TAG "ClassMate/ASR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

/* M2.6：断句只认「真实停顿」，去掉 M2 的 4.5 s 强制腰斩。
 * M2.9 修订：停顿判定改用「出字后连续无新字」计时（VAD speaking 在
 *      threshold 0.30 下几乎常开，不可靠）；护栏不再阻止收句，
 *      只在 core 侧（asr_chunker.c 严表）决定要不要与续句合并。
 *  - 0.7 s 无新字软收句，0.9 s 硬收句（无新字是解码间隔不是人的停顿，
 *    420 ms 实测过碎）
 *  - 9 s 仅作安全上限，正常一句话不会被切 */
constexpr int32_t kSilenceFinalizeMs = 700;
constexpr int32_t kSilenceHardMs     = 900;
constexpr int32_t kMaxUtteranceMs    = 9000;

struct SherpaEngine {
  const SherpaOnnxOnlineRecognizer* rec = nullptr;
  const SherpaOnnxOnlineStream*     stream = nullptr;
  const SherpaOnnxVoiceActivityDetector* vad = nullptr;
  std::vector<float> f32;
  bool     has_text = false;
  int32_t  silence_ms = 0;
  /* 出字后连续无新字计时（ms）：停顿判定主依据，取代 VAD speaking */
  int32_t  no_text_ms = 0;
  std::string last_text;
  int64_t  utt_frames = 0;
  int64_t  finals = 0;

  /* 热词缓冲必须在 recognizer 生命周期内一直有效（C API 不复制该指针） */
  std::string hotwords;

  /* M2 验收 G1（端到端出字 ≤1 s）的可观测性：本句已喂入的说话音频与首个出字延迟 */
  int64_t  utt_audio_ms = 0;
  bool     first_text_logged = false;
  std::chrono::steady_clock::time_point speech_t0;

  /* M2.8 排查（第1步）：VAD 起句闸统计，每 1 秒一条日志，只读不改行为 */
  int64_t  stat_blocks = 0;      /* 本统计窗总块数 */
  int64_t  stat_speech = 0;      /* 其中 VAD 判语音的块数 */
  double   stat_rms_sum = 0.0;   /* 块 RMS 之和（int16 域） */
  int      stat_peak = 0;        /* 本窗最大绝对值 */
  int64_t  stat_frames = 0;      /* 本窗帧数，到 16000（1 s）即打印并清零 */

  /* M2.8 第2步：滚动底噪估计（VAD 判负块 RMS 的 EMA），用于静音闸能量兜底 */
  double   noise_rms = 0.0;
  int64_t  noise_blocks = 0;
  int64_t  stat_fallback = 0;    /* 本统计窗能量兜底触发次数 */
};

void drop_vad_segments(const SherpaOnnxVoiceActivityDetector* vad) {
  /* VAD 的段落队列我们不用，但要定期清空，否则长时间上课会一直堆积 */
  while (SherpaOnnxVoiceActivityDetectorEmpty(vad) == 0) {
    const SherpaOnnxSpeechSegment* seg = SherpaOnnxVoiceActivityDetectorFront(vad);
    if (seg) SherpaOnnxDestroySpeechSegment(seg);
    SherpaOnnxVoiceActivityDetectorPop(vad);
  }
}

/* ---------------- M2.6 文本规范化：ITN + 课程热词 ---------------- */

static void utf8_split(const std::string& s, std::vector<std::string>& out) {
  out.clear();
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    size_t step = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    if (i + step > s.size()) step = 1;
    out.push_back(s.substr(i, step));
    i += step;
  }
}

static bool is_punct(const std::string& ch) {
  static const char* kPunct[] = {
    "，", "。", "？", "！", "、", "；", "：", "（", "）", "“", "”",
    ",", ".", "?", "!", ";", ":", "(", ")", "\"", " "
  };
  for (const char* p : kPunct) {
    if (ch == p) return true;
  }
  return false;
}

/* 全角 ASCII → 半角；中文与全角标点保持原样 */
static void to_halfwidth(std::string& s) {
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { ++i; continue; }
    size_t step = (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    if (c == 0xEF && i + 2 < s.size()) {
      unsigned char b1 = (unsigned char)s[i + 1];
      unsigned char b2 = (unsigned char)s[i + 2];
      /* U+FF01..U+FF5E 全角 ASCII */
      if (b1 >= 0xBC && b1 <= 0xBD) {
        uint32_t cp = 0xFF00u + (((uint32_t)(b1 & 0x3F)) << 6) +
                      ((uint32_t)(b2 & 0x3F));
        if (cp >= 0xFF01 && cp <= 0xFF5E) {
          char half = (char)(cp - 0xFF01 + 0x21);
          s.replace(i, 3, 1, half);
          ++i;
          continue;
        }
      }
    }
    if (c == 0xE3 && i + 2 < s.size() &&
        (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x80) {
      s.replace(i, 3, 1, ' ');  /* 全角空格 U+3000 */
      ++i;
      continue;
    }
    i += step;
  }
}

static int cn_digit(const std::string& ch) {
  if (ch == "零") return 0;
  if (ch == "一") return 1;
  if (ch == "二" || ch == "两") return 2;
  if (ch == "三") return 3;
  if (ch == "四") return 4;
  if (ch == "五") return 5;
  if (ch == "六") return 6;
  if (ch == "七") return 7;
  if (ch == "八") return 8;
  if (ch == "九") return 9;
  return -1;
}

static int cn_unit(const std::string& ch) {
  if (ch == "十") return 10;
  if (ch == "百") return 100;
  if (ch == "千") return 1000;
  return -1;
}

/* 第一百零二 → 102（仅用于「第X章/节/课」这类强结构，不做自由文本替换） */
static bool cn_number(const std::vector<std::string>& cps, size_t from,
                      size_t* to, int* value) {
  int section = 0, number = 0, saw = 0;
  size_t i = from;

  while (i < cps.size()) {
    int d = cn_digit(cps[i]);
    if (d >= 0) { number = d; saw = 1; ++i; continue; }
    int u = cn_unit(cps[i]);
    if (u > 0) {
      if (number == 0) number = 1;
      section += number * u;
      number = 0;
      saw = 1;
      ++i;
      continue;
    }
    break;
  }
  if (!saw) return false;
  *value = section + number;
  *to = i;
  return true;
}

static void normalize_numbers(std::string& s) {
  std::vector<std::string> cps;
  utf8_split(s, cps);

  for (size_t i = 0; i < cps.size(); ++i) {
    if (cps[i] != "第") continue;
    size_t end = 0;
    int value = 0;
    if (!cn_number(cps, i + 1, &end, &value)) continue;
    if (end >= cps.size()) continue;
    const std::string& suffix = cps[end];
    if (suffix != "章" && suffix != "节" && suffix != "课" && suffix != "页" &&
        suffix != "题" && suffix != "周" && suffix != "单元" &&
        suffix != "部分") {
      continue;
    }
    std::string head, tail;
    for (size_t k = 0; k < i + 1; ++k) head += cps[k];
    for (size_t k = end; k < cps.size(); ++k) tail += cps[k];
    s = head + std::to_string(value) + tail;
    break;  /* 一句里多次「第X」极少，替换一次后直接结束，避免索引漂移 */
  }

  /* 百分之X → X% */
  size_t pos = 0;
  while ((pos = s.find("百分之", pos)) != std::string::npos) {
    size_t p = pos + 9;
    std::vector<std::string> tail_cps;
    utf8_split(s.substr(p), tail_cps);
    size_t end = 0;
    int value = 0;
    if (cn_number(tail_cps, 0, &end, &value) && end > 0) {
      size_t cut = 0;
      for (size_t k = 0; k < end; ++k) cut += tail_cps[k].size();
      s.replace(pos, 9 + cut, std::to_string(value) + "%");
      pos += std::to_string(value).size() + 1;
    } else {
      pos += 9;
    }
  }
}

static void split_hotwords(const std::string& raw, std::vector<std::string>& out) {
  out.clear();
  std::string cur;
  std::vector<std::string> cps;
  utf8_split(raw, cps);
  for (const std::string& c : cps) {
    if (c == "," || c == ";" || c == " " || c == "\n" || c == "\r" ||
        c == "\t" || c == "，" || c == "；" || c == "、") {
      if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
}

static bool within_one_edit(const std::vector<std::string>& a,
                            const std::vector<std::string>& b) {
  if (a.size() > b.size() + 1 || b.size() > a.size() + 1) return false;
  size_t i = 0, j = 0, edits = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] == b[j]) { ++i; ++j; continue; }
    if (++edits > 1) return false;
    if (a.size() == b.size()) { ++i; ++j; }
    else if (a.size() > b.size()) { ++i; }
    else { ++j; }
  }
  if (i < a.size() || j < b.size()) ++edits;
  return edits <= 1;
}

/* 热词纠错：同长度窗口编辑距离 ≤1 且窗口内无标点才替换，避免把整句改坏 */
static void apply_hotwords(std::string& s, const std::vector<std::string>& hotwords) {
  if (hotwords.empty() || s.empty()) return;
  std::vector<std::string> cps;
  utf8_split(s, cps);

  for (const std::string& hw : hotwords) {
    std::vector<std::string> h;
    utf8_split(hw, h);
    if (h.size() < 2) continue;
    if (s.find(hw) != std::string::npos) continue;

    for (size_t i = 0; i + h.size() <= cps.size(); ++i) {
      std::vector<std::string> win(cps.begin() + i, cps.begin() + i + h.size());
      bool punct = false;
      for (const std::string& c : win) {
        if (is_punct(c)) { punct = true; break; }
      }
      if (punct || !within_one_edit(win, h)) continue;

      std::string rebuilt;
      for (size_t k = 0; k < i; ++k) rebuilt += cps[k];
      rebuilt += hw;
      for (size_t k = i + h.size(); k < cps.size(); ++k) rebuilt += cps[k];
      s = rebuilt;
      utf8_split(s, cps);
      break;
    }
  }
}

static void normalize_text(std::string& text, bool is_final,
                           const std::vector<std::string>& hotwords) {
  to_halfwidth(text);
  normalize_numbers(text);
  apply_hotwords(text, hotwords);

  if (!is_final || text.empty()) return;
  static const char* kEnds[] = {"。", "？", "！", "；", ".", "?", "!", ";"};
  bool has_end = false;
  for (const char* e : kEnds) {
    if (text.size() >= std::strlen(e) &&
        text.compare(text.size() - std::strlen(e), std::strlen(e), e) == 0) {
      has_end = true;
      break;
    }
  }
  if (!has_end) text += "。";
}

int reset_utterance(SherpaEngine* e) {
  SherpaOnnxOnlineStreamReset(e->rec, e->stream);
  if (e->vad) drop_vad_segments(e->vad);
  e->has_text = false;
  e->silence_ms = 0;
  e->no_text_ms = 0;
  e->last_text.clear();
  e->utt_frames = 0;
  e->utt_audio_ms = 0;
  e->first_text_logged = false;
  return CA_OK;
}

int sherpa_stream_feed(void* engine, const int16_t* pcm, int32_t frames,
                       int32_t sample_rate, char* out, int32_t out_cap,
                       int32_t* out_is_final) {
  if (!engine || !pcm || frames <= 0 || !out || out_cap <= 1 || !out_is_final) {
    return CA_ERR_INVALID;
  }
  auto* e = static_cast<SherpaEngine*>(engine);
  out[0] = '\0';
  *out_is_final = 0;

  if (sample_rate != 16000) return CA_ERR_INVALID;

  if (e->f32.size() < (size_t)frames) e->f32.resize((size_t)frames);
  for (int32_t i = 0; i < frames; ++i) e->f32[i] = (float)pcm[i] / 32768.0f;

  int32_t speaking = 1;
  if (e->vad) {
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(e->vad, e->f32.data(), frames);
    speaking = SherpaOnnxVoiceActivityDetectorDetected(e->vad);
  }

  /* M2.8 排查（第1步）：逐块统计，每 1 秒打印 speech/total、块 RMS 均值、峰值。
   * 只统计不改行为：VAD 闸、解码、收句逻辑一律不动。 */
  double block_rms = 0.0;  /* 本块 RMS（int16 域），静音闸能量兜底复用 */
  {
    double sum = 0.0;
    int peak = 0;
    for (int32_t i = 0; i < frames; ++i) {
      int v = pcm[i];
      sum += (double)v * (double)v;
      int a = v < 0 ? -v : v;
      if (a > peak) peak = a;
    }
    block_rms = std::sqrt(sum / (double)frames);
    e->stat_blocks++;
    if (speaking) e->stat_speech++;
    e->stat_rms_sum += block_rms;
    if (peak > e->stat_peak) e->stat_peak = peak;
    e->stat_frames += frames;
    if (e->stat_frames >= 16000) {
      LOGI("vad-stat: speech=%lld/%lld blocks, rms_avg=%.0f, peak=%d, vad=%s, fb=%lld",
           (long long)e->stat_speech, (long long)e->stat_blocks,
           e->stat_blocks > 0 ? e->stat_rms_sum / (double)e->stat_blocks : 0.0,
           e->stat_peak, e->vad ? "on" : "off", (long long)e->stat_fallback);
      e->stat_blocks = 0;
      e->stat_speech = 0;
      e->stat_rms_sum = 0.0;
      e->stat_peak = 0;
      e->stat_frames = 0;
      e->stat_fallback = 0;
    }
  }

  const int32_t chunk_ms = frames * 1000 / sample_rate;

  /* 静音且当前没有待落定的文本：直接跳过解码，省电省热。
   * M2.8 第2步能量兜底：VAD 判负且还没出字时，若该块能量高于本场滚动底噪
   * +6dB（振幅比 ×1.995），照样喂 recognizer —— 远场低电平起句靠它。
   * 底噪 = VAD 判负块 RMS 的指数滑动平均（α=0.05）；前 50 块（约 1 s）
   * 冷启动不兜底；触发兜底的 loud 块不进底噪估计，防持续语音推高底噪。 */
  if (!speaking && !e->has_text) {
    bool loud = e->noise_blocks >= 50 && block_rms > e->noise_rms * 1.995;
    if (!loud) {
      if (e->noise_blocks == 0) e->noise_rms = block_rms;
      else e->noise_rms = e->noise_rms * 0.95 + block_rms * 0.05;
      e->noise_blocks++;
      return CA_OK;
    }
    e->stat_fallback++;
  }

  if (!speaking) {
    e->silence_ms += chunk_ms;
  } else {
    e->silence_ms = 0;
    if (e->utt_audio_ms == 0) e->speech_t0 = std::chrono::steady_clock::now();
    e->utt_audio_ms += chunk_ms;
  }

  auto t0 = std::chrono::steady_clock::now();
  SherpaOnnxOnlineStreamAcceptWaveform(e->stream, sample_rate, e->f32.data(), frames);
  while (SherpaOnnxIsOnlineStreamReady(e->rec, e->stream)) {
    SherpaOnnxDecodeOnlineStream(e->rec, e->stream);
  }
  auto t1 = std::chrono::steady_clock::now();

  const SherpaOnnxOnlineRecognizerResult* r =
      SherpaOnnxGetOnlineStreamResult(e->rec, e->stream);
  std::string text;
  if (r && r->text) text = r->text;
  /* 去掉 sherpa 输出里偶发的首尾空格 */
  size_t b = text.find_first_not_of(" \t\r\n");
  size_t t = text.find_last_not_of(" \t\r\n");
  if (b != std::string::npos) text = text.substr(b, t - b + 1);

  if (!text.empty()) {
    e->has_text = true;
    e->utt_frames += frames;
    /* 停顿主依据：出字后连续无新字时长。文本有推进清零，否则累加本块时长 */
    if (text != e->last_text) { e->last_text = text; e->no_text_ms = 0; }
    else { e->no_text_ms += chunk_ms; }
    if (!e->first_text_logged && e->utt_audio_ms > 0) {
      e->first_text_logged = true;
      double lag = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - e->speech_t0).count();
      LOGI("first text: %.0fms after speech start (%lldms audio), decode %.1fms",
           lag, (long long)e->utt_audio_ms,
           std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
  } else {
    e->no_text_ms += chunk_ms;
  }
  if (r) SherpaOnnxDestroyOnlineRecognizerResult(r);

  /* M2.9 断句：无新字计时到点即收句；语义护栏移至 core 侧只管合并不管收句 */
  int32_t endpoint = SherpaOnnxOnlineStreamIsEndpoint(e->rec, e->stream);
  if (!endpoint && e->has_text) {
    bool hard = e->no_text_ms >= kSilenceHardMs;
    bool soft = e->no_text_ms >= kSilenceFinalizeMs;
    bool cap = e->utt_audio_ms >= kMaxUtteranceMs;
    if (hard || soft || cap) endpoint = 1;
  }

  if (!text.empty()) {
    static thread_local std::vector<std::string> hw_cache;
    static thread_local std::string hw_src;
    if (hw_src != e->hotwords) {
      split_hotwords(e->hotwords, hw_cache);
      hw_src = e->hotwords;
    }
    normalize_text(text, endpoint != 0, hw_cache);
    std::strncpy(out, text.c_str(), (size_t)out_cap - 1);
    out[out_cap - 1] = '\0';
  }

  if (endpoint && e->has_text) {
    *out_is_final = 1;
    e->finals++;
    LOGI("final #%lld: %lldms audio, %.1fms decode, no_text %dms",
         (long long)e->finals,
         (long long)(e->utt_frames * 1000 / sample_rate),
         std::chrono::duration<double, std::milli>(t1 - t0).count(),
         (int)e->no_text_ms);
    reset_utterance(e);
  }

  return CA_OK;
}

ca_asr_port g_port = { nullptr, sherpa_stream_feed };

}  // namespace

extern "C" {

/*
 * model_dir 下需要四个文件：
 *   encoder.int8.onnx / decoder.int8.onnx / tokens.txt / silero_vad.onnx
 * 缺 vad 时退化为「无 VAD，仅靠 recognizer 端点」。
 */
void* ca_sherpa_create(const char* model_dir, int32_t threads,
                       const char* hotwords) {
  if (!model_dir || !model_dir[0]) return nullptr;

  auto join = [&](const char* name) { return std::string(model_dir) + "/" + name; };
  std::string enc = join("encoder.int8.onnx");
  std::string dec = join("decoder.int8.onnx");
  std::string tok = join("tokens.txt");
  std::string vadp = join("silero_vad.onnx");

  SherpaOnnxOnlineRecognizerConfig cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.feat_config.sample_rate = 16000;
  cfg.feat_config.feature_dim = 80;
  cfg.model_config.transducer.encoder = nullptr;
  cfg.model_config.transducer.decoder = nullptr;
  cfg.model_config.transducer.joiner = nullptr;
  cfg.model_config.paraformer.encoder = enc.c_str();
  cfg.model_config.paraformer.decoder = dec.c_str();
  cfg.model_config.tokens = tok.c_str();
  cfg.model_config.num_threads = (threads > 0) ? threads : 2;
  cfg.model_config.provider = "cpu";
  cfg.model_config.debug = 0;
  cfg.model_config.model_type = "paraformer";
  cfg.decoding_method = "greedy_search";
  cfg.max_active_paths = 4;
  cfg.enable_endpoint = 1;
  /* rule3 关掉：M2 的 4.5 s 强制断句会把连续朗读的一句话从中间切开。
   * 现在只保留 rule1/rule2 的超长兜底（2.4 s / 1.2 s），
   * 真正的收句由 VAD 停顿 + 语义护栏决定（见 kSilenceFinalizeMs）。 */
  cfg.rule1_min_trailing_silence = 2.4f;
  cfg.rule2_min_trailing_silence = 1.2f;
  cfg.rule3_min_utterance_length = 0.0f;

  /* 课程热词：先原样交给 sherpa 做上下文增强，再在出字后做一次模糊兜底。
   * 字符串挂在 engine 上，保证 recognizer 存活期间 c_str() 一直有效。 */
  auto* e = new SherpaEngine();
  if (hotwords && hotwords[0]) e->hotwords = hotwords;
  if (!e->hotwords.empty()) {
    cfg.hotwords_buf = e->hotwords.c_str();
    cfg.hotwords_buf_size = (int32_t)e->hotwords.size();
    cfg.hotwords_score = 1.5f;
  }

  const SherpaOnnxOnlineRecognizer* rec = SherpaOnnxCreateOnlineRecognizer(&cfg);
  if (!rec) {
    LOGE("create online recognizer failed (check model files in %s)", model_dir);
    delete e;
    return nullptr;
  }

  e->rec = rec;
  e->stream = SherpaOnnxCreateOnlineStream(rec);

  /* VAD：可选的句末切分器；模型缺失不影响主链路 */
  SherpaOnnxVadModelConfig vcfg;
  std::memset(&vcfg, 0, sizeof(vcfg));
  vcfg.silero_vad.model = vadp.c_str();
  /* M2.8 第2步：threshold 0.5 → 0.30（远场低电平起句）；
   * min_speech_duration 0.1 → 0.28（压住短噪声误检） */
  vcfg.silero_vad.threshold = 0.30f;
  vcfg.silero_vad.min_silence_duration = 0.25f;
  vcfg.silero_vad.min_speech_duration = 0.28f;
  vcfg.silero_vad.max_speech_duration = 20.0f;
  vcfg.silero_vad.window_size = 512;
  vcfg.sample_rate = 16000;
  vcfg.num_threads = 1;
  vcfg.provider = "cpu";
  vcfg.debug = 0;
  e->vad = SherpaOnnxCreateVoiceActivityDetector(&vcfg, 30.0f);
  if (!e->vad) LOGI("vad model not found, fallback to endpoint-only");

  LOGI("sherpa paraformer ready: %s (threads=%d, vad=%s)",
       model_dir, cfg.model_config.num_threads, e->vad ? "on" : "off");
  return e;
}

void ca_sherpa_destroy(void* engine) {
  if (!engine) return;
  auto* e = static_cast<SherpaEngine*>(engine);
  if (e->vad) SherpaOnnxDestroyVoiceActivityDetector(e->vad);
  if (e->stream) SherpaOnnxDestroyOnlineStream(e->stream);
  if (e->rec) SherpaOnnxDestroyOnlineRecognizer(e->rec);
  delete e;
}

const ca_asr_port* ca_sherpa_port(void) {
  return &g_port;
}

}  // extern "C"
