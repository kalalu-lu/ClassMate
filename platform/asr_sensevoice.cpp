#include "classmate.h"
#include "c-api.h"

#include <sqlite3.h>

#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#define LOG_TAG "ClassMate/SenseVoice"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/*
 * M2.6 课后二次校正：拿临时 WAV 按 segments 的相对时间切片，逐句送 SenseVoice。
 * 结果写回 segments.text_corrected，原始 text 保留用于对照。
 *
 * 返回值（Kotlin 侧据此决定是否删除临时 WAV）：
 *   0  校正成功（或没有需要校正的句子）
 *   1  校正模型缺失 —— 降级为原文字幕，保留 WAV 以便重试
 *   2  WAV 不可用 / 库不可用 —— 同样保留
 *  -1  模型加载或解码失败 —— 保留
 */

namespace {

constexpr int32_t kSampleRate = 16000;
constexpr int32_t kBytesPerMs = kSampleRate * 2 / 1000;  /* 16-bit mono */
constexpr int32_t kMaxSegmentMs = 30000;

struct WavInfo {
  int32_t sample_rate = 0;
  int32_t channels = 0;
  int32_t bits = 0;
  int64_t data_offset = 0;
  int64_t data_bytes = 0;
};

bool file_exists(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

bool parse_wav(FILE* f, WavInfo* info) {
  unsigned char riff[12];
  if (fread(riff, 1, 12, f) != 12) return false;
  if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
    return false;
  }

  while (true) {
    unsigned char hdr[8];
    if (fread(hdr, 1, 8, f) != 8) return false;
    uint32_t size = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                    ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    int64_t next = (int64_t)ftell(f) + (int64_t)size + (size & 1);

    if (std::memcmp(hdr, "fmt ", 4) == 0) {
      unsigned char fmt[16];
      if (size < 16 || fread(fmt, 1, 16, f) != 16) return false;
      info->channels = (int32_t)(fmt[2] | (fmt[3] << 8));
      uint32_t rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                      ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      info->sample_rate = (int32_t)rate;
      info->bits = (int32_t)(fmt[14] | (fmt[15] << 8));
      if (fseek(f, (long)next, SEEK_SET) != 0) return false;
      continue;
    }
    if (std::memcmp(hdr, "data", 4) == 0) {
      info->data_offset = (int64_t)ftell(f);
      info->data_bytes = (int64_t)size;
      return true;
    }
    if (fseek(f, (long)next, SEEK_SET) != 0) return false;
  }
}

struct SegRow {
  int64_t id;
  int64_t start_ms;
  int64_t end_ms;
};

int64_t latest_session(sqlite3* db) {
  sqlite3_stmt* st = nullptr;
  int64_t id = -1;

  if (sqlite3_prepare_v2(db,
      "SELECT id FROM sessions WHERE status='done'"
      " ORDER BY ended_at DESC, id DESC LIMIT 1", -1, &st, NULL) != SQLITE_OK) {
    return -1;
  }
  if (sqlite3_step(st) == SQLITE_ROW) id = (int64_t)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return id;
}

int load_segments(sqlite3* db, int64_t session_id, std::vector<SegRow>* out) {
  sqlite3_stmt* st = nullptr;

  if (sqlite3_prepare_v2(db,
      "SELECT id, start_ms, end_ms FROM segments WHERE session_id=?"
      " ORDER BY start_ms, id", -1, &st, NULL) != SQLITE_OK) {
    return -1;
  }
  sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    SegRow row;
    row.id = (int64_t)sqlite3_column_int64(st, 0);
    row.start_ms = (int64_t)sqlite3_column_int64(st, 1);
    row.end_ms = (int64_t)sqlite3_column_int64(st, 2);
    out->push_back(row);
  }
  sqlite3_finalize(st);
  return 0;
}

bool read_slice(FILE* f, const WavInfo& wav, int64_t start_ms, int64_t end_ms,
                std::vector<float>* out) {
  int64_t offset = wav.data_offset + start_ms * kBytesPerMs;
  int64_t frames = (end_ms - start_ms) * kSampleRate / 1000;

  if (frames <= 0) return false;
  if (frames > kMaxSegmentMs * kSampleRate / 1000) {
    frames = kMaxSegmentMs * kSampleRate / 1000;
  }
  int64_t max_frames = wav.data_bytes / 2;
  int64_t start_frame = start_ms * kSampleRate / 1000;
  if (start_frame >= max_frames) return false;
  if (start_frame + frames > max_frames) frames = max_frames - start_frame;

  if (fseek(f, (long)offset, SEEK_SET) != 0) return false;
  std::vector<int16_t> pcm((size_t)frames);
  size_t got = fread(pcm.data(), sizeof(int16_t), (size_t)frames, f);
  if (got < (size_t)frames) frames = (int64_t)got;
  if (frames <= 0) return false;

  out->resize((size_t)frames);
  for (int64_t i = 0; i < frames; ++i) {
    out->at((size_t)i) = (float)pcm[(size_t)i] / 32768.0f;
  }
  return true;
}

}  // namespace

