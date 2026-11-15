package com.classmate.app

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.projection.MediaProjection
import android.util.Log
import java.io.Closeable

/**
 * 音频来源三档（写进 SharedPreferences，值不要改，改了老用户设置会失效）。
 */
enum class AudioMode(val pref: String, val label: String) {
    MIC("mic", "线下声音"),
    PLAYBACK("playback", "媒体音频"),
    BOTH("both", "两者一起");

    /** 后两种必须先拿到系统「录屏/投屏」授权才能采集 */
    val needsProjection: Boolean get() = this != MIC

    companion object {
        const val KEY = "audio_source_mode"

        fun fromPref(v: String?): AudioMode {
            for (m in values()) if (m.pref == v) return m
            return MIC
        }
    }
}

/**
 * M3.4 统一音频输入：不管哪种模式，读出来的都是 16 kHz / 单声道 / PCM16，
 * 下游 nativeFeed 完全不用改。
 *
 *  - MIC      = MediaRecorder.AudioSource.MIC（线下课堂，与 M2 完全一致）
 *  - PLAYBACK = AudioPlaybackCaptureConfiguration（录本机 App 播放的声音，需 MediaProjection）
 *  - BOTH     = 两个 AudioRecord 同时开，软件混音；**麦克风为主时钟**，
 *               媒体流进环形缓冲，麦克风每读一块就从缓冲取等长样本相加。
 *               缓冲快满时丢掉最旧的一小段，避免两条流的时钟漂移越攒越大。
 *
 * 内录的系统硬限制（改代码绕不过去，已写进交付文档）：
 *  1. 目标 App 自己声明了「不允许被采集」（FLAG_SECURE / ALLOW_CAPTURE_BY_NONE）→ 只能录到静音；
 *  2. 通话类音频（USAGE_VOICE_COMMUNICATION）系统不允许采集 → 微信语音/会议语音可能拿不到，
 *     视频播放、网课、B 站这类走 USAGE_MEDIA 的能录到；
 *  3. 版权保护音频（Netflix 等 DRM）拿不到；
 *  4. 手机媒体音量为 0 时，内录同样是静音（用户以为坏了）。
 */
