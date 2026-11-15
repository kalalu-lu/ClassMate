/*
 * M2.7 本地 LLM 后端：llama.cpp C API 封装。
 *
 * 设计目标：
 *  - 只暴露三个 C 函数：ca_llama_load / ca_llama_generate / ca_llama_free
 *  - ctx=2048、n_threads=4（由调用方传入，这里不硬编码，便于日后调）
 *  - Qwen2.5 chat template：<|im_start|>user\n{prompt}<|im_end|>\n<|im_start|>assistant\n
 *  - 采样：temp=0.3 / top_p=0.9 / top_k=40，适合概括压缩
 *  - stop tokens：<|im_end|>(151645)、<|endoftext|>(151643)
 *  - 用完即释放：ca_llama_free 释放 model + context + sampler
 *
 * 线程模型：单次 load → 多次 generate → free，全在调用线程串行执行，
 * 不持有任何全局状态（llama_backend_init 全局只调一次，幂等）。
 */

#include "llama.h"

#if defined(__ANDROID__)
#include <android/log.h>
#define LOG_TAG "ClassMate/LLM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
/* 桌面单测（platform/tests/test_llm_repeat.cpp）复用同一份实现，直接打到 stderr */
#define LOGI(...) do { fprintf(stderr, "[LLM] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define LOGE(...) do { fprintf(stderr, "[LLM][E] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {

struct LlamaEngine {
  llama_model*   model = nullptr;
  llama_context* ctx = nullptr;
  llama_sampler* smpl = nullptr;
  const llama_vocab* vocab = nullptr;
  int32_t        n_ctx = 0;
  int32_t        n_threads = 0;
  bool           backend_inited = false;
};

/* Qwen2.5 特殊 token id（与 tokenizer.json 中的 added_tokens 一致） */
static constexpr llama_token kTokenImStart = 151644;  // <|im_start|>
static constexpr llama_token kTokenImEnd   = 151645;  // <|im_end|>
static constexpr llama_token kTokenEot     = 151643;  // <|endoftext|>

static std::atomic<bool> g_backend_inited{false};

void* ca_llama_load(const char* model_path, int32_t ctx_len, int32_t n_threads) {
  if (!model_path || !*model_path) {
    LOGE("ca_llama_load: empty model path");
    return nullptr;
  }

  bool expected = false;
  if (g_backend_inited.compare_exchange_strong(expected, true)) {
    llama_backend_init();  /* 全局只初始化一次 */
  }

  auto* eng = new (std::nothrow) LlamaEngine();
  if (!eng) return nullptr;
  eng->backend_inited = true;

  llama_model_params mparams = llama_model_default_params();
  mparams.n_gpu_layers = 0;                        /* Android：纯 CPU，不依赖 GPU 后端 */
  mparams.load_mode    = LLAMA_LOAD_MODE_MMAP;     /* mmap 降低常驻内存峰值 */

  eng->model = llama_model_load_from_file(model_path, mparams);
  if (!eng->model) {
    LOGE("ca_llama_load: failed to load model %s", model_path);
    delete eng;
    return nullptr;
  }
  eng->vocab = llama_model_get_vocab(eng->model);

  llama_context_params cparams = llama_context_default_params();
  cparams.n_ctx         = (uint32_t)ctx_len;
  cparams.n_batch       = (uint32_t)ctx_len;  /* 整段 prompt 一次喂入 */
  cparams.n_threads     = n_threads;
  cparams.n_threads_batch = n_threads;

  eng->ctx = llama_init_from_model(eng->model, cparams);
  if (!eng->ctx) {
    LOGE("ca_llama_load: llama_init_from_model failed (ctx=%d)", ctx_len);
    llama_model_free(eng->model);
    delete eng;
    return nullptr;
  }
  eng->n_ctx = (int32_t)llama_n_ctx(eng->ctx);
  eng->n_threads = n_threads;

  /* 采样链：top_k(40) → top_p(0.9) → temp(0.3) → dist */
  llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
  eng->smpl = llama_sampler_chain_init(sparams);
  llama_sampler_chain_add(eng->smpl, llama_sampler_init_top_k(40));
  llama_sampler_chain_add(eng->smpl, llama_sampler_init_top_p(0.9f, 1));
  llama_sampler_chain_add(eng->smpl, llama_sampler_init_temp(0.3f));
  llama_sampler_chain_add(eng->smpl, llama_sampler_init_dist(0x1234abcd));

  LOGI("ca_llama_load: ok model=%s ctx=%d threads=%d vocab=%d",
       model_path, eng->n_ctx, n_threads,
       eng->vocab ? llama_vocab_n_tokens(eng->vocab) : -1);
  return eng;
}

