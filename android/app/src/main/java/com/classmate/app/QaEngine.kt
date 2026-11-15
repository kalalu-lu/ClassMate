package com.classmate.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.database.sqlite.SQLiteDatabase
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.util.ArrayDeque
import java.util.concurrent.Executors

/** 判定结果推给界面的两类事件（Activity 只负责显示） */
sealed class QaUiEvent {
    data class Triggered(val question: String) : QaUiEvent()
    data class Answered(
        val question: String,
        val answer: String?,
        val note: String,
        val latencyMs: Long,
        val promptTokens: Int,
        val completionTokens: Int,
        val fallbackContext: String
    ) : QaUiEvent()
}

/**
 * M3.1 + M3.2：点名判定/振动/通知/云端三句答案。
 *
 * 生命周期挂在 RecordingService 上（ColorOS 会冻结退后台的 Activity 线程），
 * 但做成进程单例，便于无点击注入入口在 Activity 前台时也能跑完整链路。
 * 所有网络与判定都在单线程 executor 上，主线程只收 UI 事件。
 *
 * 只发送文字：最近课堂转写 + 提问本身，音频不出设备。
 */
class QaEngine private constructor(private val app: Context) {

    companion object {
        const val TAG = "ClassMate/QA"
        private const val PREFS = "classmate_settings"
        private const val K_CONSENT = "qa_cloud_consent"
        private const val K_ENABLED = "qa_enabled"
        private const val CHANNEL_ID = "classmate_qa"
        private const val NOTIFY_ID = 1001

        @Volatile private var instance: QaEngine? = null

        fun get(ctx: Context): QaEngine =
            instance ?: synchronized(this) {
                instance ?: QaEngine(ctx.applicationContext).also { instance = it }
            }
    }

    private val prefs = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /* P1-9：判定线程与网络线程分离。云端生成再慢，新套话也能在判定线程上
     * 立刻振动/通知，不被 HTTP 阻塞。 */
    private val judgeExec = Executors.newSingleThreadExecutor { r ->
        Thread(r, "classmate-qa-judge").apply { priority = Thread.NORM_PRIORITY }
    }
    private val netExec = Executors.newSingleThreadExecutor { r ->
        Thread(r, "classmate-qa-net").apply { priority = Thread.NORM_PRIORITY }
    }
    private val main = Handler(Looper.getMainLooper())

    /** 界面回调（Activity 设置；未设置时只走通知） */
    @Volatile var uiListener: ((QaUiEvent) -> Unit)? = null

    /* P1-7：冷却时长跟随 qa_triggers.json 的 cooldown_sec，配置加载后创建 */
    @Volatile private var gate: QaGate? = null
    private val bufferLock = Any()
    private val buffer = ArrayDeque<Pair<Long, String>>()

    /* P0-2：只读当前场次。拿不到 session_id 时上下文一律为空，禁止跨场次取历史字幕 */
    @Volatile private var sessionDbFile: File? = null
    @Volatile private var sessionStartWallMs = 0L
    @Volatile private var sessionId = -1L

    /* P2-11：同一 CloudConfig 复用 CloudLlm，402 后的 quotaExhausted 短路状态才留得住 */
    private var cachedLlm: CloudLlm? = null
    private var cachedLlmCfg: CloudConfig? = null
    private var cachedLlmTimeout = -1

    @Volatile private var triggerCfg: QaTriggerConfig? = null
    @Volatile private var promptTemplate: String? = null
    private var qaCfg = QaOptions()

    data class QaOptions(
        val timeoutMs: Int = 10000,
        /** 只带上文 N 字（不是 900 字整段课堂）：老师问完立刻发，越短越快 */
        val contextChars: Int = 80,
        val maxOutputChars: Int = 120,
        val keepRecent: Int = 3,
        /** 输出上限：思考型模型先花 reasoning 再出正文，给太小会 content 为空 */
        val maxTokens: Int = 700,
        /** 点名应答关掉推理：实测不关时 reasoning 会用掉 1694 字、正文 0 字 */
        val disableThinking: Boolean = true
    )