class AudioInput private constructor(
    private val mode: AudioMode,
    private val mic: AudioRecord?,
    private val playback: AudioRecord?,
    /** 媒体流实际采样率，16000 时不需要降采样 */
    private val playbackRate: Int
) : Closeable {

    companion object {
        private const val TAG = "ClassMate/Audio"
        const val SAMPLE_RATE = 16000
        const val FRAMES = 320                 // 20 ms @ 16 kHz
        private const val RING_SECONDS = 3      // 混音缓冲上限，超过就丢最旧的一段
        private const val DROP_RATIO = 8        // 溢出时一次丢 1/8

        /**
         * 打开失败返回 null（无权限 / 设备占用 / 系统不允许内录），
         * 调用方负责提示用户，绝不让录音静默变成空文件。
         */
        fun open(context: Context, mode: AudioMode, projection: MediaProjection?): AudioInput? {
            var micRec: AudioRecord? = null
            var pbRec: AudioRecord? = null
            var pbRate = SAMPLE_RATE

            fun bail(reason: String): AudioInput? {
                Log.e(TAG, "open failed mode=${mode.pref}: $reason")
                try { micRec?.release() } catch (ignored: Throwable) {}
                try { pbRec?.release() } catch (ignored: Throwable) {}
                return null
            }

            try {
                if (mode != AudioMode.PLAYBACK) {
                    micRec = openMic() ?: return bail("麦克风 AudioRecord 初始化失败（权限或占用）")
                    micRec.startRecording()
                    if (micRec.recordingState != AudioRecord.RECORDSTATE_RECORDING) {
                        return bail("麦克风启动失败 recordingState=${micRec.recordingState}")
                    }
                }
                if (mode != AudioMode.MIC) {
                    if (projection == null) return bail("缺 MediaProjection 授权")
                    val opened = openPlayback(projection)
                        ?: return bail("媒体内录初始化失败（授权失效或系统不允许采集）")
                    pbRec = opened.first
                    pbRate = opened.second
                    pbRec.startRecording()
                    if (pbRec.recordingState != AudioRecord.RECORDSTATE_RECORDING) {
                        return bail("媒体内录启动失败 recordingState=${pbRec.recordingState}")
                    }
                }
                return AudioInput(mode, micRec, pbRec, pbRate)
            } catch (e: SecurityException) {
                return bail("SecurityException: ${e.message}")
            } catch (e: Exception) {
                return bail("${e.javaClass.simpleName}: ${e.message}")
            }
        }

        private fun openMic(): AudioRecord? {
            val minBuf = AudioRecord.getMinBufferSize(
                SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT
            )
            val bufSize = maxOf(minBuf, FRAMES * 2) * 2
            val r = AudioRecord(
                MediaRecorder.AudioSource.MIC,
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                bufSize
            )
            if (r.state != AudioRecord.STATE_INITIALIZED) {
                r.release()
                return null
            }
            return r
        }

        /**
         * 内录：先按 16 kHz 直接开；部分机型不接受 16 kHz 播放采集，
         * 退到 48 kHz，由 pump 里的三点平均降采样还原成 16 kHz。
         */
        private fun openPlayback(projection: MediaProjection): Pair<AudioRecord, Int>? {
            for (rate in intArrayOf(SAMPLE_RATE, 48000)) {
                try {
                    val cfg = AudioPlaybackCaptureConfiguration.Builder(projection)
                        .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                        .addMatchingUsage(AudioAttributes.USAGE_GAME)
                        .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                        .addMatchingUsage(AudioAttributes.USAGE_ASSISTANT)
                        .build()
                    val minBuf = AudioRecord.getMinBufferSize(
                        rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT
                    )
                    val factor = maxOf(1, rate / SAMPLE_RATE)
                    val bufSize = maxOf(minBuf, FRAMES * factor * 2) * 2
                    val r = AudioRecord.Builder()
                        .setAudioFormat(
                            AudioFormat.Builder()
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .setSampleRate(rate)
                                .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                                .build()
                        )
                        .setBufferSizeInBytes(bufSize)
                        .setAudioPlaybackCaptureConfig(cfg)
                        .build()
                    if (r.state == AudioRecord.STATE_INITIALIZED) {
                        Log.i(TAG, "playback capture @${rate}Hz ok")
                        return r to rate
                    }
                    r.release()
                } catch (e: Exception) {
                    Log.w(TAG, "playback @${rate}Hz failed: ${e.message}")
                }
            }
            return null
        }
    }

    private val lock = Any()
    private val ring = ShortArray(RING_SECONDS * SAMPLE_RATE)
    private var ringHead = 0
    private var ringSize = 0

    @Volatile private var pbRunning = false
    private var pbThread: Thread? = null
    private val downFactor = maxOf(1, playbackRate / SAMPLE_RATE)

    /** 媒体流是否真的采到过非静音样本（用来提示「内录静音」而不是让用户瞎猜） */
    @Volatile var playbackHeardAudio = false
        private set

    fun start() {
        /* 只有混音模式才需要后台泵：纯内录在前台直接读同一个 AudioRecord，
         * 两处同时读会让样本互相抢走（一半数据丢失）。 */
        if (mode != AudioMode.BOTH || playback == null) return
        pbRunning = true
        pbThread = Thread({ pumpPlayback() }, "classmate-pb").also { it.start() }
    }

    /** 与原来 AudioRecord.read 同签名：返回写入 dst 的样本数，负数=错误 */
    fun read(dst: ShortArray, frames: Int): Int {
        val m = mic
        if (m == null) {
            /* 纯内录模式：直接读媒体流并即时降采样 */
            return readPlaybackDirect(dst, frames)
        }
        val n = m.read(dst, 0, frames)
        if (n <= 0) return n
        if (playback == null) return n
        synchronized(lock) {
            val take = minOf(n, ringSize)
            for (i in 0 until take) {
                val pv = ring[ringHead].toInt()
                ringHead = (ringHead + 1) % ring.size
                ringSize--
                val s = dst[i].toInt() + pv
                dst[i] = when {
                    s > Short.MAX_VALUE -> Short.MAX_VALUE
                    s < Short.MIN_VALUE -> Short.MIN_VALUE
                    else -> s.toShort()
                }
            }
        }
        return n
    }

    /* ---------- 纯内录：主线程直接读，不做环形缓冲 ---------- */

    private val direct = ShortArray(FRAMES * 4)
    private val directOut = ShortArray(FRAMES * 4)

    private fun readPlaybackDirect(dst: ShortArray, frames: Int): Int {
        val p = playback ?: return 0
        val need = frames * downFactor
        val arr = if (need <= direct.size) direct else ShortArray(need)
        val n = p.read(arr, 0, need)
        if (n <= 0) return n
        if (downFactor == 1) {
            System.arraycopy(arr, 0, dst, 0, n)
            noteLevel(arr, n)
            return n
        }
        val outN = n / downFactor
        var peak = 0
        for (i in 0 until outN) {
            var acc = 0
            val base = i * downFactor
            for (k in 0 until downFactor) acc += arr[base + k].toInt()
            val v = acc / downFactor
            dst[i] = v.toShort()
            if (v > peak) peak = v
            if (-v > peak) peak = -v
        }
        if (peak > 300) playbackHeardAudio = true
        return outN
    }

    /* ---------- 混音：后台线程把媒体流灌进环形缓冲 ---------- */

    private fun pumpPlayback() {
        val p = playback ?: return
        val need = FRAMES * downFactor
        val buf = ShortArray(need)
        while (pbRunning) {
            val n = try {
                p.read(buf, 0, need)
            } catch (e: Exception) {
                Log.e(TAG, "playback read failed: ${e.message}")
                -1
            }
            if (n < 0) break
            if (n == 0) continue
            noteLevel(buf, n)
            val outN = if (downFactor == 1) n else n / downFactor
            synchronized(lock) {
                for (i in 0 until outN) {
                    val v = if (downFactor == 1) buf[i].toInt()
                    else {
                        var acc = 0
                        val base = i * downFactor
                        for (k in 0 until downFactor) acc += buf[base + k].toInt()
                        acc / downFactor
                    }
                    pushLocked(v.toShort())
                }
            }
        }
        Log.i(TAG, "playback pump exit")
    }

    private fun pushLocked(v: Short) {
        if (ringSize >= ring.size) {
            val drop = ring.size / DROP_RATIO
            ringHead = (ringHead + drop) % ring.size
            ringSize -= drop
        }
        ring[(ringHead + ringSize) % ring.size] = v
        ringSize++
    }

    private fun noteLevel(arr: ShortArray, n: Int) {
        if (playbackHeardAudio) return
        var peak = 0
        for (i in 0 until n) {
            val v = arr[i].toInt()
            if (v > peak) peak = v
            if (-v > peak) peak = -v
        }
        if (peak > 300) playbackHeardAudio = true   /* 约 -40 dBFS，高于线噪 */
    }

    override fun close() {
        pbRunning = false
        try { pbThread?.join(400) } catch (ignored: InterruptedException) {}
        pbThread = null
        try { mic?.stop() } catch (ignored: Throwable) {}
        try { playback?.stop() } catch (ignored: Throwable) {}
        try { mic?.release() } catch (ignored: Throwable) {}
        try { playback?.release() } catch (ignored: Throwable) {}
    }
}