static std::string buildChatPrompt(const char* user_prompt) {
  std::string p;
  p.reserve(std::strlen(user_prompt) + 64);
  p += "<|im_start|>user\n";
  p += user_prompt;
  p += "<|im_end|>\n<|im_start|>assistant\n";
  return p;
}

const char* ca_llama_generate(void* engine, const char* prompt) {
  auto* eng = static_cast<LlamaEngine*>(engine);
  if (!eng || !eng->ctx || !prompt) {
    LOGE("ca_llama_generate: invalid engine or prompt");
    return nullptr;
  }
  const auto t_start = std::chrono::steady_clock::now();

  /* 0. 每次 generate 前清空 KV 内存并重置采样器。
   * 一次笔记会多次调用 generate：若不清空，上一轮的 prompt 与生成结果会残留在
   * KV cache 中污染下一轮，且位置持续累加直到超出 ctx（第二次起 decode 失败）。 */
  llama_memory_t mem = llama_get_memory(eng->ctx);
  if (mem) llama_memory_clear(mem, /*data=*/true);
  if (eng->smpl) llama_sampler_reset(eng->smpl);

  /* 1. 拼装 chat template 并 tokenize */
  const std::string chat = buildChatPrompt(prompt);
  std::vector<llama_token> tokens(chat.size() + 16);
  int32_t n_tok = llama_tokenize(
      eng->vocab, chat.c_str(), (int32_t)chat.size(),
      tokens.data(), (int32_t)tokens.size(),
      /*add_special=*/false, /*parse_special=*/true);
  if (n_tok < 0) {
    /* 缓冲区不足：按实际需要重分配 */
    tokens.resize(-n_tok + 8);
    n_tok = llama_tokenize(
        eng->vocab, chat.c_str(), (int32_t)chat.size(),
        tokens.data(), (int32_t)tokens.size(),
        false, true);
  }
  if (n_tok <= 0) {
    LOGE("ca_llama_generate: tokenize failed (n_tok=%d)", n_tok);
    return nullptr;
  }
  tokens.resize(n_tok);

  /* 预留输出空间：ctx - prompt_tokens - 1 */
  const int32_t max_out = eng->n_ctx - n_tok - 1;
  if (max_out <= 0) {
    LOGE("ca_llama_generate: prompt too long (%d tokens, ctx=%d)", n_tok, eng->n_ctx);
    return nullptr;
  }

  /* 2. 喂入 prompt */
  llama_batch batch = llama_batch_get_one(tokens.data(), n_tok);
  int rc = llama_decode(eng->ctx, batch);
  if (rc != 0) {
    LOGE("ca_llama_generate: llama_decode(prompt) rc=%d", rc);
    return nullptr;
  }

  /* 3. 自回归生成 */
  std::string out;
  out.reserve(1024);
  llama_token cur = LLAMA_TOKEN_NULL;
  int32_t gen_count = 0;
  char piece_buf[256];

  while (gen_count < max_out) {
    cur = llama_sampler_sample(eng->smpl, eng->ctx, -1);
    llama_sampler_accept(eng->smpl, cur);

    if (cur == kTokenImEnd || cur == kTokenEot ||
        cur == llama_vocab_eos(eng->vocab)) {
      break;
    }

    int32_t n = llama_token_to_piece(eng->vocab, cur, piece_buf,
                                     (int32_t)sizeof(piece_buf), 0, false);
    if (n > 0) {
      out.append(piece_buf, n);
    }

    /* 单 token 继续解码 */
    llama_batch one = llama_batch_get_one(&cur, 1);
    rc = llama_decode(eng->ctx, one);
    if (rc != 0) {
      LOGE("ca_llama_generate: llama_decode(gen) rc=%d at step %d", rc, gen_count);
      break;
    }
    gen_count++;
  }

  const double cost_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - t_start).count();
  LOGI("ca_llama_generate: prompt=%d out=%d tokens, text_len=%d, cost=%.0fms",
       n_tok, gen_count, (int)out.size(), cost_ms);

  /* 返回的字符串由调用方持有：用 strdup 拷贝到堆上 */
  char* result = static_cast<char*>(std::malloc(out.size() + 1));
  if (!result) return nullptr;
  std::memcpy(result, out.c_str(), out.size());
  result[out.size()] = '\0';
  return result;
}

void ca_llama_free(void* engine) {
  auto* eng = static_cast<LlamaEngine*>(engine);
  if (!eng) return;
  if (eng->smpl)  { llama_sampler_free(eng->smpl);  eng->smpl = nullptr; }
  if (eng->ctx)   { llama_free(eng->ctx);           eng->ctx = nullptr; }
  if (eng->model) { llama_model_free(eng->model);   eng->model = nullptr; }
  delete eng;
  LOGI("ca_llama_free: engine released");
}

}  // extern "C"