    fun isConsentGiven(): Boolean = prefs.getBoolean(K_CONSENT, false)
    fun isEnabled(): Boolean = prefs.getBoolean(K_ENABLED, true)
    fun setEnabled(on: Boolean) = prefs.edit().putBoolean(K_ENABLED, on).apply()
    fun setConsent(given: Boolean) = prefs.edit().putBoolean(K_CONSENT, given).apply()

    /** 录音链路：每条 FINAL 进来（service 的 native 回调线程调用，必须立即返回） */
    fun onFinal(tsMs: Long, text: String) {
        if (text.isBlank()) return
        judgeExec.execute { handle(tsMs, text, injected = false) }
    }

    /**
     * 只灌上下文、不判定：回放验证时用。
     * 与 onFinal 的区别是不振动、不弹卡片、不调模型，纯攒「刚才讲了什么」。
     */
    fun feedContext(tsMs: Long, text: String) {
        if (text.isBlank()) return
        judgeExec.execute { remember(tsMs, text) }
    }

    /** 无点击注入入口：跳过音频，文本直接走完整判定→振动→通知→云端 */
    fun injectTest(text: String) {
        Log.i(TAG, "inject: $text")
        judgeExec.execute { handle(SystemClock.elapsedRealtime(), text, injected = true) }
    }

    /**
     * 新场开始：清冷却与上下文缓冲，并记住本场 WAV 同目录的库文件。
     * session_id 由 resolveSessionId() 惰性确认（sessions 里 status='recording' 的唯一一行）。
     */
    fun onSessionStart(wavPath: String?) {
        gate?.reset()
        synchronized(bufferLock) { buffer.clear() }
        sessionId = -1L
        sessionDbFile = wavPath?.let { File(File(it).absoluteFile.parentFile, "classmate.db") }
        sessionStartWallMs = System.currentTimeMillis()
        Log.i(TAG, "session start, db=${sessionDbFile?.absolutePath ?: "-"}")
    }

    /**
     * R2：停止录音时补一次会话收尾。
     *
     * 宿主 core 的 ca_storage_close 也会写同样的 UPDATE，但它没检查 sqlite3_step 的
     * 返回值（storage.c），撞上 WAL 并发写会静默丢；丢了的场次下次开录会被
     * ca_storage_init 扫成 interrupted 且 ended_at=0。这里按 status='recording'
     * 幂等再落一次，调用点必须在 QaEngine 清上下文之前。
     *
     * @return 本次补写影响的行数（0 = core 已写过，或没有可收尾的场次）
     */
    fun onSessionStop(): Int {
        val n = markSessionDone()
        synchronized(bufferLock) { buffer.clear() }
        sessionId = -1L
        sessionDbFile = null
        sessionStartWallMs = 0L
        Log.i(TAG, "session stop, done=$n")
        return n
    }

    /** 只更新仍处于 recording 的那一行（正常最多一条），自带幂等 */
    private fun markSessionDone(): Int {
        val f = sessionDbFile
            ?: File(File(app.getExternalFilesDir(null), "recordings"), "classmate.db")
        if (!f.exists()) return 0
        var db: SQLiteDatabase? = null
        return try {
            db = SQLiteDatabase.openDatabase(f.absolutePath, null, SQLiteDatabase.OPEN_READWRITE)
            val st = db.compileStatement(
                "UPDATE sessions SET ended_at=?, status='done' WHERE status='recording'"
            )
            st.bindLong(1, System.currentTimeMillis())
            val n = st.executeUpdateDelete()
            st.close()
            n
        } catch (e: Exception) {
            Log.w(TAG, "markSessionDone failed: ${e.message}")
            0
        } finally {
            try { db?.close() } catch (_: Exception) {}
        }
    }

