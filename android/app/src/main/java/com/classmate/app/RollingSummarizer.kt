package com.classmate.app

import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.os.SystemClock
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * M2.8 主路径：会中云端增量滚动摘要。
 *
 * 录音过程中每满 [RollingConfig.intervalMs]（或每满 N 条 FINAL 字幕）触发一次调用：
 * 输入 = 本批新增 segments + 上一轮滚动摘要 + 固定 prompt，**从不重发全文**。
 * 输出 JSON 落 rolling_state；逐句纠错写 text_refined（不覆盖 segments.text）。
 *
 * 停止录音只做本地合并定稿（去重 + 章节定稿 + notes_*.md），不重跑全文、不递归二分。
 * 增量失败不阻塞录音、不重试轰炸：保持 lastSegId 不变，下一轮把这段一起带上。
 */
class RollingSummarizer(
    private val dbFile: File,
    private val cloud: CloudLlm,
    private val modelId: String,
    private val cfg: RollingConfig,
    private val promptTemplate: String,
    private val onStatus: (String) -> Unit
) {

    data class RollingConfig(
        val intervalMs: Long,
        val batchFinals: Int,
        val timeoutMs: Int,
        val summaryMaxChars: Int,
        /** 与 llm_config.json 的 rolling.prompt_ver 一致，写进 rolling_state/chapters 便于追溯 */
        val promptVer: String
    )

    private data class Seg(val id: Long, val startMs: Long, val text: String)
    private data class ChapterDraft(val segId: Long, val title: String, val summary: String)
    private data class ChapterRow(
        val segId: Long, val startMs: Long, val title: String, val summary: String
    )

    private val single = Executors.newSingleThreadExecutor { r ->
        Thread(r, "classmate-rolling").apply { priority = Thread.NORM_PRIORITY - 1 }
    }
    private val busy = AtomicBoolean(false)

    @Volatile private var closed = false
    @Volatile private var sessionId = -1L
    @Volatile private var lastSegId = 0L
    @Volatile private var lastSeq = 0
    @Volatile private var summary = ""
    @Volatile private var finalsSinceBatch = 0
    /* 构造时起算：第一轮自动批次在开录约 2 分钟后，而不是首个 30 s tick 就发 */
    @Volatile private var lastAttemptAt = SystemClock.elapsedRealtime()
    @Volatile private var pending = false
    @Volatile private var committedRuns = 0

    /** 新的一句 FINAL 已落库（由 UI 线程调用，只做计数，极轻量） */
    fun onFinal() {
        if (closed) return
        finalsSinceBatch++
        if (finalsSinceBatch >= cfg.batchFinals) maybeRun(force = true)
    }

    /** 定时器每 ~30 s 调一次；到 2 分钟才真正发请求 */
    fun maybeTick() = maybeRun(force = false)

    private fun maybeRun(force: Boolean) {
        if (closed) return
        val now = SystemClock.elapsedRealtime()
        if (!force && now - lastAttemptAt < cfg.intervalMs) return
        if (busy.get()) return
        busy.set(true)
        lastAttemptAt = now
        finalsSinceBatch = 0
        single.execute {
            try {
                runOnce()
            } catch (e: Exception) {
                pending = true
                val msg = "增量待重试 seq=${lastSeq + 1}（${e.message}）"
                Log.w(TAG, msg)
                onStatus(msg)
            } finally {
                busy.set(false)
            }
        }
    }

    private fun runOnce(force: Boolean = false) {
        if (closed && !force) return
        val sid = ensureSessionId() ?: return
        val segs = newSegments(sid)
        if (segs.isEmpty()) return

        val t0 = SystemClock.elapsedRealtime()
        val prompt = buildPrompt(segs)
        val raw = cloud.generate(prompt)     /* 短超时，由 CloudLlm 构造时指定 */
        val json = parseJson(raw)
        val updated = json.optString("updated_summary", "").trim()
        val candidates = json.optJSONObject("chapter_candidates")
        val seq = lastSeq + 1

        persist(
            sid = sid, seq = seq,
            segStart = segs.first().id, segEnd = segs.last().id,
            summary = updated.ifEmpty { summary },
            candidates = candidates?.toString() ?: "{}",
            refined = json.optJSONArray("refined_segments")
        )

        summary = updated.ifEmpty { summary }
        lastSegId = segs.last().id
        lastSeq = seq
        pending = false
        committedRuns++

        val cost = SystemClock.elapsedRealtime() - t0
        /* 验收：单次输入 token 恒定，不随课堂时长增长 */
        Log.i(TAG, "rolling seq=$seq segs=${segs.size} inTok=${cloud.lastPromptTokens} " +
                "outTok=${cloud.lastCompletionTokens} cost=${cost}ms summaryLen=${summary.length}")
        onStatus("增量摘要：已完成 $committedRuns 批 · 本批 ${segs.size} 句 · ${cost}ms")
    }

    private fun buildPrompt(segs: List<Seg>): String {
        val sb = StringBuilder(promptTemplate.length + segs.size * 48 + 512)
        sb.append(promptTemplate.trimEnd()).append("\n\n")
        sb.append("上一轮滚动摘要：\n")
        sb.append(if (summary.isEmpty()) "（无，这是第一批）" else summary.take(cfg.summaryMaxChars))
        sb.append("\n\n本批新增字幕（只含新增部分，带 seg_id）：\n")
        for (s in segs) {
            sb.append('[').append(s.id).append("] ").append(s.text.trim()).append('\n')
        }
        return sb.toString()
    }

    private fun parseJson(raw: String): JSONObject {
        val start = raw.indexOf('{')
        val end = raw.lastIndexOf('}')
        if (start < 0 || end <= start) throw IllegalStateException("云端未返回 JSON")
        return JSONObject(raw.substring(start, end + 1))
    }

    /* ---------------- 存储 ---------------- */

    private fun openDb(): SQLiteDatabase =
        SQLiteDatabase.openDatabase(dbFile.absolutePath, null, SQLiteDatabase.OPEN_READWRITE)
            .also { it.rawQuery("PRAGMA busy_timeout=3000", null).use { c -> c.moveToFirst() } }

    private fun <T> withDb(block: (SQLiteDatabase) -> T?): T? {
        if (!dbFile.exists()) return null
        var db: SQLiteDatabase? = null
        return try {
            db = openDb()
            block(db)
        } catch (e: Exception) {
            Log.w(TAG, "db 访问失败：${e.message}")
            null
        } finally {
            try { db?.close() } catch (_: Exception) {}
        }
    }

    private fun ensureSessionId(): Long? {
        sessionId.takeIf { it > 0 }?.let { return it }
        val id = withDb { db ->
            db.rawQuery(
                "SELECT id FROM sessions WHERE status='recording' ORDER BY id DESC LIMIT 1", null
            ).use { c -> if (c.moveToFirst()) c.getLong(0) else -1L }
        } ?: -1L
        val resolved = if (id > 0) id else withDb { db ->
            /* 停止录音后状态已变更，补最后一批时回退到最近一场 */
            db.rawQuery("SELECT id FROM sessions ORDER BY id DESC LIMIT 1", null)
                .use { c -> if (c.moveToFirst()) c.getLong(0) else -1L }
        } ?: -1L
        if (resolved > 0) sessionId = resolved
        return resolved.takeIf { it > 0 }
    }

    private fun newSegments(sid: Long): List<Seg> = withDb { db ->
        db.rawQuery(
            "SELECT id, start_ms, text FROM segments" +
                    " WHERE session_id=? AND id>? ORDER BY id LIMIT 200",
            arrayOf(sid.toString(), lastSegId.toString())
        ).use { c ->
            val out = mutableListOf<Seg>()
            while (c.moveToNext()) {
                val text = if (c.isNull(2)) "" else c.getString(2)
                if (text.isNotBlank()) out.add(Seg(c.getLong(0), c.getLong(1), text))
            }
            out
        }
    } ?: emptyList()

    private fun persist(
        sid: Long, seq: Int, segStart: Long, segEnd: Long,
        summary: String, candidates: String, refined: org.json.JSONArray?
    ) {
        withDb { db ->
            db.beginTransaction()
            try {
                db.execSQL(
                    "INSERT INTO rolling_state(session_id, seq, seg_id_start, seg_id_end," +
                            " summary, candidates, model_id, prompt_ver) VALUES(?,?,?,?,?,?,?,?)",
                    arrayOf(sid, seq, segStart, segEnd, summary, candidates,
                        modelId, cfg.promptVer)
                )
                if (refined != null) {
                    for (i in 0 until refined.length()) {
                        val item = refined.optJSONObject(i) ?: continue
                        val segId = item.optLong("seg_id", -1)
                        val text = item.optString("text", "").trim()
                        if (segId <= 0 || text.isEmpty()) continue
                        db.execSQL(
                            "INSERT OR REPLACE INTO text_refined(session_id, seg_id, text," +
                                    " model_id, prompt_ver) VALUES(?,?,?,?,?)",
                            arrayOf(sid, segId, text, modelId, cfg.promptVer)
                        )
                    }
                }
                db.setTransactionSuccessful()
            } finally {
                db.endTransaction()
            }
        }
    }

    fun hasRows(): Boolean = withDb { db ->
        db.rawQuery(
            "SELECT 1 FROM rolling_state WHERE session_id=? LIMIT 1",
            arrayOf(ensureSessionIdForRead(db).toString())
        ).use { c -> c.moveToFirst() }
    } ?: false

    private fun ensureSessionIdForRead(db: SQLiteDatabase): Long =
        sessionId.takeIf { it > 0 } ?: db.rawQuery(
            "SELECT id FROM sessions ORDER BY id DESC LIMIT 1", null
        ).use { c -> if (c.moveToFirst()) c.getLong(0) else -1L }

    /**
     * 停止录音后的本地合并定稿：读 rolling_state → 章节标题去重定稿 → 回查 seg_id 起点
     * → 写 chapters + note_meta → 生成 notes_*.md。纯本地，不做 LLM 调用。
     */
    fun finalize(noteFile: File, startedAt: Long, title: String?): FinalResult? = withDb { db ->
        val sid = ensureSessionIdForRead(db)
        if (sid <= 0) return@withDb null

        val drafts = mutableListOf<ChapterDraft>()
        var seqCount = 0
        var firstSegId = 0L
        var firstSummary = ""
        var lastSegEnd = 0L
        db.rawQuery(
            "SELECT seq, seg_id_start, seg_id_end, summary, candidates FROM rolling_state" +
                    " WHERE session_id=? ORDER BY seq",
            arrayOf(sid.toString())
        ).use { c ->
            while (c.moveToNext()) {
                seqCount++
                val sum = (if (c.isNull(3)) "" else c.getString(3)).take(200)
                if (firstSegId <= 0L) {
                    firstSegId = if (c.isNull(1)) 0L else c.getLong(1)
                    firstSummary = sum
                }
                if (!c.isNull(2)) lastSegEnd = maxOf(lastSegEnd, c.getLong(2))
                val cand = if (c.isNull(4)) "{}" else c.getString(4)
                val obj = try { JSONObject(cand) } catch (e: Exception) { JSONObject() }
                if (!obj.optBoolean("new_chapter", false)) continue
                val segId = obj.optLong("start_seg_id", -1)
                if (segId <= 0) continue
                val t = obj.optString("title", "").trim().take(18)
                drafts.add(ChapterDraft(segId, t.ifEmpty { sum.take(18).ifEmpty { "课堂片段" } }, sum))
            }
        }
        /* 模型没标 new_chapter 时兜底开一章：绝不因为格式问题退回全文重跑 */
        if (drafts.isEmpty() && firstSegId > 0) {
            drafts.add(ChapterDraft(firstSegId,
                firstSummary.take(18).ifEmpty { "课堂片段" }, firstSummary))
        }
        if (drafts.isEmpty()) return@withDb null

        /* dedupe：同一 seg_id 只保留第一个；顺序按 seg_id */
        val seen = mutableSetOf<Long>()
        val ordered = drafts.filter { seen.add(it.segId) }.sortedBy { it.segId }

        var rejected = 0
        val chapters = mutableListOf<ChapterRow>()
        for (d in ordered) {
            val startMs = db.rawQuery(
                "SELECT start_ms FROM segments WHERE session_id=? AND id=?",
                arrayOf(sid.toString(), d.segId.toString())
            ).use { c -> if (c.moveToFirst()) c.getLong(0) else null }
            if (startMs == null) { rejected++; continue }   /* 查不到真实句：拒绝该章 */
            chapters.add(ChapterRow(d.segId, startMs, d.title, d.summary))
        }
        if (chapters.isEmpty()) return@withDb null

        db.beginTransaction()
        try {
            db.execSQL("DELETE FROM chapters WHERE session_id=?", arrayOf(sid))
            for (ch in chapters) {
                db.execSQL(
                    "INSERT INTO chapters(session_id, start_ms, title, summary, seg_id," +
                            " model_id, prompt_ver, source) VALUES(?,?,?,?,?,?,?, 'rolling')",
                    arrayOf(sid, ch.startMs, ch.title, ch.summary,
                        ch.segId, modelId, cfg.promptVer)
                )
            }
            db.execSQL(
                "INSERT OR REPLACE INTO note_meta(session_id, source, model_id, prompt_ver," +
                        " reason, llm_calls, compress_rounds, rejected_chapters, estimated)" +
                        " VALUES(?, 'rolling', ?, ?, '', ?, 0, ?, 0)",
                arrayOf(sid, modelId, cfg.promptVer, seqCount, rejected)
            )
            db.setTransactionSuccessful()
        } finally {
            db.endTransaction()
        }

        writeNoteFile(db, noteFile, sid, startedAt, title, chapters)
        /* 尾段是否已纳入摘要：滚动摘要最后覆盖到的 seg_id 之后还有多少句 */
        val tailSegs = db.rawQuery(
            "SELECT COUNT(*) FROM segments WHERE session_id=? AND id>?",
            arrayOf(sid.toString(), lastSegEnd.toString())
        ).use { c -> if (c.moveToFirst()) c.getInt(0) else 0 }
        FinalResult(sid, chapters.size, rejected, seqCount, tailSegs)
    }

    data class FinalResult(
        val sessionId: Long,
        val chapters: Int,
        val rejected: Int,
        val seq: Int,
        /** 未纳入滚动摘要的尾段句数（尾批超时/失败时 >0），需在 UI 显式标注 */
        val tailSegs: Int
    )

    private fun writeNoteFile(
        db: SQLiteDatabase, file: File, sid: Long, startedAt: Long, title: String?,
        chapters: List<ChapterRow>
    ) {
        val segs = db.rawQuery(
            "SELECT s.start_ms, s.text, r.text FROM segments s" +
                    " LEFT JOIN text_refined r ON r.session_id=s.session_id AND r.seg_id=s.id" +
                    " WHERE s.session_id=? ORDER BY s.start_ms, s.id",
            arrayOf(sid.toString())
        ).use { c ->
            val out = mutableListOf<Triple<Long, String, String>>()
            while (c.moveToNext()) {
                val refined = if (c.isNull(2)) "" else c.getString(2)
                out.add(Triple(c.getLong(0), if (c.isNull(1)) "" else c.getString(1), refined))
            }
            out
        }

        file.bufferedWriter(Charsets.UTF_8).use { w ->
            w.write("# 课堂笔记\n\n")
            w.write("- 会话：#$sid")
            if (!title.isNullOrBlank()) w.write(" $title")
            w.write("\n")
            if (startedAt > 0) w.write("- 开始：$startedAt（epoch ms，界面可切换课堂钟点）\n")
            w.write("- 说明：会中增量云端摘要，停止录音后本地合并定稿；点 [mm:ss] 可跳转字幕\n")
            w.write("- 生成：会中增量摘要（rolling）\n\n")
            var ci = 0
            for (s in segs) {
                while (ci < chapters.size && s.first >= chapters[ci].startMs) {
                    val mmss = mmss(chapters[ci].startMs)
                    w.write("## [$mmss] ${chapters[ci].title}\n\n")
                    if (chapters[ci].summary.isNotEmpty()) {
                        w.write("> ${chapters[ci].summary}\n\n")
                    }
                    ci++
                }
                val text = s.third.ifEmpty { s.second }
                w.write("- [${mmss(s.first)}] $text\n")
            }
        }
    }

    private fun mmss(ms: Long): String =
        String.format(Locale.US, "%02d:%02d", ms / 1000 / 60, ms / 1000 % 60)

    /** 会中累计 token（写入费用面板；本地档永远为 0） */
    val promptTokens: Int get() = cloud.totalPromptTokens
    val completionTokens: Int get() = cloud.totalCompletionTokens
    val cloudModelId: String get() = modelId

    /** 停止时是否还有未进摘要的 FINAL 段落（课太短或最后一段还没来得及触发） */
    fun needsFinalBatch(): Boolean = withDb { db ->
        val sid = ensureSessionIdForRead(db)
        if (sid <= 0) return@withDb false
        db.rawQuery(
            "SELECT 1 FROM segments WHERE session_id=? AND id>? LIMIT 1",
            arrayOf(sid.toString(), lastSegId.toString())
        ).use { c -> c.moveToFirst() }
    } ?: false

    /**
     * 停止录音时补最后一批（仍是增量：只带未进摘要的段落 + 上一轮摘要）。
     * 提交到同一条单线程队列，排在在途请求之后，避免同一段被发两次或多 seq 撞主键。
     *
     * [waitMs] 只等这么久：超时/失败立即返回 false，调用方先出已有终稿并标注「尾段待补」，
     * 不让「停止到终稿」被尾批拖到十几秒。
     */
    fun finalBatch(waitMs: Long = 4000L): Boolean {
        return try {
            single.submit { runOnce(force = true) }.get(waitMs, TimeUnit.MILLISECONDS)
            true
        } catch (e: Exception) {
            pending = true
            Log.w(TAG, "尾批未完成（${e.javaClass.simpleName}）：${e.message}")
            false
        }
    }

    /** 停止录音时调用：不再发起新批次（已提交的那批可能仍在等 HTTP 返回） */
    fun stopTriggering() {
        closed = true
    }

    fun close() {
        closed = true
        single.shutdown()
    }

    companion object {
        private const val TAG = "ClassMate/Rolling"
        private const val FALLBACK_PROMPT_VER = "m28-1"

        /** 读 llm_config.json 的 rolling 段（缺字段用默认值） */
        fun loadConfig(context: Context): RollingConfig {
            val cfgFile = File(File(context.filesDir, "ai"), "llm_config.json")
            return try {
                val root = JSONObject(cfgFile.readText())
                val r = root.optJSONObject("rolling")
                RollingConfig(
                    intervalMs = r?.optLong("interval_ms", 120000) ?: 120000,
                    batchFinals = r?.optInt("batch_finals", 8) ?: 8,
                    timeoutMs = r?.optInt("timeout_ms", 10000) ?: 10000,
                    summaryMaxChars = r?.optInt("summary_max_chars", 400) ?: 400,
                    /* prompt_ver 以配置为准：rolling.prompt_ver → 顶层 prompt_ver → 内置默认 */
                    promptVer = r?.optString("prompt_ver", "").orEmpty().ifEmpty {
                        root.optString("prompt_ver", "").ifEmpty { FALLBACK_PROMPT_VER }
                    }
                )
            } catch (e: Exception) {
                RollingConfig(120000, 8, 10000, 400, FALLBACK_PROMPT_VER)
            }
        }

        fun loadPrompt(context: Context): String {
            val f = File(File(File(context.filesDir, "ai"), "prompts"), "rolling_summary.txt")
            return try { f.readText() } catch (e: Exception) { "" }
        }
    }
}
