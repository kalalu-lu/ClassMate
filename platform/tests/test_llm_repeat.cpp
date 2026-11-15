/*
 * M2.7 单测：本地 llama.cpp 后端「同一 engine 连续 generate」正确性。
 *
 * 验证点：
 *  1) 同一 engine 连续 generate 两次，第二次输出与第一次一致
 *     （每次 generate 开头 llama_memory_clear + llama_sampler_reset 生效，
 *       KV cache 与采样器状态不残留）
 *  2) 第二次输出与「新加载的 engine 单独 generate 一次」一致
 *     （说明清理干净、位置从 0 重新开始，而不是在旧位置继续解码）
 *
 * 用法（需要真实 GGUF 模型，CI 无模型时以 77 跳过）：
 *   cd third_party/llama.cpp
 *   cmake -B build -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
 *         -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_TOOLS=OFF -DBUILD_SHARED_LIBS=OFF
 *   cmake --build build --config Release --target llama -j
 *   cd ../..
 *   g++ -std=c++17 -O2 -I third_party/llama.cpp/include \
 *       platform/tests/test_llm_repeat.cpp platform/llm_llama.cpp \
 *       -L third_party/llama.cpp/build/src -lllama \
 *       -L third_party/llama.cpp/build/ggml/src -lggml -lggml-base -lggml-cpu \
 *       -o build/test_llm_repeat
 *   build/test_llm_repeat path/to/qwen2.5-1.5b-instruct-q4_k_m.gguf
 *
 * 退出码：0 通过 / 1 失败 / 77 跳过（未提供模型）。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
void*       ca_llama_load(const char* model_path, int32_t ctx_len, int32_t n_threads);
const char* ca_llama_generate(void* engine, const char* prompt);
void        ca_llama_free(void* engine);
}

static const char* kPrompt = "用一句话概括：今天讲了牛顿第二定律，力等于质量乘加速度。";

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("SKIP: 需要 GGUF 模型路径，例如 qwen2.5-1.5b-instruct-q4_k_m.gguf\n");
    return 77;
  }
  const char* model_path = argv[1];

  const int32_t kCtx = 2048;
  const int32_t kThreads = 4;

  void* a = ca_llama_load(model_path, kCtx, kThreads);
  if (!a) {
    printf("FAIL: ca_llama_load 返回 null（模型路径或文件有问题）\n");
    return 1;
  }

  char* out1 = const_cast<char*>(ca_llama_generate(a, kPrompt));
  char* out2 = const_cast<char*>(ca_llama_generate(a, kPrompt));
  ca_llama_free(a);

  void* b = ca_llama_load(model_path, kCtx, kThreads);
  if (!b) {
    printf("FAIL: 第二次 ca_llama_load 失败\n");
    std::free(out1);
    std::free(out2);
    return 1;
  }
  char* out3 = const_cast<char*>(ca_llama_generate(b, kPrompt));
  ca_llama_free(b);

  int fails = 0;
  if (!out1 || !*out1) { printf("FAIL: 第 1 次输出为空\n"); fails++; }
  if (!out2 || !*out2) { printf("FAIL: 第 2 次输出为空（KV cache 未清空导致 decode 失败？）\n"); fails++; }
  if (!out3 || !*out3) { printf("FAIL: 新 engine 输出为空\n"); fails++; }

  if (out1 && out2 && std::strcmp(out1, out2) != 0) {
    printf("FAIL: 同一 engine 两次输出不一致（第二次被上一轮污染）\n");
    printf("  1st: %s\n  2nd: %s\n", out1, out2);
    fails++;
  }
  if (out2 && out3 && std::strcmp(out2, out3) != 0) {
    printf("FAIL: 第二次输出与新 engine 单独输出不一致（清理不彻底）\n");
    printf("  repeat: %s\n  fresh : %s\n", out2, out3);
    fails++;
  }

  std::free(out1);
  std::free(out2);
  std::free(out3);

  if (fails == 0) {
    printf("PASS: 连续两次 generate 与单独加载输出一致\n");
    return 0;
  }
  return 1;
}