    private fun handle(tsMs: Long, text: String, injected: Boolean) {
        /* P0-1：每条 FINAL 无条件进上下文缓冲，不再只在命中时追加 */
        if (!injected) remember(tsMs, text)

        /* P1-6：开关关闭时在振动/通知之前就返回，完全静默 */
        if (!isEnabled()) return

        val cfg = triggerCfg ?: loadTriggerConfig().also {
            triggerCfg = it
            gate = QaGate(it.cooldownSec * 1000L)
        }
        val match = QaTrigger.judge(text, cfg)
        if (match == null) {
            if (injected) Log.i(TAG, "no-hit: $text")
            return
        }
        val now = SystemClock.elapsedRealtime()
        val g = gate ?: QaGate(cfg.cooldownSec * 1000L).also { gate = it }
        if (!g.accept(now, match.normText)) {
            Log.i(TAG, "cooldown/similar, skip: ${match.rule}")
            return
        }
        Log.i(TAG, "HIT rule=${match.rule} q=$text")

        QaNotifier.vibrateOnce(app)
        QaNotifier.notifyTriggered(app, text)
        postUi(QaUiEvent.Triggered(text))

        val recent = recentSentences(qaCfg.keepRecent)
        val context = buildContext(tsMs, text, injected)

        /* P1-9：网络交给独立线程，判定线程立刻回到监听下一条 FINAL */
        netExec.execute { answer(text, context, recent) }
    }

    /**
     * 课堂名：合并输入框后约定「第一项是课堂名，后面是热词」。
     * 拿不到就给「课堂」，让 prompt 仍然说人话，不出现「这是一节课」这种别扭句子。
     */
    private fun courseName(): String {
        val raw = try {
            prefs.getString("hotwords", "") ?: ""
        } catch (e: Exception) {
            ""
        }
        var first = raw.split(',', '，').firstOrNull()?.trim().orEmpty()
        /* 旧版本单独存过 course_name，兼容一下：热词框没填时不至于丢掉课堂名 */
        if (first.isEmpty()) {
            first = try {
                prefs.getString("course_name", "")?.trim().orEmpty()
            } catch (e: Exception) {
                ""
            }
        }
        /* 空值时给「正在进行的」：拼出来是「这是一节正在进行的课」，不会出现「这是一节课堂课」 */
        return if (first.isEmpty()) "正在进行的" else first
    }

    /** 云端三句答案：只在 netExec 上跑，判定/振动不等它 */
    private fun answer(question: String, context: String, recent: List<String>) {
        val cloud = LlmSettings.resolveCloud(prefs)
        if (cloud == null) {
            finish(question, null, "未配置云端模型", 0, 0, 0, recent)
            return
        }
        if (!isConsentGiven()) {
            finish(question, null, "未同意点名上传课堂文字", 0, 0, 0, recent)
            return
        }
        if (!isEnabled()) return
        val tpl = promptTemplate ?: loadPromptTemplate().also { promptTemplate = it }
        /* 课堂名让模型知道学科语境：同类问题在「政治课」和「C 语言课」上答案完全不同 */
        val prompt = tpl.replace("{course}", courseName())
            .replace("{question}", question)
            .replace("{context}", context)
        val llm = llmFor(cloud, qaCfg.timeoutMs)
        val t0 = SystemClock.elapsedRealtime()
        try {
            val raw = llm.generate(prompt, qaCfg.maxTokens, qaCfg.disableThinking)
            val sentences = QaAnswer.sentences(raw, qaCfg.maxOutputChars)
            if (sentences.isEmpty()) {
                finish(question, null, "云端返回无法解析为三点", SystemClock.elapsedRealtime() - t0,
                    llm.lastPromptTokens, llm.lastCompletionTokens, recent)
                return
            }
            /* 3 点分行带序号：手机上要能「扫一眼就念」，不能糊成一段 */
            val body = sentences.mapIndexed { i, s -> "${i + 1}. $s" }.joinToString("\n")
            val note = if (sentences.size < 3) "模型只给了 ${sentences.size} 点" else "3 点"
            Log.i(
                TAG,
                "answer ok latency_ms=${SystemClock.elapsedRealtime() - t0} " +
                        "prompt_tokens=${llm.lastPromptTokens} completion_tokens=${llm.lastCompletionTokens} " +
                        "sentences=${sentences.size} chars=${body.length}"
            )
            Log.i(TAG, "answer text: $body")
            finish(question, body, note, SystemClock.elapsedRealtime() - t0,
                llm.lastPromptTokens, llm.lastCompletionTokens, recent)
        } catch (e: Exception) {
            val msg = when (e) {
                is java.net.SocketTimeoutException -> "云端超时"
                is IOException -> e.message ?: "网络不可用"
                else -> e.message ?: "调用失败"
            }
            Log.w(TAG, "answer failed: $msg")
            finish(question, null, msg, SystemClock.elapsedRealtime() - t0, 0, 0, recent)
        }
    }

