package com.classmate.app

import android.database.sqlite.SQLiteDatabase
import java.io.File

/**
 * M4：字幕 / 章节的只读查询。
 *
 * 数据库由 C core 写入（WAL），录音结束后已关闭，这里用只读连接读取，
 * 页面每次课后处理完成或回到前台时重新拉一遍。
 */
object SessionStore {

    data class SessionInfo(val id: Long, val startedAt: Long, val title: String?)
    data class Chapter(
        val startMs: Long,
        val title: String,
        val summary: String,
        val segId: Long,
        val source: String,
        val modelId: String
    )
    data class NoteMeta(
        val source: String,
        val reason: String,
        val llmCalls: Int,
        val compressRounds: Int,
        val rejectedChapters: Int,
        val estimated: Boolean
    )
    data class Segment(val startMs: Long, val text: String, val corrected: String?)

    private fun <T> read(dbFile: File, block: (SQLiteDatabase) -> T): T? {
        if (!dbFile.exists()) return null
        var db: SQLiteDatabase? = null
        return try {
            db = SQLiteDatabase.openDatabase(
                dbFile.absolutePath, null, SQLiteDatabase.OPEN_READONLY
            )
            block(db)
        } catch (e: Exception) {
            null
        } finally {
            try {
                db?.close()
            } catch (ignored: Exception) {
            }
        }
    }

    fun latestSession(dbFile: File): SessionInfo? = read(dbFile) { db ->
        db.rawQuery(
            "SELECT id, started_at, title FROM sessions ORDER BY id DESC LIMIT 1",
            null
        ).use { c ->
            if (!c.moveToFirst()) return@read null
            SessionInfo(
                id = c.getLong(0),
                startedAt = if (c.isNull(1)) 0L else c.getLong(1),
                title = if (c.isNull(2)) null else c.getString(2)
            )
        }
    }

    fun chapters(dbFile: File, sessionId: Long): List<Chapter> = read(dbFile) { db ->
        db.rawQuery(
            "SELECT start_ms, title, summary, seg_id, source, model_id" +
                    " FROM chapters WHERE session_id=? ORDER BY start_ms",
            arrayOf(sessionId.toString())
        ).use { c ->
            val out = mutableListOf<Chapter>()
            while (c.moveToNext()) {
                out.add(
                    Chapter(
                        startMs = c.getLong(0),
                        title = if (c.isNull(1)) "课堂片段" else c.getString(1),
                        summary = if (c.isNull(2)) "" else c.getString(2),
                        segId = if (c.isNull(3)) -1 else c.getLong(3),
                        source = if (c.isNull(4)) "extractive" else c.getString(4),
                        modelId = if (c.isNull(5)) "" else c.getString(5)
                    )
                )
            }
            out
        }
    } ?: emptyList()

    /** M2.7 降级原因/调用次数；旧库没有 note_meta 表时返回 null */
    fun noteMeta(dbFile: File, sessionId: Long): NoteMeta? = read(dbFile) { db ->
        db.rawQuery(
            "SELECT source, reason, llm_calls, compress_rounds," +
                    " rejected_chapters, estimated FROM note_meta WHERE session_id=?",
            arrayOf(sessionId.toString())
        ).use { c ->
            if (!c.moveToFirst()) return@read null
            NoteMeta(
                source = if (c.isNull(0)) "extractive" else c.getString(0),
                reason = if (c.isNull(1)) "" else c.getString(1),
                llmCalls = if (c.isNull(2)) 0 else c.getInt(2),
                compressRounds = if (c.isNull(3)) 0 else c.getInt(3),
                rejectedChapters = if (c.isNull(4)) 0 else c.getInt(4),
                estimated = !c.isNull(5) && c.getInt(5) != 0
            )
        }
    }

    fun segments(dbFile: File, sessionId: Long): List<Segment> = read(dbFile) { db ->
        fun query(withCorrected: Boolean): List<Segment> {
            val cols = if (withCorrected) "start_ms, text, text_corrected"
            else "start_ms, text"
            return db.rawQuery(
                "SELECT $cols FROM segments WHERE session_id=? ORDER BY start_ms, id",
                arrayOf(sessionId.toString())
            ).use { c ->
                val out = mutableListOf<Segment>()
                while (c.moveToNext()) {
                    out.add(
                        Segment(
                            startMs = c.getLong(0),
                            text = if (c.isNull(1)) "" else c.getString(1),
                            corrected = if (!withCorrected || c.isNull(2)) null
                            else c.getString(2)
                        )
                    )
                }
                out
            }
        }
        try {
            query(true)
        } catch (e: Exception) {
            query(false)   /* M2 旧库还没补 text_corrected 列 */
        }
    } ?: emptyList()

    /** 录音文件同目录的笔记文件名：classmate_20261108_101500.wav → notes_20261108_101500.md */
    fun noteFile(dir: File, stamp: String): File = File(dir, "notes_$stamp.md")
}
