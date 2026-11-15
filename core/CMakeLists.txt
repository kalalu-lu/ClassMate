cmake_minimum_required(VERSION 3.16)
project(classmate_core C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(CLASSMATE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# M2：SQLite amalgamation（third_party/sqlite），纯 C，桌面与 Android 共用
add_library(classmate_sqlite STATIC
  ${CLASSMATE_ROOT}/third_party/sqlite/sqlite3.c
)
target_include_directories(classmate_sqlite PUBLIC
  ${CLASSMATE_ROOT}/third_party/sqlite
)
target_compile_definitions(classmate_sqlite PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_OMIT_LOAD_EXTENSION=1
  SQLITE_DQS=0
)
set_target_properties(classmate_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(classmate_core STATIC
  src/core.c
  src/wav_writer.c
  src/asr_chunker.c
  src/storage.c
  src/note.c
  src/note_llm.c
  src/tokenizer.c
)

target_include_directories(classmate_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# core_internal.h 只在 src 内部使用
target_include_directories(classmate_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(classmate_core PUBLIC classmate_sqlite)

option(CLASSMATE_BUILD_TESTS "Build desktop unit tests" OFF)

if(CLASSMATE_BUILD_TESTS)
  enable_testing()
  add_executable(test_wav tests/test_wav.c)
  target_link_libraries(test_wav PRIVATE classmate_core)
  add_test(NAME test_wav COMMAND test_wav)

  add_executable(test_asr_chunker tests/test_asr_chunker.c)
  target_link_libraries(test_asr_chunker PRIVATE classmate_core)
  add_test(NAME test_asr_chunker COMMAND test_asr_chunker)

  add_executable(test_storage tests/test_storage.c)
  target_link_libraries(test_storage PRIVATE classmate_core)
  add_test(NAME test_storage COMMAND test_storage)
endif()
