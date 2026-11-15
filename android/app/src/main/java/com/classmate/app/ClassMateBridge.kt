package com.classmate.app

/**
 * Kotlin 与 C 核心之间唯一的通道。
 *
 * 这里只声明 native 方法，不含任何业务逻辑；C++ 侧的对应实现见
 * platform/jni_bridge.cpp，函数签名必须保持
 * Java_com_classmate_app_ClassMateBridge_xxx。
 */
class ClassMateBridge {

    /** 核心事件回调。type 对应 classmate.h 的 ca_event_type：0=ASR_PARTIAL, 1=ASR_FINAL, 6=ERROR */
    fun interface Listener {
        fun onEvent(type: Int, tsMs: Long, text: String?, score: Float, code: Int)
    }

    external fun nativeVersion(): String
    external fun nativeCreate(
        wavPath: String,
        asrModelsDir: String,   // 传 "" 表示本次不启用识别（纯录音）
        asrThreads: Int,
        saveWav: Boolean,       // false = 不落 WAV，只做识别与 SQLite 落库（M2.5）
        hotwords: String,       // M2.6 课程热词，逗号分隔；空串表示不启用
        listener: Listener?
    ): Long
    external fun nativeFeed(handle: Long, pcm: ShortArray, frames: Int, tsMs: Long): Int
    external fun nativeFlush(handle: Long): Int
    external fun nativeDestroy(handle: Long)

    /**
     * M2.6 课后二次校正：按句切片调用 SenseVoice，写回 segments.text_corrected。
     * 返回 0=成功；1=模型缺失；2=WAV/库不可用；-1=失败。
     */
    external fun nativeCorrect(wavPath: String, dbPath: String, modelDir: String): Int

    /**
     * M2.7 生成 chapters + 笔记（不依赖音频）。
     *
     * cloud 非空且 allowLlm=true 时，C 核心通过端口回调 cloud.generate(prompt)；
     * cloud 可以是 CloudLlm（云端 HTTP）或 LocalLlm（本地 GGUF），两者都有
     * generate(String): String 方法，JNI 侧通过反射调用。
     * 云端/本地/自动的选择、Key 解密、首次授权都在 Kotlin 侧完成。
     * 返回会话 id，-1 失败/无会话；降级原因写在 note_meta。
     */
    external fun nativeBuildNote(
        dbPath: String,
        notePath: String,
        configPath: String,
        backend: String,
        tokenizerPath: String,
        promptDir: String,
        targetCtx: Int,
        modelId: String,
        promptVer: String,
        allowLlm: Boolean,
        cloud: Any?
    ): Long

    /** 只做建表/补列，不重建章节；返回 0 成功 */
    external fun nativeEnsureSchema(dbPath: String): Int

    /**
     * M2.7 本地 LLM（llama.cpp GGUF）。
     * 生命周期：nativeLlmLoad → 多次 nativeLlmGenerate → nativeLlmFree。
     * 返回 0 表示加载失败（模型缺失/损坏）。
     */
    external fun nativeLlmLoad(modelPath: String, ctxLen: Int, nThreads: Int): Long
    external fun nativeLlmGenerate(handle: Long, prompt: String): String?
    external fun nativeLlmFree(handle: Long)

    companion object {
        const val EV_ASR_PARTIAL = 0
        const val EV_ASR_FINAL = 1
        const val EV_ERROR = 6

        init {
            System.loadLibrary("classmate")
        }
    }
}