    private fun remember(tsMs: Long, text: String) {
        synchronized(bufferLock) {
            buffer.addLast(tsMs to text)
            while (buffer.size > 40) buffer.removeFirst()
        }
    }

    /** P2-11：配置没变就复用同一个 CloudLlm（余额不足短路状态因此不会被丢掉） */
    private fun llmFor(cfg: CloudConfig, timeoutMs: Int): CloudLlm {
        val cur = cachedLlm
        if (cur != null && cachedLlmCfg == cfg && cachedLlmTimeout == timeoutMs) return cur
        return CloudLlm(cfg, timeoutMs).also {
            cachedLlm = it
            cachedLlmCfg = cfg
            cachedLlmTimeout = timeoutMs
        }
    }

    private fun finish(
        question: String, answer: String?, note: String, latencyMs: Long,
        promptTokens: Int, completionTokens: Int, recent: List<String>
    ) {
        QaNotifier.notifyAnswered(app, answer, note)
        postUi(
            QaUiEvent.Answered(
                question, answer, note, latencyMs, promptTokens, completionTokens,
                recent.joinToString(" / ")
            )
        )
    }

    private fun postUi(ev: QaUiEvent) {
        main.post { uiListener?.invoke(ev) }
    }

    /** 最近 N 句（供降级展示），按时间升序 */
    private fun recentSentences(n: Int): List<String> {
        val snapshot: List<Pair<Long, String>>
        synchronized(bufferLock) { snapshot = ArrayList(buffer) }
        val out = ArrayList<String>(n)
        for (i in snapshot.indices.reversed()) {
            if (out.size >= n) break
            out.add(snapshot[i].second)
        }
        out.reverse()
        if (out.size >= n) return out
        // 缓冲不够时从本场次 DB 补最近的句子
        val db = readDbSentences(40)
        if (db.isEmpty()) return out
        return (db + out).takeLast(n)
    }

    /**
     * 上下文：只要「提问之前的最近 N 个字」（默认 80 字 ≈ 上文 50–60 个汉字）。
     *
     * 为什么不做整段课堂：每多 100 字就多约 90 个 prompt token，prefill 变慢；
     * 而点名应答只需要知道「刚才在讲什么」，用不到 90 秒的完整转写。
     * 提问句本身要剔掉（prompt 里已经单独给了 {question}），否则重复占额度。
     */
    private fun buildContext(tsMs: Long, question: String, injected: Boolean): String {
        val want = qaCfg.contextChars.coerceIn(30, 400)
        val q = question.trim()
        val sb = StringBuilder()
        val snapshot: List<Pair<Long, String>>
        synchronized(bufferLock) { snapshot = ArrayList(buffer) }
        /* 没有本场次（纯注入）时不碰缓冲，避免拿到上一场的句子 */
        if (snapshot.isNotEmpty() && sessionDbFile != null) {
            val cutoff = tsMs - 90_000
            for ((t, txt) in snapshot) {
                if (txt.trim() == q) continue          /* 提问句不重复发 */
                if (!injected && t < cutoff) continue
                sb.append(txt).append("。")
            }
        }
        var ctx = sb.toString()
        if (ctx.isBlank()) {
            ctx = readDbSentences(12).filter { it.trim() != q }.joinToString("。")
        }
        ctx = ctx.replace("\n", " ").trim()
        if (ctx.length > want) ctx = ctx.takeLast(want)
        ctx = ctx.trimStart('。', '，', ',', ' ')
        return ctx.ifBlank { "（暂无课堂文字）" }
    }

