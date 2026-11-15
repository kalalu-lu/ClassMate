package com.classmate.app

import android.content.Context
import android.content.SharedPreferences
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * M2.7 模型选择链的 Kotlin 侧：
 *  - 多套 OpenAI 兼容配置（base_url / api_key / model_id / ctx_len）存 SharedPreferences；
 *  - api_key 用 Android Keystore AES/GCM 加密后落盘，明文不进日志、不进配置 JSON；
 *  - 三档策略：始终本地 / 自动（默认）/ 云端优先；
 *  - 优先级：填了 Key → 云端；没填 Key（或云端探测失败）→ 本地；本地不可用 → 抽取式降级。
 *
 * 注意：App 不内置任何服务端 Key，也不借用豆包/DeepSeek App 的免费额度；
 * 只做「去开放平台领自有 Key」的引导，额度以平台当期政策为准。
 */
data class CloudConfig(
    val id: String,
    val label: String,
    val baseUrl: String,
    val apiKey: String,
    val modelId: String,
    val ctxLen: Int
)

object LlmSettings {

    const val POLICY_LOCAL = 0
    const val POLICY_AUTO = 1
    const val POLICY_CLOUD = 2

    private const val K_POLICY = "llm_policy"
    private const val K_CONSENT = "llm_cloud_consent"
    private const val K_PROFILES = "llm_profiles"
    private const val K_LAST_PROMPT = "llm_last_prompt_tokens"
    private const val K_LAST_COMPLETION = "llm_last_completion_tokens"
    private const val K_TOTAL_PROMPT = "llm_total_prompt_tokens"
    private const val K_TOTAL_COMPLETION = "llm_total_completion_tokens"
    private const val K_SHOW_COST = "llm_show_cost"
    private const val K_LAST_MODEL = "llm_last_model_id"
    private const val KEY_ALIAS = "classmate_llm_key"
    private const val GCM_TAG_BITS = 128

    data class Profile(
        val id: String,
        val label: String,
        val baseUrl: String,
        val modelId: String,
        val ctxLen: Int,
        val apiKey: String
    )

    fun policy(prefs: SharedPreferences): Int = prefs.getInt(K_POLICY, POLICY_AUTO)

    fun setPolicy(prefs: SharedPreferences, policy: Int) {
        prefs.edit().putInt(K_POLICY, policy).apply()
    }

    fun consentGiven(prefs: SharedPreferences): Boolean = prefs.getBoolean(K_CONSENT, false)

    fun setConsent(prefs: SharedPreferences, given: Boolean) {
        prefs.edit().putBoolean(K_CONSENT, given).apply()
    }

