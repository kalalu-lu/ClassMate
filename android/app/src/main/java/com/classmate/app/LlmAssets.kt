package com.classmate.app

import android.content.Context
import java.io.File

/**
 * M2.7 随包资产：llm_config.json / tokenizer.json / prompts 目录下的 txt 模板。
 * AssetManager 不能直接给 C 读，首次使用时复制到 filesDir/ai。
 * tokenizer.json 是真实 Qwen BPE 词表，用于 token 预算（不是按字数估）。
 */
object LlmAssets {

    data class Paths(
        val configPath: String,
        val tokenizerPath: String,
        val promptDir: String
    )

    fun prepare(context: Context): Paths {
        val dir = File(context.filesDir, "ai").apply { mkdirs() }
        val prompts = File(dir, "prompts").apply { mkdirs() }

        val config = File(dir, "llm_config.json")
        /* 配置/单价随 app 升级而变：每次覆盖，保证新字段（pricing 等）生效 */
        copyAsset(context, "llm_config.json", config)

        val tokenizer = File(dir, "tokenizer.json")
        copyIfMissing(context, "tokenizer.json", tokenizer)

        for (name in listOf("refine_compress.txt", "compress_more.txt", "chapters.txt",
                "rolling_summary.txt")) {
            copyAsset(context, "prompts/$name", File(prompts, name))
        }

        return Paths(
            configPath = if (config.exists()) config.absolutePath else "",
            tokenizerPath = if (tokenizer.exists()) tokenizer.absolutePath else "",
            promptDir = prompts.absolutePath
        )
    }

    private fun copyAsset(context: Context, asset: String, dst: File) {
        try {
            context.assets.open(asset).use { input ->
                dst.outputStream().use { output -> input.copyTo(output) }
            }
        } catch (e: Exception) {
            if (dst.exists()) dst.delete()
        }
    }

    private fun copyIfMissing(context: Context, asset: String, dst: File) {
        if (dst.exists() && dst.length() > 0) return
        copyAsset(context, asset, dst)
    }
}