extern "C" {

int ca_sensevoice_correct(const char* wav_path, const char* db_path,
                          const char* model_dir,
                          int32_t* out_total, int32_t* out_corrected) {
  std::string model = std::string(model_dir ? model_dir : "") + "/model.int8.onnx";
  std::string tokens = std::string(model_dir ? model_dir : "") + "/tokens.txt";
  int total = 0, corrected = 0;

  if (out_total) *out_total = 0;
  if (out_corrected) *out_corrected = 0;

  if (!model_dir || !file_exists(model) || !file_exists(tokens)) {
    LOGI("correction model missing under %s", model_dir ? model_dir : "(null)");
    return 1;
  }
  if (!wav_path || !file_exists(wav_path) || !db_path) return 2;

  FILE* f = fopen(wav_path, "rb");
  if (!f) return 2;
  WavInfo wav;
  if (!parse_wav(f, &wav) || wav.sample_rate != kSampleRate ||
      wav.channels != 1 || wav.bits != 16) {
    LOGE("unsupported wav: rate=%d ch=%d bits=%d", wav.sample_rate, wav.channels,
         wav.bits);
    fclose(f);
    return 2;
  }

  sqlite3* db = nullptr;
  std::vector<SegRow> segs;
  int64_t session_id = -1;
  if (sqlite3_open(db_path, &db) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    fclose(f);
    return 2;
  }
  session_id = latest_session(db);
  if (session_id < 0 || load_segments(db, session_id, &segs) != 0 || segs.empty()) {
    sqlite3_close(db);
    fclose(f);
    return 0;  /* 没有句子可校正，视为成功 */
  }

  SherpaOnnxOfflineRecognizerConfig cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.feat_config.sample_rate = 16000;
  cfg.feat_config.feature_dim = 80;
  cfg.model_config.sense_voice.model = model.c_str();
  cfg.model_config.sense_voice.language = "auto";
  cfg.model_config.sense_voice.use_itn = 1;
  cfg.model_config.tokens = tokens.c_str();
  cfg.model_config.num_threads = 2;
  cfg.model_config.provider = "cpu";
  cfg.model_config.model_type = "sense_voice";
  cfg.decoding_method = "greedy_search";

  const SherpaOnnxOfflineRecognizer* rec = SherpaOnnxCreateOfflineRecognizer(&cfg);
  if (!rec) {
    LOGE("create offline recognizer failed: %s", model.c_str());
    sqlite3_close(db);
    fclose(f);
    return -1;
  }

  sqlite3_stmt* update = nullptr;
  if (sqlite3_prepare_v2(db, "UPDATE segments SET text_corrected=? WHERE id=?",
                         -1, &update, NULL) != SQLITE_OK) {
    update = nullptr;
  }

  std::vector<float> samples;
  for (const SegRow& row : segs) {
    total++;
    if (!read_slice(f, wav, row.start_ms, row.end_ms, &samples)) continue;

    const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(rec);
    if (!stream) continue;
    SherpaOnnxAcceptWaveformOffline(stream, kSampleRate, samples.data(),
                                    (int32_t)samples.size());
    SherpaOnnxDecodeOfflineStream(rec, stream);
    const SherpaOnnxOfflineRecognizerResult* r =
        SherpaOnnxGetOfflineStreamResult(stream);
    std::string text;
    if (r && r->text) text = r->text;
    if (r) SherpaOnnxDestroyOfflineRecognizerResult(r);
    SherpaOnnxDestroyOfflineStream(stream);

    if (text.empty() || !update) continue;
    sqlite3_bind_text(update, 1, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update, 2, (sqlite3_int64)row.id);
    if (sqlite3_step(update) == SQLITE_DONE) {
      corrected++;
    }
    sqlite3_reset(update);
    sqlite3_clear_bindings(update);
  }

  if (update) sqlite3_finalize(update);
  SherpaOnnxDestroyOfflineRecognizer(rec);
  sqlite3_close(db);
  fclose(f);

  if (out_total) *out_total = total;
  if (out_corrected) *out_corrected = corrected;
  LOGI("correction done: %d/%d segments (session %lld)", corrected, total,
       (long long)session_id);
  return 0;
}

}  // extern "C"
