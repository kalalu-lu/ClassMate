#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "classmate.h"

/*
 * M1 JNI 桥：在 M0（字符串/数组转换、句柄校验）之上新增两件事：
 *  1. ASR 工作线程 + 有界 PCM 队列（线程归 platform，core 保持无线程）
 *  2. 核心事件回调进 Kotlin（CA_EV_ASR_FINAL / CA_EV_ERROR）
 * 业务逻辑（窗口切分、事件拼装）仍全部在 core（纯 C）里。
 */

#define LOG_TAG "ClassMate/JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* asr_whisper.cpp 提供（M1 兜底） */
extern "C" void* ca_whisper_create(const char* model_path, int32_t threads);
extern "C" void  ca_whisper_destroy(void* engine);
extern "C" const ca_asr_port* ca_whisper_port(void);

/* asr_sherpa.cpp 提供（M2 主引擎） */
extern "C" void* ca_sherpa_create(const char* model_dir, int32_t threads,
                                  const char* hotwords);
extern "C" void  ca_sherpa_destroy(void* engine);
extern "C" const ca_asr_port* ca_sherpa_port(void);

/* asr_sensevoice.cpp 提供（M2.6 课后二次校正） */
extern "C" int ca_sensevoice_correct(const char* wav_path, const char* db_path,
                                     const char* model_dir,
                                     int32_t* out_total, int32_t* out_corrected);

/* llm_llama.cpp 提供（M2.7 本地 Qwen GGUF 后端） */
extern "C" void*       ca_llama_load(const char* model_path, int32_t ctx_len, int32_t n_threads);
extern "C" const char* ca_llama_generate(void* engine, const char* prompt);
extern "C" void        ca_llama_free(void* engine);

