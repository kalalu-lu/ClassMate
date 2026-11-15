package com.classmate.app

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.io.File

/**
 * M2.5：录音文件的体积统计、按策略清理、导出。
 *
 * 只碰 `recordings/` 下的 .wav；`classmate.db` 不参与清理（清空操作也不删库，
 * 这样字幕记录仍在）。WAV 头格式由 C core 负责，这里不做任何改写。
 */
object RecordingStore {

    private const val GB = 1024L * 1024L * 1024L

    fun dir(context: Context): File = File(context.getExternalFilesDir(null), "recordings")

    /** 目录内的 WAV，按修改时间从新到旧 */
    fun wavFiles(context: Context): List<File> =
        dir(context).listFiles { f -> f.isFile && f.name.endsWith(".wav") }
            ?.sortedByDescending { it.lastModified() }
            ?: emptyList()

    fun totalBytes(context: Context): Long =
        dir(context).listFiles()?.filter { it.isFile }?.sumOf { it.length() } ?: 0L

    fun latest(context: Context): File? = wavFiles(context).firstOrNull()

    /**
     * 启动时执行：先满足「保留最近 keepCount 个」，再满足「总量 ≤ maxGb」。
     * 两者都从最旧的开始删；正在录制的新文件因为最新，不会被删掉。
     */
    fun cleanup(
        context: Context,
        keepCount: Int,
        maxGb: Float,
        protected: Set<String> = emptySet()
    ): Int {
        val files = wavFiles(context)                 // 新 → 旧
        val maxBytes = (maxGb * GB).toLong()
        var kept = 0
        var keptBytes = 0L
        var removed = 0

        for (f in files) {
            /* 课后处理失败待重试的临时 WAV 不能被启动清理顺手删掉 */
            if (f.absolutePath in protected) {
                kept++
                keptBytes += f.length()
                continue
            }
            val overCount = keepCount > 0 && kept >= keepCount
            val overSize = maxBytes > 0 && keptBytes + f.length() > maxBytes
            if (overCount || overSize) {
                if (f.delete()) removed++
            } else {
                kept++
                keptBytes += f.length()
            }
        }
        return removed
    }

    fun clear(context: Context): Int {
        var removed = 0
        for (f in wavFiles(context)) if (f.delete()) removed++
        return removed
    }

    /** 复制到系统「下载/ClassMate」；Android 10 以下返回 null（无 MediaStore.Downloads）。 */
    fun export(context: Context, file: File): Uri? = exportAs(context, file, "audio/wav")

    /** M4.3 通用导出：笔记 .md 与 WAV 走同一条 MediaStore 通道，落到 下载/ClassMate/
     *  mime 必须与扩展名匹配：用 text/plain 时 MediaStore 会把 .md 规范化成 .md.txt，
     *  所以 markdown 要用 text/markdown（Android 10+ 的 MimeTypeMap 认得 md）。 */
    fun exportAs(context: Context, file: File, mime: String): Uri? {
        if (Build.VERSION.SDK_INT < 29) return null

        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, file.name)
            put(MediaStore.Downloads.MIME_TYPE, mime)
            put(MediaStore.Downloads.RELATIVE_PATH,
                Environment.DIRECTORY_DOWNLOADS + "/ClassMate")
            put(MediaStore.Downloads.IS_PENDING, 1)
        }
        val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: return null

        try {
            resolver.openOutputStream(uri)?.use { out ->
                file.inputStream().use { it.copyTo(out) }
            }
        } catch (e: Exception) {
            resolver.delete(uri, null, null)
            return null
        }

        values.clear()
        values.put(MediaStore.Downloads.IS_PENDING, 0)
        resolver.update(uri, values, null, null)
        return uri
    }
}
