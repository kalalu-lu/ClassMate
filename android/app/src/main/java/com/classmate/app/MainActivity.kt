package com.classmate.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.ContentValues
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.text.SpannableString
import android.text.Spanned
import android.text.method.LinkMovementMethod
import android.text.style.ClickableSpan
import android.util.Log
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioGroup
import android.widget.ScrollView
import android.database.sqlite.SQLiteDatabase
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * M2.7 / M4 薄壳 UI：
 *  - 音频三档：不存 / 临时（默认，课后处理完删）/ 长期保留
 *  - 字幕逐行列表；点章节只滚动+高亮到对应文字段落（不再播放音频，M2.7 范围收缩）
 *  - 章节与笔记只依赖 segments 文本，任何档位都生成
 *  - AI 三档：始终本地 / 自动（默认）/ 云端优先；Key 缺失或云端失败时 core 自动降级
 */
class MainActivity : Activity() {

    private lateinit var statusText: TextView
    private lateinit var pathText: TextView
    private lateinit var toggleButton: Button
    private lateinit var modelStatusText: TextView
    private lateinit var downloadButton: Button
    private lateinit var correctionStatusText: TextView
    private lateinit var correctionDownloadButton: Button
    private lateinit var subtitleList: LinearLayout
    private lateinit var subtitleScroll: ScrollView
    private lateinit var backToLatest: Button
    /* 用户是否停留在字幕底部：在底部才自动滚，上滑即停 */
    private var userAtBottom = true
    private lateinit var chapterList: LinearLayout
    private lateinit var chapterScroll: ScrollView
    private lateinit var audioPolicyGroup: RadioGroup
    private lateinit var keepCountEdit: EditText
    private lateinit var maxGbEdit: EditText
    private lateinit var storageText: TextView
    private lateinit var exportButton: Button
    private lateinit var clearButton: Button
    private lateinit var notesButton: Button
    private lateinit var retryButton: Button
    private lateinit var replayButton: Button
    /* M3.5 点名叫答卡片：字幕的兄弟视图。出现时字幕自动让位，不再被遮挡。 */
    private lateinit var qaCard: View
    private lateinit var qaCardScroll: ScrollView
    private lateinit var qaCardHandle: View
    private lateinit var qaCardTitle: TextView
    private lateinit var qaCardBody: TextView
    private lateinit var qaCardNote: TextView
    private lateinit var qaCardCopy: Button
    private lateinit var settingsToggle: Button
    /* 设置面板默认收起；展开时自身可滚、高度限屏高 40%，不再挤压字幕区 */
    private var settingsShown = false
    private lateinit var settingsScroll: ScrollView
    private lateinit var chapterLabel: TextView
    /* M3.5 文本回放：重放真实场次的字幕（WAV 按 A 档已删，判定链路仍可复现） */
    private var qaCardUserHeight = 0   /* 用户拖出来的高度，0 = 跟随内容自适应 */
    private var textReplayRunning = false
    private var textReplayToken = 0
    /* 滚动 post 合并：同一时刻至多一个 fullScroll 排队 */
    private var scrollPending = false
    /* 一次性 WAV 回放验证进行中：期间暂停任何 WAV 清理 */
    private var replaying = false
    private lateinit var rollingStatusText: TextView
    private lateinit var hotwordsEdit: EditText
    private lateinit var timeModeCheck: CheckBox
    private lateinit var correctionCheck: CheckBox
    private lateinit var llmStatusText: TextView
    private lateinit var llmConfigButton: Button
    private lateinit var llmPolicyGroup: RadioGroup
    private lateinit var localLlmStatusText: TextView
    private lateinit var localLlmDownloadButton: Button
    /* M3.4 音频来源与自动答题开关 */
    private lateinit var audioSourceGroup: RadioGroup
    private lateinit var qaAutoCheck: CheckBox

    private val prefs by lazy { getSharedPreferences("classmate_settings", MODE_PRIVATE) }
    private val handler = Handler(Looper.getMainLooper())
    private val bridge = ClassMateBridge()

    private var recording = false
    private var postRunning = false
    private var startAtMs = 0L
    private var sessionStartedAt = 0L
    private var sessionId = -1L
    private var sessionStamp = ""
    private var sessionPolicy = POLICY_TEMP
    private var sessionSavesWav = true
    private var currentWav: File? = null

    private var modelEntries: List<ModelManager.ModelEntry> = emptyList()
    private var downloading = false
    private var downloadingCorrection = false

    /* 字幕行缓存：ts → 视图，用于点章节/笔记锚点跳转与高亮 */
    private data class SubRow(val ts: Long, val text: String, val corrected: String?)
    private val liveRows = mutableListOf<SubRow>()
    private val rowViews = mutableListOf<TextView>()
    private var partialView: TextView? = null
    /* 实时字幕证据：日志里能看到录音过程中逐条上屏，而不是停止后才补 */
    private var livePartialCount = 0
    private val latestChapters = mutableListOf<SessionStore.Chapter>()
    private var latestMeta: SessionStore.NoteMeta? = null

    /* M2.8 主路径：会中云端增量滚动摘要（录音中后台跑，不阻塞录音） */
    private var rolling: RollingSummarizer? = null
    private var postStopAt = 0L

    private val ticker = object : Runnable {
        override fun run() {
            if (!recording) return
            val sec = (System.currentTimeMillis() - startAtMs) / 1000
            statusText.text = String.format("正在录音  %02d:%02d", sec / 60, sec % 60)
            refreshStorage()
            handler.postDelayed(this, 1000)
        }
    }

