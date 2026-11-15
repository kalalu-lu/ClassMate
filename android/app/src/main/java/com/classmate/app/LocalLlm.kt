package com.classmate.app

import java.io.IOException

/**
 * M2.7 本地 LLM 后端：封装 llama.cpp GGUF 推理。
 *
 * 与 CloudLlm 一样暴露 generate(String): String，因此可直接作为
 * nativeBuildNote 的 cloud 参数传入（JNI 侧通过反射调用）。
 *
 * 生命周期：load → 多次 generate → close（用完释放，不常驻内存）。
 * 模型路径来自 ModelManager（manifest.json 的 llm-gguf 条目），不打进 APK。
 */
class LocalLlm private constructor(
    private val bridge: ClassMateBridge,
    private val handle: Long
) {

    /** 调用本地模型生成文本；失败抛 IOException，由 core 降级抽取式 */
    fun generate(prompt: String): String {
        val out = bridge.nativeLlmGenerate(handle, prompt)
        if (out.isNullOrBlank()) {
            throw IOException("本地 LLM 返回空（模型加载失败或推理异常）")
        }
        return out
    }

    /** 释放模型与上下文（用完即释放，降低内存峰值） */
    fun close() {
        if (handle != 0L) {
            bridge.nativeLlmFree(handle)
        }
    }

    companion object {
        /** 加载 GGUF 模型；失败返回 null（调用方应降级抽取式并 UI 标注） */
        fun load(
            bridge: ClassMateBridge,
            modelPath: String,
            ctxLen: Int = 2048,
            nThreads: Int = 4
        ): LocalLlm? {
            val h = bridge.nativeLlmLoad(modelPath, ctxLen, nThreads)
            if (h == 0L) return null
            return LocalLlm(bridge, h)
        }
    }
}
