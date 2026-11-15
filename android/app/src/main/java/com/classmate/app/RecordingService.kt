package com.classmate.app

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.util.Log

/**
 * M2 前台服务：锁屏后继续录音；模型就绪时同时跑 sherpa 流式识别。
 *
 * 关键点（Android 14 / targetSdk 34）：
 *  - onCreate 里立刻 startForeground，且必须带 microphone 类型
 *  - 录音在独立线程，UI 线程不做任何音频读写
 *  - ASR 工作线程在 native 侧（jni_bridge），识别结果经事件回调回到主线程
 *  - 停止时 flush + close，WAV 头才会回填正确长度，drain 出的尾句也会落库
 *
 * M3.4：音频来源三档（线下 / 媒体内录 / 两者混音），采集封装在 AudioInput，
 * 送进 native 的仍然是同一份 16 kHz 单声道 PCM，ASR 与点名判定完全复用。
 * 内录走 MediaProjection，顺序必须是「先 startForeground(带 mediaProjection 类型)
 * → 再 getMediaProjection」，反过来 Android 14 会直接抛 SecurityException。
 */
class RecordingService : Service() {

    companion object {
        const val EXTRA_WAV_PATH = "wav_path"
        const val EXTRA_MODELS_DIR = "models_dir"   // 空串 = 本次不识别
        const val EXTRA_ASR_THREADS = "asr_threads"
        const val EXTRA_SAVE_WAV = "save_wav"   // false = 不落 WAV，只做识别与落库
        const val EXTRA_HOTWORDS = "hotwords"   // M2.6 课程热词，逗号分隔
        /* M3.4 内录：MainActivity 把系统授权结果原样带进来 */
        const val EXTRA_PROJECTION_CODE = "projection_code"
        const val EXTRA_PROJECTION_DATA = "projection_data"
        private const val TAG = "ClassMate/Record"
        private const val CHANNEL_ID = "classmate_recording"
        private const val NOTIFICATION_ID = 1
        private const val SAMPLE_RATE = AudioInput.SAMPLE_RATE
        private const val FRAMES_PER_READ = AudioInput.FRAMES   // 20 ms @ 16 kHz

        /** MainActivity 订阅字幕/错误事件；回调已切到主线程 */
        @Volatile
        var eventListener: ((type: Int, tsMs: Long, text: String?, code: Int) -> Unit)? = null

        /** 核心关闭（WAV 已收尾、DB 已关）后回调，MainActivity 在这里做课后处理 */
        @Volatile
        var onClosed: (() -> Unit)? = null
    }

    private val bridge = ClassMateBridge()

