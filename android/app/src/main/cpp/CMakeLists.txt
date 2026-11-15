cmake_minimum_required(VERSION 3.22.1)
project(classmate_android C CXX)

# android/app/src/main/cpp -> 上溯 5 层回到工程根目录 classmate/
get_filename_component(CLASSMATE_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

# Android 构建只编译 core + JNI，不编译桌面单元测试
set(CLASSMATE_BUILD_TESTS OFF CACHE BOOL "Build desktop unit tests" FORCE)

# M2.7：llama.cpp 本地 LLM 后端。
# 先于 whisper.cpp 构建：两者都定义 ggml target，llama 的 ggml(0.29) 比 whisper 的(0.26) 新，
# 且都有 `if(NOT TARGET ggml)` 守卫，先建的会被复用。核心 C API 向下兼容，whisper 可在 0.29 上编译。
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE          OFF CACHE BOOL "" FORCE)
set(GGML_LTO             OFF CACHE BOOL "" FORCE)
set(LLAMA_ALL_WARNINGS   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
add_subdirectory("${CLASSMATE_ROOT}/third_party/llama.cpp"
                 "${CMAKE_CURRENT_BINARY_DIR}/llama_build")

# whisper.cpp（M1 vendored 于 third_party）：只要库，不要它的测试/示例/服务器
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(WHISPER_CURL           OFF CACHE BOOL "" FORCE)
set(WHISPER_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
# 复用上面 llama.cpp 已建的 ggml target（见上）
set(WHISPER_USE_SYSTEM_GGML ON CACHE BOOL "" FORCE)
add_subdirectory("${CLASSMATE_ROOT}/third_party/whisper.cpp"
                 "${CMAKE_CURRENT_BINARY_DIR}/whisper_build")

add_subdirectory("${CLASSMATE_ROOT}/core"
                 "${CMAKE_CURRENT_BINARY_DIR}/core_build")
add_subdirectory("${CLASSMATE_ROOT}/platform"
                 "${CMAKE_CURRENT_BINARY_DIR}/platform_build")