    /**
     * 当前场次 id：sessions 里 status='recording' 且 started_at 落在本场窗口内的一行。
     * 宿主 C 核心只允许改 Kotlin 侧，所以用只读连接反查；查不到返回 null（上下文置空）。
     */
    private fun resolveSessionId(): Long? {
        val cached = sessionId
        if (cached > 0) return cached
        val f = sessionDbFile ?: return null
        if (sessionStartWallMs <= 0L || !f.exists()) return null
        var db: SQLiteDatabase? = null
        val id: Long? = try {
            db = SQLiteDatabase.openDatabase(f.absolutePath, null, SQLiteDatabase.OPEN_READONLY)
            db.rawQuery(
                "SELECT id FROM sessions WHERE status='recording'" +
                        " AND started_at>=? AND started_at<=? ORDER BY id DESC LIMIT 1",
                arrayOf(
                    (sessionStartWallMs - 60_000L).toString(),
                    (sessionStartWallMs + 600_000L).toString()
                )
            ).use { c -> if (c.moveToFirst()) c.getLong(0) else null }
        } catch (e: Exception) {
            Log.w(TAG, "resolveSessionId failed: ${e.message}")
            null
        } finally {
            try { db?.close() } catch (_: Exception) {}
        }
        if (id != null && id > 0) sessionId = id
        return id
    }

    /** 只读查库：本场次最近 limit 条字幕（优先 text_refined），按时间升序 */
    private fun readDbSentences(limit: Int): List<String> {
        val f = sessionDbFile ?: return emptyList()
        val sid = resolveSessionId() ?: return emptyList()
        if (!f.exists()) return emptyList()
        var db: SQLiteDatabase? = null
        return try {
            db = SQLiteDatabase.openDatabase(f.absolutePath, null, SQLiteDatabase.OPEN_READONLY)
            val out = ArrayList<String>()
            try {
                db.rawQuery(
                    "SELECT COALESCE(NULLIF(r.text,''), s.text) AS t" +
                            " FROM segments s LEFT JOIN text_refined r" +
                            " ON r.session_id=s.session_id AND r.seg_id=s.id" +
                            " WHERE s.session_id=?" +
                            " AND COALESCE(NULLIF(r.text,''), s.text) IS NOT NULL" +
                            " AND length(COALESCE(NULLIF(r.text,''), s.text))>1" +
                            " ORDER BY s.id DESC LIMIT ?",
                    arrayOf(sid.toString(), limit.toString())
                ).use { c ->
                    while (c.moveToNext()) out.add(c.getString(0))
                }
            } catch (e: Exception) {
                /* 旧库没有 text_refined 表：退回纯 segments.text */
                Log.w(TAG, "text_refined 不可用，退回 segments.text: ${e.message}")
                out.clear()
                db.rawQuery(
                    "SELECT text FROM segments WHERE session_id=?" +
                            " AND text IS NOT NULL AND length(text)>1" +
                            " ORDER BY id DESC LIMIT ?",
                    arrayOf(sid.toString(), limit.toString())
                ).use { c ->
                    while (c.moveToNext()) out.add(c.getString(0))
                }
            }
            out.reverse()
            out
        } catch (e: Exception) {
            Log.w(TAG, "readDbSentences failed: ${e.message}")
            emptyList()
        } finally {
            try { db?.close() } catch (_: Exception) {}
        }
    }

    /* ---------------- 配置加载 ---------------- */