    /** 读取全部配置（Key 已解密；Keystore 失效时该条 apiKey 为空，需用户重填） */
    fun profiles(prefs: SharedPreferences): List<Profile> {
        val raw = prefs.getString(K_PROFILES, "") ?: ""
        if (raw.isEmpty()) return emptyList()
        return try {
            val arr = JSONArray(raw)
            val out = mutableListOf<Profile>()
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                val id = o.optString("id")
                if (id.isEmpty()) continue
                out.add(
                    Profile(
                        id = id,
                        label = o.optString("label", "云端模型"),
                        baseUrl = o.optString("base_url"),
                        modelId = o.optString("model_id"),
                        ctxLen = o.optInt("ctx_len", 32768),
                        apiKey = decrypt(
                            prefs.getString("llm_key_$id", "") ?: "",
                            prefs.getString("llm_iv_$id", "") ?: ""
                        )
                    )
                )
            }
            out
        } catch (e: Exception) {
            emptyList()
        }
    }

    fun upsertProfile(prefs: SharedPreferences, p: Profile) {
        val list = profiles(prefs).toMutableList()
        val idx = list.indexOfFirst { it.id == p.id }
        if (idx >= 0) list[idx] = p else list.add(p)

        val arr = JSONArray()
        for (item in list) {
            arr.put(
                JSONObject().apply {
                    put("id", item.id)
                    put("label", item.label)
                    put("base_url", item.baseUrl)
                    put("model_id", item.modelId)
                    put("ctx_len", item.ctxLen)
                }
            )
        }
        val editor = prefs.edit().putString(K_PROFILES, arr.toString())
        if (p.apiKey.isNotEmpty()) {
            val (ct, iv) = encrypt(p.apiKey)
            if (ct.isNotEmpty()) {
                editor.putString("llm_key_${p.id}", ct)
                editor.putString("llm_iv_${p.id}", iv)
            }
        }
        editor.apply()
    }

    fun deleteProfile(prefs: SharedPreferences, id: String) {
        val list = profiles(prefs).filterNot { it.id == id }
        val arr = JSONArray()
        for (item in list) {
            arr.put(
                JSONObject().apply {
                    put("id", item.id)
                    put("label", item.label)
                    put("base_url", item.baseUrl)
                    put("model_id", item.modelId)
                    put("ctx_len", item.ctxLen)
                }
            )
        }
        prefs.edit()
            .putString(K_PROFILES, arr.toString())
            .remove("llm_key_$id")
            .remove("llm_iv_$id")
            .apply()
    }

    /** 本次课后整理是否走云端：填了 Key 且策略不是「始终本地」 */
    fun resolveCloud(prefs: SharedPreferences): CloudConfig? {
        if (policy(prefs) == POLICY_LOCAL) return null
        val p = profiles(prefs).firstOrNull { it.apiKey.isNotEmpty() } ?: return null
        if (p.baseUrl.isEmpty() || p.modelId.isEmpty()) return null
        return CloudConfig(p.id, p.label, p.baseUrl, p.apiKey, p.modelId, p.ctxLen)
    }

    fun statusLabel(context: Context, prefs: SharedPreferences): String {
        val cloud = resolveCloud(prefs)
        val localReady = localModelReady(context)
        return when {
            cloud != null -> "AI：云端 · ${cloud.modelId}（${cloud.ctxLen} ctx）"
            policy(prefs) == POLICY_LOCAL ->
                if (localReady) "AI：始终本地 · Qwen1.5B-Q4（已就绪）"
                else "AI：始终本地 · 模型未就绪，将抽取式降级"
            localReady -> "AI：自动 · 未填 Key，使用本地 Qwen1.5B-Q4"
            else -> "AI：自动 · 未填 Key，本地不可用时抽取式降级"
        }
    }

    /** 本地 GGUF 是否已下载就绪（manifest 中 kind=llm-gguf 的条目） */
    fun localModelReady(context: Context): Boolean {
        val entries = ModelManager.loadModels(context) ?: return false
        val entry = entries.firstOrNull { it.kind == "llm-gguf" } ?: return false
        val f = ModelManager.modelFile(context, entry)
        return f.exists() && f.length() > 0 &&
                (entry.sizeBytes <= 0 || f.length() == entry.sizeBytes)
    }

    /* ---------------- 费用与 token 统计 ---------------- */

    data class LlmPricing(
        val currency: String,
        val inputPer1k: Double,
        val outputPer1k: Double,
        /** 是否配置了有效单价；全 0 表示「未配置」，UI 不显示金额、不估算费用 */
        val configured: Boolean
    )

    data class CostSummary(
        val lastPromptTokens: Int,
        val lastCompletionTokens: Int,
        val totalPromptTokens: Int,
        val totalCompletionTokens: Int,
        val lastCost: Double,
        val totalCost: Double,
        val currency: String,
        val priceConfigured: Boolean
    )

    /** 读取单价配置（llm_config.json 的 pricing 段），按 model_id 匹配，没有则用 default。
     *  优先读 filesDir/ai 副本（LlmAssets 已拷贝），缺失时直接读 assets 兜底。 */
    fun loadPricing(context: Context, modelId: String): LlmPricing {
        return try {
            val cfg = File(context.filesDir, "ai/llm_config.json")
            val text = if (cfg.exists()) cfg.readText()
            else context.assets.open("llm_config.json").bufferedReader(Charsets.UTF_8).readText()
            val root = JSONObject(text)
            val pricing = root.optJSONObject("pricing")
                ?: return LlmPricing("CNY", 0.0, 0.0, configured = false)
            val currency = pricing.optString("currency", "CNY")
            val def = pricing.optJSONObject("default")
            var input = def?.optDouble("input_per_1k", 0.0) ?: 0.0
            var output = def?.optDouble("output_per_1k", 0.0) ?: 0.0
            pricing.optJSONObject("models")?.optJSONObject(modelId)?.let { m ->
                input = m.optDouble("input_per_1k", input)
                output = m.optDouble("output_per_1k", output)
            }
            LlmPricing(currency, input, output, configured = input > 0.0 || output > 0.0)
        } catch (e: Exception) {
            LlmPricing("CNY", 0.0, 0.0, configured = false)
        }
    }

    /** 记录本次课后整理的 token 用量，并累加到累计值 */
    fun recordSessionUsage(prefs: SharedPreferences, promptTokens: Int, completionTokens: Int) {
        prefs.edit()
            .putInt(K_LAST_PROMPT, promptTokens)
            .putInt(K_LAST_COMPLETION, completionTokens)
            .putInt(K_TOTAL_PROMPT, prefs.getInt(K_TOTAL_PROMPT, 0) + promptTokens)
            .putInt(K_TOTAL_COMPLETION, prefs.getInt(K_TOTAL_COMPLETION, 0) + completionTokens)
            .apply()
    }

    fun showCost(prefs: SharedPreferences): Boolean = prefs.getBoolean(K_SHOW_COST, true)

    fun setShowCost(prefs: SharedPreferences, show: Boolean) {
        prefs.edit().putBoolean(K_SHOW_COST, show).apply()
    }

    /** 记录上次实际调用云端的 model_id，用于按模型取单价 */
    fun recordLastModel(prefs: SharedPreferences, modelId: String) {
        prefs.edit().putString(K_LAST_MODEL, modelId).apply()
    }

    fun lastModel(prefs: SharedPreferences): String = prefs.getString(K_LAST_MODEL, "") ?: ""

    /** 本次 + 累计 token 与估算费用（费用可关闭显示，但内部始终统计） */
    fun costSummary(context: Context, prefs: SharedPreferences, modelId: String): CostSummary {
        val p = loadPricing(context, modelId)
        val lp = prefs.getInt(K_LAST_PROMPT, 0)
        val lc = prefs.getInt(K_LAST_COMPLETION, 0)
        val tp = prefs.getInt(K_TOTAL_PROMPT, 0)
        val tc = prefs.getInt(K_TOTAL_COMPLETION, 0)
        return CostSummary(
            lastPromptTokens = lp,
            lastCompletionTokens = lc,
            totalPromptTokens = tp,
            totalCompletionTokens = tc,
            lastCost = (lp * p.inputPer1k + lc * p.outputPer1k) / 1000.0,
            totalCost = (tp * p.inputPer1k + tc * p.outputPer1k) / 1000.0,
            currency = p.currency,
            priceConfigured = p.configured
        )
    }

    fun formatCost(cost: Double, currency: String): String {
        val symbol = if (currency == "CNY") "¥" else "$currency "
        return "$symbol${String.format("%.6f", cost).trimEnd('0').trimEnd('.').ifEmpty { "0" }}"
    }

    /** 测试连通：同步阻塞，调用方放在后台线程 */
    fun testConnection(cfg: CloudConfig): String = CloudLlm(cfg).test()

    /* ---------------- Keystore 加密 ---------------- */

    private fun secretKey(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getEntry(KEY_ALIAS, null) as? KeyStore.SecretKeyEntry)
            ?.let { return it.secretKey }

        val gen = KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore"
        )
        gen.init(
            KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build()
        )
        return gen.generateKey()
    }

    private fun encrypt(plain: String): Pair<String, String> {
        return try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.ENCRYPT_MODE, secretKey())
            val ct = cipher.doFinal(plain.toByteArray(Charsets.UTF_8))
            val iv = cipher.iv
            Pair(
                Base64.encodeToString(ct, Base64.NO_WRAP),
                Base64.encodeToString(iv, Base64.NO_WRAP)
            )
        } catch (e: Exception) {
            Pair("", "")
        }
    }

    private fun decrypt(cipherB64: String, ivB64: String): String {
        if (cipherB64.isEmpty() || ivB64.isEmpty()) return ""
        return try {
            val iv = Base64.decode(ivB64, Base64.NO_WRAP)
            val ct = Base64.decode(cipherB64, Base64.NO_WRAP)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(
                Cipher.DECRYPT_MODE, secretKey(),
                GCMParameterSpec(GCM_TAG_BITS, iv)
            )
            String(cipher.doFinal(ct), Charsets.UTF_8)
        } catch (e: Exception) {
            ""   /* Keystore 被清/换机：视为未填 Key，让用户重填 */
        }
    }
}
