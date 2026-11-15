cmake_minimum_required(VERSION 3.16)
project(classmate_desktop C)

# 桌面通道：只编译纯 C 核心与它的单元测试，不碰 JNI / Android。
# 用法：cmake -S . -B build && cmake --build build && ctest --test-dir build
set(CLASSMATE_BUILD_TESTS ON CACHE BOOL "Build desktop unit tests" FORCE)
add_subdirectory(core)
