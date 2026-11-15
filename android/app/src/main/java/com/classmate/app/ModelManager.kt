package com.classmate.app

import android.content.Context
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * M2 模型管理：按 assets 里的 manifest.json 把多个模型文件下载到私有目录。
 *
 * 约定：
 *  - 目标路径 filesDir/models/<name>（sherpa 要求 encoder/decoder/tokens/VAD 同目录）
 *  - 下载中写 .tmp，校验通过后原子改名；支持 Range 断点续传，urls 按顺序回退
 *  - sha256 为空串时跳过校验（不信任则不下载，由用户自行 adb push）
 *  - 全部方法都在调用方线程执行，UI 侧自行包线程
 */
object ModelManager {

    data class ModelEntry(
        val name: String,
        val kind: String,
        val sizeBytes: Long,
        val sha256: String,
        val urls: List<String>
    )

    class DownloadException(message: String) : Exception(message)

    /** 读取 manifest 全量条目；清单缺失或格式错误返回 null */
    fun loadModels(context: Context): List<ModelEntry>? {
        return try {
            val text = context.assets.open("manifest.json").bufferedReader().use { it.readText() }
            val arr = JSONObject(text).getJSONArray("models")
            val out = mutableListOf<ModelEntry>()
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                val urls = mutableListOf<String>()
                val u = o.getJSONArray("urls")
                for (j in 0 until u.length()) urls.add(u.getString(j))
                out.add(
                    ModelEntry(
                        name = o.getString("name"),
                        kind = o.optString("kind", "asr"),
                        sizeBytes = o.optLong("size_bytes", 0),
                        sha256 = o.optString("sha256", ""),
                        urls = urls
                    )
                )
            }
            out.ifEmpty { null }
        } catch (e: Exception) {
            null
        }
    }

    fun modelsDir(context: Context): File = File(context.filesDir, "models")

    fun modelFile(context: Context, entry: ModelEntry): File =
        File(modelsDir(context), entry.name)

    /** 单个文件就绪 = 存在且大小匹配（sha256 只在下载后校验一次，避免每次启动全量哈希） */
    fun isReady(context: Context, entry: ModelEntry): Boolean {
        val f = modelFile(context, entry)
        if (!f.exists() || f.length() == 0L) return false
        return entry.sizeBytes <= 0 || f.length() == entry.sizeBytes
    }

    /** 全部（或指定分组）文件就绪；清单缺失视为未就绪 */
    fun isReadyAll(context: Context, filter: ((ModelEntry) -> Boolean)? = null): Boolean {
        val entries = loadModels(context) ?: return false
        val target = if (filter == null) entries else entries.filter(filter)
        return target.isNotEmpty() && target.all { isReady(context, it) }
    }

    fun totalBytes(entries: List<ModelEntry>): Long =
        entries.sumOf { if (it.sizeBytes > 0) it.sizeBytes else 0L }

    /**
     * 下载全部缺失文件。onProgress 为整体进度 0..100（拿不到总长时 -1）。
     * 已就绪的文件会跳过；任一文件失败抛 DownloadException。
     */
    fun downloadAll(
        context: Context,
        filter: ((ModelEntry) -> Boolean)? = null,
        onProgress: (Int) -> Unit
    ) {
        val all = loadModels(context) ?: throw DownloadException("manifest 缺失")
        val entries = if (filter == null) all else all.filter(filter)
        if (entries.isEmpty()) throw DownloadException("没有可下载的条目")
        val total = totalBytes(entries)
        var doneBase = entries.filter { isReady(context, it) }
            .sumOf { if (it.sizeBytes > 0) it.sizeBytes else 0L }

        for (entry in entries) {
            if (isReady(context, entry)) continue
            val size = if (entry.sizeBytes > 0) entry.sizeBytes else 0L
            download(context, entry) { p ->
                if (total > 0 && size > 0 && p >= 0) {
                    val done = doneBase + size * p / 100
                    onProgress(((done * 100) / total).toInt().coerceIn(0, 99))
                } else {
                    onProgress(-1)
                }
            }
            doneBase += size
        }
        onProgress(100)
    }

    /** 单文件下载（可断点续传）；成功返回模型文件 */
    fun download(
        context: Context,
        entry: ModelEntry,
        onProgress: (Int) -> Unit
    ): File {
        val target = modelFile(context, entry)
        val tmp = File(target.parentFile, target.name + ".tmp")
        target.parentFile?.mkdirs()

        var lastError = ""
        for (url in entry.urls) {
            try {
                downloadFrom(url, tmp, entry.sizeBytes, onProgress)
                if (entry.sha256.isNotEmpty() && !verifySha256(tmp, entry.sha256)) {
                    lastError = "sha256 校验失败"
                    tmp.delete()
                    continue
                }
                if (target.exists()) target.delete()
                if (!tmp.renameTo(target)) {
                    tmp.copyTo(target, overwrite = true)
                    tmp.delete()
                }
                return target
            } catch (e: Exception) {
                lastError = "${url.substringBefore("/resolve")}: ${e.message}"
            }
        }
        throw DownloadException("所有镜像都失败：$lastError")
    }

    private fun downloadFrom(url: String, tmp: File, totalHint: Long, onProgress: (Int) -> Unit) {
        val existing = if (tmp.exists()) tmp.length() else 0L
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15000
            readTimeout = 30000
            if (existing > 0) setRequestProperty("Range", "bytes=$existing-")
            instanceFollowRedirects = true
        }
        conn.connect()
        val code = conn.responseCode
        if (code !in 200..299) {
            conn.disconnect()
            throw DownloadException("HTTP $code")
        }
        // 服务器不支持续传时从头下
        val append = (code == 206 && existing > 0)
        val total = when {
            code == 206 && totalHint > 0 -> totalHint
            conn.contentLengthLong > 0 -> conn.contentLengthLong + (if (append) existing else 0)
            else -> totalHint
        }

        conn.inputStream.use { input ->
            val out = if (append) {
                FileOutputStream(tmp, true)
            } else {
                if (tmp.exists()) tmp.delete()
                FileOutputStream(tmp)
            }
            out.use { output ->
                val buf = ByteArray(64 * 1024)
                var done = if (append) existing else 0L
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    output.write(buf, 0, n)
                    done += n
                    if (total > 0) onProgress(((done * 100) / total).toInt().coerceIn(0, 99)) else onProgress(-1)
                }
            }
        }
        conn.disconnect()
    }

    private fun verifySha256(file: File, expected: String): Boolean {
        val md = MessageDigest.getInstance("SHA-256")
        RandomAccessFile(file, "r").use { raf ->
            val buf = ByteArray(1024 * 1024)
            while (true) {
                val n = raf.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
            }
        }
        return md.digest().joinToString("") { "%02x".format(it) } == expected.lowercase()
    }
}