    @Volatile
    private var running = false
    private var thread: Thread? = null
    private var handle = 0L
    private var wakeLock: PowerManager.WakeLock? = null
    private var audioMode: AudioMode = AudioMode.MIC
    private var projection: MediaProjection? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
        audioMode = AudioMode.fromPref(
            getSharedPreferences("classmate_settings", MODE_PRIVATE).getString(AudioMode.KEY, null)
        )
        /* Android 14 要求：带类型的 startForeground，调用前通知必须已就绪。
         * 内录必须先以 mediaProjection 类型进入前台，之后才能 getMediaProjection。 */
        if (Build.VERSION.SDK_INT >= 29) {
            val types = if (audioMode.needsProjection) {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE or
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            } else {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            }
            val note = buildNotification("准备录音")
            try {
                startForeground(NOTIFICATION_ID, note, types)
            } catch (t: Throwable) {
                /* 个别机型/ROM 不认组合类型：退回只声明麦克风，至少不让录音起不来 */
                Log.w(TAG, "startForeground(types=$types) failed: ${t.message}, fallback")
                startForeground(
                    NOTIFICATION_ID, note, ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
                )
            }
        } else {
            startForeground(NOTIFICATION_ID, buildNotification("准备录音"))
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (running) return START_NOT_STICKY

        val path = intent?.getStringExtra(EXTRA_WAV_PATH)
        if (path == null) {
            Log.e(TAG, "missing wav path")
            stopSelf()
            return START_NOT_STICKY
        }

        val modelsDir = intent.getStringExtra(EXTRA_MODELS_DIR) ?: ""
        val asrThreads = intent.getIntExtra(EXTRA_ASR_THREADS, 4)
        val saveWav = intent.getBooleanExtra(EXTRA_SAVE_WAV, true)
        val hotwords = intent.getStringExtra(EXTRA_HOTWORDS) ?: ""
        val mainHandler = Handler(Looper.getMainLooper())

        /* 内录授权：拿不到就直接失败提示，绝不让用户以为在录、其实一个字幕都不出 */
        if (audioMode.needsProjection) {
            projection = createProjection(intent)
            if (projection == null) {
                Log.e(TAG, "media projection missing/denied mode=${audioMode.pref}")
                updateNotification("内录授权失败：请重新开始录音")
                mainHandler.post {
                    eventListener?.invoke(
                        ClassMateBridge.EV_ERROR, 0L,
                        "内录未授权，无法录到手机播放的声音", 2
                    )
                }
                stopSelf()
                return START_NOT_STICKY
            }
        }

        val qa = QaEngine.get(this)
        /* P0-2：把本场 WAV 路径交给 QA，用来反查本场 session_id；
         * 反查不到时上下文为空，绝不跨场次取历史字幕 */
        qa.onSessionStart(path)
        val listener = ClassMateBridge.Listener { type, tsMs, text, _, code ->
            /* M3：点名判定挂在服务里（Activity 退后台会被 ColorOS 冻结）。
             * onFinal 只入队立即返回，判定与网络都在 QA 单线程上。 */
            if (type == ClassMateBridge.EV_ASR_FINAL && !text.isNullOrBlank()) {
                qa.onFinal(tsMs, text)
            }
            mainHandler.post { eventListener?.invoke(type, tsMs, text, code) }
        }

        handle = bridge.nativeCreate(
            path, modelsDir, asrThreads, saveWav, hotwords, listener
        )
        if (handle == 0L) {
            Log.e(TAG, "nativeCreate failed, see core error log")
            updateNotification("无法创建 WAV 文件")
            stopSelf()
            return START_NOT_STICKY
        }
        val asrOn = modelsDir.isNotEmpty()

        acquireWakeLock()
        running = true
        thread = Thread({ recordLoop() }, "classmate-audio").also { it.start() }
        updateNotification(if (asrOn) "正在录音 + 识别（可锁屏）" else "正在录音（可锁屏）")
        return START_NOT_STICKY
    }