    private fun readConfigJson(): JSONObject? {
        val assetText = try {
            app.assets.open("llm_config.json").bufferedReader(Charsets.UTF_8).readText()
        } catch (e: Exception) {
            null
        }
        val assetObj = assetText?.let {
            try { JSONObject(it) } catch (e: Exception) { null }
        }
        val f = File(app.filesDir, "ai/llm_config.json")
        val fileObj = if (f.exists()) {
            try { JSONObject(f.readText()) } catch (e: Exception) { null }
        } else null

        /* 内置配置 version 更高就用内置的，并写回文件。
         * 否则设备上遗留的旧配置会一直盖住新参数（改 max_tokens / context_chars 无效）。 */
        if (assetObj != null && (fileObj == null ||
                    assetObj.optInt("version", 1) > fileObj.optInt("version", 1))
        ) {
            try {
                f.parentFile?.mkdirs()
                f.writeText(assetText!!)
                Log.i(TAG, "llm_config upgraded：v${fileObj?.optInt("version", 1) ?: 0} -> " +
                        "v${assetObj.optInt("version", 1)}")
            } catch (e: Exception) {
                Log.w(TAG, "llm_config 写回失败：${e.message}")
            }
            return assetObj
        }
        return fileObj ?: assetObj
    }

    private fun loadTriggerConfig(): QaTriggerConfig {
        readConfigJson()?.optJSONObject("qa")?.let { q ->
            qaCfg = QaOptions(
                timeoutMs = q.optInt("timeout_ms", qaCfg.timeoutMs),
                contextChars = q.optInt("context_chars", qaCfg.contextChars),
                maxOutputChars = q.optInt("max_output_chars", qaCfg.maxOutputChars),
                keepRecent = q.optInt("keep_recent", qaCfg.keepRecent),
                maxTokens = q.optInt("max_tokens", qaCfg.maxTokens),
                disableThinking = q.optBoolean("disable_thinking", qaCfg.disableThinking)
            )
        }
        val external = File(app.getExternalFilesDir(null), "qa_triggers.json")
        if (!external.exists()) releaseDefaultTriggers(external)
        /* 词表随 APK 升级：内置 version 比外部新就覆盖（旧文件留 .bak），
         * 否则改了 asset 里的词表，已装机的手机永远用旧的那份。 */
        else upgradeTriggersIfNewer(external)
        val json = try {
            JSONObject(external.readText())
        } catch (e: Exception) {
            Log.w(TAG, "qa_triggers.json 读取失败，用内置兜底：${e.message}")
            null
        }
        return if (json != null) parseTrigger(json) ?: builtinTrigger()
        else builtinTrigger()
    }

    private fun releaseDefaultTriggers(dst: File) {
        try {
            dst.parentFile?.mkdirs()
            app.assets.open("qa_triggers.json").use { input ->
                dst.outputStream().use { out -> input.copyTo(out) }
            }
            Log.i(TAG, "released default triggers -> ${dst.absolutePath}")
        } catch (e: Exception) {
            Log.w(TAG, "release default triggers failed: ${e.message}")
        }
    }

    /** 内置词表 version 更高时覆盖外部文件（旧文件备份成 .vN.bak） */
    private fun upgradeTriggersIfNewer(dst: File) {
        try {
            val assetVer = JSONObject(
                app.assets.open("qa_triggers.json")
                    .bufferedReader(Charsets.UTF_8).readText()
            ).optInt("version", 1)
            val curVer = try {
                JSONObject(dst.readText()).optInt("version", 1)
            } catch (e: Exception) {
                0
            }
            if (assetVer > curVer) {
                dst.copyTo(File(dst.parentFile, "${dst.name}.v$curVer.bak"), overwrite = true)
                releaseDefaultTriggers(dst)
                Log.i(TAG, "triggers upgraded v$curVer -> v$assetVer（旧文件已备份）")
            }
        } catch (e: Exception) {
            Log.w(TAG, "triggers upgrade skipped: ${e.message}")
        }
    }

    private fun parseTrigger(o: JSONObject): QaTriggerConfig? {
        try {
            fun arr(name: String): List<String> {
                val a = o.optJSONArray(name) ?: return emptyList()
                val out = ArrayList<String>(a.length())
                for (i in 0 until a.length()) {
                    val s = a.optString(i, "")
                    if (s.isNotEmpty()) out.add(s)
                }
                return out
            }
            val wl = arr("whitelist")
            if (wl.isEmpty()) return null
            return QaTriggerConfig(
                whitelist = wl,
                callAnchors = arr("call_anchors"),
                actionAnchors = arr("action_anchors"),
                exclusions = arr("exclusions"),
                cooldownSec = o.optInt("cooldown_sec", 25)
            )
        } catch (e: Exception) {
            return null
        }
    }