    /* 每 30 s 看一次是否到 2 分钟；真正发请求由 RollingSummarizer 自己节流 */
    private val rollingTick = object : Runnable {
        override fun run() {
            if (!recording) return
            rolling?.maybeTick()
            handler.postDelayed(this, 30_000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.statusText)
        pathText = findViewById(R.id.pathText)
        toggleButton = findViewById(R.id.toggleButton)
        modelStatusText = findViewById(R.id.modelStatusText)
        downloadButton = findViewById(R.id.downloadButton)
        correctionStatusText = findViewById(R.id.correctionStatusText)
        correctionDownloadButton = findViewById(R.id.correctionDownloadButton)
        subtitleList = findViewById(R.id.subtitleList)
        subtitleScroll = findViewById(R.id.subtitleScroll)
        backToLatest = findViewById(R.id.backToLatest)
        /* 手动上滑离开底部即停自动滚并露出「回到最新」；回到底部自动恢复 */
        subtitleScroll.setOnScrollChangeListener { _, _, scrollY, _, _ ->
            val child = subtitleScroll.getChildAt(0)
            val atBottom = child == null ||
                scrollY >= child.height - subtitleScroll.height - 32
            userAtBottom = atBottom
            backToLatest.visibility = if (atBottom) View.GONE else View.VISIBLE
        }
        backToLatest.setOnClickListener {
            userAtBottom = true
            backToLatest.visibility = View.GONE
            subtitleScroll.post { subtitleScroll.fullScroll(ScrollView.FOCUS_DOWN) }
        }
        /* 长按字幕区：重命名当前场 */
        subtitleScroll.setOnLongClickListener { showRenameDialog(); true }
        chapterList = findViewById(R.id.chapterList)
        chapterScroll = findViewById(R.id.chapterScroll)
        audioPolicyGroup = findViewById(R.id.audioPolicyGroup)
        keepCountEdit = findViewById(R.id.keepCountEdit)
        maxGbEdit = findViewById(R.id.maxGbEdit)
        storageText = findViewById(R.id.storageText)
        exportButton = findViewById(R.id.exportButton)
        clearButton = findViewById(R.id.clearButton)
        notesButton = findViewById(R.id.notesButton)
        retryButton = findViewById(R.id.retryButton)
        replayButton = findViewById(R.id.replayButton)
        qaCard = findViewById(R.id.qaCard)
        qaCardScroll = findViewById(R.id.qaCardScroll)
        qaCardHandle = findViewById(R.id.qaCardHandle)
        settingsScroll = findViewById(R.id.settingsScroll)
        chapterLabel = findViewById(R.id.chapterLabel)
        qaCardTitle = findViewById(R.id.qaCardTitle)
        qaCardBody = findViewById(R.id.qaCardBody)
        qaCardNote = findViewById(R.id.qaCardNote)
        qaCardCopy = findViewById(R.id.qaCardCopy)
        /* M3.5：整卡不再可点（会把滚动/拖动手势吃掉），关闭走按钮或手柄下拖 */
        setupQaCardDrag()
        findViewById<Button>(R.id.qaCardClose).setOnClickListener { hideQaCard() }
        qaCardCopy.setOnClickListener {
            val cm = getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager
            cm.setPrimaryClip(android.content.ClipData.newPlainText("classmate_qa", qaCardBody.text))
            Toast.makeText(this, "已复制", Toast.LENGTH_SHORT).show()
        }
        rollingStatusText = findViewById(R.id.rollingStatusText)
        hotwordsEdit = findViewById(R.id.hotwordsEdit)
        settingsToggle = findViewById(R.id.settingsToggle)
        timeModeCheck = findViewById(R.id.timeModeCheck)
        correctionCheck = findViewById(R.id.correctionCheck)
        llmStatusText = findViewById(R.id.llmStatusText)
        llmConfigButton = findViewById(R.id.llmConfigButton)
        llmPolicyGroup = findViewById(R.id.llmPolicyGroup)
        localLlmStatusText = findViewById(R.id.localLlmStatusText)
        localLlmDownloadButton = findViewById(R.id.localLlmDownloadButton)
        audioSourceGroup = findViewById(R.id.audioSourceGroup)
        qaAutoCheck = findViewById(R.id.qaAutoCheck)

        pathText.text = "核心版本：${bridge.nativeVersion()}"
        toggleButton.setOnClickListener { if (recording) stopRecording() else startRecording() }
        downloadButton.setOnClickListener { startDownload(correction = false) }
        correctionDownloadButton.setOnClickListener { startDownload(correction = true) }
        localLlmDownloadButton.setOnClickListener { startDownloadLocal() }
        exportButton.setOnClickListener { exportLatest() }
        clearButton.setOnClickListener { clearRecordings() }
        notesButton.setOnClickListener { showNotes() }
        retryButton.setOnClickListener { retryPostprocess() }
        replayButton.setOnClickListener { startReplayPick() }
        llmConfigButton.setOnClickListener { showCloudSettings() }

        bindSettings()
        setupSettingsToggle()
        refreshModelStatus()
        runCleanup(showToast = false)
        refreshStorage()
        cleanupStaleRecordingSessions()
        refreshSessionUi()
        ensureSchemaAndChapters()

        RecordingService.eventListener = { type, tsMs, text, code ->
            onCoreEvent(type, tsMs, text, code)
        }
        /* M3：判定/振动/网络都在服务侧，Activity 只渲染卡片 */
        QaEngine.get(this).uiListener = { ev -> renderQaCard(ev) }
        handleReplayIntent(intent)
        handleQaIntent(intent)
        handleDevIntent(intent)
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleReplayIntent(intent)
        handleQaIntent(intent)
        handleDevIntent(intent)
    }

    /**
     * 无点击注入入口（M3.2）：
     * adb shell am start -f 0x20000000 -n com.classmate.app/.MainActivity \
     *   --es qa_test_text "有没有同学回答一下"
     * 文本直接进 QaEngine：判定 → 振动 → 通知 → 云端三句 → 卡片。
     */
    private fun handleQaIntent(i: Intent?) {
        val t = i?.getStringExtra("qa_test_text")
        if (t.isNullOrBlank()) return
        i.removeExtra("qa_test_text")
        Log.i("ClassMate/QA", "inject(intent): $t")
        QaEngine.get(this).injectTest(t)
    }

    /**
     * 无点击验证入口（M3.4）：regression 时不用手点界面——
     *   adb shell am start -n com.classmate.app/.MainActivity --es start_record mic
     *   adb shell am start -n com.classmate.app/.MainActivity --ez stop_record true
     * start_record 可选 mic / playback / both；playback 与 both 仍会弹系统
     * 「录制或投放」授权框（系统安全要求，代码绕不过），需要人点一次「立即开始」。
     */
    private fun handleDevIntent(i: Intent?) {
        if (i == null) return
        val start = i.getStringExtra("start_record")
        if (!start.isNullOrBlank()) {
            i.removeExtra("start_record")
            val mode = AudioMode.fromPref(start)
            prefs.edit().putString(AudioMode.KEY, mode.pref).apply()
            when (mode) {
                AudioMode.PLAYBACK -> audioSourceGroup.check(R.id.sourcePlayback)
                AudioMode.BOTH -> audioSourceGroup.check(R.id.sourceBoth)
                else -> audioSourceGroup.check(R.id.sourceMic)
            }
            Log.i(POST_TAG, "dev start_record mode=${mode.pref}")
            handler.post { if (!recording) startRecording() }
        }
        if (i.getBooleanExtra("stop_record", false)) {
            i.removeExtra("stop_record")
            Log.i(POST_TAG, "dev stop_record")
            handler.post { if (recording) stopRecording() }
        }
        if (i.getBooleanExtra("show_settings", false)) {
            i.removeExtra("show_settings")
            handler.post { applySettingsVisibility(true) }
        }
        if (i.getBooleanExtra("hide_settings", false)) {
            i.removeExtra("hide_settings")
            handler.post { applySettingsVisibility(false) }
        }
        if (i.getBooleanExtra("export_note", false)) {
            i.removeExtra("export_note")
            handler.post {
                val dir = File(getExternalFilesDir(null), "recordings")
                val note = if (sessionStamp.isNotEmpty()) SessionStore.noteFile(dir, sessionStamp)
                else dir.listFiles { f -> f.name.startsWith("notes_") }
                    ?.maxByOrNull { it.lastModified() }
                if (note == null || !note.exists()) {
                    Log.w(POST_TAG, "dev export_note: no note file")
                } else {
                    exportNote(note)
                }
            }
        }
        if (i.getBooleanExtra("show_notes", false)) {
            i.removeExtra("show_notes")
            handler.post { showNotes() }
        }
        val sess = i.getStringExtra("replay_session")
        if (!sess.isNullOrBlank()) {
            i.removeExtra("replay_session")
            val speed = i.getFloatExtra("replay_speed", 8f).coerceIn(1f, 60f)
            handler.postDelayed({ runSessionReplay(sess, speed) }, 800)
        }
    }

    /* ===== M3.5 文本回放：固定用 2026-11-15 12:42 那堂经济学课 =====
     * 那是唯一一次「真人说话」跑通套话判定的录音（session 45）。WAV 按 A 档在
     * 出稿成功后已删除，所以这里把它的 33 条 FINAL 字幕按真实时间轴重放，
     * 走和录音完全一致的 QaEngine.onFinal 链路（判定→振动→通知→云端→卡片）。
     * 入口：am start ... --es replay_session eco [--ei replay_speed 8]（整数倍速）
     */
    private fun runSessionReplay(which: String, speed: Float) {
        if (textReplayRunning || recording) {
            Log.w(POST_TAG, "text replay busy running=$textReplayRunning recording=$recording")
            return
        }
        val assetName = when (which.lowercase(Locale.ROOT)) {
            "eco", "last", "45", "economics" -> "replay/eco_20261115_124248.json"
            else -> if (which.endsWith(".json")) which else "replay/$which.json"
        }
        val segs = try {
            val txt = assets.open(assetName).bufferedReader().use { it.readText() }
            JSONObject(txt).getJSONArray("segments")
        } catch (e: Exception) {
            Log.e(POST_TAG, "replay asset missing: $assetName", e)
            Toast.makeText(this, "回放语料缺失：$assetName", Toast.LENGTH_SHORT).show()
            return
        }
        val total = segs.length()
        if (total == 0) return

        textReplayRunning = true
        textReplayToken++
        val token = textReplayToken
        replaying = true
        liveRows.clear()
        subtitleList.removeAllViews()
        partialView = null

        /* M4.1/M4.5：先建真实场次（status=recording），再启动会中增量 */
        val dir = File(getExternalFilesDir(null), "recordings")
        val stamp = "replay_eco_" +
                SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val sid = beginReplaySession(File(dir, "classmate.db"), stamp)
        sessionStamp = stamp
        sessionStartedAt = System.currentTimeMillis()
        currentWav = null
        sessionPolicy = POLICY_NONE
        startRolling(dir)

        val qa = QaEngine.get(this)
        qa.onSessionStart(null)   /* 清冷却与上下文缓冲，等价于「刚要开录」 */
        Log.i(POST_TAG, "text replay start asset=$assetName segs=$total speed=${speed}x " +
                "sid=$sid rolling=${rolling != null}")

        var idx = 0
        val step = object : Runnable {
            override fun run() {
                if (token != textReplayToken || !textReplayRunning) return
                if (idx >= total) {
                    textReplayRunning = false
                    replaying = false
                    Log.i(POST_TAG, "text replay done segs=$total")
                    postprocessTextReplay(segs, assetName)
                    return
                }
                val o = segs.getJSONObject(idx)
                val ts = o.optLong("start_ms")
                val text = o.optString("text")
                idx++
                if (text.isNotBlank()) {
                    val row = SubRow(ts, text, null)
                    liveRows.add(row)
                    addRowView(row)
                    scrollToBottom()
                    appendReplaySegment(ts, o.optLong("end_ms"), text)
                    rolling?.onFinal()          /* 每满 8 句自动发一批增量 */
                    qa.onFinal(ts, text)
                    Log.i(POST_TAG, "replay $idx/$total ts=${ts}ms text=$text")
                }
                val nextTs = if (idx < total) segs.getJSONObject(idx).optLong("start_ms") else ts
                handler.postDelayed(this, ((nextTs - ts) / speed).toLong().coerceAtLeast(0L))
            }
        }
        handler.post(step)
    }

    /** 点名上传文字许可：只问这一项，可单独拒绝，不影响课后摘要 */
    private fun promptQaConsent() {
        AlertDialog.Builder(this)
            .setTitle("点名助手需要上传文字")
            .setMessage(
                "老师点名时会发送「最近课堂文字 + 提问」给云端生成 3 点回答。\n" +
                        "只发文字，不发音频，也不上传录音文件。\n\n" +
                        "拒绝只影响点名助手，课后摘要照常。"
            )
            .setPositiveButton("同意") { _, _ ->
                QaEngine.get(this).setConsent(true)
                Toast.makeText(this, "已同意；重新注入或再提问即可生成", Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton("不同意") { _, _ ->
                QaEngine.get(this).setConsent(false)
            }
            .show()
    }

    private fun renderQaCard(ev: QaUiEvent) {
        /* 卡片和设置面板不同时占地方：点名时自动收起设置，空间全给答案 */
        if (settingsShown) applySettingsVisibility(false)
        qaCard.visibility = View.VISIBLE
        qaCardUserHeight = 0   /* 新一次点名：高度重新按内容自适应 */
        refreshSectionVisibility()
        when (ev) {
            is QaUiEvent.Triggered -> {
                qaCardTitle.text = "老师点名了 · 正在生成…"
                qaCardBody.text = ev.question
                qaCardNote.text = ""
                qaCardCopy.visibility = View.GONE
            }
            is QaUiEvent.Answered -> {
                qaCardCopy.visibility = View.VISIBLE
                if (ev.answer != null) {
                    qaCardTitle.text = "可以这样回答（${ev.note}）"
                    qaCardBody.text = ev.answer
                    qaCardNote.text = "latency ${ev.latencyMs}ms · tokens ${ev.promptTokens}/${ev.completionTokens}"
                } else {
                    qaCardTitle.text = "未生成答案（${ev.note}）"
                    val ctx = if (ev.fallbackContext.isBlank()) "" else "\n最近：${ev.fallbackContext}"
                    qaCardBody.text = ev.question + ctx
                    qaCardNote.text = ""
                    /* 缺点名上传许可时给出可单独同意的入口，不影响课后摘要 */
                    if (ev.note == "未同意点名上传课堂文字") promptQaConsent()
                }
            }
        }
        clampQaCard()
        Log.i("ClassMate/QA", "card: ${qaCardTitle.text}")
    }

    /** 卡片高度：内容少时贴合内容，超出上限才在卡片内滚动。
     *  M3.5 起卡片是字幕的兄弟视图，上限按屏幕算（36%），不再受字幕区高度约束，
     *  也不会再把字幕盖住。 */
    private fun clampQaCard() {
        val dm = resources.displayMetrics
        val cap = (dm.heightPixels * 0.36f).toInt()
        val handleH = (26 * dm.density).toInt()
        /* 用户手动拖过高度就尊重它（新答案进来时会重置），不再自动缩回内容高度 */
        if (qaCardUserHeight > 0) {
            val minH = handleH + (40 * dm.density).toInt()
            applyQaCardHeight(qaCardUserHeight.coerceIn(minH, cap), handleH)
            return
        }
        qaCardScroll.layoutParams = qaCardScroll.layoutParams.apply {
            height = ViewGroup.LayoutParams.WRAP_CONTENT
        }
        qaCard.post {
            val handleH = qaCardHandle.height.takeIf { it > 0 } ?: (26 * dm.density).toInt()
            val contentH = (qaCardScroll.getChildAt(0)?.height ?: 0) +
                    qaCardScroll.paddingTop + qaCardScroll.paddingBottom
            val full = handleH + contentH
            applyQaCardHeight(if (full > cap) cap else full, handleH)
            if (qaCard.visibility == View.VISIBLE) scrollToBottom()
            Log.i("ClassMate/QA", "card h=${qaCard.height} content=$contentH cap=$cap")
        }
    }

    /** 卡片定高后把手柄以外的空间全给内容滚动区，这样卡内可滚、字幕可读 */
    private fun applyQaCardHeight(total: Int, handleH: Int) {
        qaCard.layoutParams = qaCard.layoutParams.apply { height = total }
        qaCardScroll.layoutParams = qaCardScroll.layoutParams.apply {
            height = (total - handleH).coerceAtLeast(1)
        }
    }

    private fun hideQaCard() {
        qaCard.visibility = View.GONE
        qaCardUserHeight = 0
        refreshSectionVisibility()
        qaCard.layoutParams = qaCard.layoutParams.apply {
            height = ViewGroup.LayoutParams.WRAP_CONTENT
        }
        qaCardScroll.layoutParams = qaCardScroll.layoutParams.apply {
            height = ViewGroup.LayoutParams.WRAP_CONTENT
        }
        Log.i("ClassMate/QA", "card closed")
    }

    /** 手柄拖动：上拖变高、下拖变矮，松手时低于阈值就收起。
     *  只有 26dp 的手柄条响应拖动，内容和手柄不抢手势。 */
    private fun setupQaCardDrag() {
        val dm = resources.displayMetrics
        val handleH = (26 * dm.density).toInt()
        val cap = (dm.heightPixels * 0.36f).toInt()
        val collapseAt = handleH + (56 * dm.density).toInt()
        var startY = 0f
        var startH = 0
        qaCardHandle.setOnTouchListener { _, ev ->
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    startY = ev.rawY
                    startH = if (qaCard.height > 0) qaCard.height else cap
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val h = (startH + (startY - ev.rawY).toInt())
                        .coerceIn(collapseAt - (24 * dm.density).toInt(), cap)
                    qaCardUserHeight = h
                    applyQaCardHeight(h, handleH)
                    true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    /* 收起的两种判定：拖到很矮，或者一口气拖掉一半以上 */
                    val shrunkHalf = startH > 0 &&
                            (startH - qaCardUserHeight) * 2 >= startH
                    if (qaCardUserHeight < collapseAt || shrunkHalf) {
                        hideQaCard()
                    } else {
                        clampQaCard()
                    }
                    true
                }
                else -> false
            }
        }
    }

    /* 设置面板：所有选项集中在 settingsScroll 里，点「设置」才显示。
     * 展开高度限制在屏高 40%，超出的部分在面板内部滑动 —— 之前所有设置行直接
     * 平铺在根 LinearLayout 里，一展开就把 0dp+weight=1 的字幕区压住、页面又
     * 整体不可滚，看起来就是「字幕卡住、滑不动」。 */
    private fun setupSettingsToggle() {
        settingsToggle.text = "设置"
        settingsToggle.setOnClickListener { applySettingsVisibility(!settingsShown) }
    }

    private fun applySettingsVisibility(show: Boolean) {
        settingsShown = show
        settingsScroll.visibility = if (show) View.VISIBLE else View.GONE
        settingsToggle.text = if (show) "收起设置" else "设置"
        if (show) clampSettingsPanel()
        refreshSectionVisibility()
    }

    /** 设置面板或答案卡片占地方时收起章节区：这两块都是「临时要看的东西」，
     *  章节相对次要，让出来的高度全部给字幕，避免出现「字幕只剩两行」。 */
    private fun refreshSectionVisibility() {
        val hide = settingsShown || qaCard.visibility == View.VISIBLE
        val v = if (hide) View.GONE else View.VISIBLE
        chapterLabel.visibility = v
        chapterScroll.visibility = v
    }

    /** 面板最高 40% 屏高：内容再多也在面板内部滚，字幕区永远留得住 */
    private fun clampSettingsPanel() {
        val dm = resources.displayMetrics
        val cap = (dm.heightPixels * 0.40f).toInt()
        settingsScroll.layoutParams = settingsScroll.layoutParams.apply {
            height = ViewGroup.LayoutParams.WRAP_CONTENT
        }
        settingsScroll.post {
            if (!settingsShown) return@post
            val content = settingsScroll.getChildAt(0)?.height ?: 0
            val chrome = (16 * dm.density).toInt()
            if (content + chrome > cap) {
                settingsScroll.layoutParams = settingsScroll.layoutParams.apply { height = cap }
            }
            Log.i("ClassMate/UI", "settings panel h=${settingsScroll.height} content=$content cap=$cap")
        }
    }

    /* 后台回放触发：adb am start 带 --es replay_file <文件名> 即可，
     * 全程无需模拟点击；replay_background=true 时把本 App 自动移到后台，
     * 不抢用户正在看的悬浮窗/直播。WAV 只是软件喂 PCM，不会播放出声。 */
    private fun handleReplayIntent(i: Intent?) {
        val name = i?.getStringExtra("replay_file") ?: return
        val dir = File(getExternalFilesDir(null), "recordings/_audio_archive_20261109")
        val src = File(dir, name)
        if (!src.isFile) {
            Log.w(POST_TAG, "replay request file missing: ${src.absolutePath}")
            return
        }
        val background = i.getBooleanExtra("replay_background", false)
        i.removeExtra("replay_file")   /* 防止 intent 复用导致重复触发 */
        handler.postDelayed({
            if (background) moveTaskToBack(true)
            if (recording || postRunning || replaying) {
                Log.w(POST_TAG, "replay skipped: busy (recording=$recording post=$postRunning replaying=$replaying)")
            } else {
                runReplay(src)
            }
        }, 900)
    }

    /**
     * 老库（M2）没有 chapters 表 / text_corrected 列：启动时只做建表/补列，
     * 只有章节为空时才用 forceLocal 重建一次（绝不在启动时私自上传云端）。
     * note_path 传空 = 只写 chapters，不写文件。
     */
    private fun ensureSchemaAndChapters() {
        val dir = File(getExternalFilesDir(null), "recordings")
        val db = File(dir, "classmate.db")
        if (!db.exists()) return
        Thread({
            bridge.nativeEnsureSchema(db.absolutePath)
            val info = SessionStore.latestSession(db)
            var rebuilt = false
            if (info != null && SessionStore.chapters(db, info.id).isEmpty()) {
                rebuilt = buildNote(db.absolutePath, "", forceLocal = true) >= 0
            }
            handler.post { if (rebuilt) refreshSessionUi() }
        }, "classmate-schema").start()
    }

    /**
     * M2.7 生成 chapters/笔记。模型选择链：
     *  - forceLocal / 策略=始终本地 → 优先本地 GGUF；未就绪则抽取式降级（UI 标注，不静默）
     *  - 策略=自动/云端优先 → 填了 Key 且已授权走云端；没填 Key 退到本地；本地也没有则抽取式
     *  - 任何 LLM 调用失败由 core 自动降级抽取式并写 note_meta.reason
     */
    private fun buildNote(dbPath: String, notePath: String, forceLocal: Boolean): Long {
        val paths = LlmAssets.prepare(this)
        val policy = LlmSettings.policy(prefs)

        var backend = "local"
        var targetCtx = 2048
        var modelId = "qwen1.5b-q4"
        var allowLlm = false
        var cloud: Any? = null          /* CloudLlm 或 LocalLlm，都有 generate(String) */
        var localLlm: LocalLlm? = null  /* 用完需 close */
        var localDegraded = false

        val wantLocal = forceLocal || policy == LlmSettings.POLICY_LOCAL

        if (wantLocal) {
            /* 始终本地：先看 GGUF 是否就绪 */
            val modelPath = localLlmModelPath()
            if (modelPath != null) {
                localLlm = LocalLlm.load(bridge, modelPath, ctxLen = 2048, nThreads = 4)
                if (localLlm != null) {
                    cloud = localLlm
                    allowLlm = true
                    backend = "local"
                    targetCtx = 2048
                    modelId = "qwen1.5b-q4"
                } else {
                    localDegraded = true   /* 模型存在但加载失败（损坏/版本不符） */
                }
            } else {
                localDegraded = true       /* 模型未下载 */
            }
        } else {
            /* 自动 / 云端优先：有 Key 且授权则云端 */
            val cloudCfg = LlmSettings.resolveCloud(prefs)
            if (cloudCfg != null && LlmSettings.consentGiven(prefs)) {
                cloud = CloudLlm(cloudCfg)
                allowLlm = true
                backend = "cloud"
                targetCtx = cloudCfg.ctxLen
                modelId = cloudCfg.modelId
            } else {
                /* 没填 Key：退到本地 GGUF */
                val modelPath = localLlmModelPath()
                if (modelPath != null) {
                    localLlm = LocalLlm.load(bridge, modelPath, ctxLen = 2048, nThreads = 4)
                    if (localLlm != null) {
                        cloud = localLlm
                        allowLlm = true
                        backend = "local"
                        targetCtx = 2048
                        modelId = "qwen1.5b-q4"
                    } else {
                        localDegraded = true
                    }
                } else {
                    localDegraded = true
                }
            }
        }

        val t0 = System.currentTimeMillis()
        val sessionId = bridge.nativeBuildNote(
            dbPath, notePath, paths.configPath, backend,
            paths.tokenizerPath, paths.promptDir,
            targetCtx, modelId, "m27-1", allowLlm, cloud
        )

        /* 用完即释放本地模型（云端不需要释放） */
        localLlm?.close()

        /* 验收观测：课后处理耗时 + 进程峰值内存（VmHWM，KB），真机 logcat 可直接取证 */
        Log.i("ClassMate/Post",
            "session=$sessionId backend=$backend llm=$allowLlm degraded=$localDegraded " +
                    "elapsed=${System.currentTimeMillis() - t0}ms peakRssKb=${peakRssKb()}")

        /* 本地模型未就绪：UI 标注，不静默降级 */
        if (localDegraded) {
            handler.post {
                Toast.makeText(this,
                    "本地模型未就绪，已降级为抽取式整理（可在模型管理下载 GGUF）",
                    Toast.LENGTH_LONG).show()
            }
        }

        /* 云端整理完成后：记录本场合计 token（本地档不调用云端，永远 0、永远免费） */
        if (cloud is CloudLlm) {
            LlmSettings.recordLastModel(prefs, modelId)
            LlmSettings.recordSessionUsage(prefs, cloud.totalPromptTokens, cloud.totalCompletionTokens)
        }
        return sessionId
    }

    /** 本地 GGUF 模型路径：manifest 中 kind=llm-gguf 且文件已下载，否则 null */
    private fun localLlmModelPath(): String? {
        val entries = ModelManager.loadModels(this) ?: return null
        val entry = entries.firstOrNull { it.kind == "llm-gguf" } ?: return null
        val f = ModelManager.modelFile(this, entry)
        return if (f.exists() && f.length() > 0) f.absolutePath else null
    }

    /** 进程峰值内存（/proc/self/status 的 VmHWM，单位 KB）；读不到返回 -1 */
    private fun peakRssKb(): Long = try {
        File("/proc/self/status").readLines()
            .firstOrNull { it.startsWith("VmHWM:") }
            ?.trim()?.split(Regex("\\s+"))?.getOrNull(1)?.toLongOrNull() ?: -1L
    } catch (e: Exception) {
        -1L
    }

    /** 首次启用云端前的一次性告知：仅上传课堂文字，不上传音频。
     *  M3 起点名时会额外发送「最近课堂文字 + 提问」，这里一并说明，可单独拒绝。 */
    private fun ensureCloudConsent(onReady: () -> Unit) {
        val wantsCloud = LlmSettings.policy(prefs) != LlmSettings.POLICY_LOCAL &&
                LlmSettings.resolveCloud(prefs) != null
        val qaAsked = prefs.contains(KEY_QA_CONSENT)
        if (!wantsCloud || (LlmSettings.consentGiven(prefs) && qaAsked)) {
            onReady()
            return
        }
        AlertDialog.Builder(this)
            .setTitle("启用云端摘要与点名助手")
            .setMessage(
                "开启后会中每 2 分钟上传一次「本批新增的课堂文字片段」（每批几百到千余字，\n" +
                        "不含此前全文、不含音频、不含录音文件）；音频始终留在本机。\n\n" +
                        "点名时会发送「最近课堂文字 + 老师提问」给云端生成 3 点回答，\n" +
                        "同样只发文字，不发音频；可单独拒绝这一项。\n\n" +
                        "停止录音后的章节与笔记由本机合并生成，不再额外上传全文。\n\n" +
                        "拒绝则本次录音全程本地档：课中零云端调用、永久 0 费用。"
            )
            .setPositiveButton("全部同意") { _, _ ->
                LlmSettings.setConsent(prefs, true)
                QaEngine.get(this).setConsent(true)
                onReady()
            }
            .setNeutralButton("仅摘要，不点名") { _, _ ->
                LlmSettings.setConsent(prefs, true)
                QaEngine.get(this).setConsent(false)
                onReady()
            }
            .setNegativeButton("保持本地") { _, _ ->
                LlmSettings.setConsent(prefs, false)
                QaEngine.get(this).setConsent(false)
                LlmSettings.setPolicy(prefs, LlmSettings.POLICY_LOCAL)
                bindLlmPolicyUi()
                refreshLlmStatus()
                onReady()
            }
            .setCancelable(false)
            .show()
    }

    /* ---------------- 设置 ---------------- */

    private fun bindSettings() {
        when (prefs.getInt(KEY_AUDIO_POLICY, POLICY_TEMP)) {
            POLICY_NONE -> audioPolicyGroup.check(R.id.radioNone)
            POLICY_KEEP -> audioPolicyGroup.check(R.id.radioKeep)
            else -> audioPolicyGroup.check(R.id.radioTemp)
        }
        keepCountEdit.setText(prefs.getInt(KEY_KEEP_COUNT, 10).toString())
        maxGbEdit.setText(prefs.getFloat(KEY_MAX_GB, 2f).toString())
        hotwordsEdit.setText(prefs.getString(KEY_HOTWORDS, ""))
        timeModeCheck.isChecked = prefs.getBoolean(KEY_CLOCK, false)
        correctionCheck.isChecked = prefs.getBoolean(KEY_CORRECTION, false)

        audioPolicyGroup.setOnCheckedChangeListener { _, checkedId ->
            prefs.edit().putInt(KEY_AUDIO_POLICY, policyOf(checkedId)).apply()
            if (!recording) renderChapters()
        }
        keepCountEdit.setOnFocusChangeListener { _, hasFocus ->
            if (!hasFocus) saveStorageLimits()
        }
        maxGbEdit.setOnFocusChangeListener { _, hasFocus ->
            if (!hasFocus) saveStorageLimits()
        }
        /* 实时保存：清空也必须立刻写盘。
         * 旧实现用 OnFocusChangeListener，用户全选删除后若按 Home / 被系统回收，
         * 焦点不会转移，prefs 里仍是旧值，下次 onCreate 又被 setText 回填，
         * 表现为「怎么删都删不掉」。注册在 setText 之后，初始化不会误写。 */
        hotwordsEdit.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: android.text.Editable?) {
                prefs.edit().putString(KEY_HOTWORDS, s?.toString()?.trim() ?: "").apply()
            }
        })
        timeModeCheck.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(KEY_CLOCK, checked).apply()
            renderSessionViews()
        }
        correctionCheck.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(KEY_CORRECTION, checked).apply()
        }

        /* M3.4 音频来源：改动立即落盘；录音中改只影响下一次，避免中途换源把字幕切成两段 */
        when (AudioMode.fromPref(prefs.getString(AudioMode.KEY, null))) {
            AudioMode.PLAYBACK -> audioSourceGroup.check(R.id.sourcePlayback)
            AudioMode.BOTH -> audioSourceGroup.check(R.id.sourceBoth)
            else -> audioSourceGroup.check(R.id.sourceMic)
        }
        audioSourceGroup.setOnCheckedChangeListener { _, checkedId ->
            val mode = when (checkedId) {
                R.id.sourcePlayback -> AudioMode.PLAYBACK
                R.id.sourceBoth -> AudioMode.BOTH
                else -> AudioMode.MIC
            }
            prefs.edit().putString(AudioMode.KEY, mode.pref).apply()
            if (recording) {
                Toast.makeText(this, "已保存，下次录音生效", Toast.LENGTH_SHORT).show()
            }
        }

        /* M3.4 自动答题总开关（默认开）：关掉后只出字幕，不判定、不振动、不调云端 */
        val qa = QaEngine.get(this)
        qaAutoCheck.isChecked = qa.isEnabled()
        qaAutoCheck.setOnCheckedChangeListener { _, checked ->
            qa.setEnabled(checked)
            Toast.makeText(
                this,
                if (checked) "已开启：老师点名会自动生成 3 点回答"
                else "已关闭：只出字幕，不生成答案（不消耗 token）",
                Toast.LENGTH_SHORT
            ).show()
        }

        bindLlmPolicyUi()
        refreshLlmStatus()
    }

    private fun bindLlmPolicyUi() {
        when (LlmSettings.policy(prefs)) {
            LlmSettings.POLICY_LOCAL -> llmPolicyGroup.check(R.id.llmLocalPolicy)
            LlmSettings.POLICY_CLOUD -> llmPolicyGroup.check(R.id.llmCloudPolicy)
            else -> llmPolicyGroup.check(R.id.llmAutoPolicy)
        }
        llmPolicyGroup.setOnCheckedChangeListener { _, checkedId ->
            val policy = when (checkedId) {
                R.id.llmLocalPolicy -> LlmSettings.POLICY_LOCAL
                R.id.llmCloudPolicy -> LlmSettings.POLICY_CLOUD
                else -> LlmSettings.POLICY_AUTO
            }
            LlmSettings.setPolicy(prefs, policy)
            refreshLlmStatus()
        }
    }

    private fun refreshLlmStatus() {
        llmStatusText.text = LlmSettings.statusLabel(this, prefs)
    }

    /* ---------- 云端配置（多套 OpenAI 兼容） ---------- */

    private fun showCloudSettings() {
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 8, 24, 8)
        }

        /* ---- token / 费用统计（本地档永远 0，永远免费） ---- */
        val activeModel = LlmSettings.resolveCloud(prefs)?.modelId ?: ""
        val summary = LlmSettings.costSummary(this, prefs, activeModel)
        val showCost = LlmSettings.showCost(prefs)
        val statText = StringBuilder().apply {
            append("本次整理：输入 ${summary.lastPromptTokens} tok · 输出 ${summary.lastCompletionTokens} tok")
            if (showCost) {
                if (summary.priceConfigured) append("  估算 ${LlmSettings.formatCost(summary.lastCost, summary.currency)}")
                else append("  未配置单价、不估算费用")
            }
            append("\n累计：输入 ${summary.totalPromptTokens} tok · 输出 ${summary.totalCompletionTokens} tok")
            if (showCost) {
                if (summary.priceConfigured) append("  估算 ${LlmSettings.formatCost(summary.totalCost, summary.currency)}")
                else append("  未配置单价、不估算费用")
            }
        }
        container.addView(TextView(this).apply {
            textSize = 12f
            setPadding(0, 4, 0, 8)
            text = statText.toString()
        })
        container.addView(CheckBox(this).apply {
            text = "显示估算费用"
            isChecked = showCost
            setOnCheckedChangeListener { _, checked ->
                LlmSettings.setShowCost(prefs, checked)
                /* 关闭时不清除已统计的 token，只是不显示金额 */
            }
        })
        container.addView(TextView(this).apply {
            textSize = 11f
            setTextColor(Color.GRAY)
            setPadding(0, 2, 0, 8)
            text = "单价单位为「元 / 千 token」，从 llm_config.json 的 pricing 读取；" +
                    "本期未配置单价时不估算费用。实际费用以平台控制台当期账单为准。"
        })

        val list = LlmSettings.profiles(prefs)
        if (list.isEmpty()) {
            container.addView(TextView(this).apply {
                text = "还没有云端配置。可添加 DeepSeek / 火山方舟等 OpenAI 兼容服务。\n" +
                        "不填 Key 时不会发任何网络请求，仍会生成章节。"
                textSize = 12f
                setPadding(0, 4, 0, 8)
            })
        } else {
            for (p in list) {
                container.addView(TextView(this).apply {
                    textSize = 13f
                    setPadding(0, 10, 0, 10)
                    isClickable = true
                    text = "${p.label} · ${p.modelId} · ${p.ctxLen} ctx\n${p.baseUrl}"
                    setOnClickListener { showProfileDialog(p) }
                })
            }
        }
        container.addView(Button(this).apply {
            text = "新增配置"
            setOnClickListener { showProfileDialog(null) }
        })
        container.addView(Button(this).apply {
            text = "没有 Key？去领免费额度"
            setOnClickListener { showKeyGuide() }
        })
        AlertDialog.Builder(this)
            .setTitle("云端模型设置")
            .setView(ScrollView(this).apply { addView(container) })
            .setPositiveButton("关闭", null)
            .show()
    }

    private fun showProfileDialog(existing: LlmSettings.Profile?) {
        val wrap = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 8, 24, 8)
        }
        fun field(label: String, value: String, inputType: Int = InputType.TYPE_CLASS_TEXT): EditText {
            wrap.addView(TextView(this).apply { text = label; textSize = 12f; setPadding(0, 6, 0, 0) })
            return EditText(this).apply {
                setText(value)
                this.inputType = inputType
                textSize = 13f
            }.also { wrap.addView(it) }
        }
        val labelEdit = field("名称", existing?.label ?: "DeepSeek")
        val baseEdit = field("base_url", existing?.baseUrl ?: "https://api.deepseek.com/v1")
        val keyEdit = field(
            "api_key（仅本机加密保存）", existing?.apiKey ?: "",
            InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        )
        val modelEdit = field("model_id", existing?.modelId ?: "deepseek-flash")
        val ctxEdit = field(
            "ctx_len", (existing?.ctxLen ?: 32768).toString(), InputType.TYPE_CLASS_NUMBER
        )

        fun draftConfig(): CloudConfig {
            val ctxLen = ctxEdit.text.toString().toIntOrNull()?.takeIf { it > 0 } ?: 32768
            return CloudConfig(
                existing?.id ?: "",
                labelEdit.text.toString().trim().ifEmpty { "云端模型" },
                baseEdit.text.toString().trim(),
                keyEdit.text.toString().trim(),
                modelEdit.text.toString().trim(),
                ctxLen
            )
        }

        wrap.addView(Button(this).apply {
            text = "测试连通"
            setOnClickListener {
                val cfg = draftConfig()
                if (cfg.baseUrl.isEmpty() || cfg.modelId.isEmpty() || cfg.apiKey.isEmpty()) {
                    Toast.makeText(this@MainActivity, "请先填 base_url / model_id / api_key",
                        Toast.LENGTH_SHORT).show()
                    return@setOnClickListener
                }
                Toast.makeText(this@MainActivity, "测试中…", Toast.LENGTH_SHORT).show()
                Thread({
                    val result = LlmSettings.testConnection(cfg)
                    handler.post {
                        Toast.makeText(this@MainActivity, result, Toast.LENGTH_LONG).show()
                    }
                }, "classmate-llm-test").start()
            }
        })

        var dlg: AlertDialog? = null
        if (existing != null) {
            wrap.addView(Button(this).apply {
                text = "删除此配置"
                setOnClickListener {
                    LlmSettings.deleteProfile(prefs, existing.id)
                    refreshLlmStatus()
                    dlg?.dismiss()
                }
            })
        }

        val built = AlertDialog.Builder(this)
            .setTitle(if (existing == null) "新增云端配置" else "编辑云端配置")
            .setView(ScrollView(this).apply { addView(wrap) })
            .setPositiveButton("保存") { _, _ ->
                val cfg = draftConfig()
                if (cfg.baseUrl.isEmpty() || cfg.modelId.isEmpty()) {
                    Toast.makeText(this, "base_url / model_id 不能为空", Toast.LENGTH_SHORT).show()
                    return@setPositiveButton
                }
                LlmSettings.upsertProfile(prefs, LlmSettings.Profile(
                    id = existing?.id ?: "p${System.currentTimeMillis()}",
                    label = cfg.label,
                    baseUrl = cfg.baseUrl,
                    modelId = cfg.modelId,
                    ctxLen = cfg.ctxLen,
                    apiKey = cfg.apiKey
                ))
                refreshLlmStatus()
                Toast.makeText(this, "已保存（Key 加密存本机）", Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton("取消", null)
            .create()
        dlg = built
        built.show()
    }

    private fun showKeyGuide() {
        AlertDialog.Builder(this)
            .setTitle("没有 Key？去领免费额度")
            .setMessage(
                "ClassMate 不内置任何平台 Key，也不代充值、不代付；\n" +
                        "用量和费用完全由你在平台账号里自行管理。\n\n" +
                        "以下平台提供 OpenAI 兼容接口，需自行注册并实名认证：\n" +
                        "• DeepSeek 开放平台：platform.deepseek.com\n" +
                        "• 火山方舟：console.volcengine.com/ark\n" +
                        "• 阿里云百炼：bailian.console.aliyun.com\n\n" +
                        "百炼免费额度特别提醒：\n" +
                        "• 免费额度仅适用于新加坡地域，国内地域按量计费；\n" +
                        "• 使用百炼即表示同意将课堂文字发送至新加坡地域处理（文字出境）；\n" +
                        "• 启用前需二次确认：确认知晓文字出境并同意平台服务条款。\n\n" +
                        "重要提醒：\n" +
                        "• 免费额度有有效期，过期后不再抵扣，以平台控制台当期政策为准；\n" +
                        "• 领到 Key 后请先在平台账号设置「消费上限」或关闭「自动按量付费」，\n" +
                        "  避免免费额度用完后被自动按量扣费；\n" +
                        "• 本应用不会替你充值或垫付任何费用。\n\n" +
                        "拿到 Key 后回到「云端设置」新增一条配置，base_url 填平台给出的接口地址。"
            )
            .setPositiveButton("知道了", null)
            .show()
    }

    private fun policyOf(checkedId: Int): Int = when (checkedId) {
        R.id.radioNone -> POLICY_NONE
        R.id.radioKeep -> POLICY_KEEP
        else -> POLICY_TEMP
    }

    private fun selectedPolicy(): Int = policyOf(audioPolicyGroup.checkedRadioButtonId)

    private fun saveStorageLimits() {
        val keep = keepCountEdit.text.toString().toIntOrNull()
        val maxGb = maxGbEdit.text.toString().toFloatOrNull()
        prefs.edit()
            .putInt(KEY_KEEP_COUNT, (keep ?: 10).coerceAtLeast(0))
            .putFloat(KEY_MAX_GB, (maxGb ?: 2f).coerceAtLeast(0f))
            .apply()
    }

    private fun pendingWavPath(): String = prefs.getString(KEY_PENDING_WAV, "") ?: ""

    private fun runCleanup(showToast: Boolean) {
        if (replaying) {
            Toast.makeText(this, "回放验证进行中，暂停 WAV 清理", Toast.LENGTH_SHORT).show()
            return
        }
        val keep = prefs.getInt(KEY_KEEP_COUNT, 10)
        val maxGb = prefs.getFloat(KEY_MAX_GB, 2f)
        val pending = pendingWavPath()
        val protected = if (pending.isNotEmpty() && File(pending).exists()) setOf(pending)
        else emptySet()
        val removed = RecordingStore.cleanup(this, keep, maxGb, protected)
        if (showToast && removed > 0) {
            Toast.makeText(this, "已按策略清理 $removed 个旧录音", Toast.LENGTH_SHORT).show()
        }
        refreshStorage()
    }

    private fun refreshStorage() {
        val count = RecordingStore.wavFiles(this).size
        val total = RecordingStore.totalBytes(this)
        val current = currentWav?.takeIf { it.exists() }?.length() ?: 0L
        storageText.text = String.format("%s ｜ %d 个 / %s",
            fmtBytes(current), count, fmtBytes(total))
        /* 重启后若还有待重试的临时录音，入口也要在 */
        if (!postRunning && retryButton.visibility != View.VISIBLE &&
            File(pendingWavPath()).exists()) {
            retryButton.visibility = View.VISIBLE
        }
    }

    private fun fmtBytes(bytes: Long): String = when {
        bytes >= 1024L * 1024 * 1024 -> String.format("%.1f GB", bytes / 1024.0 / 1024 / 1024)
        bytes >= 1024L * 1024 -> String.format("%.1f MB", bytes / 1024.0 / 1024)
        bytes >= 1024 -> String.format("%.1f KB", bytes / 1024.0)
        else -> "$bytes B"
    }

    private fun exportLatest() {
        val file = RecordingStore.latest(this) ?: run {
            Toast.makeText(this, "没有可导出的录音（临时档处理后会删除）", Toast.LENGTH_SHORT).show()
            return
        }
        val uri = RecordingStore.export(this, file)
        Toast.makeText(
            this,
            if (uri == null) "导出失败（需 Android 10 以上）"
            else "已导出：下载/ClassMate/${file.name}",
            Toast.LENGTH_LONG
        ).show()
    }

    private fun clearRecordings() {
        if (recording) {
            Toast.makeText(this, "请先停止录音", Toast.LENGTH_SHORT).show()
            return
        }
        val removed = RecordingStore.clear(this)
        Toast.makeText(this, "已清空 $removed 个音频文件（数据库与笔记保留）",
            Toast.LENGTH_SHORT).show()
        refreshStorage()
    }

    override fun onResume() {
        super.onResume()
        if (!downloading) refreshModelStatus()
        refreshStorage()
        if (!recording && !postRunning) refreshSessionUi()
    }

    override fun onDestroy() {
        RecordingService.eventListener = null
        RecordingService.onClosed = null
        QaEngine.get(this).uiListener = null
        handler.removeCallbacks(ticker)
        super.onDestroy()
    }

    /* ---------- 模型 ---------- */

    private fun asrEntries(): List<ModelManager.ModelEntry> =
        modelEntries.filter { it.kind.startsWith("asr-sherpa") || it.kind.startsWith("vad-") }

    private fun correctionEntries(): List<ModelManager.ModelEntry> =
        modelEntries.filter { it.kind.startsWith("sensevoice") }

    /** 本地 GGUF 条目（kind=llm-gguf，1.1 GB，只从 manifest 下载，不打进 APK） */
    private fun localLlmEntries(): List<ModelManager.ModelEntry> =
        modelEntries.filter { it.kind == "llm-gguf" }

    private fun asrReady(): Boolean =
        ModelManager.isReadyAll(this) { it.kind.startsWith("asr-sherpa") } &&
                ModelManager.isReadyAll(this) { it.kind.startsWith("vad-") }

    private fun correctionReady(): Boolean =
        ModelManager.isReadyAll(this) { it.kind.startsWith("sensevoice") }

    private fun localLlmReady(): Boolean =
        ModelManager.isReadyAll(this) { it.kind == "llm-gguf" }

    private fun refreshModelStatus() {
        modelEntries = ModelManager.loadModels(this) ?: emptyList()
        if (modelEntries.isEmpty()) {
            modelStatusText.text = "模型：清单缺失，只能录音"
            downloadButton.isEnabled = false
            correctionStatusText.text = "校正模型：清单缺失"
            correctionDownloadButton.isEnabled = false
            localLlmStatusText.text = "本地模型：清单缺失"
            localLlmDownloadButton.isEnabled = false
            return
        }
        val asrMb = ModelManager.totalBytes(asrEntries()) / 1024 / 1024
        if (asrReady()) {
            modelStatusText.text = "模型：就绪（约 ${asrMb} MB）"
            downloadButton.isEnabled = false
        } else {
            val missing = asrEntries().count { !ModelManager.isReady(this, it) }
            modelStatusText.text = "模型：未下载（缺 $missing/${asrEntries().size}，约 ${asrMb} MB）"
            downloadButton.isEnabled = !downloading
        }

        if (correctionReady()) {
            correctionStatusText.text = "校正模型：就绪"
            correctionDownloadButton.isEnabled = false
        } else {
            correctionStatusText.text = "校正模型：未下载（可跳过，课后处理会等下载）"
            correctionDownloadButton.isEnabled = !downloading
        }

        val localMb = ModelManager.totalBytes(localLlmEntries()) / 1024 / 1024
        if (localLlmEntries().isEmpty()) {
            localLlmStatusText.text = "本地模型：清单缺失"
            localLlmDownloadButton.isEnabled = false
        } else if (localLlmReady()) {
            localLlmStatusText.text = "本地模型：就绪（约 ${localMb} MB · Qwen1.5B Q4_K_M）"
            localLlmDownloadButton.isEnabled = false
        } else {
            localLlmStatusText.text =
                "本地模型：未下载（约 ${localMb} MB / 1.1 GB，始终本地前需下载）"
            localLlmDownloadButton.isEnabled = !downloading
        }

        /* 本地模型下载完成后，AI 状态文案要同步刷新（就绪 / 未就绪） */
        llmStatusText.text = LlmSettings.statusLabel(this, prefs)
    }

    private fun startDownload(correction: Boolean) {
        if (correction) {
            startDownloadFor(correctionEntries(), "校正模型", correctionStatusText)
        } else {
            startDownloadFor(
                modelEntries.filter { it.kind.startsWith("asr-sherpa") || it.kind.startsWith("vad-") },
                "识别模型", modelStatusText
            )
        }
    }

    /** 下载本地 GGUF 模型（约 1.1 GB，国内镜像，sha256 校验由 ModelManager 完成） */
    private fun startDownloadLocal() {
        startDownloadFor(localLlmEntries(), "本地模型", localLlmStatusText)
    }

    /**
     * 按条目分组下载：一次只允许一个下载任务，进度写回对应的状态 TextView。
     * filter 直接复用 ModelManager.downloadAll 的谓词，已就绪文件自动跳过。
     */
    private fun startDownloadFor(
        entries: List<ModelManager.ModelEntry>,
        label: String,
        statusView: TextView
    ) {
        if (downloading) return
        if (entries.isEmpty()) {
            Toast.makeText(this, "$label：清单缺失", Toast.LENGTH_SHORT).show()
            return
        }
        downloading = true
        downloadButton.isEnabled = false
        correctionDownloadButton.isEnabled = false
        localLlmDownloadButton.isEnabled = false
        val names = entries.map { it.name }.toSet()
        val filter: (ModelManager.ModelEntry) -> Boolean = { it.name in names }

        Thread({
            try {
                ModelManager.downloadAll(this, filter) { percent ->
                    handler.post {
                        statusView.text =
                            if (percent >= 0) "$label：下载中 $percent%" else "$label：下载中…"
                    }
                }
                handler.post {
                    downloading = false
                    refreshModelStatus()
                    Toast.makeText(this, "$label 下载完成", Toast.LENGTH_SHORT).show()
                }
            } catch (e: Exception) {
                handler.post {
                    downloading = false
                    refreshModelStatus()
                    Toast.makeText(this, "$label 下载失败：${e.message}", Toast.LENGTH_LONG).show()
                }
            }
        }, "classmate-model-dl").start()
    }

    /* ---------- 录音 ---------- */

    private fun neededPermissions(): Array<String> {
        val list = mutableListOf(Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= 33) list.add(Manifest.permission.POST_NOTIFICATIONS)
        return list.toTypedArray()
    }

    private fun hasPermissions(): Boolean = neededPermissions().all {
        checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED
    }

    private fun startRecording() {
        if (postRunning) {
            Toast.makeText(this, "课后处理还没结束，请稍等", Toast.LENGTH_SHORT).show()
            return
        }
        if (!hasPermissions()) {
            requestPermissions(neededPermissions(), REQ_PERMISSIONS)
            return
        }
        /* 授权前移：开录前一次性告知「仅上传课堂文字增量块、不上传音频」，
         * 录音过程中不再弹窗；拒绝则本次自动走本地档。 */
        ensureCloudConsent {
            /* M3.4：内录必须先拿系统授权，拿不到就不开录，避免录出空场次 */
            if (selectedAudioMode().needsProjection) requestProjection()
            else doStartRecording(Activity.RESULT_CANCELED, null)
        }
    }

    private fun selectedAudioMode(): AudioMode = when (audioSourceGroup.checkedRadioButtonId) {
        R.id.sourcePlayback -> AudioMode.PLAYBACK
        R.id.sourceBoth -> AudioMode.BOTH
        else -> AudioMode.MIC
    }

    /** 系统「录制或投放」授权：只是一次性取音频采集权限，不截屏、不传画面 */
    private fun requestProjection() {
        val mgr = getSystemService(MediaProjectionManager::class.java)
        if (mgr == null) {
            Toast.makeText(this, "本机不支持内录", Toast.LENGTH_LONG).show()
            return
        }
        try {
            startActivityForResult(mgr.createScreenCaptureIntent(), REQ_PROJECTION)
        } catch (e: Exception) {
            Log.e(POST_TAG, "createScreenCaptureIntent failed: ${e.message}")
            Toast.makeText(this, "打不开内录授权：${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_PROJECTION) return
        if (resultCode == Activity.RESULT_OK && data != null) {
            doStartRecording(resultCode, data)
        } else {
            Toast.makeText(
                this, "没有授权内录：录不到手机播放的声音，本次已取消", Toast.LENGTH_LONG
            ).show()
        }
    }

    private fun doStartRecording(projectionCode: Int, projectionData: Intent?) {
        val dir = File(getExternalFilesDir(null), "recordings")
        if (!dir.exists()) dir.mkdirs()

        sessionStamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val wav = File(dir, "classmate_$sessionStamp.wav")
        currentWav = wav
        sessionPolicy = selectedPolicy()
        sessionSavesWav = sessionPolicy != POLICY_NONE
        sessionStartedAt = System.currentTimeMillis()
        sessionId = -1L
        liveRows.clear()
        latestChapters.clear()
        renderSessionViews()

        val modelReady = asrReady()
        val modelsDir = if (modelReady) ModelManager.modelsDir(this).absolutePath else ""
        val hotwords = hotwordsEdit.text.toString().trim()
        prefs.edit().putString(KEY_HOTWORDS, hotwords).apply()

        /* 临时档 + 二次校正：首次没有模型时提示只出原文，避免用户以为卡住 */
        if (sessionPolicy == POLICY_TEMP && correctionCheck.isChecked && !correctionReady()) {
            Toast.makeText(this,
                "校正模型未下载：本次只出原文字幕，临时录音课后照常删除",
                Toast.LENGTH_LONG).show()
        }

        RecordingService.onClosed = { beginPostprocess() }

        val intent = Intent(this, RecordingService::class.java)
            .putExtra(RecordingService.EXTRA_WAV_PATH, wav.absolutePath)
            .putExtra(RecordingService.EXTRA_MODELS_DIR, modelsDir)
            .putExtra(RecordingService.EXTRA_ASR_THREADS, 4)
            .putExtra(RecordingService.EXTRA_SAVE_WAV, sessionSavesWav)
            .putExtra(RecordingService.EXTRA_HOTWORDS, hotwords)
        if (projectionData != null) {
            intent.putExtra(RecordingService.EXTRA_PROJECTION_CODE, projectionCode)
            intent.putExtra(RecordingService.EXTRA_PROJECTION_DATA, projectionData)
        }
        startForegroundService(intent)

        val mode = selectedAudioMode()
        recording = true
        startAtMs = System.currentTimeMillis()
        rollingStatusText.text = ""
        startRolling(dir)   /* 有 Key + 已授权才启动；否则课中零云端调用 */
        handler.postDelayed(rollingTick, 30_000)
        statusText.text = "正在录音  00:00"
        toggleButton.text = "停止录音"
        pathText.text = when (sessionPolicy) {
            POLICY_NONE -> "不存音频：字幕与章节进 classmate.db（${mode.label}）"
            POLICY_TEMP -> "临时音频：出稿成功后自动删除（${mode.label}）"
            else -> "长期保留：${wav.absolutePath}（${mode.label}）"
        }
        subtitleList.removeAllViews()
        partialView = null
        handler.post(ticker)
    }

    /** 启动会中增量：仅「有 Key + 非始终本地 + 已授权」时启用，否则本次零云端调用 */
    private fun startRolling(dir: File) {
        rolling?.close()
        rolling = null
        /* 始终本地档：课中零云端调用；没填 Key：resolveCloud 返回 null */
        if (LlmSettings.policy(prefs) == LlmSettings.POLICY_LOCAL) return
        val cfg = LlmSettings.resolveCloud(prefs) ?: return
        if (!LlmSettings.consentGiven(prefs)) return
        LlmAssets.prepare(this)
        val rollingCfg = RollingSummarizer.loadConfig(this)
        val prompt = RollingSummarizer.loadPrompt(this)
        if (prompt.isBlank()) return
        val cloud = CloudLlm(cfg, rollingCfg.timeoutMs)
        rolling = RollingSummarizer(
            File(dir, "classmate.db"),
            cloud,
            cfg.modelId,
            rollingCfg,
            prompt
        ) { msg -> handler.post { rollingStatusText.text = msg } }
        rollingStatusText.text = "增量摘要：待首批（每 2 分钟一批）"
    }

    private fun stopRecording() {
        stopService(Intent(this, RecordingService::class.java))
        recording = false
        handler.removeCallbacks(ticker)
        handler.removeCallbacks(rollingTick)
        rolling?.stopTriggering()   /* 不再发起新批次；已提交的批次最多等当前请求返回 */
        postStopAt = System.currentTimeMillis()
        toggleButton.text = "开始录音"
        statusText.text = "正在收尾（合并章节并出稿）…"
        pathText.text = if (!sessionSavesWav) "字幕与时间戳已存入 classmate.db"
        else "音频：${currentWav?.absolutePath ?: "无"}"
        refreshStorage()
    }

    /* ---------- 一次性 WAV 回放验证（不进正式录音路径） ---------- */

    private fun startReplayPick() {
        if (recording || postRunning || replaying) {
            Toast.makeText(this, "录音/收尾/回放进行中，稍后再试", Toast.LENGTH_SHORT).show()
            return
        }
        val dir = File(getExternalFilesDir(null), "recordings/_audio_archive_20261109")
        val wavs = dir.listFiles { f -> f.extension.equals("wav", true) }?.sortedBy { it.name }
        if (wavs.isNullOrEmpty()) {
            Toast.makeText(this, "回放目录无 WAV：${dir.absolutePath}", Toast.LENGTH_LONG).show()
            return
        }
        val names = wavs.map { it.name }.toTypedArray()
        AlertDialog.Builder(this).setTitle("选择回放 WAV（一次性验证入口）")
            .setItems(names) { _, which -> runReplay(wavs[which]) }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun runReplay(src: File) {
        replaying = true
        toggleButton.isEnabled = false
        replayButton.isEnabled = false
        /* 滚动状态重置：从头自动滚 */
        userAtBottom = true
        backToLatest.visibility = View.GONE
        subtitleScroll.post { subtitleScroll.scrollTo(0, 0) }
        rolling?.close(); rolling = null   /* 回放不触发 RollingSummarizer */
        /* 回放当作一次「新场」：清掉上次的冷却与上下文，并把库文件指过去，
         * 让 QA 的上下文兜底能读到本场文字 */
        QaEngine.get(this).onSessionStart(src.absolutePath)
        Log.i(POST_TAG, "replay start: ${src.name}（replay，不含云端增量）")
        statusText.text = "回放验证中：${src.name}"
        sessionStamp = "replay_" + src.nameWithoutExtension.removePrefix("classmate_")
        sessionPolicy = POLICY_NONE   /* 无 WAV 落盘、无删除动作、跳过课后校正 */
        sessionSavesWav = false
        currentWav = null
        sessionStartedAt = System.currentTimeMillis()
        sessionId = -1L
        liveRows.clear()
        latestChapters.clear()
        latestMeta = null
        /* 回放开始必须清空上一场残留行：renderSessionViews() 在 replaying=true 时
         * 会刻意保留实时行，不清就会出现「没录也没回放却有一堆旧字幕」。 */
        subtitleList.removeAllViews()
        rowViews.clear()
        partialView = null
        renderChapters()
        Thread({
            val err = replayFeed(src)
            handler.post {
                if (err != null) {
                    replaying = false
                    toggleButton.isEnabled = true
                    replayButton.isEnabled = true
                    statusText.text = "回放失败"
                    pathText.text = "回放失败：$err"
                } else {
                    statusText.text = "回放喂完，收尾出稿…"
                    postStopAt = System.currentTimeMillis()   /* 供 stop->note 日志 */
                    beginPostprocess()
                    Thread({
                        while (postRunning) Thread.sleep(200)
                        handler.post {
                            replaying = false
                            toggleButton.isEnabled = true
                            replayButton.isEnabled = true
                            /* 出稿后按 DB 全量重绘一次，再重置滚动回顶部 */
                            refreshSessionUi()
                            userAtBottom = true
                            backToLatest.visibility = View.GONE
                            subtitleScroll.post { subtitleScroll.scrollTo(0, 0) }
                            Log.i(POST_TAG, "replay done: ${src.name}（新 session 已入库，旧场保留对照）")
                        }
                    }, "classmate-replay-wait").start()
                }
            }
        }, "classmate-replay").start()
    }

    /** 顺序喂 WAV：320 样本/块（20ms）实时节奏；返回 null 成功，否则错误描述 */
    private fun replayFeed(src: File): String? {
        val bytes = src.readBytes()
        if (bytes.size < 44) return "文件不足 44B 头"
        /* 校验 RIFF + 16k/16bit/mono；data_size 字段不可信（早期 3 份为 0），
         * 样本数一律按「文件实际长度 - 44」计算 */
        if (!(bytes[0] == 'R'.code.toByte() && bytes[1] == 'I'.code.toByte() &&
              bytes[2] == 'F'.code.toByte() && bytes[3] == 'F'.code.toByte() &&
              bytes[8] == 'W'.code.toByte())) return "不是 RIFF/WAVE"
        val le16 = { off: Int ->
            (bytes[off].toInt() and 0xFF) or ((bytes[off + 1].toInt() and 0xFF) shl 8) }
        val le32 = { off: Int ->
            (bytes[off].toInt() and 0xFF) or ((bytes[off + 1].toInt() and 0xFF) shl 8) or
            ((bytes[off + 2].toInt() and 0xFF) shl 16) or ((bytes[off + 3].toInt() and 0xFF) shl 24) }
        if (le16(22) != 1) return "非 mono（channels=${le16(22)}）"
        if (le32(24) != 16000) return "非 16kHz（${le32(24)} Hz）"
        if (le16(34) != 16) return "非 16bit（${le16(34)} bit）"
        val totalSamples = (bytes.size - 44) / 2
        val modelsDir = if (asrReady()) ModelManager.modelsDir(this).absolutePath
                        else return "识别模型未就绪"
        val dir = File(getExternalFilesDir(null), "recordings")
        /* saveWav=false，且 dummy 路径绝不指向源 WAV（ca_wav_open "wb" 会截断）；
         * 路径仍放 recordings/ 下，保证新 session 进同一个 classmate.db；
         * 文件名带原 WAV 名，场标题经 base_title 落成 replay_<原名> */
        val dummy = File(dir, "replay_${src.name}").absolutePath
        val mainHandler = Handler(Looper.getMainLooper())
        val listener = ClassMateBridge.Listener { type, tsMs, text, _, code ->
            mainHandler.post { onCoreEvent(type, tsMs, text, code) }
        }
        val handle = bridge.nativeCreate(dummy, modelsDir, 4, false,
            hotwordsEdit.text.toString().trim(), listener)
        if (handle == 0L) return "nativeCreate 失败"
        var dropped = 0
        var off = 44
        var tsMs = 0L
        val block = ShortArray(320)
        try {
            while (off + 640 <= bytes.size) {
                var i = 0
                var p = off
                while (i < 320) {
                    block[i] = ((bytes[p + 1].toInt() shl 8) or
                                (bytes[p].toInt() and 0xFF)).toShort()
                    i++; p += 2
                }
                if (bridge.nativeFeed(handle, block, 320, tsMs) != 0) dropped++
                tsMs += 20
                off += 640
                Thread.sleep(20)   /* 按实时节奏喂 */
            }
            /* 不足 320 样本的尾块也喂掉 */
            val remain = (bytes.size - off) / 2
            if (remain > 0) {
                val tail = ShortArray(remain)
                var p = off
                var i = 0
                while (i < remain) {
                    tail[i] = ((bytes[p + 1].toInt() shl 8) or
                               (bytes[p].toInt() and 0xFF)).toShort()
                    i++; p += 2
                }
                if (bridge.nativeFeed(handle, tail, remain, tsMs) != 0) dropped++
            }
            bridge.nativeFlush(handle)   /* 收尾 drain */
        } finally {
            bridge.nativeDestroy(handle)
        }
        Log.i(POST_TAG, "replay fed: ${src.name} samples=$totalSamples dropped=$dropped")
        if (dropped != 0) return "dropped=$dropped（必须为 0）"
        return null
    }

    /* ---------- 停止后处理：主路径只做本地合并；降级才跑 note_llm ---------- */

    private fun beginPostprocess() {
        if (postRunning) return
        /* 授权已在开录前完成，这里不再弹窗 */
        startPostprocess()
    }

    private fun startPostprocess() {
        if (postRunning) return
        postRunning = true
        RecordingService.onClosed = null
        val dir = File(getExternalFilesDir(null), "recordings")
        val db = File(dir, "classmate.db")
        val note = SessionStore.noteFile(dir, sessionStamp)
        val wav = currentWav
        val policy = sessionPolicy
        val useCorrection = correctionCheck.isChecked
        val correctionDir = File(ModelManager.modelsDir(this), "sensevoice").absolutePath

        statusText.text = "收尾中：合并章节 → 出稿…"
        retryButton.visibility = View.GONE

        Thread({
            /* ---- M4：增量只负责「实时纠错 + 会中进度」，出稿统一走语义分章 ----
             * 实测（经济学课 4 分钟）：增量定稿只有 2 章，且第 2 章摘要会重复第 1 章；
             * 语义分章能出 11 章。代价是多一次云端调用（约 3 s），文本质量不减 ——
             * 因为 core/src/note.c 现在优先读增量回写的 text_refined 纠错文本。 */
            val r = rolling
            var tailPending = false
            var hadRolling = false
            if (r != null) {
                /* 尾批最多等 4 s：超时也照常出稿，尾段在 UI 标注「待补」 */
                if (r.needsFinalBatch()) tailPending = !r.finalBatch(4000L)
                hadRolling = r.hasRows()
                /* 会中 token 计入费用面板（每批都是真实云端调用，不能漏记） */
                LlmSettings.recordLastModel(prefs, r.cloudModelId)
                LlmSettings.recordSessionUsage(prefs, r.promptTokens, r.completionTokens)
                r.close()
                rolling = null
            }

            /* ---- 出稿：语义分章（云端 1 次调用；没 Key/离线时内部自动降级抽取式） ---- */
            var correctionRc = 0
            var correctionRan = false
            if (policy != POLICY_NONE && useCorrection && wav != null && wav.exists()) {
                correctionRan = true
                correctionRc = bridge.nativeCorrect(wav.absolutePath, db.absolutePath, correctionDir)
            }
            val noteSession = buildNote(db.absolutePath, note.absolutePath, forceLocal = false)

            var keepTemp = false
            var wavDeleted = false
            if (policy == POLICY_TEMP && wav != null && wav.exists()) {
                /* 校正未启用 / 校正成功 / 校正模型未下载（A 档校正默认关，不留 WAV）都删；
                 * 只有校正真正失败或笔记未生成时才保留待重试 */
                val correctionOk = !useCorrection || correctionRc == 0 || correctionRc == 1
                if (correctionOk && noteSession >= 0) {
                    wavDeleted = wav.delete()
                } else {
                    keepTemp = true
                    prefs.edit().putString(KEY_PENDING_WAV, wav.absolutePath).apply()
                }
            }
            if (!keepTemp) prefs.edit().putString(KEY_PENDING_WAV, "").apply()

            val ms = System.currentTimeMillis() - postStopAt
            Log.i(POST_TAG, "stop->note=${ms}ms source=${if (hadRolling) "rolling+note" else "note"} " +
                    "session=$noteSession rolling=$hadRolling " +
                    "tail=${if (tailPending) "pending" else "ok"}")
            handler.post {
                postRunning = false
                refreshStorage()
                stampSessionInfo()
                refreshSessionUi()
                retryButton.visibility = if (keepTemp) View.VISIBLE else View.GONE
                reportPostResult(
                    policy, useCorrection, correctionRan, correctionRc,
                    noteSession, keepTemp, note, wavDeleted
                )
                toggleButton.isEnabled = true
            }
        }, "classmate-post").start()
    }

    /** 主路径终稿结果：明确标注「会中增量终稿」，不出现「AI 精修中」 */
    private fun reportRollingResult(
        fr: RollingSummarizer.FinalResult, ms: Long, mergeMs: Long,
        note: File, tailPending: Boolean, wavDeleted: Boolean
    ) {
        val tailMissing = tailPending || fr.tailSegs > 0
        val msg = StringBuilder()
        if (tailMissing) {
            msg.append("终稿已出（先出已有结果 · ${ms} ms）\n")
        } else {
            msg.append("终稿已出（${ms} ms，本地合并 ${mergeMs} ms）\n")
        }
        msg.append("章节 ${fr.chapters} 个 · 会中增量 ${fr.seq} 批")
        if (fr.rejected > 0) msg.append(" · 起点无效丢弃 ${fr.rejected}")
        msg.append("\n来源：会中云端增量摘要 + 本地合并（未重跑全文）")
        if (tailMissing) {
            msg.append("\n尾段待补：${fr.tailSegs} 句未纳入摘要（尾批超时/失败），字幕与章节已照常保存")
        }
        if (sessionPolicy == POLICY_TEMP) {
            msg.append(if (wavDeleted) "\n临时音频已删除" else "\n临时音频删除失败：已保留")
        }
        msg.append("\n笔记：${note.absolutePath}")
        pathText.text = msg.toString()
        rollingStatusText.text = "增量摘要：已完成 ${fr.seq} 批（终稿已合并）"
        statusText.text = if (tailMissing) {
            "完成：终稿已出（先出已有结果）· 章节 ${fr.chapters} 个 · 尾段待补 ${fr.tailSegs} 句"
        } else {
            "完成：终稿已出（${ms} ms，合并 ${mergeMs} ms）· 章节 ${fr.chapters} 个 · 会中增量 ${fr.seq} 批"
        }
    }

    private fun reportPostResult(
        policy: Int,
        useCorrection: Boolean,
        correctionRan: Boolean,
        correctionRc: Int,
        noteSession: Long,
        keepTemp: Boolean,
        note: File,
        wavDeleted: Boolean
    ) {
        val seconds = (liveRows.lastOrNull()?.ts ?: 0L) / 1000.0
        val meta = if (noteSession >= 0)
            SessionStore.noteMeta(File(note.parentFile, "classmate.db"), noteSession) else null
        statusText.text = if (noteSession >= 0) {
            val src = when (meta?.source) {
                "rolling" -> "会中增量终稿"
                "llm" -> "LLM 语义分章"
                else -> "抽取式章节（降级结果）"
            }
            val base = String.format("完成：%.1f 秒，章节 %d 个 · %s", seconds, latestChapters.size, src)
            if ((meta?.source == "llm" || meta?.source == "rolling") && LlmSettings.showCost(prefs)) {
                val s = LlmSettings.costSummary(this, prefs, LlmSettings.lastModel(prefs))
                if (s.priceConfigured)
                    base + " · 估算 ${LlmSettings.formatCost(s.lastCost, s.currency)}（${s.lastPromptTokens + s.lastCompletionTokens} tok）"
                else base + " · 未配置单价、不估算费用（${s.lastPromptTokens + s.lastCompletionTokens} tok）"
            } else base
        } else "字幕已保存，章节生成失败"
        val notesInfo = if (note.exists()) "笔记：${note.name}" else "笔记未生成"
        val tail = if (noteSession >= 0 && meta != null && meta.source != "rolling") {
            if (meta.reason.isNotEmpty()) "；降级结果：${meta.reason}" else "；降级结果：未使用会中增量路径"
        } else ""
        pathText.text = (when (policy) {
            POLICY_NONE -> "不存音频；$notesInfo"
            POLICY_TEMP -> {
                if (keepTemp) "临时音频已保留（可重试）；$notesInfo"
                else if (wavDeleted) "临时音频已删除；$notesInfo"
                else "临时音频删除失败：已保留；$notesInfo"
            }
            else -> "长期保留音频；$notesInfo"
        }) + tail

        val msg = when {
            policy == POLICY_NONE -> "已生成章节与笔记（无音频）"
            !correctionRan -> "已生成章节与笔记（未做二校正）"
            !useCorrection -> "已生成章节与笔记；临时音频按设置处理"
            correctionRc == 0 -> "二次校正完成，章节与笔记已更新"
            correctionRc == 1 -> "校正模型未下载：本次保留原文字幕"
            correctionRc == 2 -> "没有可用音频：保留原文与章节"
            else -> "二次校正失败：原文字幕保留，临时录音未删除"
        }
        Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
    }

    private fun retryPostprocess() {
        if (recording || postRunning) return
        val pending = pendingWavPath()
        if (pending.isEmpty() || !File(pending).exists()) {
            Toast.makeText(this, "没有待重试的临时录音", Toast.LENGTH_SHORT).show()
            retryButton.visibility = View.GONE
            return
        }
        currentWav = File(pending)
        sessionStamp = currentWav!!.name.removePrefix("classmate_").removeSuffix(".wav")
        sessionPolicy = POLICY_TEMP
        beginPostprocess()
    }

    /* ---------- 事件 ---------- */

    private fun onCoreEvent(type: Int, tsMs: Long, text: String?, code: Int) {
        when (type) {
            ClassMateBridge.EV_ASR_PARTIAL -> {
                if (text.isNullOrEmpty()) return
                val v = partialView ?: TextView(this).also {
                    it.textSize = 14f
                    it.setTextColor(Color.DKGRAY)
                    it.setPadding(0, 4, 0, 4)
                    subtitleList.addView(it)
                    partialView = it
                }
                val shown = "[${fmtTs(tsMs)}] $text（识别中）"
                if (v.text.toString() != shown) v.text = shown   /* 节流：文本没变不重绘 */
                livePartialCount++
                if (livePartialCount % 10 == 1) {
                    Log.i(POST_TAG, "live PARTIAL #$livePartialCount ts=${tsMs}ms")
                }
                scrollToBottom()
            }
            ClassMateBridge.EV_ASR_FINAL -> {
                if (text.isNullOrEmpty()) return
                partialView?.let { subtitleList.removeView(it) }
                partialView = null
                liveRows.add(SubRow(tsMs, text, null))
                addRowView(SubRow(tsMs, text, null))
                Log.i(POST_TAG, "live FINAL #${liveRows.size} ts=${tsMs}ms")
                /* 回放路径不经过 RecordingService，所以这里手动灌 QA 上下文，
                 * 否则回放时注入套话永远拿不到「上文」，无法验证关联 50 字。 */
                if (replaying) QaEngine.get(this).feedContext(tsMs, text)
                scrollToBottom()
                rolling?.onFinal()   /* 满 N 条 FINAL 触发一次增量（不阻塞本回调） */
            }
            ClassMateBridge.EV_ERROR -> {
                pathText.text = "核心错误 $code：${text ?: ""}"
            }
        }
    }

    /* ---------- 字幕 / 章节视图 ---------- */

    private fun addRowView(row: SubRow) {
        val tv = TextView(this).apply {
            textSize = 14f
            setPadding(0, 5, 0, 5)
            isClickable = true
            tag = row.ts
            setOnClickListener { onTimestampClick(row.ts) }
            text = formatRow(row)
        }
        subtitleList.addView(tv)
        rowViews.add(tv)
    }

    private fun formatRow(row: SubRow): String {
        val head = "[${fmtTs(row.ts)}] ${row.text}"
        return if (row.corrected != null && row.corrected != row.text)
            "$head\n校正：${row.corrected}"
        else head
    }

    private fun scrollToBottom() {
        /* 仅当用户本来就在底部时才自动滚；手动上滑后停住不被拽回。
         * pending 合并：同一时刻至多一个 fullScroll 排队，不随 20ms 块刷屏 */
        if (!userAtBottom || scrollPending) return
        scrollPending = true
        subtitleScroll.post {
            scrollPending = false
            if (userAtBottom) subtitleScroll.fullScroll(ScrollView.FOCUS_DOWN)
        }
    }

    private fun fmtTs(tsMs: Long): String {
        if (prefs.getBoolean(KEY_CLOCK, false) && sessionStartedAt > 0) {
            val date = Date(sessionStartedAt + tsMs)
            return SimpleDateFormat("HH:mm:ss", Locale.US).format(date)
        }
        val sec = (tsMs / 1000)
        return String.format("%02d:%02d", sec / 60, sec % 60)
    }

    /* 单一数据源：热词框第一项即课堂名，其余项仍作为 ASR 热词。 */
    private val courseName: String
        get() = hotwordsEdit.text.toString().trim()
            .split(',', '，').firstOrNull()?.trim().orEmpty()

    private fun setCourseName(name: String) {
        val parts = hotwordsEdit.text.toString().split(',', '，')
            .map { it.trim() }.filter { it.isNotEmpty() }
        val merged = (listOf(name.trim()) + parts.drop(1))
            .filter { it.isNotEmpty() }.joinToString(",")
        hotwordsEdit.setText(merged)   /* TextWatcher 会实时写 prefs */
    }

    /* 出稿后把课堂名写入场记录：title/course 同步为课堂名（note.c 出笔记读
     * sessions.title；空则不写，保留 base_title 默认名）。SQLiteDatabase 直开，
     * 不动 core。 */
    private fun stampSessionInfo() {
        val course = courseName
        if (course.isEmpty()) return
        val dir = File(getExternalFilesDir(null), "recordings")
        val info = SessionStore.latestSession(File(dir, "classmate.db")) ?: return
        try {
            val db = SQLiteDatabase.openDatabase(
                File(dir, "classmate.db").absolutePath, null, SQLiteDatabase.OPEN_READWRITE)
            db.execSQL("UPDATE sessions SET title=?, course=? WHERE id=?",
                arrayOf(course, course, info.id))
            db.close()
            Log.i(POST_TAG, "session ${info.id} titled: $course")
        } catch (e: Exception) {
            Log.w(POST_TAG, "stampSessionInfo failed: $e")
        }
    }

    /* 长按字幕区：重命名当前场（写 sessions.title/course，不改文件名） */
    private fun showRenameDialog() {
        val dir = File(getExternalFilesDir(null), "recordings")
        val dbFile = File(dir, "classmate.db")
        val info = SessionStore.latestSession(dbFile)
        if (info == null) {
            Toast.makeText(this, "当前没有任何场次", Toast.LENGTH_SHORT).show()
            return
        }
        val input = EditText(this).apply {
            hint = "课堂名"
            setText(courseName)
        }
        AlertDialog.Builder(this).setTitle("重命名当前场（session ${info.id}）")
            .setView(input)
            .setPositiveButton("保存") { _, _ ->
                val name = input.text.toString().trim()
                if (name.isEmpty()) return@setPositiveButton
                setCourseName(name)
                try {
                    val db = SQLiteDatabase.openDatabase(
                        dbFile.absolutePath, null, SQLiteDatabase.OPEN_READWRITE)
                    db.execSQL("UPDATE sessions SET title=?, course=? WHERE id=?",
                        arrayOf(name, name, info.id))
                    db.close()
                    Toast.makeText(this, "已重命名为「$name」", Toast.LENGTH_SHORT).show()
                } catch (e: Exception) {
                    Toast.makeText(this, "重命名失败：$e", Toast.LENGTH_LONG).show()
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /* ===== M4.1 / M4.5：文本回放走真实链路 =====
     * 以前文本回放只往界面塞字幕、库里没有场次，所以既出不了笔记，也压不到增量摘要。
     * 现在按真实录音的顺序走：
     *   建场次(status='recording') → 逐条落库 → RollingSummarizer 每 8 句增量一次
     *   → 结束把场次改 done → startPostprocess() 合并定稿出稿
     * 这样一节经济学课就能同时验证「增量不超时」和「出稿可读」，不必再用旧 WAV。 */
    private var replayDb: SQLiteDatabase? = null
    private var replaySessionId = -1L

    private fun beginReplaySession(dbFile: File, stamp: String): Long {
        closeReplayDb()
        if (!dbFile.exists()) {
            Log.e(POST_TAG, "beginReplaySession: db missing ${dbFile.absolutePath}")
            return -1L
        }
        val db = try {
            SQLiteDatabase.openDatabase(dbFile.absolutePath, null, SQLiteDatabase.OPEN_READWRITE)
        } catch (e: Exception) {
            Log.e(POST_TAG, "beginReplaySession open failed", e)
            return -1L
        }
        val course = prefs.getString("hotwords", "")?.substringBefore(",")?.trim()
            ?.takeIf { it.isNotEmpty() } ?: "经济学"
        val cv = ContentValues().apply {
            put("title", stamp)
            put("course", course)
            put("started_at", System.currentTimeMillis())
            put("ended_at", 0L)
            /* 增量摘要靠 status='recording' 定位场次；出稿只认 status='done' */
            put("status", "recording")
        }
        val sid = try {
            db.insertOrThrow("sessions", null, cv)
        } catch (e: Exception) {
            Log.e(POST_TAG, "insert replay session failed", e)
            db.close()
            return -1L
        }
        replayDb = db
        replaySessionId = sid
        Log.i(POST_TAG, "replay session begin sid=$sid stamp=$stamp course=$course")
        return sid
    }

    /** 每条字幕单独提交：增量的 newSegments 要能立刻读到，不能攒在大事务里 */
    private fun appendReplaySegment(startMs: Long, endMs: Long, text: String) {
        val db = replayDb ?: return
        try {
            db.execSQL(
                "INSERT INTO segments(session_id,start_ms,end_ms,text) VALUES(?,?,?,?)",
                arrayOf<Any>(
                    replaySessionId, startMs,
                    if (endMs > startMs) endMs else startMs + 2000L, text
                )
            )
        } catch (e: Exception) {
            Log.e(POST_TAG, "append replay segment failed", e)
        }
    }

    private fun finishReplaySession() {
        val db = replayDb
        if (db != null) {
            try {
                val cv = ContentValues().apply {
                    put("ended_at", System.currentTimeMillis())
                    put("status", "done")
                }
                db.update("sessions", cv, "id=?", arrayOf(replaySessionId.toString()))
                Log.i(POST_TAG, "replay session finish sid=$replaySessionId status=done")
            } catch (e: Exception) {
                Log.e(POST_TAG, "finish replay session failed", e)
            } finally {
                closeReplayDb()
            }
        }
    }

    private fun closeReplayDb() {
        try {
            replayDb?.close()
        } catch (ignored: Exception) {
        }
        replayDb = null
    }

    /** 回放喂完：交给正式课后处理（有增量走合并，没增量退回 buildNote） */
    private fun postprocessTextReplay(segs: org.json.JSONArray, assetName: String) {
        finishReplaySession()
        Log.i(POST_TAG, "text replay postprocess start segs=${segs.length()} " +
                "sid=$replaySessionId rolling=${rolling != null} asset=$assetName")
        postStopAt = System.currentTimeMillis()
        statusText.text = "回放结束：合并章节 → 出稿…"
        startPostprocess()
    }

    /** 上次崩溃/被系统杀掉会留下 status='recording' 的脏场次。增量摘要靠
     *  status='recording' 定位当前场次，脏数据会让新一场的纠错写到旧场次上
     *  （实测出现过 refined 落到两天前的 8 句场次）。App 启动时没有任何录音在跑，
     *  这些必然是遗留，统一收敛为 done。 */
    private fun cleanupStaleRecordingSessions() {
        val dbFile = File(File(getExternalFilesDir(null), "recordings"), "classmate.db")
        if (!dbFile.exists()) return
        var db: SQLiteDatabase? = null
        try {
            db = SQLiteDatabase.openDatabase(
                dbFile.absolutePath, null, SQLiteDatabase.OPEN_READWRITE
            )
            val st = db.compileStatement(
                "UPDATE sessions SET status='done', ended_at=?" +
                        " WHERE status='recording' AND (ended_at IS NULL OR ended_at=0)"
            )
            st.bindLong(1, System.currentTimeMillis())
            val n = st.executeUpdateDelete()
            st.close()
            if (n > 0) Log.w(POST_TAG, "cleanup stale recording sessions: $n")
        } catch (e: Exception) {
            Log.w(POST_TAG, "cleanupStaleRecordingSessions failed: ${e.message}")
        } finally {
            try {
                db?.close()
            } catch (ignored: Exception) {
            }
        }
    }

    private fun refreshSessionUi() {
        val dir = File(getExternalFilesDir(null), "recordings")
        val db = File(dir, "classmate.db")
        val info = SessionStore.latestSession(db) ?: return
        sessionId = info.id
        sessionStartedAt = info.startedAt
        latestChapters.clear()
        latestChapters.addAll(SessionStore.chapters(db, info.id))
        latestMeta = SessionStore.noteMeta(db, info.id)
        if (!recording && !replaying) {
            liveRows.clear()
            liveRows.addAll(SessionStore.segments(db, info.id).map {
                SubRow(it.startMs, it.text, it.corrected)
            })
        }
        renderSessionViews()
    }

    private fun renderSessionViews() {
        /* 录音/回放中保留实时字幕，不按 DB 覆盖；其余情况按 DB（含校正）渲染 */
        if (!recording && !replaying) {
            subtitleList.removeAllViews()
            rowViews.clear()
            partialView = null
            for (row in liveRows) addRowView(row)
        }
        renderChapters()
    }

    private fun renderChapters() {
        chapterList.removeAllViews()
        val meta = latestMeta
        if (meta != null) {
            chapterList.addView(TextView(this).apply {
                textSize = 11f
                setTextColor(
                    if (meta.source == "rolling") 0xFF2E7D32.toInt() else Color.GRAY
                )
                setPadding(0, 0, 0, 4)
                text = when (meta.source) {
                    "rolling" ->
                        "生成：会中增量云端摘要 · ${meta.llmCalls} 批 · 本地合并定稿"
                    "llm" ->
                        "生成：LLM 语义分章（降级路径） · ${meta.compressRounds} 轮压缩 · ${meta.llmCalls} 次调用"
                    else ->
                        "生成：抽取式章节（降级）" +
                                (if (meta.reason.isNotEmpty()) " · ${meta.reason}" else "")
                }
            })
        }
        if (latestChapters.isEmpty()) {
            chapterList.addView(TextView(this).apply {
                text = if (recording) "录音中，课后生成章节" else "暂无章节（先录一段课）"
                textSize = 12f
                setTextColor(Color.GRAY)
            })
            return
        }
        for (ch in latestChapters) {
            val tv = TextView(this).apply {
                textSize = 13f
                setPadding(0, 5, 0, 5)
                isClickable = true
                text = "[${fmtTs(ch.startMs)}] ${ch.title}"
                setOnClickListener { onTimestampClick(ch.startMs) }
            }
            if (ch.summary.isNotEmpty()) {
                tv.append("\n${ch.summary}")
                tv.textSize = 12f
            }
            chapterList.addView(tv)
        }
    }

    private fun onTimestampClick(tsMs: Long) {
        val target = rowViews.lastOrNull { (it.tag as? Long ?: Long.MIN_VALUE) <= tsMs }
            ?: rowViews.firstOrNull()
        if (target == null) {
            Toast.makeText(this, "本场还没有可跳转的字幕", Toast.LENGTH_SHORT).show()
            return
        }
        subtitleScroll.post {
            subtitleScroll.smoothScrollTo(0, target.top)
            highlight(target)
        }
    }

    private fun highlight(view: TextView) {
        view.setBackgroundColor(0x33FFEB3B)
        handler.postDelayed({ view.setBackgroundColor(Color.TRANSPARENT) }, 1600)
    }

    /** M4.3：把当前的 notes_*.md 导出到 下载/ClassMate/，可直接用数据线拷到电脑 */
    private fun exportNote(note: File) {
        val uri = RecordingStore.exportAs(this, note, "text/markdown")
        Log.i(POST_TAG, "note export file=${note.name} bytes=${note.length()} uri=$uri")
        Toast.makeText(
            this,
            if (uri == null) "导出失败（需 Android 10 以上）"
            else "已导出：下载/ClassMate/${note.name}",
            Toast.LENGTH_LONG
        ).show()
    }

    private fun showNotes() {
        val dir = File(getExternalFilesDir(null), "recordings")
        val note = if (sessionStamp.isNotEmpty()) SessionStore.noteFile(dir, sessionStamp)
        else dir.listFiles { f -> f.name.startsWith("notes_") }?.maxByOrNull { it.lastModified() }
        if (note == null || !note.exists()) {
            Toast.makeText(this, "笔记还没生成（录完一段课会自动生成）", Toast.LENGTH_SHORT).show()
            return
        }
        val content = note.readText()
        val spannable = SpannableString(content)
        val regex = Regex("\\[(\\d{2}):(\\d{2})\\]")
        for (m in regex.findAll(content)) {
            val start = m.range.first
            val end = m.range.last + 1
            val ms = (m.groupValues[1].toLong() * 60 + m.groupValues[2].toLong()) * 1000
            spannable.setSpan(object : ClickableSpan() {
                override fun onClick(widget: View) {
                    onTimestampClick(ms)
                    (widget as? TextView)?.let { highlight(it) }
                }
            }, start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        val tv = TextView(this).apply {
            text = spannable
            textSize = 13f
            movementMethod = LinkMovementMethod.getInstance()
            setPadding(16, 16, 16, 16)
        }
        val scroll = ScrollView(this).apply { addView(tv) }
        AlertDialog.Builder(this)
            .setTitle("课堂笔记 · ${note.name}")
            .setView(scroll)
            .setPositiveButton("导出到下载") { _, _ -> exportNote(note) }
            .setNegativeButton("关闭", null)
            .show()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        val granted = grantResults.all { it == PackageManager.PERMISSION_GRANTED }
        if (requestCode == REQ_PERMISSIONS && granted) startRecording()
        else statusText.text = "需要录音权限才能开始"
    }

    companion object {
        private const val REQ_PERMISSIONS = 1001
        /* M3.4 内录授权（系统「录制或投放」对话框） */
        private const val REQ_PROJECTION = 1002
        private const val POLICY_NONE = 0
        private const val POLICY_TEMP = 1
        private const val POLICY_KEEP = 2

        private const val KEY_AUDIO_POLICY = "audio_policy"
        private const val KEY_KEEP_COUNT = "keep_count"
        private const val KEY_MAX_GB = "max_gb"
        private const val KEY_CLOCK = "clock_mode"
        /* 与 QaEngine 共用同一 prefs 文件的键 */
        private const val KEY_QA_CONSENT = "qa_cloud_consent"
        private const val KEY_CORRECTION = "correction_on"
        private const val KEY_HOTWORDS = "hotwords"
        private const val KEY_PENDING_WAV = "pending_wav"
        private const val POST_TAG = "ClassMate/Post"
    }
}