namespace {

bool fileExists(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

constexpr int32_t kSampleRate        = 16000;
constexpr int32_t kMaxQueuedSeconds  = 30;   // 队列上限：识别落后太多就丢最旧的
constexpr int32_t kMaxQueuedFrames   = kSampleRate * kMaxQueuedSeconds;

/* M2：会话数据库与 WAV 同目录，命名 classmate.db（Kotlin 无需新增参数） */
constexpr const char* kDbFileName = "/classmate.db";

std::string dbPathForWav(const std::string& wav_path) {
  size_t slash = wav_path.find_last_of("/\\");
  std::string dir = (slash == std::string::npos) ? std::string(".") : wav_path.substr(0, slash);
  return dir + kDbFileName;
}

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

JavaVM* g_vm = nullptr;

struct CaHandle {
  bool    valid = false;
  int64_t frames = 0;

  /* Kotlin 回调 */
  jobject   listener = nullptr;   // global ref
  jmethodID onEvent  = nullptr;

  /* ASR */
  void* asr_engine = nullptr;
  int32_t asr_kind = 0;      /* 0=无, 1=whisper 整窗, 2=sherpa 流式 */
  std::thread worker;
  std::mutex mtx;
  std::condition_variable cv;
  std::deque<std::vector<int16_t>> queue;
  int32_t queued_frames = 0;
  std::atomic<bool> stop{false};
  int64_t asr_ts_ms = 0;

  /* 可观测性（M1 复核要求③）：丢帧计数与识别耗时统计 */
  int64_t dropped_batches = 0;
  int64_t dropped_frames = 0;
  int64_t fed_frames = 0;

  /* M2 复核②：首帧 PCM 墙钟 → 首个 PARTIAL 回调的端到端计时 */
  int64_t first_frame_ms = 0;
  bool    first_partial_logged = false;
};

/* M0/M1 单实例。多会话时改为堆分配 context 指针。 */
CaHandle g_handle;

void on_core_event(const ca_event* ev, void* /*user*/) {
  if (!g_handle.valid || !g_handle.listener || !g_handle.onEvent) {
    if (ev->type == CA_EV_ERROR) {
      LOGE("core error code=%d text=%s", ev->code, ev->text ? ev->text : "");
    }
    return;
  }

  JNIEnv* env = nullptr;
  bool attached_here = false;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    attached_here = true;
  }

  jstring text = ev->text ? env->NewStringUTF(ev->text) : nullptr;

  /* M2 复核②：首帧 PCM → 首个 PARTIAL 回调的端到端计时（Kotlin 收到即 setText 上屏） */
  if (ev->type == CA_EV_ASR_PARTIAL && g_handle.first_frame_ms > 0 &&
      !g_handle.first_partial_logged) {
    g_handle.first_partial_logged = true;
    LOGI("first partial: %lldms since first pcm frame",
         (long long)(nowMs() - g_handle.first_frame_ms));
  }

  env->CallVoidMethod(g_handle.listener, g_handle.onEvent,
                      (jint)ev->type, (jlong)ev->ts_ms, text,
                      (jfloat)ev->score, (jint)ev->code);
  if (text) env->DeleteLocalRef(text);
  if (env->ExceptionCheck()) env->ExceptionClear();

  if (attached_here) g_vm->DetachCurrentThread();
}

/* ASR 工作线程：攒批喂给 core；停止时先把已取出的 pending 喂完再退出 */
void workerMain() {
  JNIEnv* env = nullptr;   // 常驻 attach，事件回调走快路径
  g_vm->AttachCurrentThread(&env, nullptr);

  std::vector<int16_t> pending;
  for (;;) {
    bool stopping = false;
    {
      std::unique_lock<std::mutex> lk(g_handle.mtx);
      g_handle.cv.wait(lk, [] {
        return g_handle.stop.load() || !g_handle.queue.empty();
      });
      while (!g_handle.queue.empty()) {
        auto& batch = g_handle.queue.front();
        pending.insert(pending.end(), batch.begin(), batch.end());
        g_handle.queued_frames -= (int32_t)batch.size();
        g_handle.queue.pop_front();
      }
      stopping = g_handle.stop.load() && g_handle.queue.empty();
    }

    if (!pending.empty()) {
      ca_asr_feed(pending.data(), (int32_t)pending.size(), g_handle.asr_ts_ms);
      g_handle.asr_ts_ms += (int64_t)pending.size() * 1000 / kSampleRate;
      pending.clear();
    }

    /* 修 M1 问题①：原来在 break 前丢掉刚取出的 pending，最后一两秒音频
     * 永远识别不到。现在保证 pending 先喂完，再补一次收尾。 */
    if (stopping) {
      ca_asr_drain();
      break;
    }
  }
  g_vm->DetachCurrentThread();
}

CaHandle* checked(jlong handle) {
  CaHandle* h = reinterpret_cast<CaHandle*>(handle);
  if (!h || h != &g_handle || !h->valid) return nullptr;
  return h;
}

/* M2.7 云端端口：云端 HTTP 在 Kotlin（HttpURLConnection + Keystore 解密），
 * 这里只把阻塞调用转成 ca_llm_port 的同步回调。超时/429/离线由 Kotlin 抛异常，
 * 这里清掉异常并返回失败，core 会整体降级抽取式。 */
struct CloudJsonCall {
  JavaVM* vm = nullptr;
  jobject obj = nullptr;            // global ref
  jmethodID generate = nullptr;
  int32_t calls = 0;                // 尝试次数（含失败）
  int32_t oks = 0;                  // 成功返回次数，用于区分真实 LLM / 降级
};

int cloud_generate(void* engine, const char* prompt,
                   ca_llm_on_token /*on_token*/, ca_llm_on_done on_done,
                   void* user) {
  CloudJsonCall* c = reinterpret_cast<CloudJsonCall*>(engine);
  JNIEnv* env = nullptr;
  bool attached = false;
  jstring jp = nullptr;
  int rc = CA_ERR_STATE;

  if (!c || !c->obj || !c->generate || !prompt) return CA_ERR_INVALID;
  c->calls++;
  if (c->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
    if (c->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return CA_ERR_STATE;
    attached = true;
  }
  jp = env->NewStringUTF(prompt);
  if (!jp) { if (attached) c->vm->DetachCurrentThread(); return CA_ERR_IO; }

  jobject result = env->CallObjectMethod(c->obj, c->generate, jp);
  env->DeleteLocalRef(jp);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    LOGE("cloud llm call threw (timeout/429/offline/key missing)");
    rc = CA_ERR_STATE;
  } else if (!result) {
    rc = CA_ERR_STATE;
  } else {
    jstring out = static_cast<jstring>(result);
    const char* text = env->GetStringUTFChars(out, nullptr);
    if (text) {
      on_done(CA_OK, text, -1, user);
      c->oks++;
      env->ReleaseStringUTFChars(out, text);
      rc = CA_OK;
    } else {
      rc = CA_ERR_IO;
    }
    env->DeleteLocalRef(result);
  }

  if (rc != CA_OK) on_done(rc, nullptr, 0, user);
  if (attached) c->vm->DetachCurrentThread();
  return rc;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  g_vm = vm;
  return JNI_VERSION_1_6;
}

JNIEXPORT jstring JNICALL
Java_com_classmate_app_ClassMateBridge_nativeVersion(JNIEnv* env, jobject) {
  return env->NewStringUTF(ca_version());
}

JNIEXPORT jlong JNICALL
Java_com_classmate_app_ClassMateBridge_nativeCreate(
    JNIEnv* env, jobject, jstring jWavPath, jstring jModelsDir,
    jint asrThreads, jboolean saveWav, jstring jHotwords, jobject jListener) {
  if (g_handle.valid) {
    LOGE("nativeCreate called twice");
    return 0;
  }
  if (!jWavPath) return 0;

  const char* path = env->GetStringUTFChars(jWavPath, nullptr);
  if (!path) return 0;

  /* M2：引擎选择优先级 —— sherpa 流式 Paraformer > whisper 整窗兜底 > 纯录音 */
  std::string models_dir;
  if (jModelsDir) {
    const char* mp = env->GetStringUTFChars(jModelsDir, nullptr);
    if (mp) { models_dir = mp; env->ReleaseStringUTFChars(jModelsDir, mp); }
  }

  void* engine = nullptr;
  const ca_asr_port* port = nullptr;
  int32_t engine_kind = 0;

  if (!models_dir.empty() && fileExists(models_dir + "/encoder.int8.onnx") &&
      fileExists(models_dir + "/decoder.int8.onnx") &&
      fileExists(models_dir + "/tokens.txt")) {
    std::string hotwords;
    if (jHotwords) {
      const char* hp = env->GetStringUTFChars(jHotwords, nullptr);
      if (hp) {
        hotwords = hp;
        env->ReleaseStringUTFChars(jHotwords, hp);
        LOGI("hotwords: %s", hotwords.c_str());
      }
    }
    engine = ca_sherpa_create(models_dir.c_str(), asrThreads,
                              hotwords.empty() ? nullptr : hotwords.c_str());
    if (engine) {
      port = ca_sherpa_port();
      engine_kind = 2;
      LOGI("engine = sherpa-onnx streaming paraformer");
    }
  }

  if (!engine && !models_dir.empty() &&
      fileExists(models_dir + "/ggml-base.bin")) {
    engine = ca_whisper_create((models_dir + "/ggml-base.bin").c_str(), asrThreads);
    if (engine) {
      port = ca_whisper_port();
      engine_kind = 1;
      LOGI("engine = whisper.cpp (fallback, windowed 4s)");
    }
  }

  if (!engine) LOGI("no asr model ready, record only");

  ca_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  std::string db_path = dbPathForWav(path);
  cfg.wav_path      = path;             /* ca_init 内部会复制，此处可以安全释放 */
  cfg.db_path       = db_path.c_str();  /* M2：句级转写落 SQLite（与 WAV 同目录） */
  cfg.started_at_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  cfg.asr_port      = port;
  cfg.asr_engine    = engine;
  cfg.asr_threads   = asrThreads;
  cfg.save_wav      = saveWav ? 1 : 0;   /* M2.5：关闭时 core 跳过 ca_wav_open/ca_wav_write */

  int rc = ca_init(&cfg, on_core_event, nullptr);
  env->ReleaseStringUTFChars(jWavPath, path);

  if (rc != CA_OK) {
    LOGE("ca_init failed rc=%d", rc);
    if (engine) { if (engine_kind == 2) ca_sherpa_destroy(engine); else ca_whisper_destroy(engine); }
    return 0;
  }

  g_handle.valid = true;
  g_handle.frames = 0;
  g_handle.asr_engine = engine;
  g_handle.asr_kind = engine_kind;
  g_handle.stop.store(false);
  g_handle.queued_frames = 0;
  g_handle.asr_ts_ms = 0;
  g_handle.dropped_batches = 0;
  g_handle.dropped_frames = 0;
  g_handle.fed_frames = 0;
  g_handle.first_frame_ms = 0;
  g_handle.first_partial_logged = false;

  if (jListener) {
    g_handle.listener = env->NewGlobalRef(jListener);
    jclass cls = env->GetObjectClass(jListener);
    g_handle.onEvent = env->GetMethodID(
        cls, "onEvent", "(IJLjava/lang/String;FI)V");
    env->DeleteLocalRef(cls);
    if (!g_handle.onEvent) LOGE("onEvent method not found on listener");
  }

  if (engine) {
    g_handle.worker = std::thread(workerMain);
  }

  LOGI("recording opened, core=%s, asr=%s, save_wav=%d, db=%s", ca_version(),
       engine_kind == 2 ? "sherpa-stream" : (engine_kind == 1 ? "whisper-window" : "off"),
       (int)cfg.save_wav, db_path.c_str());
  return reinterpret_cast<jlong>(&g_handle);
}

JNIEXPORT jint JNICALL
Java_com_classmate_app_ClassMateBridge_nativeFeed(
    JNIEnv* env, jobject, jlong handle, jshortArray pcm, jint frames, jlong tsMs) {
  CaHandle* h = checked(handle);
  if (!h) return CA_ERR_STATE;
  if (!pcm || frames <= 0) return CA_ERR_INVALID;
  if (env->GetArrayLength(pcm) < frames) return CA_ERR_INVALID;

  jshort* data = env->GetShortArrayElements(pcm, nullptr);
  if (!data) return CA_ERR_IO;

  /* 1) WAV 落盘（M0 行为，快速） */
  int rc = ca_feed_pcm(reinterpret_cast<const int16_t*>(data), frames, tsMs);

  /* 2) 拷贝一份进 ASR 队列（识别慢于实时就丢最旧的，绝不阻塞录音） */
  if (rc == CA_OK && h->asr_engine) {
    std::lock_guard<std::mutex> lk(h->mtx);
    h->queue.emplace_back(data, data + frames);
    h->queued_frames += frames;
    h->fed_frames += frames;
    while (h->queued_frames > kMaxQueuedFrames && !h->queue.empty()) {
      h->queued_frames -= (int32_t)h->queue.front().size();
      h->dropped_frames += (int64_t)h->queue.front().size();
      h->queue.pop_front();
      h->dropped_batches++;
      if (h->dropped_batches % 25 == 1) {   /* 每 25 批打一次，避免刷屏 */
        LOGE("asr slow: dropped %lld batches (%lld ms audio) so far",
             (long long)h->dropped_batches,
             (long long)(h->dropped_frames * 1000 / kSampleRate));
      }
    }
    h->cv.notify_one();
  }

  env->ReleaseShortArrayElements(pcm, data, JNI_ABORT);  /* 只读，不回写 */

  if (rc == CA_OK) {
    if (h->frames == 0) h->first_frame_ms = nowMs();   /* 复核②：本场首帧 PCM 时刻 */
    h->frames += frames;
  }
  return rc;
}

JNIEXPORT jint JNICALL
Java_com_classmate_app_ClassMateBridge_nativeFlush(JNIEnv*, jobject, jlong handle) {
  CaHandle* h = checked(handle);
  if (!h) return CA_ERR_STATE;
  return ca_flush();
}

JNIEXPORT void JNICALL
Java_com_classmate_app_ClassMateBridge_nativeDestroy(JNIEnv* env, jobject, jlong handle) {
  CaHandle* h = checked(handle);
  if (!h) return;

  /* 先停工作线程并等它退出，保证销毁期间不再有回调进来 */
  if (h->worker.joinable()) {
    {
      std::lock_guard<std::mutex> lk(h->mtx);
      h->stop.store(true);
    }
    h->cv.notify_all();
    h->worker.join();
  }

  void* engine = h->asr_engine;
  int32_t kind = h->asr_kind;
  h->asr_engine = nullptr;
  h->asr_kind = 0;

  /* 先关核心：drain 出的最后一句会同步走到 listener（Kotlin 侧还在订阅），
   * 同时写入 SQLite、flush WAV。之后再释放 listener 与引擎。 */
  ca_shutdown();

  if (h->listener) {
    env->DeleteGlobalRef(h->listener);
    h->listener = nullptr;
    h->onEvent = nullptr;
  }

  if (engine) {
    if (kind == 2) ca_sherpa_destroy(engine);
    else           ca_whisper_destroy(engine);
  }

  LOGI("recording closed: wav=%lld frames, asr_fed=%lld frames, dropped=%lld batches/%lld ms",
       (long long)h->frames, (long long)h->fed_frames,
       (long long)h->dropped_batches,
       (long long)(h->dropped_frames * 1000 / kSampleRate));
  h->dropped_batches = 0;
  h->dropped_frames = 0;
  h->fed_frames = 0;
  h->valid = false;
  h->frames = 0;
}

/* M2.6：课后二次校正。返回 0=成功；1=模型缺失；2=WAV/库不可用；-1=失败。
 * 调用时机：录音服务已销毁、临时 WAV 落盘完成之后（Kotlin 后台线程）。 */
JNIEXPORT jint JNICALL
Java_com_classmate_app_ClassMateBridge_nativeCorrect(
    JNIEnv* env, jobject, jstring jWav, jstring jDb, jstring jModelDir) {
  std::string wav, db, model;
  if (jWav) {
    const char* p = env->GetStringUTFChars(jWav, nullptr);
    if (p) { wav = p; env->ReleaseStringUTFChars(jWav, p); }
  }
  if (jDb) {
    const char* p = env->GetStringUTFChars(jDb, nullptr);
    if (p) { db = p; env->ReleaseStringUTFChars(jDb, p); }
  }
  if (jModelDir) {
    const char* p = env->GetStringUTFChars(jModelDir, nullptr);
    if (p) { model = p; env->ReleaseStringUTFChars(jModelDir, p); }
  }
  if (wav.empty() || db.empty()) return 2;
  int32_t total = 0, corrected = 0;
  int rc = ca_sensevoice_correct(wav.c_str(), db.c_str(),
                                 model.empty() ? nullptr : model.c_str(),
                                 &total, &corrected);
  LOGI("nativeCorrect rc=%d corrected=%d/%d", rc, corrected, total);
  return rc;
}

/* M4/M2.7：只依赖 segments 文本生成 chapters + 笔记文件，任何音频档位都可调用。
 * 返回会话 id（-1 表示失败/无会话）。
 *
 * jCloud 非空且 allowLlm=true 时，核心通过 ca_llm_port 回调 Kotlin
 * CloudLlm.generate(prompt)：配置选择（云端/本地/自动）、Key 解密、授权都在
 * Kotlin 侧，core 只拿到「能调用/不能调用」的端口，业务代码与模型无关。 */

JNIEXPORT jint JNICALL
Java_com_classmate_app_ClassMateBridge_nativeEnsureSchema(
    JNIEnv* env, jobject, jstring jDb) {
  std::string db;
  if (jDb) {
    const char* p = env->GetStringUTFChars(jDb, nullptr);
    if (p) { db = p; env->ReleaseStringUTFChars(jDb, p); }
  }
  if (db.empty()) return CA_ERR_INVALID;
  return (jint)ca_note_ensure_schema(db.c_str());
}

JNIEXPORT jlong JNICALL
Java_com_classmate_app_ClassMateBridge_nativeBuildNote(
    JNIEnv* env, jobject, jstring jDb, jstring jNotePath,
    jstring jConfigPath, jstring jBackend, jstring jTokenizerPath,
    jstring jPromptDir, jint targetCtx, jstring jModelId, jstring jPromptVer,
    jboolean allowLlm, jobject jCloud) {
  std::string db, note, config, backend, tokenizer, promptDir, modelId, promptVer;
  auto read = [&](jstring s, std::string& out) {
    if (!s) return;
    const char* p = env->GetStringUTFChars(s, nullptr);
    if (p) { out.assign(p); env->ReleaseStringUTFChars(s, p); }
  };
  read(jDb, db);
  read(jNotePath, note);
  read(jConfigPath, config);
  read(jBackend, backend);
  read(jTokenizerPath, tokenizer);
  read(jPromptDir, promptDir);
  read(jModelId, modelId);
  read(jPromptVer, promptVer);
  if (db.empty()) return -1;

  ca_llm_port port;
  std::memset(&port, 0, sizeof(port));
  CloudJsonCall call{};
  bool useCloud = (jCloud != nullptr) && allowLlm;

  if (useCloud) {
    call.vm = g_vm;
    call.obj = env->NewGlobalRef(jCloud);
    jclass cls = env->GetObjectClass(jCloud);
    call.generate = env->GetMethodID(cls, "generate", "(Ljava/lang/String;)Ljava/lang/String;");
    env->DeleteLocalRef(cls);
    if (!call.generate || !call.obj) {
      LOGE("CloudLlm.generate(String) not found");
      useCloud = false;
      if (call.obj) { env->DeleteGlobalRef(call.obj); call.obj = nullptr; }
    } else {
      port.generate = cloud_generate;
      port.ctx_len = (int32_t)targetCtx;
      port.model_id = modelId.empty() ? nullptr : modelId.c_str();
      port.prompt_ver = promptVer.empty() ? nullptr : promptVer.c_str();
    }
  }

  ca_note_options opt;
  std::memset(&opt, 0, sizeof(opt));
  opt.config_path = config.empty() ? nullptr : config.c_str();
  opt.backend = backend.empty() ? nullptr : backend.c_str();
  opt.tokenizer_path = tokenizer.empty() ? nullptr : tokenizer.c_str();
  opt.prompt_dir = promptDir.empty() ? nullptr : promptDir.c_str();
  opt.model_id = modelId.empty() ? nullptr : modelId.c_str();
  opt.prompt_ver = promptVer.empty() ? nullptr : promptVer.c_str();
  opt.target_ctx = (int32_t)targetCtx;
  opt.allow_llm = (allowLlm && useCloud) ? 1 : 0;

  jlong id = (jlong)ca_note_build_ex(db.c_str(),
                                     note.empty() ? nullptr : note.c_str(),
                                     &opt,
                                     useCloud ? &port : nullptr,
                                     useCloud ? &call : nullptr);
  if (call.obj) env->DeleteGlobalRef(call.obj);
  /* 按实际后端打日志：
   *  - 未启用 LLM → extractive
   *  - 启用且有成功调用 → backend（local / cloud）
   *  - 启用但全部失败（core 已降级）→ extractive(fallback)
   * 这样本地档成功后日志明确是 local，不会误标成 cloud。 */
  const char* mode;
  if (!useCloud) {
    mode = "extractive";
  } else if (call.oks > 0) {
    mode = backend.empty() ? "cloud" : backend.c_str();
  } else {
    mode = "extractive(fallback)";
  }
  LOGI("nativeBuildNote session=%lld llm=%s calls=%d ok=%d model=%s ctx=%d note=%s",
       (long long)id, mode, call.calls, call.oks,
       modelId.empty() ? "-" : modelId.c_str(), (int)targetCtx,
       note.empty() ? "(none)" : note.c_str());
  return id;
}

/* M2.7 本地 LLM（llama.cpp GGUF）JNI 方法。
 * 生命周期由 Kotlin LocalLlm 持有：load → 多次 generate → free。
 * generate 返回的字符串由 ca_llama_generate 在堆上分配，这里拷贝后立即释放。 */

JNIEXPORT jlong JNICALL
Java_com_classmate_app_ClassMateBridge_nativeLlmLoad(
    JNIEnv* env, jobject, jstring jModelPath, jint ctxLen, jint nThreads) {
  if (!jModelPath) return 0;
  const char* p = env->GetStringUTFChars(jModelPath, nullptr);
  if (!p) return 0;
  void* eng = ca_llama_load(p, (int32_t)ctxLen, (int32_t)nThreads);
  env->ReleaseStringUTFChars(jModelPath, p);
  return reinterpret_cast<jlong>(eng);
}

JNIEXPORT jstring JNICALL
Java_com_classmate_app_ClassMateBridge_nativeLlmGenerate(
    JNIEnv* env, jobject, jlong handle, jstring jPrompt) {
  void* eng = reinterpret_cast<void*>(handle);
  if (!eng || !jPrompt) return nullptr;
  const char* p = env->GetStringUTFChars(jPrompt, nullptr);
  if (!p) return nullptr;
  const char* out = ca_llama_generate(eng, p);
  env->ReleaseStringUTFChars(jPrompt, p);
  if (!out) return nullptr;
  jstring result = env->NewStringUTF(out);
  std::free((void*)out);   /* ca_llama_generate 用 malloc 分配 */
  return result;
}

JNIEXPORT void JNICALL
Java_com_classmate_app_ClassMateBridge_nativeLlmFree(JNIEnv*, jobject, jlong handle) {
  void* eng = reinterpret_cast<void*>(handle);
  if (eng) ca_llama_free(eng);
}

}  // extern "C"