    private fun builtinTrigger(): QaTriggerConfig = QaTriggerConfig(
        whitelist = listOf(
            "有没有同学回答一下", "有没有同学回答", "有没有人回答", "谁来回答一下",
            "谁来回答", "谁来说说", "哪位同学回答", "找个同学回答", "点个名回答",
            "点一位同学", "随机点个", "抽一位同学", "大家想一想", "大家思考一下",
            "有同学补充吗", "谁来补充", "举手回答", "自愿回答", "这个问题谁来"
        ),
        callAnchors = listOf("同学", "哪位", "谁", "有人", "大家", "找个", "点个", "举手", "自愿", "点名"),
        actionAnchors = listOf("回答", "说说", "讲讲", "说一下", "补充", "想想", "想一想", "想一下", "思考"),
        exclusions = listOf(
            "答案是", "正确答案是", "我们来看", "上节课", "已经讲过", "作业", "考试",
            "解析", "板书", "ppt", "举个例子", "也就是说", "所以", "因此", "总结一下", "没有人",
            "回答得", "回答正确", "回答很好", "回答不错", "回答得很", "答得不错", "答得很好",
            "回答的基本", "回答都"
        ),
        cooldownSec = 25
    )

    private fun loadPromptTemplate(): String {
        return try {
            app.assets.open("prompts/answer_3sentences.txt")
                .bufferedReader(Charsets.UTF_8).readText()
        } catch (e: Exception) {
            "这是一节{course}课，正在上课。\n老师的提问：{question}\n提问之前的课堂原文：{context}\n" +
                    "请输出恰好 3 点，每点单独一行、每点不超过 30 字，依次给结论、依据、例子；" +
                    "不要写「老师我认为」「我认为」「首先」这类开场话，直接给答案。"
        }
    }
}

/** 三句答案后处理：去编号/引号，按句切分，句边界收口到 120 字 */
object QaAnswer {

    private val NUMBERING = Regex(
        "^\\s*(?:[0-9]{1,2}\\s*[.、)．]|[（(][0-9一二三四五]{1,2}[)）]|[①②③④⑤⑥]|第[一二三](?:点|句|条)[:：]?)\\s*"
    )

    /* 用户明确要求「省略『老师我认为』这类废话」：prompt 里已经禁止一遍，
     * 这里再兜底剥一遍——模型十次里总有一两次会加开场白。 */
    private val LEAD_NOISE = listOf(
        "老师，我认为", "老师我认为", "老师，我觉得", "老师我觉得",
        "老师认为", "我认为", "我觉得", "个人认为",
        "简单来说，", "简单来说,", "总的来说，", "总的来说,",
        "首先，", "首先,", "首先", "回答：", "答案：", "回答:", "答案:",
        "老师，", "老师,", "老师："
    )

    private fun stripLead(src: String): String {
        var s = src
        var changed = true
        while (changed) {
            changed = false
            for (n in LEAD_NOISE) {
                if (s.startsWith(n)) {
                    s = s.removePrefix(n).trim()
                    changed = true
                }
            }
        }
        return s
    }

    fun sentences(raw: String, maxChars: Int = 120): List<String> {
        var text = raw.replace("\r", "")
            .replace("```", "")
            .trim()
        // 去掉常见的引导语（模型偶尔不听话）
        text = text.removePrefix("回答：").removePrefix("答案：").trim()

        val parts = ArrayList<String>()
        for (seg in text.split(Regex("[。！？!?\\n]+"))) {
            var s = seg.replace(NUMBERING, "").trim()
            s = stripLead(s)
            s = s.trim('"', '\'', '“', '”', '「', '」', '‘', '’', ' ', '、', ',', '，')
            if (s.isNotEmpty()) parts.add(s)
            if (parts.size >= 3) break
        }
        if (parts.isEmpty()) return emptyList()

        val out = ArrayList<String>(3)
        var used = 0
        for (s in parts) {
            val remain = maxChars - used
            if (remain <= 0) break
            if (s.length <= remain) {
                out.add(s)
                used += s.length
            } else {
                if (out.isEmpty()) out.add(s.take(remain).trimEnd() + "…")
                break
            }
            if (out.size >= 3) break
        }
        return out
    }
}

