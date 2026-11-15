package com.classmate.app

import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.IOException
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.security.NoSuchAlgorithmException

/**
 * M2.7：OpenAI 兼容的云端调用（HttpURLConnection，不引入 okhttp）。
 *
 * 被 native 的 ca_llm_port 通过 JNI 阻塞调用：成功返回纯文本，失败抛异常
 * （超时 / 429 / 401 / 余额不足 / 离线），由 core 捕获后整体降级抽取式。
 * 只有「课程文字」会进入 prompt；音频不在这里，也不会上传。
 */
class CloudLlm(
    private val cfg: CloudConfig,
    /** 读超时：会中增量用短超时（10 s），课后整理用默认长超时 */
    private val readTimeoutMs: Int = 180000
) {

    /** 本次 buildNote 期间累计的输入/输出 token（仅供 Kotlin 层读取，不进 JNI 返回值） */
    @Volatile var totalPromptTokens: Int = 0
        private set
    @Volatile var totalCompletionTokens: Int = 0
        private set
    @Volatile var callCount: Int = 0
        private set

    /** 最近一次请求的输入/输出 token（会中增量日志用） */
    @Volatile var lastPromptTokens: Int = 0
        private set
    @Volatile var lastCompletionTokens: Int = 0
        private set

    /** 余额不足(402)后短路：不再发请求，避免反复烧额度 */
    @Volatile private var quotaExhausted: Boolean = false

    fun generate(prompt: String): String {
        if (quotaExhausted) throw IOException("余额不足，已停止重试")
        /* M3.4：课后整理/会中增量必须关思考。
         * deepseek-flash 默认带推理，实测一次整理 reasoning_len=1033、耗时 60–90 s，
         * 会中增量 10 s 超时直接全灭（日志「增量待重试 seq=1（timeout）」×N），
         * 停录笔记也会超时降级成抽取式。关掉后同一份文本 3–8 s 出结果。 */
        val content = chat(prompt, maxTokens(), noThink = true)
        if (content.isBlank()) throw IOException("云端返回空内容")
        return content
    }

    /**
     * 指定输出上限。点名应答只要 3 句（≤120 字 ≈ 180 token），
     * 用默认的 ctx/3（实际会被夹到 2048）会让模型长篇输出，白等好几秒。
     *
     * noThink=true 时附带关闭推理的字段（不同网关用不同字段名，多写两个无副作用：
     * 不认识的字段会被忽略，不会报错）。点名场景要的是「快」，不是「想得深」。
     */
    fun generate(prompt: String, maxOutputTokens: Int, noThink: Boolean = false): String {
        if (quotaExhausted) throw IOException("余额不足，已停止重试")
        val content = chat(prompt, maxOutputTokens.coerceIn(48, 4096), noThink)
        if (content.isBlank()) throw IOException("云端返回空内容")
        return content
    }

    fun test(): String {
        val content = chat("只回复两个字母：OK", 16)
        return if (content.isBlank()) "连通但返回空内容" else "连通成功：${content.trim().take(60)}"
    }

    private fun maxTokens(): Int {
        val byCtx = cfg.ctxLen / 3
        return byCtx.coerceIn(256, 2048)
    }

    private fun endpoint(): String {
        val base = cfg.baseUrl.trim().trimEnd('/')
        return if (base.endsWith("/chat/completions")) base else "$base/chat/completions"
    }

    private fun chat(prompt: String, maxTokens: Int, noThink: Boolean = false): String {
        val body = JSONObject().apply {
            put("model", cfg.modelId)
            put("temperature", 0)
            put("max_tokens", maxTokens)
            if (noThink) {
                /* 思考型模型会把 max_tokens 全花在 reasoning 上，content 为空。
                 * 三个字段覆盖常见网关写法，不支持的会被忽略。 */
                put("thinking", JSONObject().put("type", "disabled"))
                put("enable_thinking", false)
                put("reasoning_effort", "none")
            }
            put("messages", JSONArray().put(
                JSONObject().apply {
                    put("role", "user")
                    put("content", prompt)
                }
            ))
        }.toString()

        var conn: HttpURLConnection? = null
        /* M3.4 诊断：把「建连/首字节」与「读体」分开计时。
         * 笔记与点名叫答走同一个端点，延迟差 60 倍时必须能指出卡在哪一段。 */
        val t0 = android.os.SystemClock.elapsedRealtime()
        try {
            conn = (URL(endpoint()).openConnection() as HttpURLConnection).apply {
                requestMethod = "POST"
                connectTimeout = minOf(15000, readTimeoutMs)
                readTimeout = readTimeoutMs
                doOutput = true
                setRequestProperty("Content-Type", "application/json; charset=utf-8")
                setRequestProperty("Authorization", "Bearer ${cfg.apiKey}")
            }
            conn.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }

            val code = conn.responseCode
            val tHdr = android.os.SystemClock.elapsedRealtime() - t0
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.use { s ->
                BufferedReader(InputStreamReader(s, Charsets.UTF_8)).readText()
            } ?: ""

            if (code !in 200..299) {
                val hint = when (code) {
                    401, 403 -> "Key 无效或无权限"
                    402 -> {
                        quotaExhausted = true   /* 余额不足：本轮停止重试，避免烧额度 */
                        "余额不足"
                    }
                    429 -> "触发限流（429）"
                    in 500..599 -> "服务端错误"
                    else -> "HTTP $code"
                }
                throw IOException("$hint：${text.take(120)}")
            }

            val json = JSONObject(text)
            json.optJSONObject("usage")?.let { u ->
                lastPromptTokens = u.optInt("prompt_tokens", 0)
                lastCompletionTokens = u.optInt("completion_tokens", 0)
                totalPromptTokens += lastPromptTokens
                totalCompletionTokens += lastCompletionTokens
                callCount++
            }
            val choices = json.optJSONArray("choices")
                ?: throw IOException("响应缺少 choices")
            val first = choices.optJSONObject(0)
                ?: throw IOException("choices 为空")
            val msg = first.optJSONObject("message")
            val finish = first.optString("finish_reason", "")
            val content = msg?.optString("content", "") ?: ""
            /* 诊断用：思考型模型会把 max_tokens 全花在 reasoning_content 上，
             * 结果 content 为空、finish_reason=length。日志里能直接看出来。 */
            val reasoningLen = msg?.optString("reasoning_content", "")?.length ?: 0
            android.util.Log.i(
                "ClassMate/Llm",
                "finish=$finish reasoning_len=$reasoningLen content_len=${content.length} " +
                        "max_tokens=$maxTokens prompt_chars=${prompt.length} " +
                        "ttfb=${tHdr}ms total=${android.os.SystemClock.elapsedRealtime() - t0}ms"
            )
            return content
        } catch (e: NoSuchAlgorithmException) {
            throw IOException("TLS 不可用：${e.message}")
        } finally {
            conn?.disconnect()
        }
    }
}