    /**
     * 按系统授权结果换 MediaProjection。
     * Android 14 起必须先 registerCallback，否则开始采集会抛 IllegalStateException。
     */
    private fun createProjection(intent: Intent?): MediaProjection? {
        val code = intent?.getIntExtra(EXTRA_PROJECTION_CODE, Activity.RESULT_CANCELED)
            ?: Activity.RESULT_CANCELED
        val data: Intent? = if (Build.VERSION.SDK_INT >= 33) {
            intent?.getParcelableExtra(EXTRA_PROJECTION_DATA, Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent?.getParcelableExtra(EXTRA_PROJECTION_DATA)
        }
        if (code != Activity.RESULT_OK || data == null) return null
        val manager = getSystemService(MediaProjectionManager::class.java) ?: return null
        val p = try {
            manager.getMediaProjection(code, data)
        } catch (t: Throwable) {
            Log.e(TAG, "getMediaProjection failed: ${t.message}")
            null
        } ?: return null
        /* Android 14（API 34）起：必须先注册回调再开始采集，否则抛 IllegalStateException。
         * 低版本没有这个公开方法，跳过即可——用 Throwable 兜住 NoSuchMethodError，
         * 否则旧机型会直接崩在录音启动那一刻。 */
        if (Build.VERSION.SDK_INT >= 34) {
            try {
                p.registerCallback(object : MediaProjection.Callback() {
                    override fun onStop() {
                        /* 用户点了系统「停止共享」/系统回收：马上收尾，不留假录音 */
                        Log.w(TAG, "projection stopped by system")
                        stopSelf()
                    }
                }, Handler(Looper.getMainLooper()))
            } catch (t: Throwable) {
                Log.w(TAG, "registerCallback failed: ${t.message}")
            }
        }
        return p
    }

    private fun recordLoop() {
        val input = AudioInput.open(this, audioMode, projection)
        if (input == null) {
            Log.e(TAG, "audio input open failed mode=${audioMode.pref}")
            val msg = when (audioMode) {
                AudioMode.MIC -> "麦克风打不开（可能被其他 App 占用）"
                AudioMode.PLAYBACK -> "媒体内录打不开（授权失效或该 App 不允许被采集）"
                AudioMode.BOTH -> "混音打不开（麦克风被占用或内录授权失效）"
            }
            updateNotification(msg)
            Handler(Looper.getMainLooper()).post {
                eventListener?.invoke(ClassMateBridge.EV_ERROR, 0L, msg, 3)
            }
            return
        }
        input.start()

        val buffer = ShortArray(FRAMES_PER_READ)
        var tsMs = 0L
        /* M2.6 时间基准：tsMs 来自帧计数；同时用 nanoTime 墙钟做第二轨，
         * 每分钟对一次账，drift 超阈值会打到日志，便于判断时间戳是否可信 */
        val t0Ns = System.nanoTime()
        var lastDriftLogMs = 0L
        var silentWarned = false
        try {
            while (running) {
                val n = input.read(buffer, FRAMES_PER_READ)
                when {
                    n > 0 -> {
                        val rc = bridge.nativeFeed(handle, buffer, n, tsMs)
                        if (rc != 0) {
                            Log.e(TAG, "nativeFeed rc=$rc")
                            break
                        }
                        tsMs += n * 1000L / SAMPLE_RATE
                        /* 内录最容易「静默失败」：30 秒还没采到声音就提醒一次 */
                        if (!silentWarned && audioMode.needsProjection && tsMs > 30_000L
                            && !input.playbackHeardAudio
                        ) {
                            silentWarned = true
                            Log.w(TAG, "playback silent after ${tsMs}ms")
                            Handler(Looper.getMainLooper()).post {
                                eventListener?.invoke(
                                    ClassMateBridge.EV_ERROR, tsMs,
                                    "内录还没采到声音：检查媒体音量、或该 App 不允许被录制", 4
                                )
                            }
                        }
                        if (tsMs - lastDriftLogMs >= 60_000L) {
                            lastDriftLogMs = tsMs
                            val wallMs = (System.nanoTime() - t0Ns) / 1_000_000L
                            Log.i(TAG, "drift: wall=${wallMs}ms frame=${tsMs}ms " +
                                    "diff=${wallMs - tsMs}ms mode=${audioMode.pref}")
                        }
                    }
                    n < 0 -> {
                        Log.e(TAG, "audio read error=$n mode=${audioMode.pref}")
                        break
                    }
                }
            }
        } catch (e: SecurityException) {
            Log.e(TAG, "no audio permission", e)
        } catch (e: Exception) {
            Log.e(TAG, "recordLoop failed", e)
        } finally {
            input.close()
        }
    }

    override fun onDestroy() {
        running = false
        thread?.join(2000)
        if (handle != 0L) {
            bridge.nativeFlush(handle)
            bridge.nativeDestroy(handle)   /* 内含 ca_shutdown：先收尾会话再关库 */
            handle = 0L
            /* R2：core 的收尾 UPDATE 会静默丢，这里幂等补一次；先落库再清 QA 上下文 */
            QaEngine.get(this).onSessionStop()
        }
        releaseWakeLock()
        /* 内录句柄必须显式释放，否则下次录音拿到的授权是「已停」状态，只能录到静音 */
        try { projection?.stop() } catch (ignored: Throwable) {}
        projection = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        super.onDestroy()

        /* DB / WAV 已关闭，主线程再做课后处理（校正 → 章节 → 笔记） */
        val closed = onClosed
        if (closed != null) Handler(Looper.getMainLooper()).post { closed.invoke() }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createChannel() {
        val manager = getSystemService(NotificationManager::class.java)
        val channel = NotificationChannel(
            CHANNEL_ID,
            "课堂录音",
            NotificationManager.IMPORTANCE_LOW
        )
        manager.createNotificationChannel(channel)
    }

    private fun buildNotification(text: String): Notification {
        val intent = Intent(this, MainActivity::class.java)
        val pending = PendingIntent.getActivity(
            this, 0, intent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("ClassMate")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .setContentIntent(pending)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification(text: String) {
        getSystemService(NotificationManager::class.java)
            .notify(NOTIFICATION_ID, buildNotification(text))
    }

    private fun acquireWakeLock() {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ClassMate:recording")
            .apply { acquire(2 * 60 * 60 * 1000L) }
    }

    private fun releaseWakeLock() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
    }
}