/** 振动与通知：触发一次 80ms 短振；答案到了再短振一次。只在本机，不涉及网络 */
object QaNotifier {

    private const val CHANNEL_ID = "classmate_qa"
    private const val NOTIFY_ID = 1001

    private fun vibrator(ctx: Context): Vibrator? {
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                val mgr = ctx.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager
                mgr?.defaultVibrator
            } else {
                @Suppress("DEPRECATION")
                ctx.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
            }
        } catch (e: Exception) {
            null
        }
    }

    fun vibrateOnce(ctx: Context) {
        try {
            val v = vibrator(ctx) ?: return
            if (!v.hasVibrator()) return
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                v.vibrate(VibrationEffect.createOneShot(80, 120))
            } else {
                @Suppress("DEPRECATION")
                v.vibrate(80)
            }
        } catch (e: Exception) {
            Log.w(QaEngine.TAG, "vibrate failed: ${e.message}")
        }
    }

    private fun vibrateAnswer(ctx: Context) {
        try {
            val v = vibrator(ctx) ?: return
            if (!v.hasVibrator()) return
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                v.vibrate(VibrationEffect.createWaveform(longArrayOf(0, 60, 60, 90), -1))
            } else {
                @Suppress("DEPRECATION")
                v.vibrate(longArrayOf(0, 60, 60, 90), -1)
            }
        } catch (e: Exception) {
            Log.w(QaEngine.TAG, "vibrate failed: ${e.message}")
        }
    }

    private fun ensureChannel(ctx: Context): NotificationManager? {
        val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager ?: return null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            if (nm.getNotificationChannel(CHANNEL_ID) == null) {
                nm.createNotificationChannel(
                    NotificationChannel(CHANNEL_ID, "点名叫答", NotificationManager.IMPORTANCE_HIGH).apply {
                        description = "老师点名提问时的提醒与建议答案"
                        enableVibration(false)   /* 振动由 QaNotifier 精确控制 */
                    }
                )
            }
        }
        return nm
    }

    private fun contentIntent(ctx: Context): PendingIntent {
        val i = Intent(ctx, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_NEW_TASK
            putExtra("qa_show_card", true)
        }
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        else PendingIntent.FLAG_UPDATE_CURRENT
        return PendingIntent.getActivity(ctx, 2001, i, flags)
    }

    fun notifyTriggered(ctx: Context, question: String) {
        val nm = ensureChannel(ctx) ?: return
        val n = Notification.Builder(ctx, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setContentTitle("老师点名了")
            .setContentText(question.take(60))
            .setPriority(Notification.PRIORITY_HIGH)
            .setAutoCancel(true)
            .setContentIntent(contentIntent(ctx))
            .addAction(0, "查看上下文文字", contentIntent(ctx))
            .build()
        try { nm.notify(NOTIFY_ID, n) } catch (e: Exception) { Log.w(QaEngine.TAG, "notify failed: ${e.message}") }
    }

    fun notifyAnswered(ctx: Context, answer: String?, note: String) {
        val nm = ensureChannel(ctx) ?: return
        if (answer != null) vibrateAnswer(ctx)
        val n = Notification.Builder(ctx, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setContentTitle(if (answer != null) "可以这样回答" else "未生成答案")
            .setContentText((answer ?: note).take(80))
            .setStyle(Notification.BigTextStyle().bigText(answer ?: note))
            .setPriority(Notification.PRIORITY_HIGH)
            .setAutoCancel(true)
            .setContentIntent(contentIntent(ctx))
            .addAction(0, "查看上下文文字", contentIntent(ctx))
            .build()
        try { nm.notify(NOTIFY_ID, n) } catch (e: Exception) { Log.w(QaEngine.TAG, "notify failed: ${e.message}") }
    }
}
