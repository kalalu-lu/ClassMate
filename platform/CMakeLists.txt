cmake_minimum_required(VERSION 3.22.1)
project(classmate_jni CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(CLASSMATE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_library(classmate SHARED
  jni_bridge.cpp
  asr_whisper.cpp
  asr_sherpa.cpp
  asr_sensevoice.cpp
  llm_llama.cpp
)

# sherpa-onnx C API：M2 使用预编译 libsherpa-onnx-c-api.so + libonnxruntime.so
# （来自 sherpa-onnx v2.0.1 android 发行包），头文件 vendored 于 third_party。
target_include_directories(classmate PRIVATE
  ${CLASSMATE_ROOT}/third_party/sherpa-onnx/include
)

if(ANDROID)
  add_library(sherpa_capi SHARED IMPORTED)
  set_target_properties(sherpa_capi PROPERTIES IMPORTED_LOCATION
    "${CLASSMATE_ROOT}/android/app/src/main/jniLibs/${ANDROID_ABI}/libsherpa-onnx-c-api.so")
endif()

# classmate_core 的 include 目录是 PUBLIC，会自动传递到这里；
# whisper 的 include 目录由 whisper 目标 PUBLIC 传递。
# llama 的 include 目录由 llama 目标 PUBLIC 传递。
target_link_libraries(classmate PRIVATE
  classmate_core
  whisper
  llama
  log
)

if(ANDROID)
  target_link_libraries(classmate PRIVATE sherpa_capi)
endif()
