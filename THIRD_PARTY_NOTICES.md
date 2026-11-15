# 第三方组件与许可 / Third-Party Notices

本项目自身代码以 MIT 协议发布（见 [LICENSE](LICENSE)）。下面是被链接、打包或运行时下载的
第三方组件及其许可。分发二进制时请保留本文件。

## 打包进 APK 的原生库

| 组件 | 版本 / 来源 | 许可 | 说明 |
| --- | --- | --- | --- |
| sherpa-onnx | v2.0.1 C API，`android/app/src/main/jniLibs/arm64-v8a/libsherpa-onnx-c-api.so` | Apache-2.0 | 流式语音识别 C 接口；`third_party/sherpa-onnx/include/` 仅保留头文件 |
| ONNX Runtime | 预编译 `libonnxruntime.so`（sherpa-onnx 官方 Android 包附带） | MIT | 神经网络推理后端 |
| llama.cpp | vendored 源码 `third_party/llama.cpp` | MIT | 本地 GGUF 模型推理（M2.7 引入，可选路径） |
| whisper.cpp | vendored 源码 tag `b5454`(v1.9.5) `third_party/whisper.cpp` | MIT | 整窗识别兜底 |
| SQLite | 3.45 amalgamation `third_party/sqlite/` | Public Domain | 本地会话 / 字幕 / 笔记存储 |

## 运行时下载的模型（不随 APK 分发）

| 模型 | 许可 | 出处 |
| --- | --- | --- |
| sherpa-onnx-streaming-paraformer-bilingual-zh-en (int8) | Apache-2.0 | https://github.com/k2-fsa/sherpa-onnx/releases |
| Silero VAD | MIT | https://github.com/snakers4/silero-vad |
| sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17 (int8) | MIT (FunAudioLLM) | https://github.com/FunAudioLLM/SenseVoice |
| Qwen2.5-1.5B-Instruct GGUF (Q4_K_M) | Apache-2.0 | https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF |
| Qwen2.5 tokenizer（`assets/tokenizer.json`） | Apache-2.0 | 同上 |

模型文件由用户在 App 内主动下载，本项目不重新分发这些权重。若你把模型重新打包分发，
请自行遵守对应许可（Apache-2.0 需要保留 NOTICE 与许可全文）。

## 构建工具（不进入产物）

Android Gradle Plugin 8.5.2 / Kotlin 1.9.24 / Gradle 8.7，均为 Apache-2.0 或 MIT。

## 免责

本项目为个人学习性质的开源项目，与上述任何组织无隶属关系，也未获得其背书。
使用本项目录制课堂内容前，请确认符合你所在学校、单位的规定及当地法律。
